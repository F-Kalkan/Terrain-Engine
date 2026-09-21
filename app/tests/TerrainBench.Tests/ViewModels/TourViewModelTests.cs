using TerrainBench.Engine;
using TerrainBench.Services;
using TerrainBench.ViewModels;
using Xunit;

namespace TerrainBench.Tests.ViewModels;

public class TourViewModelTests
{
    private readonly MemorySettings _settings = new();
    private readonly FakeEngine _engine = new();

    private MainViewModel Create(EngineResult<ITerrainEngine>? engine = null) =>
        new(engine ?? EngineResult<ITerrainEngine>.Ok(_engine), new FakeDialogs(), new FakeClipboard(), _settings, new FakeExporter(),
            "1.2.3", Path.Combine(Path.GetTempPath(), "N36W112.hgt"));

    private static int IndexOf(TourGoal goal) => TourViewModel.Steps.ToList().FindIndex(s => s.Goal == goal);

    [Fact]
    public void A_page_with_a_goal_waits_for_that_goal_and_no_other()
    {
        var tour = new TourViewModel();
        tour.Open();
        Assert.True(tour.HasNext);
        tour.Next();

        Assert.Equal(TourGoal.TileOpen, tour.Current.Goal);
        Assert.False(tour.HasNext);
        tour.Reached(TourGoal.ObserverPlaced);
        tour.Reached(TourGoal.ViewshedTab);
        Assert.Equal(TourGoal.TileOpen, tour.Current.Goal);

        tour.Reached(TourGoal.TileOpen);
        Assert.Equal(TourGoal.ObserverPlaced, tour.Current.Goal);

        // A goal reached while the tour is closed moves nothing.
        tour.Close();
        tour.Reached(TourGoal.ObserverPlaced);
        Assert.Equal(TourGoal.ObserverPlaced, tour.Current.Goal);
    }

    [Fact]
    public void Pages_whose_goal_already_holds_are_skipped_both_ways()
    {
        var tour = new TourViewModel { Holds = goal => goal == TourGoal.TileOpen };
        tour.Open();
        tour.Next();
        Assert.Equal(TourGoal.ObserverPlaced, tour.Current.Goal);

        tour.Back();
        Assert.Equal(0, tour.Index);
    }

    [Fact]
    public void Every_page_has_a_title_a_few_sentences_and_the_last_ends_the_tour()
    {
        Assert.InRange(TourViewModel.Steps.Count, 5, 10);
        foreach (var step in TourViewModel.Steps)
        {
            Assert.False(string.IsNullOrWhiteSpace(step.Title));
            Assert.InRange(step.Body.Length, 60, 420);
        }
        Assert.Null(TourViewModel.Steps[0].Target);
        Assert.All(TourViewModel.Steps.Skip(1), step => Assert.NotNull(step.Target));

        var tour = new TourViewModel();
        int closed = 0;
        tour.Closed += (_, _) => closed++;
        tour.Open();
        tour.Index = TourViewModel.Steps.Count - 1;
        Assert.Equal($"Step {TourViewModel.Steps.Count} of {TourViewModel.Steps.Count}", tour.Progress);
        tour.Reached(TourViewModel.Steps[^1].Goal);
        Assert.False(tour.IsOpen);
        Assert.Equal(1, closed);
        tour.Close();
        Assert.Equal(1, closed);
    }

    [Fact]
    public async Task The_whole_tour_is_walked_by_using_the_app()
    {
        var vm = Create();
        vm.Start();
        Assert.True(vm.Tour.IsOpen);
        vm.Tour.Next();

        vm.OpenSampleTileCommand.Execute(null);
        Assert.Equal(TourGoal.ObserverPlaced, vm.Tour.Current.Goal);

        vm.OnMapClicked(36.4, -111.6);
        Assert.Equal(TourGoal.TargetPlaced, vm.Tour.Current.Goal);
        vm.OnMapClicked(36.45, -111.5, target: true);
        Assert.Equal("LineOfSightResult", vm.Tour.Current.Target);
        Assert.True(vm.LineOfSight.HasResult);

        vm.Tour.Next();
        vm.ShowTab(MainTab.Viewshed);
        Assert.Equal(TourGoal.ViewshedResult, vm.Tour.Current.Goal);

        await vm.Viewshed.RunAsync();
        Assert.True(vm.Viewshed.HasResult);
        Assert.Equal("ViewshedLegend", vm.Tour.Current.Target);

        vm.Tour.Next();
        vm.ShowTab(MainTab.About);
        Assert.False(vm.Tour.IsOpen);

        vm.SaveSettings();
        Assert.True(_settings.Saved!.TourSeen);
    }

    [Fact]
    public void Opened_again_with_a_tile_already_open_the_tour_skips_opening_one()
    {
        var vm = Create();
        vm.Start();
        vm.Tour.Close();
        vm.OpenSampleTileCommand.Execute(null);

        vm.Tour.Open();
        vm.Tour.Next();
        Assert.Equal(TourGoal.ObserverPlaced, vm.Tour.Current.Goal);
        Assert.Equal(IndexOf(TourGoal.ObserverPlaced), vm.Tour.Index);
    }

    [Fact]
    public void Once_closed_the_tour_stays_closed_and_left_open_it_comes_back()
    {
        var first = Create();
        first.Start();
        first.Tour.Next();
        first.SaveSettings();
        Assert.False(_settings.Saved!.TourSeen);

        var second = Create();
        second.Start();
        Assert.True(second.Tour.IsOpen);
        second.Tour.Close();
        second.SaveSettings();
        Assert.True(_settings.Saved!.TourSeen);

        var third = Create();
        third.Start();
        Assert.False(third.Tour.IsOpen);
        third.SaveSettings();
        Assert.True(_settings.Saved!.TourSeen);
    }

    [Fact]
    public void Settings_from_before_the_tour_show_it_once()
    {
        _settings.Saved = new AppSettings { SelectedTab = "Viewshed" };

        var vm = Create();
        vm.Start();

        Assert.True(vm.Tour.IsOpen);
        Assert.Equal(MainTab.Viewshed, vm.SelectedTab);
    }

    [Fact]
    public void Without_the_engine_there_is_no_tour_over_the_explanation()
    {
        var vm = Create(EngineResult<ITerrainEngine>.Fail(EngineErrorKind.EngineMissing, "The terrain engine, TerrainEngineApi.dll, is missing."));
        vm.Start();

        Assert.False(vm.Tour.IsOpen);
        Assert.True(vm.HasEngineError);
    }
}
