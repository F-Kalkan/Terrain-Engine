using TerrainBench.Engine;
using Xunit;

namespace TerrainBench.Tests.Interop;

/// <summary>
/// The minimum visible height through the real TerrainEngineApi.dll: the same grid as the viewshed,
/// and, asked for one target height, exactly the viewshed the DLL gives for it.
/// </summary>
public class MinimumVisibleHeightTests
{
    private static ITerrainEngine Engine => NativeTerrainEngine.Load().Value;

    private static ViewshedQuery Query(ViewshedAlgorithm algorithm, double radiusKm = 2) =>
        new(36.3, -111.6, 2, radiusKm, 30, 4.0 / 3.0, Interpolation.Nearest, algorithm);

    [Theory]
    [InlineData(ViewshedAlgorithm.Fast, 2.0)]
    [InlineData(ViewshedAlgorithm.Naive, 0.6)]
    public void Asked_for_one_target_height_it_is_exactly_the_viewshed_at_that_height(ViewshedAlgorithm algorithm, double radiusKm)
    {
        // Round heights, and heights the grid itself holds with the double just below each, where a
        // rounding either way would show. The fast version is held to the fast viewshed, the exact
        // reference to the naive one.
        using var tile = Engine.OpenTile(RepositoryFiles.SampleTile, 36, -112).Value;
        var heights = tile.MinimumVisibleHeight(Query(algorithm, radiusKm), null, default);
        Assert.True(heights.IsOk, heights.Error?.Message);
        var map = heights.Value;

        var held = map.HeightsM!.Where(h => double.IsFinite(h) && h > 0).Distinct().Order().ToArray();
        Assert.True(held.Length > 20, $"Only {held.Length} distinct heights: pick a busier observer.");
        var asked = new List<double> { 0, 2, 10, 50 };
        foreach (double h in held.Where((_, i) => i % (held.Length / 6) == 0))
        {
            asked.Add(h);
            asked.Add(Math.BitDecrement(h));
        }

        foreach (double targetM in asked)
        {
            var viewshed = tile.Viewshed(Query(algorithm, radiusKm) with { TargetHeightAboveGroundM = targetM }, null, default).Value;
            Assert.Equal((map.Rows, map.Cols, map.SouthWestCellLatitudeDeg), (viewshed.Rows, viewshed.Cols, viewshed.SouthWestCellLatitudeDeg));
            for (int i = 0; i < map.Cells.Length; i++)
            {
                var state = map.Cells[i];
                var expected = state is CellState.Visible or CellState.NotVisible
                    ? (targetM >= map.HeightsM![i] ? CellState.Visible : CellState.NotVisible)
                    : state;
                Assert.True(expected == viewshed.Cells[i], $"Target {targetM:R} m, cell {i}: height {map.HeightsM![i]:R} m says {expected}, the viewshed {viewshed.Cells[i]}.");
            }
        }
    }

    [Fact]
    public void Every_cell_has_a_height_exactly_where_it_has_a_confident_answer()
    {
        // The ground's own viewshed carries the states: 0 where the ground is seen, more where it
        // isn't, and NaN where there is no confident answer -- here the cells past the tile's
        // western edge, which a 3 km circle around an observer 0.02 degrees from it reaches.
        using var tile = Engine.OpenTile(RepositoryFiles.SampleTile, 36, -112).Value;
        var map = tile.MinimumVisibleHeight(new ViewshedQuery(36.3, -111.98, 2, 3, 30, 4.0 / 3.0, Interpolation.Nearest, ViewshedAlgorithm.Fast), null, default).Value;
        var ground = tile.Viewshed(new ViewshedQuery(36.3, -111.98, 2, 3, 30, 4.0 / 3.0, Interpolation.Nearest, ViewshedAlgorithm.Fast), null, default).Value;

        Assert.Equal(ground.Cells, map.Cells);
        Assert.Contains(CellState.Degraded, map.Cells);
        for (int i = 0; i < map.Cells.Length; i++)
        {
            double h = map.HeightsM![i];
            switch (map.Cells[i])
            {
                case CellState.Visible: Assert.Equal(0.0, h); break;
                case CellState.NotVisible: Assert.True(h > 0, $"Cell {i}: hidden, yet needs {h} m."); break;
                default: Assert.True(double.IsNaN(h), $"Cell {i}: no confident answer, yet {h} m."); break;
            }
        }
        Assert.Equal(0.0, map.HeightAt(map.ObserverRow, map.ObserverCol));
    }

    [Fact]
    public void It_is_refused_as_the_viewshed_is_and_takes_no_target_height()
    {
        using var tile = Engine.OpenTile(RepositoryFiles.SampleTile, 36, -112).Value;

        var spacing = tile.MinimumVisibleHeight(Query(ViewshedAlgorithm.Fast) with { SpacingM = 0 }, null, default).Error;
        Assert.Equal(EngineErrorKind.InvalidArgument, spacing?.Kind);
        Assert.Contains("spacing", spacing!.Message, StringComparison.OrdinalIgnoreCase);

        using var polar = Engine.OpenTile(RepositoryFiles.SampleTile, 89, -112).Value;
        var pole = polar.MinimumVisibleHeight(Query(ViewshedAlgorithm.Fast) with { ObserverLatitudeDeg = 89.8, RadiusKm = 30 }, null, default).Error;
        Assert.Equal(EngineErrorKind.InvalidArgument, pole?.Kind);
        Assert.Contains("pole", pole!.Message, StringComparison.OrdinalIgnoreCase);

        var outside = tile.MinimumVisibleHeight(Query(ViewshedAlgorithm.Fast) with { ObserverLatitudeDeg = 35.5 }, null, default).Error;
        Assert.Equal(EngineErrorKind.OutsideTile, outside?.Kind);

        // The answer covers every target height, so the query's own isn't looked at -- not even a NaN.
        var anyTarget = tile.MinimumVisibleHeight(Query(ViewshedAlgorithm.Fast) with { TargetHeightAboveGroundM = double.NaN }, null, default);
        Assert.True(anyTarget.IsOk, anyTarget.Error?.Message);
    }

    [Fact]
    public void It_reports_progress_and_stops_when_cancelled()
    {
        using var tile = Engine.OpenTile(RepositoryFiles.SampleTile, 36, -112).Value;
        var fractions = new List<double>();
        var done = tile.MinimumVisibleHeight(Query(ViewshedAlgorithm.Fast), new Reporter(fractions.Add), default);
        Assert.True(done.IsOk, done.Error?.Message);
        Assert.Equal(0.0, fractions[0]);
        Assert.Equal(1.0, fractions[^1]);

        using var cancel = new CancellationTokenSource();
        var stopped = tile.MinimumVisibleHeight(Query(ViewshedAlgorithm.Naive, 30),
            new Reporter(fraction => { if (fraction > 0) cancel.Cancel(); }), cancel.Token);
        Assert.Equal(EngineErrorKind.Cancelled, stopped.Error?.Kind);
        Assert.Contains("minimum visible height", stopped.Error!.Message);
    }

    /// <summary>Reports on the calling thread, so a test sees every report in order.</summary>
    private sealed class Reporter(Action<double> report) : IProgress<double>
    {
        public void Report(double value) => report(value);
    }
}
