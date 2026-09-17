using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using TerrainBench.Engine;
using TerrainBench.Presentation;
using TerrainBench.Services;

namespace TerrainBench.ViewModels;

public enum MainTab
{
    Terrain,
    LineOfSight,
    Viewshed,
    About,
}

/// <summary>The window's view model: the tile, the three tools, the shared map and the app-wide commands.</summary>
public sealed partial class MainViewModel : ObservableObject, IDisposable
{
    private readonly ITerrainEngine? _engine;
    private readonly IDialogService _dialogs;
    private readonly IClipboardService _clipboard;
    private readonly ISettingsStore _settings;
    private readonly IMapExporter _exporter;
    private readonly string _sampleTilePath;

    public MainViewModel(
        EngineResult<ITerrainEngine> engine,
        IDialogService dialogs,
        IClipboardService clipboard,
        ISettingsStore settings,
        IMapExporter exporter,
        string appVersion,
        string sampleTilePath)
    {
        _engine = engine.IsOk ? engine.Value : null;
        _dialogs = dialogs;
        _clipboard = clipboard;
        _settings = settings;
        _exporter = exporter;
        _sampleTilePath = sampleTilePath;

        Terrain = new TerrainViewModel(_engine);
        LineOfSight = new LineOfSightViewModel(() => Terrain.Tile);
        Viewshed = new ViewshedViewModel(() => Terrain.Tile);
        About = new AboutViewModel(appVersion, _engine?.Version ?? "unavailable", _engine?.Commit ?? "unavailable");

        EngineError = engine.IsOk ? null : engine.Error!.Message;

        Terrain.TileChanged += (_, _) =>
        {
            LineOfSight.OnTileChanged();
            Viewshed.OnTileChanged();
            ShowSamplePrompt = false;
            NotifyTileCommands();
        };
        LineOfSight.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName == nameof(LineOfSightViewModel.Analysis)) ExportProfileCsvCommand.NotifyCanExecuteChanged();
        };
        Viewshed.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName == nameof(ViewshedViewModel.Map)) ExportViewshedPngCommand.NotifyCanExecuteChanged();
        };
    }

    public TerrainViewModel Terrain { get; }

    public LineOfSightViewModel LineOfSight { get; }

    public ViewshedViewModel Viewshed { get; }

    public AboutViewModel About { get; }

    /// <summary>Set when TerrainEngineApi.dll couldn't be loaded; the app can then only explain why.</summary>
    public string? EngineError { get; }

    public bool HasEngineError => EngineError is not null;

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(IsTerrainTab), nameof(IsLineOfSightTab), nameof(IsViewshedTab), nameof(IsAboutTab), nameof(SelectedTabIndex))]
    private MainTab _selectedTab = MainTab.Terrain;

    public int SelectedTabIndex
    {
        get => (int)SelectedTab;
        set => SelectedTab = (MainTab)Math.Clamp(value, 0, 3);
    }

    public bool IsTerrainTab => SelectedTab == MainTab.Terrain;

    public bool IsLineOfSightTab => SelectedTab == MainTab.LineOfSight;

    public bool IsViewshedTab => SelectedTab == MainTab.Viewshed;

    public bool IsAboutTab => SelectedTab == MainTab.About;

    /// <summary>The side panel's width, as last dragged.</summary>
    public double PanelWidth { get; set; } = DefaultPanelWidth;

    public const double DefaultPanelWidth = 440;

    /// <summary>The profile chart's panel under the map; closed, it leaves a Profile button to reopen it.</summary>
    [ObservableProperty]
    private bool _isProfileOpen = true;

    /// <summary>The profile panel's height, as last dragged.</summary>
    public double ProfileHeight { get; set; } = DefaultProfileHeight;

    public const double DefaultProfileHeight = 260;

    /// <summary>One side panel shows at a time; its tab picks it.</summary>
    [RelayCommand]
    public void ShowTab(MainTab tab) => SelectedTab = tab;

    [RelayCommand]
    public void CloseProfile() => IsProfileOpen = false;

    [RelayCommand]
    public void OpenProfile() => IsProfileOpen = true;

    /// <summary>Which of the profile chart's lines show; the legend's buttons turn each on and off.</summary>
    [ObservableProperty]
    private bool _profileShowTerrain = true;

    [ObservableProperty]
    private bool _profileShowCurvature = true;

    [ObservableProperty]
    private bool _profileShowSightLine = true;

    [ObservableProperty]
    private bool _profileShowFresnel = true;

    /// <summary>First start, or a remembered tile that is gone: offer the bundled sample tile.</summary>
    [ObservableProperty]
    private bool _showSamplePrompt;

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(HasStatus))]
    private string? _statusMessage;

    [ObservableProperty]
    private bool _statusIsError;

    public bool HasStatus => StatusMessage is not null;

    /// <summary>Reads settings and reopens the last tile, or offers the sample tile.</summary>
    public void Start()
    {
        var saved = _settings.Load();
        if (saved is null)
        {
            ShowSamplePrompt = _engine is not null;
            return;
        }

        LineOfSight.Restore(saved.Fields);
        Viewshed.Restore(saved.Fields);
        LineOfSight.Interpolation = saved.LineOfSightInterpolation;
        Viewshed.Interpolation = saved.ViewshedInterpolation;
        Viewshed.Algorithm = saved.ViewshedAlgorithm;
        if (Enum.TryParse<MainTab>(saved.SelectedTab, out var tab)) SelectedTab = tab;
        IsProfileOpen = saved.ProfileOpen;
        ProfileShowTerrain = saved.ProfileShowTerrain;
        ProfileShowCurvature = saved.ProfileShowCurvature;
        ProfileShowSightLine = saved.ProfileShowSightLine;
        ProfileShowFresnel = saved.ProfileShowFresnel;
        Viewshed.OverlayOpacity = double.IsFinite(saved.ViewshedOpacity) ? Math.Clamp(saved.ViewshedOpacity, 0, 100) : 100;
        if (saved.HiddenViewshedLayers is { Length: MapPixels.HighlightLayer + 1 } hidden) Viewshed.HiddenLayers = hidden;
        ProfileHeight = double.IsFinite(saved.ProfileHeight) && saved.ProfileHeight >= 120 ? saved.ProfileHeight : DefaultProfileHeight;
        PanelWidth = double.IsFinite(saved.PanelWidth) && saved.PanelWidth >= 320 ? saved.PanelWidth : DefaultPanelWidth;

        Terrain.SouthWestLatitude.Text = saved.SouthWestLatitude;
        Terrain.SouthWestLongitude.Text = saved.SouthWestLongitude;

        if (saved.TilePath is not null && File.Exists(saved.TilePath) && Terrain.Open(saved.TilePath) is null)
        {
            // Restored fields were validated before the tile's extent was known.
            foreach (var field in LineOfSight.Fields.Concat(Viewshed.Fields)) field.Validate();
            LineOfSight.CheckCommand.NotifyCanExecuteChanged();
            Viewshed.RunCommand.NotifyCanExecuteChanged();
            return;
        }

        ShowSamplePrompt = _engine is not null;
        if (saved.TilePath is not null) Status($"The last tile, {Path.GetFileName(saved.TilePath)}, couldn't be opened again. {Terrain.LoadError}", error: true);
    }

    public void SaveSettings()
    {
        var fields = new Dictionary<string, string>();
        foreach (var field in LineOfSight.Fields.Concat(Viewshed.Fields)) fields[field.Key] = field.Text;

        _settings.Save(new AppSettings
        {
            TilePath = Terrain.TilePath,
            SouthWestLatitude = Terrain.SouthWestLatitude.Text,
            SouthWestLongitude = Terrain.SouthWestLongitude.Text,
            Fields = fields,
            LineOfSightInterpolation = LineOfSight.Interpolation,
            ViewshedInterpolation = Viewshed.Interpolation,
            ViewshedAlgorithm = Viewshed.Algorithm,
            SelectedTab = SelectedTab.ToString(),
            ProfileOpen = IsProfileOpen,
            ProfileShowTerrain = ProfileShowTerrain,
            ProfileShowCurvature = ProfileShowCurvature,
            ProfileShowSightLine = ProfileShowSightLine,
            ProfileShowFresnel = ProfileShowFresnel,
            ViewshedOpacity = Viewshed.OverlayOpacity,
            HiddenViewshedLayers = Viewshed.HiddenLayers,
            ProfileHeight = ProfileHeight,
            PanelWidth = PanelWidth,
        });
    }

    public bool CanUseEngine => _engine is not null;

    [RelayCommand(CanExecute = nameof(CanUseEngine))]
    public async Task OpenTileAsync()
    {
        var path = await _dialogs.PickTileAsync();
        if (path is null) return;
        OpenPath(path);
    }

    [RelayCommand(CanExecute = nameof(CanUseEngine))]
    public void OpenSampleTile() => OpenPath(_sampleTilePath);

    public bool CanReopen => _engine is not null && Terrain.TilePath is not null;

    /// <summary>Opens the current file again with the south-west corner as edited.</summary>
    [RelayCommand(CanExecute = nameof(CanReopen))]
    public void Reopen()
    {
        var error = Terrain.Reopen();
        if (error is null) Status($"Opened {Terrain.FileName} with its south-west corner at {Terrain.SouthWestLatitude.Text}, {Terrain.SouthWestLongitude.Text}.");
        else Status(error, error: true);
    }

    public void OpenPath(string path)
    {
        if (!Terrain.FillCornerFromName(path) && !Terrain.SouthWestLatitude.IsValid)
        {
            Status($"{Path.GetFileName(path)} doesn't name its south-west corner (like N36W112). Enter the corner, then choose Reopen.", error: true);
            Terrain.TilePath = path;
            NotifyTileCommands();
            return;
        }

        var error = Terrain.Open(path);
        if (error is null) Status($"Opened {Terrain.FileName}.");
        else Status(error, error: true);
        NotifyTileCommands();
    }

    /// <summary>
    /// A click on the map, at a coordinate the map view worked out from the pixel. A plain click
    /// places the observer; with <paramref name="target"/> (Ctrl + click) it places the target, which
    /// only the line of sight has. The viewshed panel's observer is placed while that panel is open.
    /// </summary>
    public void OnMapClicked(double latitudeDeg, double longitudeDeg, bool target = false)
    {
        if (Terrain.Tile is null) return;
        if (SelectedTab == MainTab.Viewshed)
        {
            if (!target) Viewshed.PlaceObserver(latitudeDeg, longitudeDeg);
            return;
        }

        SelectedTab = MainTab.LineOfSight;
        if (target) LineOfSight.PlaceTarget(latitudeDeg, longitudeDeg);
        else LineOfSight.PlaceObserver(latitudeDeg, longitudeDeg);
    }

    /// <summary>F5: checks the line of sight or runs the viewshed, whichever panel is open; on the others it does nothing.</summary>
    [RelayCommand]
    public async Task RunActivePanelAsync()
    {
        switch (SelectedTab)
        {
            case MainTab.LineOfSight when LineOfSight.CheckCommand.CanExecute(null):
                LineOfSight.CheckCommand.Execute(null);
                break;
            case MainTab.Viewshed when Viewshed.RunCommand.CanExecute(null):
                await Viewshed.RunCommand.ExecuteAsync(null);
                break;
        }
    }

    /// <summary>Puts the full engine commit on the clipboard, from the About panel.</summary>
    [RelayCommand]
    public async Task CopyCommitAsync()
    {
        await _clipboard.SetTextAsync(About.EngineCommit);
        Status("Engine commit copied to the clipboard.");
    }

    public bool CanCopyResults => true;

    [RelayCommand]
    public async Task CopyResultsAsync()
    {
        var report = new ResultsReport(About.AppVersion, About.EngineVersion, About.EngineCommit);
        report.Section("Tile")
            .Line("File", Terrain.TilePath ?? "none")
            .Line("South-west corner", $"{Terrain.SouthWestLatitude.Text}, {Terrain.SouthWestLongitude.Text}")
            .Line("Extent", Terrain.ExtentText)
            .Line("Grid", Terrain.GridText)
            .Line("Resolution", Terrain.ResolutionText)
            .Line("Voids", Terrain.VoidText);

        if (SelectedTab == MainTab.Viewshed) Viewshed.AppendReport(report);
        else LineOfSight.AppendReport(report);

        await _clipboard.SetTextAsync(report.ToString());
        Status("Results copied to the clipboard.");
    }

    public bool CanExportProfile => LineOfSight.Analysis is not null;

    [RelayCommand(CanExecute = nameof(CanExportProfile))]
    public async Task ExportProfileCsvAsync()
    {
        if (LineOfSight.Analysis is not { } analysis) return;
        var path = await _dialogs.PickSaveFileAsync("Export profile", "profile.csv", "csv", "CSV file");
        if (path is null) return;

        try
        {
            await File.WriteAllTextAsync(path, ProfileCsv.Write(analysis));
            Status($"Profile saved to {path}.");
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            Status($"The profile couldn't be saved: {ex.Message}", error: true);
        }
    }

    public bool CanExportMap => Terrain.Posts is not null;

    [RelayCommand(CanExecute = nameof(CanExportMap))]
    public Task ExportMapPngAsync() => ExportPng(includeViewshed: false);

    public bool CanExportViewshed => Terrain.Posts is not null && Viewshed.Map is not null;

    [RelayCommand(CanExecute = nameof(CanExportViewshed))]
    public Task ExportViewshedPngAsync() => ExportPng(includeViewshed: true);

    private async Task ExportPng(bool includeViewshed)
    {
        if (Terrain.Posts is not { } posts || Terrain.Tile is not { } tile) return;
        var path = await _dialogs.PickSaveFileAsync(includeViewshed ? "Export viewshed" : "Export map", includeViewshed ? "viewshed.png" : "map.png", "png", "PNG image");
        if (path is null) return;

        try
        {
            await _exporter.SavePngAsync(path, posts, includeViewshed ? Viewshed.Map : null, includeViewshed ? Viewshed.Highlights : null, tile.Info);
            Status($"Image saved to {path}.");
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            Status($"The image couldn't be saved: {ex.Message}", error: true);
        }
    }

    public void Status(string message, bool error = false)
    {
        StatusIsError = error;
        StatusMessage = message;
    }

    private void NotifyTileCommands()
    {
        ReopenCommand.NotifyCanExecuteChanged();
        ExportMapPngCommand.NotifyCanExecuteChanged();
        ExportViewshedPngCommand.NotifyCanExecuteChanged();
        ExportProfileCsvCommand.NotifyCanExecuteChanged();
    }

    public void Dispose() => Terrain.Dispose();
}

/// <summary>Versions for the About page.</summary>
public sealed class AboutViewModel(string appVersion, string engineVersion, string engineCommit)
{
    public string AppVersion { get; } = appVersion;

    public string EngineVersion { get; } = engineVersion;

    public string EngineCommit { get; } = engineCommit;

    public string Summary => $"TerrainBench {AppVersion}, engine {EngineVersion}, built from commit {EngineCommit}.";
}
