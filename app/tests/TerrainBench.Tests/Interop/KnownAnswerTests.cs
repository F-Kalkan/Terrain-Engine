using System.Diagnostics;
using System.Globalization;
using TerrainBench.Engine;
using Xunit;

namespace TerrainBench.Tests.Interop;

/// <summary>
/// Known answers through the real TerrainEngineApi.dll: the engine's hand-checkable cases, built as
/// .hgt tiles, and a query on the sample tile checked against the command-line tool itself.
/// </summary>
public class KnownAnswerTests
{
    private static ITerrainEngine Engine => NativeTerrainEngine.Load().Value;

    [Fact]
    public void The_wall_blocks_the_sight_line_by_38_m()
    {
        // Tests.h's wall: ground 10 m, a 50 m peak two posts along with 30 m shoulders, 2 m eyes at
        // both ends. The sight line sits at 12 m, so the peak blocks it by 38 m, plus the curvature
        // drop halfway along a four-post path (185 m): 185.3^2 / (2 * 4/3 * 6371000) = 0.002 m.
        const int col = 600;
        const int firstRow = 600;
        using var tile = SyntheticTile.Write(10, posts =>
        {
            short[] wall = [10, 30, 50, 30, 10];
            for (int i = 0; i < wall.Length; i++) posts[SyntheticTile.Index(firstRow + i, col)] = wall[i];
        });

        using var opened = Engine.OpenTile(tile.Path, 36, -112).Value;
        double lon = SyntheticTile.ColLongitude(-112, col);
        double spacingM = Engine.DistanceM(36.5, lon, 36.5 - SyntheticTile.RowStepDeg, lon).Value;

        var result = opened.AnalyzePath(new PathQuery(
            SyntheticTile.RowLatitude(36, firstRow), lon, 2,
            SyntheticTile.RowLatitude(36, firstRow + 4), lon, 2,
            spacingM, 4.0 / 3.0, Interpolation.Nearest));

        Assert.True(result.IsOk, result.Error?.Message);
        var analysis = result.Value;
        Assert.Equal(ComputationStatus.Ok, analysis.LineOfSightStatus);
        Assert.False(analysis.IsVisible);
        Assert.Equal(5, analysis.Samples.Count);
        Assert.NotNull(analysis.Blocking);
        Assert.Equal(38.0, analysis.Blocking.ClearanceDeficitM, 0.01);
        Assert.Equal(50.0, analysis.Blocking.ElevationM);
        Assert.Equal(TerrainFeature.LocalPeak, analysis.Blocking.Feature);
        Assert.Equal(2, analysis.Blocking.SampleIndex);
    }

    [Fact]
    public void Curvature_hides_flat_ground_50_km_away_by_34_79_m()
    {
        // Tests.h's curvature case: flat ground at sea level, 2 m eyes, 50 km apart. Midway the Earth
        // rises 25000^2 / (2 * 4/3 * 6371000) = 36.79 m, so the sight line misses by 34.79 m -- and
        // with curvature switched off (k huge) the same path is clear.
        using var tile = SyntheticTile.Write(0);
        using var opened = Engine.OpenTile(tile.Path, 36, -112).Value;

        double metresPerDegree = 1.0 / Engine.SpacingToDegrees(1.0).Value;
        double end = 36.2 + 50000.0 / metresPerDegree;

        PathQuery Query(double k) => new(36.2, -111.5, 2, end, -111.5, 2, 5000, k, Interpolation.Nearest);

        var curved = opened.AnalyzePath(Query(4.0 / 3.0)).Value;
        var flat = opened.AnalyzePath(Query(1e12)).Value;

        Assert.Equal(ComputationStatus.Ok, curved.LineOfSightStatus);
        Assert.False(curved.IsVisible);
        Assert.Equal(34.79, curved.Blocking!.ClearanceDeficitM, 0.01);
        Assert.Equal(ComputationStatus.Ok, flat.LineOfSightStatus);
        Assert.True(flat.IsVisible);
    }

    [Fact]
    public void A_line_of_sight_on_the_sample_tile_matches_the_command_line_tool()
    {
        // README's example path, asked through the DLL with a 30 m spacing, and through
        // TerrainEngine.exe with the exact spacing in degrees the DLL used. The CLI prints six
        // significant digits; the DLL's answer must read the same at that precision.
        Assert.True(File.Exists(RepositoryFiles.Cli), $"Build the engine first: {RepositoryFiles.Cli} is missing.");

        using var tile = Engine.OpenTile(RepositoryFiles.SampleTile, 36, -112).Value;
        var analysis = tile.AnalyzePath(new PathQuery(36.3, -111.5, 2, 36.35, -111.45, 2, 30, 4.0 / 3.0, Interpolation.Nearest)).Value;

        var cli = RunCli("los", RepositoryFiles.SampleTile, "36", "-112", "36.3", "-111.5", "36.35", "-111.45",
            analysis.SpacingDeg.ToString("R", CultureInfo.InvariantCulture), "2", "2");

        Assert.Contains("Visible: NO", cli);
        Assert.NotNull(analysis.Blocking);
        Assert.Contains($"Blocking point: {Cout(analysis.Blocking.LatitudeDeg)}, {Cout(analysis.Blocking.LongitudeDeg)}", cli);
        Assert.Contains($"Blocking elevation: {Cout(analysis.Blocking.ElevationM)}", cli);
        Assert.Contains($"Clearance deficit: {Cout(analysis.Blocking.ClearanceDeficitM)}", cli);
        Assert.Contains("Blocking feature: falling slope", cli);
        Assert.Equal(TerrainFeature.FallingSlope, analysis.Blocking.Feature);
        Assert.Contains("Status: ok", cli);
    }

    [Fact]
    public void The_sample_tile_reports_its_facts()
    {
        using var tile = Engine.OpenTile(RepositoryFiles.SampleTile, 36, -112).Value;

        Assert.Equal(1201, tile.Info.PostsPerSide);
        Assert.Equal(0, tile.Info.VoidCount);
        Assert.Equal(3.0, tile.Info.PostSpacingArcsec, 1e-9);
        Assert.Equal(92.66, tile.Info.PostSpacingNorthSouthM, 0.01);
        Assert.Equal(37.0, tile.Info.NorthEastLatitudeDeg);
        Assert.Equal(1935.0, tile.Elevation(36.5, -111.5, Interpolation.Nearest).Value);
    }

    /// <summary>A double as std::cout prints it by default: six significant digits.</summary>
    private static string Cout(double value) => value.ToString("G6", CultureInfo.InvariantCulture);

    private static string RunCli(params string[] arguments)
    {
        var start = new ProcessStartInfo(RepositoryFiles.Cli)
        {
            RedirectStandardOutput = true,
            UseShellExecute = false,
            WorkingDirectory = RepositoryFiles.Root,
        };
        foreach (var argument in arguments) start.ArgumentList.Add(argument);

        using var process = Process.Start(start)!;
        string output = process.StandardOutput.ReadToEnd();
        process.WaitForExit();
        Assert.Equal(0, process.ExitCode);
        return output;
    }
}
