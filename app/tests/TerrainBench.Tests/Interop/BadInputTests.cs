using TerrainBench.Engine;
using Xunit;

namespace TerrainBench.Tests.Interop;

/// <summary>
/// Every kind of input that can hurt a native library, through the real DLL: each must come back as an error
/// a person can read, and none may take the test process down.
/// </summary>
public class BadInputTests : IDisposable
{
    private readonly string _folder = Path.Combine(Path.GetTempPath(), $"terrainbench-bad-{Guid.NewGuid():N}");
    private readonly ITerrainEngine _engine = NativeTerrainEngine.Load().Value;

    public BadInputTests() => Directory.CreateDirectory(_folder);

    public void Dispose()
    {
        try { Directory.Delete(_folder, recursive: true); } catch (IOException) { }
    }

    [Fact]
    public void A_missing_file_is_a_load_failure()
    {
        var result = _engine.OpenTile(Path.Combine(_folder, "N36W112.hgt"), 36, -112);
        AssertError(result.Error, EngineErrorKind.LoadFailed, "doesn't exist");
    }

    [Fact]
    public void An_empty_file_is_a_load_failure()
    {
        var path = Path.Combine(_folder, "empty.hgt");
        File.WriteAllBytes(path, []);
        AssertError(_engine.OpenTile(path, 36, -112).Error, EngineErrorKind.LoadFailed, "is empty");
    }

    [Fact]
    public void A_truncated_file_is_a_load_failure_not_a_tile_of_voids()
    {
        var path = Path.Combine(_folder, "truncated.hgt");
        var bytes = File.ReadAllBytes(RepositoryFiles.SampleTile);
        File.WriteAllBytes(path, bytes[..^2]);
        AssertError(_engine.OpenTile(path, 36, -112).Error, EngineErrorKind.LoadFailed, "cut short");
    }

    [Fact]
    public void A_path_with_non_ascii_characters_opens()
    {
        var folder = Path.Combine(_folder, "Masaüstü şğı");
        Directory.CreateDirectory(folder);
        var path = Path.Combine(folder, "N36W112.hgt");
        File.Copy(RepositoryFiles.SampleTile, path);

        var result = _engine.OpenTile(path, 36, -112);

        Assert.True(result.IsOk, result.Error?.Message);
        result.Value.Dispose();
    }

    [Fact]
    public void A_point_outside_the_tile_is_reported_as_outside()
    {
        using var tile = _engine.OpenTile(RepositoryFiles.SampleTile, 36, -112).Value;
        AssertError(tile.Elevation(35.5, -111.5, Interpolation.Nearest).Error, EngineErrorKind.OutsideTile, "outside the open tile");
        // On the southern edge, bilinear needs the row past it: not a void, but ground not given.
        AssertError(tile.Elevation(36.0, -111.5, Interpolation.Bilinear).Error, EngineErrorKind.OutsideTile, "needs posts past it");
        Assert.NotNull(tile.Elevation(36.0, -111.5, Interpolation.Nearest).Value);
        AssertError(tile.AnalyzePath(Path1() with { TargetLatitudeDeg = 37.5 }).Error, EngineErrorKind.OutsideTile, "outside the open tile");
    }

    [Theory]
    [InlineData(0.0)]
    [InlineData(-30.0)]
    [InlineData(double.NaN)]
    public void A_spacing_that_is_not_positive_is_refused(double spacing)
    {
        using var tile = _engine.OpenTile(RepositoryFiles.SampleTile, 36, -112).Value;
        AssertError(tile.AnalyzePath(Path1() with { SpacingM = spacing }).Error, EngineErrorKind.InvalidArgument, "spacing");
        AssertError(tile.Viewshed(View1() with { SpacingM = spacing }, null, default).Error, EngineErrorKind.InvalidArgument, "spacing");
    }

    [Theory]
    [InlineData(-1)]
    [InlineData(double.NaN)]
    [InlineData(1e9)]
    public void A_viewshed_target_height_below_ground_or_not_a_number_is_refused(double targetHeightM)
    {
        using var tile = _engine.OpenTile(RepositoryFiles.SampleTile, 36, -112).Value;
        AssertError(tile.Viewshed(View1() with { TargetHeight = targetHeightM }, null, default).Error, EngineErrorKind.InvalidArgument, "target height");
    }

    [Fact]
    public void A_viewshed_at_a_pole_latitude_is_refused()
    {
        using var polar = _engine.OpenTile(RepositoryFiles.SampleTile, 89, -112).Value;
        AssertError(polar.Viewshed(View1() with { ObserverLatitudeDeg = 90 }, null, default).Error, EngineErrorKind.InvalidArgument, "pole");
    }

    [Fact]
    public void A_viewshed_whose_grid_would_reach_the_pole_is_refused()
    {
        // The observer is 0.2 degrees from the pole, allowed on its own; a 30 km radius
        // is 0.27 degrees, so the grid's northern rows would reach past it. The engine
        // refuses that grid, and the DLL says so instead of reading an empty answer.
        using var polar = _engine.OpenTile(RepositoryFiles.SampleTile, 89, -112).Value;
        AssertError(polar.Viewshed(View1() with { ObserverLatitudeDeg = 89.8, RadiusKm = 30 }, null, default).Error, EngineErrorKind.InvalidArgument, "pole");
    }

    [Fact]
    public void A_south_west_corner_off_the_globe_is_refused()
    {
        AssertError(_engine.OpenTile(RepositoryFiles.SampleTile, 95, -112).Error, EngineErrorKind.InvalidArgument, "latitude");
    }

    [Fact]
    public void A_closed_tile_is_an_invalid_handle()
    {
        var tile = _engine.OpenTile(RepositoryFiles.SampleTile, 36, -112).Value;
        tile.Dispose();
        tile.Dispose();

        AssertError(tile.Elevation(36.5, -111.5, Interpolation.Nearest).Error, EngineErrorKind.InvalidHandle, "isn't open");
        AssertError(tile.AnalyzePath(Path1()).Error, EngineErrorKind.InvalidHandle, "isn't open");
    }

    [Fact]
    public void A_viewshed_reports_progress_and_stops_when_cancelled()
    {
        using var tile = _engine.OpenTile(RepositoryFiles.SampleTile, 36, -112).Value;
        var fractions = new List<double>();
        var progress = new SynchronousProgress(fractions.Add);

        var done = tile.Viewshed(View1(), progress, default);
        Assert.True(done.IsOk, done.Error?.Message);
        Assert.Equal(133, done.Value.Rows);
        Assert.Equal(0.0, fractions[0]);
        Assert.Equal(1.0, fractions[^1]);

        using var cancel = new CancellationTokenSource();
        var stopped = tile.Viewshed(View1() with { RadiusKm = 30, Algorithm = ViewshedAlgorithm.Naive },
            new SynchronousProgress(fraction => { if (fraction > 0) cancel.Cancel(); }), cancel.Token);
        AssertError(stopped.Error, EngineErrorKind.Cancelled, "cancelled");
    }

    private static PathQuery Path1() => new(36.3, -111.5, 2, 36.35, -111.45, 2, 30, 4.0 / 3.0, Interpolation.Nearest);

    private static ViewshedQuery View1() => new(36.5, -111.5, 2, 2, 30, 4.0 / 3.0, Interpolation.Nearest, ViewshedAlgorithm.Fast);

    private static void AssertError(EngineError? error, EngineErrorKind kind, string mentions)
    {
        Assert.NotNull(error);
        Assert.Equal(kind, error.Kind);
        Assert.Contains(mentions, error.Message, StringComparison.OrdinalIgnoreCase);
    }

    /// <summary>Reports on the calling thread, unlike <see cref="Progress{T}"/>, so a test sees every report in order.</summary>
    private sealed class SynchronousProgress(Action<double> report) : IProgress<double>
    {
        public void Report(double value) => report(value);
    }
}
