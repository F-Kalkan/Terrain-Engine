using TerrainBench.Engine;
using TerrainBench.Services;
using TerrainBench.ViewModels;
using Xunit;

namespace TerrainBench.Tests.ViewModels;

public class MainViewModelTests : IDisposable
{
    private readonly FakeEngine _engine = new();
    private readonly FakeDialogs _dialogs = new();
    private readonly FakeClipboard _clipboard = new();
    private readonly MemorySettings _settings = new();
    private readonly FakeExporter _exporter = new();
    private readonly string _folder = Path.Combine(Path.GetTempPath(), $"terrainbench-vm-{Guid.NewGuid():N}");

    public MainViewModelTests() => Directory.CreateDirectory(_folder);

    public void Dispose()
    {
        try { Directory.Delete(_folder, true); } catch (IOException) { }
    }

    private MainViewModel Create(EngineResult<ITerrainEngine>? engine = null) =>
        new(engine ?? EngineResult<ITerrainEngine>.Ok(_engine), _dialogs, _clipboard, _settings, _exporter, "1.2.3", Path.Combine(_folder, "N36W112.hgt"));

    [Fact]
    public void A_first_start_offers_the_sample_tile_and_one_click_opens_it()
    {
        var vm = Create();
        vm.Start();
        Assert.True(vm.ShowSamplePrompt);

        vm.OpenSampleTileCommand.Execute(null);

        Assert.True(vm.Terrain.IsLoaded);
        Assert.False(vm.ShowSamplePrompt);
        Assert.Equal("36", vm.Terrain.SouthWestLatitude.Text);
        Assert.Equal("-112", vm.Terrain.SouthWestLongitude.Text);
        Assert.Equal("Opened N36W112.hgt.", vm.StatusMessage);
    }

    [Fact]
    public void A_failed_open_is_shown_in_plain_words_and_leaves_no_tile()
    {
        _engine.OnOpen = (_, _, _) => EngineResult<ITile>.Fail(EngineErrorKind.LoadFailed, "The file N36W112.hgt is empty.");
        var vm = Create();

        vm.OpenSampleTileCommand.Execute(null);

        Assert.False(vm.Terrain.IsLoaded);
        Assert.True(vm.StatusIsError);
        Assert.Equal("The file N36W112.hgt is empty.", vm.StatusMessage);
    }

    [Fact]
    public async Task A_file_without_a_corner_in_its_name_asks_for_the_corner_instead_of_guessing()
    {
        _dialogs.TileToPick = Path.Combine(_folder, "my-terrain.hgt");
        var vm = Create();

        await vm.OpenTileCommand.ExecuteAsync(null);

        Assert.Empty(_engine.Opened);
        Assert.Contains("doesn't name its south-west corner", vm.StatusMessage);

        vm.Terrain.SouthWestLatitude.Text = "36";
        vm.Terrain.SouthWestLongitude.Text = "-112";
        vm.ReopenCommand.Execute(null);

        Assert.True(vm.Terrain.IsLoaded);
    }

    [Fact]
    public void A_click_places_the_observer_and_a_ctrl_click_the_target_every_time()
    {
        var vm = Create();
        vm.OpenSampleTileCommand.Execute(null);

        vm.OnMapClicked(36.25, -111.75);
        vm.OnMapClicked(36.75, -111.25, target: true);
        vm.OnMapClicked(36.3, -111.7);

        Assert.Equal(MainTab.LineOfSight, vm.SelectedTab);
        Assert.Equal("36.300000", vm.LineOfSight.ObserverLatitude.Text);
        Assert.Equal("36.750000", vm.LineOfSight.TargetLatitude.Text);
    }

    [Fact]
    public void On_the_viewshed_tab_a_click_moves_the_viewshed_observer_and_a_ctrl_click_does_nothing()
    {
        var vm = Create();
        vm.OpenSampleTileCommand.Execute(null);
        vm.SelectedTab = MainTab.Viewshed;

        vm.OnMapClicked(36.4, -111.6);
        vm.OnMapClicked(36.9, -111.1, target: true);

        Assert.Equal(MainTab.Viewshed, vm.SelectedTab);
        Assert.Equal("36.400000", vm.Viewshed.ObserverLatitude.Text);
        Assert.Equal("-111.600000", vm.Viewshed.ObserverLongitude.Text);
        Assert.Equal("36.35", vm.LineOfSight.TargetLatitude.Text);
    }

    [Fact]
    public void Viewshed_layers_hidden_under_an_older_version_stay_hidden_in_their_new_places()
    {
        // Saved before cells past the tile's edge had a layer of their own: states 0-3, the
        // highlights at 4, the height bands from 5. Here "not visible", the highlights and the
        // first band were hidden; each must stay hidden, and the new layer start shown.
        var older = new bool[13];
        older[(int)CellState.NotVisible] = true;
        older[4] = true;
        older[5] = true;
        _settings.Saved = new AppSettings { HiddenViewshedLayers = older };

        var vm = Create();
        vm.Start();

        var hidden = vm.Viewshed.HiddenLayers;
        Assert.Equal(Presentation.MapPixels.LayerCount, hidden.Length);
        Assert.True(hidden[(int)CellState.NotVisible]);
        Assert.True(hidden[Presentation.MapPixels.HighlightLayer]);
        Assert.True(hidden[Presentation.MapPixels.FirstHeightLayer]);
        Assert.False(hidden[(int)CellState.DataNotGiven]);
        Assert.Equal(3, hidden.Count(h => h));
    }

    [Fact]
    public void The_last_tile_and_every_parameter_are_remembered_between_runs()
    {
        var tilePath = Path.Combine(_folder, "N36W112.hgt");
        File.WriteAllBytes(tilePath, [0]);

        var first = Create();
        first.OpenPath(tilePath);
        first.LineOfSight.Spacing.Text = "45";
        first.LineOfSight.Interpolation = Interpolation.Bilinear;
        first.Viewshed.RadiusKm.Text = "12.5";
        first.Viewshed.Algorithm = ViewshedAlgorithm.Naive;
        first.SelectedTab = MainTab.Viewshed;
        first.SaveSettings();

        var second = Create();
        second.Start();

        Assert.True(second.Terrain.IsLoaded);
        Assert.Equal(tilePath, second.Terrain.TilePath);
        Assert.Equal("45", second.LineOfSight.Spacing.Text);
        Assert.Equal(Interpolation.Bilinear, second.LineOfSight.Interpolation);
        Assert.Equal("12.5", second.Viewshed.RadiusKm.Text);
        Assert.Equal(ViewshedAlgorithm.Naive, second.Viewshed.Algorithm);
        Assert.Equal(MainTab.Viewshed, second.SelectedTab);
        Assert.False(second.ShowSamplePrompt);
    }

    [Fact]
    public void A_side_tab_shows_its_panel_and_the_profile_opens_closes_and_is_remembered()
    {
        var vm = Create();
        Assert.Equal(MainTab.Terrain, vm.SelectedTab);
        Assert.True(vm.IsProfileOpen);

        vm.ShowTabCommand.Execute(MainTab.Viewshed);
        Assert.True(vm.IsViewshedTab);
        vm.ShowTabCommand.Execute(MainTab.Viewshed);
        Assert.True(vm.IsViewshedTab);

        vm.CloseProfileCommand.Execute(null);
        Assert.False(vm.IsProfileOpen);

        vm.PanelWidth = 520;
        vm.ProfileHeight = 300;
        vm.SaveSettings();
        var second = Create();
        second.Start();
        Assert.False(second.IsProfileOpen);
        Assert.Equal(520, second.PanelWidth);
        Assert.Equal(300, second.ProfileHeight);
        Assert.Equal(MainTab.Viewshed, second.SelectedTab);

        second.OpenProfileCommand.Execute(null);
        Assert.True(second.IsProfileOpen);
    }

    [Fact]
    public async Task F5_runs_what_the_open_panel_runs_and_nothing_elsewhere()
    {
        var vm = Create();
        vm.OpenSampleTileCommand.Execute(null);

        vm.SelectedTab = MainTab.About;
        await vm.RunActivePanelCommand.ExecuteAsync(null);
        Assert.Null(vm.LineOfSight.Analysis);
        Assert.Null(vm.Viewshed.Map);

        vm.SelectedTab = MainTab.LineOfSight;
        await vm.RunActivePanelCommand.ExecuteAsync(null);
        Assert.NotNull(vm.LineOfSight.Analysis);

        vm.SelectedTab = MainTab.Viewshed;
        await vm.RunActivePanelCommand.ExecuteAsync(null);
        Assert.NotNull(vm.Viewshed.Map);
    }

    [Fact]
    public async Task The_about_panel_copies_the_full_engine_commit()
    {
        var vm = Create();

        await vm.CopyCommitCommand.ExecuteAsync(null);

        Assert.Equal("abc1234", _clipboard.Text);
        Assert.Equal("Engine commit copied to the clipboard.", vm.StatusMessage);
    }

    [Fact]
    public async Task Copy_results_puts_the_versions_the_inputs_and_the_outputs_on_the_clipboard()
    {
        var vm = Create();
        vm.OpenSampleTileCommand.Execute(null);
        vm.LineOfSight.Check();

        await vm.CopyResultsCommand.ExecuteAsync(null);

        Assert.NotNull(_clipboard.Text);
        Assert.StartsWith("TerrainBench 1.2.3 (engine 9.9.9, commit abc1234)", _clipboard.Text);
        Assert.Contains("Observer: 36.3, -111.5 degrees, 2 m above ground", _clipboard.Text);
        Assert.Contains("Verdict: Visible", _clipboard.Text);
    }

    [Fact]
    public async Task The_profile_exports_as_csv_once_there_is_one()
    {
        var vm = Create();
        vm.OpenSampleTileCommand.Execute(null);
        Assert.False(vm.ExportProfileCsvCommand.CanExecute(null));

        vm.LineOfSight.Check();
        _dialogs.SavePath = Path.Combine(_folder, "profile.csv");
        await vm.ExportProfileCsvCommand.ExecuteAsync(null);

        Assert.StartsWith("distance_m,", await File.ReadAllTextAsync(_dialogs.SavePath));
    }

    [Fact]
    public async Task The_map_and_the_viewshed_export_as_png()
    {
        var vm = Create();
        vm.OpenSampleTileCommand.Execute(null);
        _dialogs.SavePath = Path.Combine(_folder, "map.png");
        Assert.False(vm.ExportViewshedPngCommand.CanExecute(null));

        await vm.ExportMapPngCommand.ExecuteAsync(null);
        await vm.Viewshed.RunAsync();
        await vm.ExportViewshedPngCommand.ExecuteAsync(null);

        Assert.Equal(new[] { (_dialogs.SavePath, false), (_dialogs.SavePath, true) }, _exporter.Saved);
    }

    [Fact]
    public void A_missing_engine_is_explained_and_nothing_tries_to_use_it()
    {
        var vm = Create(EngineResult<ITerrainEngine>.Fail(EngineErrorKind.EngineMissing, "The terrain engine, TerrainEngineApi.dll, is missing."));
        vm.Start();

        Assert.True(vm.HasEngineError);
        Assert.False(vm.OpenSampleTileCommand.CanExecute(null));
        Assert.False(vm.ShowSamplePrompt);
        Assert.Equal("unavailable", vm.About.EngineCommit);
    }
}
