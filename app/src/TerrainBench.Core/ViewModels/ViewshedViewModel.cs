using System.Collections.ObjectModel;
using System.Diagnostics;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using TerrainBench.Engine;
using TerrainBench.Presentation;

namespace TerrainBench.ViewModels;

public enum ViewshedComparison
{
    None,
    FastAndNaive,
    PreviousRun,
}

/// <summary>One line of the viewshed legend: a state's colour, name and cell count; clicking it hides or shows those cells.</summary>
public sealed partial class LegendRow(string name, Rgba colour, string count, int layer) : ObservableObject
{
    public string Name { get; } = name;
    public Rgba Colour { get; } = colour;
    public string Count { get; } = count;

    /// <summary>The layer this row stands for: a <see cref="CellState"/>, or <see cref="MapPixels.HighlightLayer"/>.</summary>
    public int Layer { get; } = layer;

    /// <summary>False when the map leaves this layer out.</summary>
    [ObservableProperty]
    private bool _isShown = true;
}

/// <summary>
/// A viewshed run: parameters in, a map of cell states out. Runs off the UI thread, reports
/// progress, can be cancelled, and in compare mode runs both algorithms and marks where they disagree.
/// </summary>
public sealed partial class ViewshedViewModel : ObservableObject
{
    private readonly Func<ITile?> _tile;
    private CancellationTokenSource? _cancellation;

    public ViewshedViewModel(Func<ITile?> tile)
    {
        _tile = tile;
        ObserverLatitude = new NumberField("vs-observer-lat", "Observer Latitude", "degrees", "36.5", InsideTile(true), shortLabel: "Latitude");
        ObserverLongitude = new NumberField("vs-observer-lon", "Observer Longitude", "degrees", "-111.5", InsideTile(false), shortLabel: "Longitude");
        ObserverHeight = new NumberField("vs-observer-height", "Observer Height", "m above ground", "2", Rules.HeightAboveGround("The observer"), hint: PlainWords.HeightExplanation, shortLabel: "Height");
        RadiusKm = new NumberField("vs-radius", "Radius", "km", "30", Rules.Positive("The radius", 1000), hint: PlainWords.RadiusExplanation);
        Spacing = new NumberField("vs-spacing", "Cell Spacing", "m", "30", Rules.Positive("The spacing", 100000), hint: PlainWords.CellSpacingExplanation);
        RefractionK = new NumberField("vs-k", "Refraction Factor - k", "", "4/3", Rules.Positive("k"), hint: PlainWords.RefractionExplanation);

        foreach (var field in Fields)
        {
            bool observer = field == ObserverLatitude || field == ObserverLongitude;
            field.Changed += (_, _) =>
            {
                RunCommand.NotifyCanExecuteChanged();
                OnSettingChanged(observer);
            };
        }
    }

    public NumberField ObserverLatitude { get; }
    public NumberField ObserverLongitude { get; }
    public NumberField ObserverHeight { get; }
    public NumberField RadiusKm { get; }
    public NumberField Spacing { get; }
    public NumberField RefractionK { get; }

    public IReadOnlyList<NumberField> Fields => [ObserverLatitude, ObserverLongitude, ObserverHeight, RadiusKm, Spacing, RefractionK];

    public IReadOnlyList<Interpolation> InterpolationChoices { get; } = [Interpolation.Nearest, Interpolation.Bilinear];

    public IReadOnlyList<ViewshedAlgorithm> AlgorithmChoices { get; } = [ViewshedAlgorithm.Fast, ViewshedAlgorithm.Naive];

    [ObservableProperty]
    private Interpolation _interpolation = Interpolation.Nearest;

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(IsAlgorithmChoiceEnabled))]
    private ViewshedAlgorithm _algorithm = ViewshedAlgorithm.Fast;

    public IReadOnlyList<string> ComparisonChoices { get; } = ["None", "Fast vs. Naive (This Run)", "Previous Run"];

    /// <summary>Index into <see cref="ComparisonChoices"/>.</summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(IsAlgorithmChoiceEnabled), nameof(Comparison))]
    private int _comparisonIndex;

    /// <summary>
    /// What a run is compared against: nothing; both algorithms on the same run, marking where they
    /// disagree; or the previous run, marking every cell that changed -- so the effect of changing
    /// one parameter, like k, shows even when it moves a few thousand cells out of millions.
    /// </summary>
    public ViewshedComparison Comparison
    {
        get => (ViewshedComparison)Math.Clamp(ComparisonIndex, 0, 2);
        set => ComparisonIndex = (int)value;
    }

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(IsAlgorithmChoiceEnabled), nameof(IsIdle), nameof(ShowResult), nameof(NeedsRunAgain))]
    [NotifyCanExecuteChangedFor(nameof(RunCommand), nameof(CancelCommand))]
    private bool _isRunning;

    /// <summary>0 to 1.</summary>
    [ObservableProperty]
    private double _progress;

    [ObservableProperty]
    private string _progressText = string.Empty;

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(HasResult), nameof(ShowResult), nameof(ShowLayer), nameof(NeedsRunAgain))]
    private ViewshedMap? _map;

    /// <summary>
    /// The cells the comparison marks: where fast and naive disagree (both confident, different
    /// answers), or which changed since the previous run. Null without a comparison.
    /// </summary>
    [ObservableProperty]
    private bool[]? _highlights;

    private (ViewshedMap Map, IReadOnlyList<(string Name, string Value)> Settings)? _previous;

    [ObservableProperty]
    private string? _summary;

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(HasError))]
    private string? _error;

    public ObservableCollection<LegendRow> Legend { get; } = [];

    public bool HasResult => Map is not null;

    /// <summary>The summary and legend show only for a finished run: while another runs, the last one's numbers would mislead.</summary>
    public bool ShowResult => HasResult && !IsRunning && !IsObserverMoved;

    public bool HasError => Error is not null;

    public bool IsIdle => !IsRunning;

    public bool IsAlgorithmChoiceEnabled => Comparison != ViewshedComparison.FastAndNaive && !IsRunning;

    public bool CanRun => !IsRunning && _tile() is not null && Fields.All(f => f.IsValid);

    public bool CanCancel => IsRunning;

    [RelayCommand(CanExecute = nameof(CanRun))]
    public async Task RunAsync()
    {
        var tile = _tile();
        if (tile is null || !CanRun) return;

        var query = new ViewshedQuery(
            ObserverLatitude.Value!.Value, ObserverLongitude.Value!.Value, ObserverHeight.Value!.Value,
            RadiusKm.Value!.Value, Spacing.Value!.Value, RefractionK.Value!.Value, Interpolation, Algorithm);

        _cancellation = new CancellationTokenSource();
        var token = _cancellation.Token;
        IsRunning = true;
        Error = null;
        Progress = 0;

        try
        {
            if (Comparison != ViewshedComparison.FastAndNaive)
            {
                var (result, elapsed) = await RunOne(tile, query, $"{PlainWords.Algorithm(query.Algorithm)} Viewshed", token);
                if (!result.IsOk) { Fail(result.Error!); return; }

                var description = Describe(query);
                string summary = $"{PlainWords.Algorithm(query.Algorithm)} viewshed of {Format.Count(result.Value.Rows)} × {Format.Count(result.Value.Cols)} cells in {Format.Milliseconds(elapsed)}.";
                bool[]? changed = null;

                if (Comparison == ViewshedComparison.PreviousRun)
                {
                    if (_previous is { } previous && SameGrid(previous.Map, result.Value))
                    {
                        (changed, int count) = Changes(previous.Map, result.Value);
                        summary += " " + DescribeChange(previous.Settings, description, count);
                    }
                    else
                    {
                        summary += _previous is null
                            ? " There's no previous run to compare with yet: this run is the reference for the next one."
                            : " The previous run covered a different grid, so there's nothing to compare cell for cell; this run is the new reference.";
                    }
                }

                Show(result.Value, changed, "Changed Since the Previous Run", query);
                Summary = summary;
                _previous = (result.Value, description);
                return;
            }

            var (fast, fastTime) = await RunOne(tile, query with { Algorithm = ViewshedAlgorithm.Fast }, "Fast Viewshed (1 of 2)", token);
            if (!fast.IsOk) { Fail(fast.Error!); return; }
            var (naive, naiveTime) = await RunOne(tile, query with { Algorithm = ViewshedAlgorithm.Naive }, "Naive Viewshed (2 of 2)", token);
            if (!naive.IsOk) { Fail(naive.Error!); return; }

            var (disagree, compared, differing) = Disagreement(fast.Value, naive.Value);
            Show(naive.Value, disagree, "Fast and Naive Disagree", query);
            Summary = $"{Format.Count(differing)} of {Format.Count(compared)} cells disagree ({Format.Percent(compared == 0 ? 0 : (double)differing / compared)}), counting only cells both algorithms answered with confidence. " +
                      $"Fast took {Format.Milliseconds(fastTime)}, naive {Format.Milliseconds(naiveTime)}. The map shows the naive result, with disagreements highlighted.";
            _previous = (naive.Value, Describe(query with { Algorithm = ViewshedAlgorithm.Naive }));
        }
        finally
        {
            IsRunning = false;
            ProgressText = string.Empty;
            _cancellation.Dispose();
            _cancellation = null;
        }
    }

    [RelayCommand(CanExecute = nameof(CanCancel))]
    public void Cancel() => _cancellation?.Cancel();

    /// <summary>
    /// The observer placed on the map. The old result is taken off the map, and a plain fast run
    /// (no comparison) is redone straight away; anything slower waits for Run.
    /// </summary>
    public void PlaceObserver(double latitudeDeg, double longitudeDeg)
    {
        ObserverLatitude.SetValue(latitudeDeg);
        ObserverLongitude.SetValue(longitudeDeg);
        if (Map is not null && CanRunByItself) _ = RunAsync();
    }

    public void OnTileChanged()
    {
        _cancellation?.Cancel();
        foreach (var field in Fields) field.Validate();
        Map = null;
        Highlights = null;
        Summary = null;
        Error = null;
        _previous = null;
        IsObserverMoved = false;
        IsStale = false;
        Legend.Clear();
        RunCommand.NotifyCanExecuteChanged();
    }

    public void AppendReport(ResultsReport report)
    {
        report.Section("Viewshed inputs")
            .Line("Observer", $"{ObserverLatitude.Text}, {ObserverLongitude.Text} degrees, {ObserverHeight.Text} m above ground")
            .Line("Radius", RadiusKm.Text + " km")
            .Line("Cell spacing", Spacing.Text + " m")
            .Line("Refraction factor k", RefractionK.Text)
            .Line("Interpolation", PlainWords.Interpolation(Interpolation))
            .Line("Algorithm", Comparison == ViewshedComparison.FastAndNaive ? "Both" : PlainWords.Algorithm(Algorithm))
            .Line("Compared with", ComparisonChoices[(int)Comparison]);

        report.Section("Viewshed result");
        if (Map is null)
        {
            report.Line("Result", Error ?? "not computed");
            return;
        }

        report.Line("Grid", $"{Map.Rows} x {Map.Cols} cells");
        foreach (var row in Legend) report.Line(row.Name, row.Count);
        if (Summary is not null) report.Line("Summary", Summary);
    }

    public void Restore(IReadOnlyDictionary<string, string> saved)
    {
        foreach (var field in Fields)
        {
            if (saved.TryGetValue(field.Key, out var text)) field.Text = text;
        }
    }

    /// <summary>Cells where both algorithms answered with confidence and the answers differ.</summary>
    public static (bool[] Mask, int Compared, int Differing) Disagreement(ViewshedMap a, ViewshedMap b)
    {
        var mask = new bool[a.Cells.Length];
        int compared = 0, differing = 0;
        for (int i = 0; i < mask.Length; i++)
        {
            bool confident = IsConfident(a.Cells[i]) && IsConfident(b.Cells[i]);
            if (!confident) continue;
            compared++;
            if (a.Cells[i] != b.Cells[i])
            {
                mask[i] = true;
                differing++;
            }
        }
        return (mask, compared, differing);
    }

    /// <summary>Every cell whose state differs between two runs over the same grid.</summary>
    public static (bool[] Mask, int Changed) Changes(ViewshedMap before, ViewshedMap after)
    {
        var mask = new bool[after.Cells.Length];
        int changed = 0;
        for (int i = 0; i < mask.Length; i++)
        {
            if (before.Cells[i] == after.Cells[i]) continue;
            mask[i] = true;
            changed++;
        }
        return (mask, changed);
    }

    /// <summary>Two runs cover the same cells when the engine laid out the very same grid.</summary>
    public static bool SameGrid(ViewshedMap a, ViewshedMap b) =>
        a.Rows == b.Rows && a.Cols == b.Cols && a.SpacingDeg == b.SpacingDeg && a.ColStepDeg == b.ColStepDeg
        && a.SouthWestCellLatitudeDeg == b.SouthWestCellLatitudeDeg && a.SouthWestCellLongitudeDeg == b.SouthWestCellLongitudeDeg;

    /// <summary>The settings a run's cells depend on, by name, so two runs can say which of them changed.</summary>
    private IReadOnlyList<(string Name, string Value)> Describe(ViewshedQuery query) =>
    [
        ("k", RefractionK.Text.Trim()),
        ("height", Format.Number(query.ObserverHeightAboveGroundM) + " m"),
        ("algorithm", PlainWords.Algorithm(query.Algorithm).ToLowerInvariant()),
        ("interpolation", PlainWords.Interpolation(query.Interpolation).ToLowerInvariant()),
    ];

    /// <summary>"k: 4/3 → 1e12 changed 8,583 cells.", or that nothing was changed to change anything.</summary>
    public static string DescribeChange(IReadOnlyList<(string Name, string Value)> before, IReadOnlyList<(string Name, string Value)> after, int changedCells)
    {
        var differences = before.Zip(after).Where(p => p.First.Value != p.Second.Value)
            .Select(p => $"{p.First.Name}: {p.First.Value} → {p.Second.Value}").ToList();
        if (differences.Count == 0)
        {
            return changedCells == 0
                ? "Same settings as the previous run, so nothing changed."
                : $"Same settings as the previous run, yet {Format.Count(changedCells)} cells changed.";
        }
        return $"{string.Join(" and ", differences)} changed {Format.Count(changedCells)} {(changedCells == 1 ? "cell" : "cells")}.";
    }

    private static bool IsConfident(CellState state) => state is CellState.Visible or CellState.NotVisible;

    private async Task<(EngineResult<ViewshedMap> Result, TimeSpan Elapsed)> RunOne(ITile tile, ViewshedQuery query, string label, CancellationToken token)
    {
        ProgressText = label;
        Progress = 0;
        var progress = new Progress<double>(fraction => Progress = fraction);
        var stopwatch = Stopwatch.StartNew();
        var result = await Task.Run(() => tile.Viewshed(query, progress, token), CancellationToken.None);
        stopwatch.Stop();
        return (result, stopwatch.Elapsed);
    }

    private void Fail(EngineError error)
    {
        Map = null;
        Highlights = null;
        Legend.Clear();
        Summary = null;
        Error = error.Kind == EngineErrorKind.Cancelled ? "Cancelled. The previous result was cleared; run again to compute a new one." : error.Message;
    }

    private void Show(ViewshedMap map, bool[]? highlights, string highlightName, ViewshedQuery query)
    {
        int[] counts = new int[4];
        foreach (var cell in map.Cells) counts[(int)cell]++;

        Legend.Clear();
        AddLegendRow("Visible", MapPixels.VisibleColour, counts[(int)CellState.Visible], (int)CellState.Visible);
        AddLegendRow("Not Visible", MapPixels.NotVisibleColour, counts[(int)CellState.NotVisible], (int)CellState.NotVisible);
        AddLegendRow("No Confident Answer (Missing Data on the Way)", MapPixels.DegradedColour, counts[(int)CellState.Degraded], (int)CellState.Degraded);
        AddLegendRow("Not Reached", MapPixels.NotReachedColour, counts[(int)CellState.NotCovered], (int)CellState.NotCovered);
        if (highlights is not null)
        {
            AddLegendRow(highlightName, MapPixels.DisagreementColour, highlights.Count(d => d), MapPixels.HighlightLayer);
        }

        RadiusLabel = Format.Number(query.RadiusKm) + " km";
        Highlights = highlights;
        Map = map;
        IsObserverMoved = false;
        IsStale = false;
        Progress = 1;
    }

    private void AddLegendRow(string name, Rgba colour, int count, int layer) =>
        Legend.Add(new LegendRow(name, colour, Format.Count(count), layer) { IsShown = !HiddenLayers[layer] });

    /// <summary>Hides or shows one legend entry's cells on the map; the counts stay as they are.</summary>
    [RelayCommand]
    public void ToggleLayer(LegendRow row)
    {
        row.IsShown = !row.IsShown;
        var hidden = (bool[])HiddenLayers.Clone();
        hidden[row.Layer] = !row.IsShown;
        HiddenLayers = hidden;
    }

    /// <summary>Which layers the map leaves out, indexed by <see cref="CellState"/> and then <see cref="MapPixels.HighlightLayer"/>.</summary>
    [ObservableProperty]
    private bool[] _hiddenLayers = new bool[MapPixels.HighlightLayer + 1];

    /// <summary>How opaque the viewshed layer is drawn, 0 to 100.</summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(MapOpacity))]
    private double _overlayOpacity = 100;

    /// <summary>The radius of the shown run, for the ring drawn around it.</summary>
    [ObservableProperty]
    private string? _radiusLabel;

    /// <summary>The observer moved since the shown run: the map no longer shows it, as it would be in the wrong place.</summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(ShowResult), nameof(ShowLayer), nameof(NeedsRunAgain), nameof(RunAgainText))]
    private bool _isObserverMoved;

    /// <summary>Another setting changed since the shown run: the map still shows it, faded, for reference.</summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(MapOpacity), nameof(NeedsRunAgain), nameof(RunAgainText))]
    private bool _isStale;

    public bool ShowLayer => Map is not null && !IsObserverMoved;

    public double MapOpacity => Math.Clamp(OverlayOpacity, 0, 100) / 100 * (IsStale ? 0.35 : 1);

    public bool NeedsRunAgain => Map is not null && !IsRunning && (IsObserverMoved || IsStale);

    public string RunAgainText => IsObserverMoved
        ? "The observer moved. Run the viewshed again for the new position."
        : "Settings changed. The map shows the last run, faded; run the viewshed again to update it.";

    /// <summary>A plain fast run is quick enough to redo by itself when the observer is placed on the map.</summary>
    public bool CanRunByItself => Algorithm == ViewshedAlgorithm.Fast && Comparison == ViewshedComparison.None && CanRun;

    private void OnSettingChanged(bool observer)
    {
        if (Map is null || IsRunning) return;
        if (observer) IsObserverMoved = true;
        else IsStale = true;
    }

    partial void OnInterpolationChanged(Interpolation value) => OnSettingChanged(observer: false);

    partial void OnAlgorithmChanged(ViewshedAlgorithm value) => OnSettingChanged(observer: false);

    private Func<double, string?> InsideTile(bool latitude) => value =>
    {
        var info = _tile()?.Info;
        string what = latitude ? "The observer latitude" : "The observer longitude";
        if (info is null) return latitude ? Rules.Between(what, -89.9, 89.9)(value) : Rules.Between(what, -180, 180)(value);
        double min = latitude ? info.SouthWestLatitudeDeg : info.SouthWestLongitudeDeg;
        double max = latitude ? info.NorthEastLatitudeDeg : info.NorthEastLongitudeDeg;
        if (value < min || value > max) return $"{what} must be inside the open tile: {Format.Number(min)} to {Format.Number(max)}.";
        if (latitude && Math.Abs(value) > 89.9) return "A viewshed can't be laid out this close to a pole. Use a latitude between -89.9 and 89.9.";
        return null;
    };
}
