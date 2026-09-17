using TerrainBench.Engine;
using TerrainBench.Presentation;
using TerrainBench.ViewModels;
using Xunit;

namespace TerrainBench.Tests.ViewModels;

public class LineOfSightViewModelTests
{
    private readonly FakeTile _tile = new("N36W112.hgt", 36, -112);

    [Fact]
    public void Nothing_can_be_checked_without_a_tile()
    {
        var vm = new LineOfSightViewModel(() => null);
        Assert.False(vm.CheckCommand.CanExecute(null));
    }

    [Fact]
    public void An_invalid_field_stops_the_check_and_says_what_is_allowed_before_the_engine_runs()
    {
        var vm = new LineOfSightViewModel(() => _tile);
        Assert.True(vm.CheckCommand.CanExecute(null));

        vm.Spacing.Text = "0";

        Assert.False(vm.CheckCommand.CanExecute(null));
        Assert.Equal("The spacing must be greater than 0.", vm.Spacing.Error);
        vm.Check();
        Assert.Empty(_tile.PathQueries);
    }

    [Fact]
    public void A_point_outside_the_open_tile_is_caught_at_the_field()
    {
        var vm = new LineOfSightViewModel(() => _tile);
        vm.TargetLatitude.Text = "37.5";
        Assert.Equal("The target latitude must be inside the open tile: 36 to 37.", vm.TargetLatitude.Error);
    }

    [Fact]
    public void A_blocked_path_says_where_how_high_how_far_short_and_what_feature()
    {
        _tile.OnAnalyze = _ => EngineResult<PathAnalysis>.Ok(Analyses.Blocked());
        var vm = new LineOfSightViewModel(() => _tile);
        vm.Frequency.Text = "2400";

        vm.Check();

        Assert.Equal(Verdict.Blocked, vm.Verdict);
        Assert.Equal("Blocked", vm.VerdictText);
        Assert.Contains(vm.Details, r => r.Label == "Blocked At" && r.Value == "36.327734°, -111.472277°");
        Assert.Contains(vm.Details, r => r.Label == "Terrain Height There" && r.Value == "1900.00 m above mean sea level");
        Assert.Contains(vm.Details, r => r.Label == "Sight Line Falls Short By" && r.Value == "195.99 m");
        Assert.Contains(vm.Details, r => r.Label == "Terrain Feature" && r.Value == "A falling slope");
        Assert.StartsWith("Fresnel clearance: blocked.", vm.FresnelText);
        Assert.Equal(2400, _tile.PathQueries.Single().FrequencyMHz);

        // The card's headline sentence, and the rows split into the obstruction and the path.
        Assert.StartsWith("A falling slope ", vm.Reason);
        Assert.EndsWith("from the observer rises 195.99 m above the sight line.", vm.Reason);
        Assert.True(vm.HasObstruction);
        Assert.Equal(5, vm.ObstructionDetails.Count);
        Assert.Contains(vm.PathDetails, r => r.Label == "Path Length");
        Assert.Equal(FresnelState.Blocked, vm.Fresnel);
        Assert.Equal("Blocked", vm.FresnelVerdictText);
        Assert.Equal(0, vm.FresnelFreeFraction);
    }

    [Fact]
    public void A_partly_obstructed_fresnel_zone_shows_how_much_is_free()
    {
        _tile.OnAnalyze = _ => EngineResult<PathAnalysis>.Ok(Analyses.Blocked() with { Fresnel = new FresnelClearance(ComputationStatus.Ok, 0.42, 132, 5.1) });
        var vm = new LineOfSightViewModel(() => _tile);
        vm.Frequency.Text = "2400";

        vm.Check();

        Assert.Equal(FresnelState.PartlyObstructed, vm.Fresnel);
        Assert.Equal("Partly Obstructed", vm.FresnelVerdictText);
        Assert.Equal(0.42, vm.FresnelFreeFraction);
        Assert.StartsWith("Fresnel clearance: partly obstructed. Only 42", vm.FresnelText);

        // A later result without a frequency clears the Fresnel card.
        vm.Frequency.Text = "";
        _tile.OnAnalyze = _ => EngineResult<PathAnalysis>.Ok(Analyses.Blocked() with { Fresnel = null });
        vm.Check();
        Assert.False(vm.HasFresnel);
        Assert.Equal(FresnelState.None, vm.Fresnel);
    }

    [Fact]
    public void Swapping_trades_the_points_and_their_heights_and_checks_again()
    {
        var vm = new LineOfSightViewModel(() => _tile);
        vm.ObserverHeight.Text = "10";

        vm.SwapCommand.Execute(null);

        Assert.Equal("36.35", vm.ObserverLatitude.Text);
        Assert.Equal("-111.45", vm.ObserverLongitude.Text);
        Assert.Equal("2", vm.ObserverHeight.Text);
        Assert.Equal("36.3", vm.TargetLatitude.Text);
        Assert.Equal("10", vm.TargetHeight.Text);
        var query = _tile.PathQueries.Single();
        Assert.Equal(36.35, query.ObserverLatitudeDeg);
        Assert.Equal(10, query.TargetHeightAboveGroundM);
    }

    [Fact]
    public void A_visible_path_says_so()
    {
        var vm = new LineOfSightViewModel(() => _tile);
        vm.Check();
        Assert.Equal("Visible", vm.VerdictText);
        Assert.DoesNotContain(vm.Details, r => r.Label == "Blocked At");
        Assert.False(vm.HasObstruction);
    }

    [Theory]
    [InlineData(ComputationStatus.VoidInProfile, "The path crosses missing data")]
    [InlineData(ComputationStatus.EndpointMissing, "no terrain data under the observer or the target")]
    public void No_confident_answer_explains_why_from_the_engine_status(ComputationStatus status, string words)
    {
        _tile.OnAnalyze = _ => EngineResult<PathAnalysis>.Ok(Analyses.NoConfidentAnswer(status));
        var vm = new LineOfSightViewModel(() => _tile);

        vm.Check();

        Assert.Equal("No Confident Answer", vm.VerdictText);
        Assert.Contains(words, vm.Reason);
    }

    [Fact]
    public void An_engine_error_is_shown_as_the_engine_worded_it()
    {
        _tile.OnAnalyze = _ => EngineResult<PathAnalysis>.Fail(EngineErrorKind.InvalidArgument, "The spacing is too small for a path this long.");
        var vm = new LineOfSightViewModel(() => _tile);

        vm.Check();

        Assert.Equal("The spacing is too small for a path this long.", vm.Error);
        Assert.Null(vm.Analysis);
        Assert.Equal(Verdict.None, vm.Verdict);
    }

    [Fact]
    public void Placing_a_marker_fills_the_fields_and_checks_again()
    {
        var vm = new LineOfSightViewModel(() => _tile);

        vm.PlaceTarget(36.123456789, -111.987654321);

        Assert.Equal("36.123457", vm.TargetLatitude.Text);
        Assert.Equal("-111.987654", vm.TargetLongitude.Text);
        Assert.Equal(36.123457, _tile.PathQueries.Single().TargetLatitudeDeg);
    }

    [Fact]
    public void The_report_gives_the_exact_engine_spacing_and_a_command_line_that_repeats_the_query()
    {
        using var turkish = new CultureScope("tr-TR");
        _tile.OnAnalyze = _ => EngineResult<PathAnalysis>.Ok(Analyses.Blocked());
        var vm = new LineOfSightViewModel(() => _tile);
        vm.Check();

        var report = new ResultsReport("1.0.0", "1.0.0", "abc1234");
        vm.AppendReport(report);
        var text = report.ToString();

        Assert.Contains("Spacing passed to the engine: 0.0002697964817756191 degrees", text);
        Assert.Contains("TerrainEngine.exe los <tile> <swLat> <swLon> 36.3 -111.5 36.35 -111.45 0.0002697964817756191 2 2 1.3333333333333333 nearest", text);
        Assert.Contains("Clearance deficit (exact): 195.9932532157111 m", text);
        Assert.DoesNotContain("0,000269", text);
    }
}
