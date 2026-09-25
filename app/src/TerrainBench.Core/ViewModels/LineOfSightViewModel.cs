using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using TerrainBench.Engine;
using TerrainBench.Presentation;

namespace TerrainBench.ViewModels;

public enum Verdict
{
    None,
    Visible,
    Blocked,
    NoConfidentAnswer,
}

public enum FresnelState
{
    None,
    Clear,
    PartlyObstructed,
    Blocked,
    NoConfidentAnswer,
}

/// <summary>A labelled value in a result list, e.g. "Terrain Height There: 1900 m above mean sea level".</summary>
public sealed record ResultRow(string Label, string Value);

/// <summary>Observer, target and parameters in; the verdict, its reason, the chart data and Fresnel out.</summary>
public sealed partial class LineOfSightViewModel : ObservableObject
{
    private readonly Func<ITile?> _tile;

    public LineOfSightViewModel(Func<ITile?> tile)
    {
        _tile = tile;
        ObserverLatitude = new NumberField("los-observer-lat", "Observer Latitude", "degrees", "36.3", InsideTileLatitude("observer"), shortLabel: "Latitude");
        ObserverLongitude = new NumberField("los-observer-lon", "Observer Longitude", "degrees", "-111.5", InsideTileLongitude("observer"), shortLabel: "Longitude");
        ObserverHeight = new HeightInput("los-observer-height", "Observer Height", "The observer", "2", PlainWords.HeightExplanation);
        TargetLatitude = new NumberField("los-target-lat", "Target Latitude", "degrees", "36.35", InsideTileLatitude("target"), shortLabel: "Latitude");
        TargetLongitude = new NumberField("los-target-lon", "Target Longitude", "degrees", "-111.45", InsideTileLongitude("target"), shortLabel: "Longitude");
        TargetHeight = new HeightInput("los-target-height", "Target Height", "The target", "2", PlainWords.HeightExplanation);
        RefractionK = new NumberField("los-k", "Refraction Factor - k", "", "4/3", Rules.Positive("k"), hint: PlainWords.RefractionExplanation);
        Spacing = new NumberField("los-spacing", "Sample Spacing", "m along the path", "30", Rules.Positive("The spacing", 100000), hint: PlainWords.PathSpacingExplanation);
        Frequency = new NumberField("los-frequency", "Radio Frequency", "MHz, optional", "", Rules.Positive("The frequency", 1e6), optional: true,
            hint: PlainWords.FrequencyExplanation);

        foreach (var field in Fields) field.Changed += (_, _) => CheckCommand.NotifyCanExecuteChanged();
        foreach (var height in Heights) height.Changed += (_, _) => CheckCommand.NotifyCanExecuteChanged();
    }

    public NumberField ObserverLatitude { get; }
    public NumberField ObserverLongitude { get; }
    public HeightInput ObserverHeight { get; }
    public NumberField TargetLatitude { get; }
    public NumberField TargetLongitude { get; }

    /// <summary>The target's height: above its ground, or -- to place it by altitude -- above sea level or the ellipsoid.</summary>
    public HeightInput TargetHeight { get; }
    public NumberField RefractionK { get; }
    public NumberField Spacing { get; }
    public NumberField Frequency { get; }

    public IReadOnlyList<NumberField> Fields => [ObserverLatitude, ObserverLongitude, .. ObserverHeight.Fields, TargetLatitude, TargetLongitude, .. TargetHeight.Fields, RefractionK, Spacing, Frequency];

    public IReadOnlyList<HeightInput> Heights => [ObserverHeight, TargetHeight];

    public IReadOnlyList<Interpolation> InterpolationChoices { get; } = [Interpolation.Nearest, Interpolation.Bilinear];

    [ObservableProperty]
    private Interpolation _interpolation = Interpolation.Nearest;

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(HasResult), nameof(IsBlocked))]
    private PathAnalysis? _analysis;

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(VerdictText), nameof(IsBlocked), nameof(IsVisibleVerdict), nameof(IsNoConfidentAnswer))]
    private Verdict _verdict = Verdict.None;

    public bool IsVisibleVerdict => Verdict == Verdict.Visible;

    public bool IsNoConfidentAnswer => Verdict == Verdict.NoConfidentAnswer;

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(HasReason))]
    private string? _reason;

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(HasError))]
    private string? _error;

    /// <summary>The Fresnel result as one line of text, for the copied report.</summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(HasFresnel))]
    private string? _fresnelText;

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(FresnelVerdictText), nameof(IsFresnelClear), nameof(IsFresnelPartlyObstructed), nameof(IsFresnelBlocked), nameof(IsFresnelUnknown))]
    private FresnelState _fresnel = FresnelState.None;

    [ObservableProperty]
    private string? _fresnelDetail;

    /// <summary>How much of the first Fresnel zone radius stays free at the worst point, clamped to 0..1 for a bar.</summary>
    [ObservableProperty]
    private double _fresnelFreeFraction;

    public bool HasFresnel => FresnelText is not null;

    public bool IsFresnelClear => Fresnel == FresnelState.Clear;

    public bool IsFresnelPartlyObstructed => Fresnel == FresnelState.PartlyObstructed;

    public bool IsFresnelBlocked => Fresnel == FresnelState.Blocked;

    public bool IsFresnelUnknown => Fresnel == FresnelState.NoConfidentAnswer;

    public string FresnelVerdictText => Fresnel switch
    {
        FresnelState.Clear => "Clear",
        FresnelState.PartlyObstructed => "Partly Obstructed",
        FresnelState.Blocked => "Blocked",
        FresnelState.NoConfidentAnswer => "No Confident Answer",
        _ => string.Empty,
    };

    /// <summary>The path itself: its length, sampling and eye heights.</summary>
    public ObservableCollection<ResultRow> PathDetails { get; } = [];

    /// <summary>Where and how the terrain blocks the sight line; empty unless the verdict is Blocked.</summary>
    public ObservableCollection<ResultRow> ObstructionDetails { get; } = [];

    public bool HasObstruction => ObstructionDetails.Count > 0;

    /// <summary>Every result row, path first, then the obstruction.</summary>
    public IReadOnlyList<ResultRow> Details => [.. PathDetails, .. ObstructionDetails];

    public bool HasResult => Analysis is not null;

    public bool HasReason => Reason is not null;

    public bool HasError => Error is not null;

    public bool IsBlocked => Verdict == Verdict.Blocked;

    public string VerdictText => Verdict switch
    {
        Verdict.Visible => "Visible",
        Verdict.Blocked => "Blocked",
        Verdict.NoConfidentAnswer => "No Confident Answer",
        _ => string.Empty,
    };

    /// <summary>The query as it stood when the shown result was computed.</summary>
    public PathQuery? LastQuery { get; private set; }

    public bool CanCheck => _tile() is not null && Fields.All(f => f.IsValid);

    [RelayCommand(CanExecute = nameof(CanCheck))]
    public void Check()
    {
        var tile = _tile();
        if (tile is null || !CanCheck) return;

        var query = new PathQuery(
            ObserverLatitude.Value!.Value, ObserverLongitude.Value!.Value, ObserverHeight.Height,
            TargetLatitude.Value!.Value, TargetLongitude.Value!.Value, TargetHeight.Height,
            Spacing.Value!.Value, RefractionK.Value!.Value, Interpolation, Frequency.Value ?? 0);

        var clock = System.Diagnostics.Stopwatch.StartNew();
        var result = tile.AnalyzePath(query);
        LastCheckDuration = clock.Elapsed;
        ClearResultDetails();

        if (!result.IsOk)
        {
            Analysis = null;
            LastQuery = null;
            Verdict = Verdict.None;
            Reason = null;
            Error = result.Error!.Message;
            return;
        }

        var analysis = result.Value;
        Error = null;
        LastQuery = query;

        if (analysis.LineOfSightStatus != ComputationStatus.Ok)
        {
            Verdict = Verdict.NoConfidentAnswer;
            Reason = PlainWords.WhyNoConfidentAnswer(analysis.LineOfSightStatus);
        }
        else if (analysis.IsVisible)
        {
            Verdict = Verdict.Visible;
            Reason = "The sight line clears the terrain, with Earth's curvature under the chosen k, all the way to the target.";
        }
        else
        {
            Verdict = Verdict.Blocked;
            Reason = null;
        }

        PathDetails.Add(new ResultRow("Path Length", Format.Distance(analysis.TotalDistanceM)));
        PathDetails.Add(new ResultRow("Samples", $"{Format.Count(analysis.Samples.Count)}, every {Format.Metres(Spacing.Value!.Value)}"));
        // Every height the engine returns is in the tile's own datum, whatever datum the query's heights came in.
        string heightsAbove = " " + PlainWords.DatumWords(analysis.HeightsDatum);
        if (analysis.ObserverEyeHeightM is double observerEye)
        {
            PathDetails.Add(new ResultRow("Observer Eye", Format.Metres(observerEye) + heightsAbove));
            PathDetails.Add(new ResultRow("Target Eye", Format.Metres(analysis.TargetEyeHeightM!.Value) + heightsAbove));
        }

        if (analysis.Blocking is { } blocking && Verdict == Verdict.Blocked)
        {
            Reason = PlainWords.BlockingSentence(blocking);
            ObstructionDetails.Add(new ResultRow("Blocked At", $"{Format.Degrees(blocking.LatitudeDeg)}, {Format.Degrees(blocking.LongitudeDeg)}"));
            ObstructionDetails.Add(new ResultRow("Distance from Observer", Format.Distance(blocking.DistanceM)));
            ObstructionDetails.Add(new ResultRow("Terrain Height There", Format.Metres(blocking.ElevationM) + heightsAbove));
            ObstructionDetails.Add(new ResultRow("Sight Line Falls Short By", Format.Metres(blocking.ClearanceDeficitM)));
            ObstructionDetails.Add(new ResultRow("Terrain Feature", PlainWords.Feature(blocking.Feature)));
        }
        OnPropertyChanged(nameof(HasObstruction));

        if (analysis.Fresnel is { } fresnel)
        {
            (Fresnel, FresnelDetail) = fresnel.Status != ComputationStatus.Ok
                ? (FresnelState.NoConfidentAnswer, PlainWords.WhyNoConfidentAnswer(fresnel.Status))
                : fresnel.MinClearanceFraction >= 1
                    ? (FresnelState.Clear, $"The worst point keeps {Format.Percent(fresnel.MinClearanceFraction)} of the first Fresnel zone radius free.")
                    : fresnel.MinClearanceFraction >= 0
                        ? (FresnelState.PartlyObstructed, $"Only {Format.Percent(fresnel.MinClearanceFraction)} of the first Fresnel zone radius is free at the worst point.")
                        : (FresnelState.Blocked, $"The terrain rises through the sight line itself (clearance fraction {Format.Number(fresnel.MinClearanceFraction, "0.000")}).");
            FresnelFreeFraction = fresnel.Status == ComputationStatus.Ok ? Math.Clamp(fresnel.MinClearanceFraction, 0, 1) : 0;
            FresnelText = $"Fresnel clearance: {FresnelVerdictText.ToLowerInvariant()}. {FresnelDetail}";
        }

        Analysis = analysis;
    }

    private void ClearResultDetails()
    {
        PathDetails.Clear();
        ObstructionDetails.Clear();
        OnPropertyChanged(nameof(HasObstruction));
        FresnelText = null;
        FresnelDetail = null;
        FresnelFreeFraction = 0;
        Fresnel = FresnelState.None;
    }

    /// <summary>Looks the other way: the observer and the target trade places and heights, and the path is checked again.</summary>
    [RelayCommand]
    public void Swap()
    {
        (ObserverLatitude.Text, TargetLatitude.Text) = (TargetLatitude.Text, ObserverLatitude.Text);
        (ObserverLongitude.Text, TargetLongitude.Text) = (TargetLongitude.Text, ObserverLongitude.Text);
        ObserverHeight.SwapWith(TargetHeight);
        if (CanCheck) Check();
    }

    /// <summary>How long the last check took, so a marker being dragged can tell whether checking live keeps up.</summary>
    public TimeSpan LastCheckDuration { get; private set; }

    public void PlaceObserver(double latitudeDeg, double longitudeDeg)
    {
        ObserverLatitude.SetValue(latitudeDeg);
        ObserverLongitude.SetValue(longitudeDeg);
        if (CanCheck) Check();
    }

    public void PlaceTarget(double latitudeDeg, double longitudeDeg)
    {
        TargetLatitude.SetValue(latitudeDeg);
        TargetLongitude.SetValue(longitudeDeg);
        if (CanCheck) Check();
    }

    /// <summary>Re-checks the coordinate fields against a newly opened tile and clears a stale result.</summary>
    public void OnTileChanged()
    {
        foreach (var field in Fields) field.Validate();
        Analysis = null;
        LastQuery = null;
        Verdict = Verdict.None;
        Reason = null;
        Error = null;
        ClearResultDetails();
        CheckCommand.NotifyCanExecuteChanged();
    }

    public void AppendReport(ResultsReport report)
    {
        report.Section("Line of sight inputs")
            .Line("Observer", $"{ObserverLatitude.Text}, {ObserverLongitude.Text} degrees, {ObserverHeight.AsTyped}")
            .Line("Target", $"{TargetLatitude.Text}, {TargetLongitude.Text} degrees, {TargetHeight.AsTyped}")
            .Line("Refraction factor k", RefractionK.Text)
            .Line("Sample spacing", Spacing.Text + " m")
            .Line("Interpolation", PlainWords.Interpolation(Interpolation))
            .Line("Frequency", string.IsNullOrWhiteSpace(Frequency.Text) ? "none" : Frequency.Text + " MHz");

        if (Analysis is null || LastQuery is null)
        {
            report.Section("Line of sight result").Line("Result", Error ?? "not computed");
            return;
        }

        report.Section("Line of sight result").Line("Verdict", VerdictText);
        if (Reason is not null) report.Line("Why", Reason);
        foreach (var row in Details) report.Line(row.Label, row.Value);
        if (FresnelText is not null) report.Line("Fresnel", FresnelText);
        if (Analysis.Blocking is { } blocking)
        {
            report.Line("Clearance deficit (exact)", Format.RoundTrip(blocking.ClearanceDeficitM) + " m");
        }

        report.Line("Spacing passed to the engine", Format.RoundTrip(Analysis.SpacingDeg) + " degrees");
        report.Line("Same query on the command line",
            $"TerrainEngine.exe los <tile> <swLat> <swLon> {Format.RoundTrip(LastQuery.ObserverLatitudeDeg)} {Format.RoundTrip(LastQuery.ObserverLongitudeDeg)} " +
            $"{Format.RoundTrip(LastQuery.TargetLatitudeDeg)} {Format.RoundTrip(LastQuery.TargetLongitudeDeg)} {Format.RoundTrip(Analysis.SpacingDeg)} " +
            $"{PlainWords.CommandLine(LastQuery.ObserverHeight)} {PlainWords.CommandLine(LastQuery.TargetHeight)} {Format.RoundTrip(LastQuery.RefractionK)} " +
            (LastQuery.Interpolation == Interpolation.Bilinear ? "bilinear" : "nearest"));
    }

    public void Restore(IReadOnlyDictionary<string, string> saved)
    {
        foreach (var height in Heights) height.Restore(saved);
        foreach (var field in Fields)
        {
            if (saved.TryGetValue(field.Key, out var text)) field.Text = text;
        }
    }

    private Func<double, string?> InsideTileLatitude(string what) => value =>
    {
        var info = _tile()?.Info;
        if (info is null) return Rules.Between($"The {what} latitude", -90, 90)(value);
        return value < info.SouthWestLatitudeDeg || value > info.NorthEastLatitudeDeg
            ? $"The {what} latitude must be inside the open tile: {Format.Number(info.SouthWestLatitudeDeg)} to {Format.Number(info.NorthEastLatitudeDeg)}."
            : null;
    };

    private Func<double, string?> InsideTileLongitude(string what) => value =>
    {
        var info = _tile()?.Info;
        if (info is null) return Rules.Between($"The {what} longitude", -180, 180)(value);
        return value < info.SouthWestLongitudeDeg || value > info.NorthEastLongitudeDeg
            ? $"The {what} longitude must be inside the open tile: {Format.Number(info.SouthWestLongitudeDeg)} to {Format.Number(info.NorthEastLongitudeDeg)}."
            : null;
    };
}
