using TerrainBench.Engine;
using TerrainBench.ViewModels;
using Xunit;

namespace TerrainBench.Tests.ViewModels;

public class ViewshedViewModelTests
{
    private readonly FakeTile _tile = new("N36W112.hgt", 36, -112);

    [Fact]
    public async Task A_run_shows_the_map_and_counts_every_state()
    {
        var vm = new ViewshedViewModel(() => _tile);

        await vm.RunAsync();

        Assert.NotNull(vm.Map);
        Assert.False(vm.IsRunning);
        Assert.Collection(vm.Legend,
            row => Assert.Equal(("Visible", "1"), (row.Name, row.Count)),
            row => Assert.Equal(("Not Visible", "1"), (row.Name, row.Count)),
            row => Assert.Equal("No Confident Answer (Missing Data on the Way)", row.Name),
            row => Assert.Equal("Not Reached", row.Name));
        Assert.StartsWith("Fast viewshed of 2 × 2 cells", vm.Summary);
    }

    [Fact]
    public async Task A_running_viewshed_can_be_cancelled_and_leaves_the_window_usable()
    {
        var started = new TaskCompletionSource();
        _tile.OnViewshed = (_, progress, token) =>
        {
            progress?.Report(0.1);
            started.SetResult();
            while (!token.IsCancellationRequested) Thread.Sleep(5);
            return EngineResult<ViewshedMap>.Fail(EngineErrorKind.Cancelled, "The viewshed was cancelled.");
        };
        var vm = new ViewshedViewModel(() => _tile);

        var run = vm.RunAsync();
        await started.Task;

        Assert.True(vm.IsRunning);
        Assert.False(vm.RunCommand.CanExecute(null));
        Assert.True(vm.CancelCommand.CanExecute(null));

        vm.CancelCommand.Execute(null);
        await run;

        Assert.False(vm.IsRunning);
        Assert.StartsWith("Cancelled.", vm.Error);
        Assert.Null(vm.Map);
        Assert.True(vm.RunCommand.CanExecute(null));
    }

    [Fact]
    public async Task Compare_mode_runs_both_algorithms_and_marks_the_cells_they_disagree_on()
    {
        _tile.OnViewshed = (query, _, _) => EngineResult<ViewshedMap>.Ok(query.Algorithm == ViewshedAlgorithm.Fast
            ? Maps.Of(CellState.Visible, CellState.Visible, CellState.Degraded, CellState.NotVisible)
            : Maps.Of(CellState.Visible, CellState.NotVisible, CellState.Visible, CellState.NotVisible));
        var vm = new ViewshedViewModel(() => _tile) { Comparison = ViewshedComparison.FastAndNaive };

        await vm.RunAsync();

        Assert.Equal(new[] { false, true, false, false }, vm.Highlights);
        Assert.StartsWith("1 of 3 cells disagree (33.33 %)", vm.Summary);
        Assert.Contains("Fast took", vm.Summary);
        Assert.Contains(vm.Legend, row => row.Name == "Fast and Naive Disagree" && row.Count == "1");
    }


    [Fact]
    public async Task Moving_the_observer_takes_the_old_map_off_and_a_plain_fast_run_redoes_itself()
    {
        var vm = new ViewshedViewModel(() => _tile);
        await vm.RunAsync();
        Assert.True(vm.ShowLayer);
        int runs = _tile.ViewshedQueries.Count;

        vm.ObserverLatitude.Text = "36.6";
        Assert.True(vm.IsObserverMoved);
        Assert.False(vm.ShowLayer);
        Assert.False(vm.ShowResult);
        Assert.True(vm.NeedsRunAgain);
        Assert.StartsWith("The observer moved.", vm.RunAgainText);
        Assert.Equal(runs, _tile.ViewshedQueries.Count);

        // Placed on the map, a plain fast run is redone by itself.
        vm.PlaceObserver(36.7, -111.7);
        for (int i = 0; i < 100 && vm.IsRunning; i++) await Task.Delay(10);
        Assert.Equal(runs + 1, _tile.ViewshedQueries.Count);
        Assert.Equal(36.7, _tile.ViewshedQueries[^1].ObserverLatitudeDeg);
        Assert.False(vm.IsObserverMoved);
        Assert.True(vm.ShowLayer);
    }

    [Fact]
    public async Task Changing_another_setting_fades_the_map_and_a_slow_or_compared_run_waits_for_run()
    {
        var vm = new ViewshedViewModel(() => _tile);
        await vm.RunAsync();

        vm.RefractionK.Text = "1e12";
        Assert.True(vm.IsStale);
        Assert.True(vm.ShowLayer);
        Assert.True(vm.ShowResult);
        Assert.Equal(0.35, vm.MapOpacity, 1e-9);
        Assert.StartsWith("Settings changed.", vm.RunAgainText);

        vm.OverlayOpacity = 50;
        Assert.Equal(0.175, vm.MapOpacity, 1e-9);

        vm.Algorithm = ViewshedAlgorithm.Naive;
        int runs = _tile.ViewshedQueries.Count;
        vm.PlaceObserver(36.7, -111.7);
        await Task.Delay(50);
        Assert.Equal(runs, _tile.ViewshedQueries.Count);
        Assert.True(vm.IsObserverMoved);

        await vm.RunAsync();
        Assert.False(vm.IsStale);
        Assert.False(vm.NeedsRunAgain);
    }

    [Fact]
    public async Task A_legend_entry_hides_its_cells_and_keeps_its_count()
    {
        var vm = new ViewshedViewModel(() => _tile);
        await vm.RunAsync();
        var notVisible = vm.Legend.Single(r => r.Name == "Not Visible");

        vm.ToggleLayerCommand.Execute(notVisible);

        Assert.False(notVisible.IsShown);
        Assert.True(vm.HiddenLayers[(int)CellState.NotVisible]);
        Assert.Equal("1", notVisible.Count);

        // A new run keeps the layer hidden.
        await vm.RunAsync();
        Assert.False(vm.Legend.Single(r => r.Name == "Not Visible").IsShown);

        vm.ToggleLayerCommand.Execute(vm.Legend.Single(r => r.Name == "Not Visible"));
        Assert.False(vm.HiddenLayers[(int)CellState.NotVisible]);
    }
    [Fact]
    public async Task The_target_height_reaches_the_engine_and_a_change_to_it_is_named()
    {
        var vm = new ViewshedViewModel(() => _tile) { Comparison = ViewshedComparison.PreviousRun };
        await vm.RunAsync();
        Assert.Equal(0, _tile.ViewshedQueries[^1].TargetHeightAboveGroundM);

        vm.TargetHeight.Text = "10";
        Assert.True(vm.IsStale);
        await vm.RunAsync();

        Assert.Equal(10, _tile.ViewshedQueries[^1].TargetHeightAboveGroundM);
        Assert.Contains("target height: 0 m → 10 m", vm.Summary);

        vm.TargetHeight.Text = "-1";
        Assert.Equal("The target height must be between 0 and 100000.", vm.TargetHeight.Error);
        Assert.False(vm.RunCommand.CanExecute(null));
    }

    [Fact]
    public async Task Comparing_with_the_previous_run_marks_what_changing_k_did()
    {
        _tile.OnViewshed = (query, _, _) => EngineResult<ViewshedMap>.Ok(query.RefractionK > 1e6
            ? Maps.Of(CellState.Visible, CellState.Visible, CellState.NotVisible, CellState.Visible)
            : Maps.Of(CellState.Visible, CellState.NotVisible, CellState.NotVisible, CellState.NotVisible));
        var vm = new ViewshedViewModel(() => _tile) { Comparison = ViewshedComparison.PreviousRun };

        await vm.RunAsync();
        Assert.Null(vm.Highlights);
        Assert.Contains("no previous run to compare with yet", vm.Summary);

        vm.RefractionK.Text = "1e12";
        await vm.RunAsync();

        Assert.Equal(new[] { false, true, false, true }, vm.Highlights);
        Assert.EndsWith("k: 4/3 → 1e12 changed 2 cells.", vm.Summary);
        Assert.Contains(vm.Legend, row => row.Name == "Changed Since the Previous Run" && row.Count == "2");

        // Running again with nothing changed says so, instead of listing the settings twice.
        await vm.RunAsync();
        Assert.EndsWith("Same settings as the previous run, so nothing changed.", vm.Summary);
    }

    [Fact]
    public async Task An_engine_error_is_shown_and_clears_the_old_map()
    {
        var vm = new ViewshedViewModel(() => _tile);
        await vm.RunAsync();
        _tile.OnViewshed = (_, _, _) => EngineResult<ViewshedMap>.Fail(EngineErrorKind.InvalidArgument, "That grid would be more than 4001 cells across.");

        await vm.RunAsync();

        Assert.Equal("That grid would be more than 4001 cells across.", vm.Error);
        Assert.Null(vm.Map);
    }

    [Fact]
    public void A_pole_latitude_is_caught_at_the_field()
    {
        var polar = new FakeTile("N89W112.hgt", 89, -112);
        var vm = new ViewshedViewModel(() => polar);

        vm.ObserverLatitude.Text = "90";

        Assert.Contains("close to a pole", vm.ObserverLatitude.Error);
        Assert.False(vm.RunCommand.CanExecute(null));
    }
}
