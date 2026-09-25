using System.Diagnostics;
using System.Globalization;
using TerrainBench.Engine;
using TerrainBench.Presentation;
using TerrainBench.ViewModels;
using Xunit;

namespace TerrainBench.Tests.Interop;

/// <summary>
/// Heights in every datum, on every surface. One line of sight -- README's path on the sample tile, eyes
/// 1.7654321 m above the ground at both ends -- is asked with each eye given above the ground, as the same
/// height above sea level, and as the same height above the WGS84 ellipsoid with its geoid undulation: through
/// the DLL, the command line and TerrainBench. Each surface gives the same verdict all three ways, with heights
/// agreeing to 1e-9 m, says which datum every height it returns is in, and refuses a height above the ellipsoid
/// given without its undulation, with a message.
/// </summary>
public class DatumTests
{
    private const double ObserverLat = 36.3, ObserverLon = -111.5, TargetLat = 36.35, TargetLon = -111.45;

    // An uneven eye height, so no eye lands on a whole metre and every digit printed of it is compared.
    private const double EyeM = 1.7654321;

    // The geoid's height above the WGS84 ellipsoid. Any value will do: it is the caller's, and the engine only
    // subtracts it again. Not a round number, so the ellipsoidal heights aren't either.
    private const double Undulation = -21.63;

    private static readonly ITerrainEngine Engine = NativeTerrainEngine.Load().Value;

    /// <summary>The same two eyes, three ways: above the ground, above sea level and above the ellipsoid.</summary>
    private static (Height Observer, Height Target)[] ThreeWays(ITile tile)
    {
        double observerGround = tile.Elevation(ObserverLat, ObserverLon, Interpolation.Nearest).Value!.Value;
        double targetGround = tile.Elevation(TargetLat, TargetLon, Interpolation.Nearest).Value!.Value;
        return
        [
            (EyeM, EyeM),
            (new Height(observerGround + EyeM, HeightDatum.AboveSeaLevel), new Height(targetGround + EyeM, HeightDatum.AboveSeaLevel)),
            (new Height(observerGround + EyeM + Undulation, HeightDatum.AboveEllipsoid, Undulation),
             new Height(targetGround + EyeM + Undulation, HeightDatum.AboveEllipsoid, Undulation)),
        ];
    }

    private static PathQuery Path((Height Observer, Height Target) heights) =>
        new(ObserverLat, ObserverLon, heights.Observer, TargetLat, TargetLon, heights.Target, 30, 4.0 / 3.0, Interpolation.Nearest);

    [Fact]
    public void Through_the_dll_one_eye_in_three_datums_gets_one_answer()
    {
        using var tile = Engine.OpenTile(RepositoryFiles.SampleTile, 36, -112).Value;
        var answers = ThreeWays(tile).Select(heights => tile.AnalyzePath(Path(heights)).Value).ToList();
        var first = answers[0];

        Assert.Equal(ComputationStatus.Ok, first.LineOfSightStatus);
        Assert.False(first.IsVisible);
        foreach (var answer in answers)
        {
            Assert.Equal(HeightDatum.AboveSeaLevel, answer.HeightsDatum);
            Assert.Equal(first.LineOfSightStatus, answer.LineOfSightStatus);
            Assert.Equal(first.IsVisible, answer.IsVisible);
            Assert.Equal(first.Blocking!.SampleIndex, answer.Blocking!.SampleIndex);
            Assert.Equal(first.Blocking.ElevationM, answer.Blocking.ElevationM);
            Assert.Equal(first.Blocking.ClearanceDeficitM, answer.Blocking.ClearanceDeficitM, 1e-9);
            Assert.Equal(first.ObserverEyeHeightM!.Value, answer.ObserverEyeHeightM!.Value, 1e-9);
            Assert.Equal(first.TargetEyeHeightM!.Value, answer.TargetEyeHeightM!.Value, 1e-9);
            for (int i = 0; i < first.Samples.Count; i++)
            {
                Assert.Equal(first.Samples[i].SightLineHeightM!.Value, answer.Samples[i].SightLineHeightM!.Value, 1e-9);
            }
        }
    }

    [Fact]
    public void Through_the_dll_a_viewshed_and_minimum_visible_heights_from_one_eye_in_three_datums_are_the_same()
    {
        using var tile = Engine.OpenTile(RepositoryFiles.SampleTile, 36, -112).Value;
        var observers = ThreeWays(tile).Select(heights => heights.Observer).ToList();
        ViewshedQuery Query(Height observer) => new(ObserverLat, ObserverLon, observer, 3, 30, 4.0 / 3.0, Interpolation.Nearest, ViewshedAlgorithm.Fast);

        var maps = observers.Select(o => tile.Viewshed(Query(o), null, default).Value).ToList();
        var heights = observers.Select(o => tile.MinimumVisibleHeight(Query(o), null, default).Value).ToList();

        Assert.Contains(CellState.Visible, maps[0].Cells);
        Assert.Equal(HeightDatum.AboveGround, heights[0].HeightsDatum);
        for (int k = 1; k < 3; k++)
        {
            Assert.Equal(maps[0].Cells, maps[k].Cells);
            Assert.Equal(heights[0].Cells, heights[k].Cells);
            for (int i = 0; i < heights[0].HeightsM!.Length; i++)
            {
                double a = heights[0].HeightsM![i], b = heights[k].HeightsM![i];
                Assert.True(a == b || Math.Abs(a - b) <= 1e-9 || (double.IsNaN(a) && double.IsNaN(b)), $"Cell {i}: {a} against {b}.");
            }
        }
    }

    [Fact]
    public void Through_the_dll_every_height_says_its_datum_and_one_above_the_ellipsoid_needs_its_undulation()
    {
        using var tile = Engine.OpenTile(RepositoryFiles.SampleTile, 36, -112).Value;
        Assert.Equal(HeightDatum.AboveSeaLevel, tile.Info.ElevationDatum);

        var noUndulation = new Height(1900, HeightDatum.AboveEllipsoid);
        var path = tile.AnalyzePath(Path((2, noUndulation)));
        Assert.Equal(EngineErrorKind.InvalidArgument, path.Error!.Kind);
        Assert.Contains("target height is above the WGS84 ellipsoid but no geoid undulation was given", path.Error.Message);

        var grid = tile.Viewshed(new ViewshedQuery(ObserverLat, ObserverLon, noUndulation, 1, 30, 4.0 / 3.0, Interpolation.Nearest, ViewshedAlgorithm.Fast), null, default);
        Assert.Equal(EngineErrorKind.InvalidArgument, grid.Error!.Kind);
        Assert.Contains("observer height is above the WGS84 ellipsoid but no geoid undulation was given", grid.Error.Message);

        var heights = tile.MinimumVisibleHeight(new ViewshedQuery(ObserverLat, ObserverLon, noUndulation, 1, 30, 4.0 / 3.0, Interpolation.Nearest, ViewshedAlgorithm.Fast), null, default);
        Assert.Equal(EngineErrorKind.InvalidArgument, heights.Error!.Kind);
        Assert.Contains("observer height is above the WGS84 ellipsoid but no geoid undulation was given", heights.Error.Message);

        var target = tile.Viewshed(new ViewshedQuery(ObserverLat, ObserverLon, 2, 1, 30, 4.0 / 3.0, Interpolation.Nearest, ViewshedAlgorithm.Fast, noUndulation), null, default);
        Assert.Contains("target height is above the WGS84 ellipsoid but no geoid undulation was given", target.Error!.Message);
    }

    [Fact]
    public void Through_the_command_line_one_eye_in_three_datums_gets_one_answer_and_says_its_datum()
    {
        Assert.True(File.Exists(RepositoryFiles.Cli), $"Build the engine first: {RepositoryFiles.Cli} is missing.");
        using var tile = Engine.OpenTile(RepositoryFiles.SampleTile, 36, -112).Value;
        var dll = tile.AnalyzePath(Path((EyeM, EyeM))).Value;

        var outputs = ThreeWays(tile).Select(heights => Los(dll.SpacingDeg, PlainWords.CommandLine(heights.Observer), PlainWords.CommandLine(heights.Target))).ToList();
        foreach (var (exitCode, output) in outputs)
        {
            Assert.Equal(0, exitCode);
            Assert.Contains("Visible: NO", output);
            Assert.Contains($"Blocking elevation: {dll.Blocking!.ElevationM.ToString("G6", CultureInfo.InvariantCulture)} m above mean sea level", output);
            Assert.Equal(dll.ObserverEyeHeightM!.Value, Eye(output, "Observer eye"), 1e-9);
            Assert.Equal(dll.TargetEyeHeightM!.Value, Eye(output, "Target eye"), 1e-9);
            Assert.Equal(Line(outputs[0].Output, "Blocking point"), Line(output, "Blocking point"));
            Assert.Equal(Line(outputs[0].Output, "Clearance deficit"), Line(output, "Clearance deficit"));
        }

        var (refusedCode, refused) = Los(dll.SpacingDeg, "2", "1900:hae");
        Assert.Equal(1, refusedCode);
        Assert.Contains("hB is a height above the WGS84 ellipsoid with no geoid undulation", refused);
    }

    [Fact]
    public void Every_command_that_takes_a_height_refuses_one_above_the_ellipsoid_without_its_undulation()
    {
        Assert.True(File.Exists(RepositoryFiles.Cli), $"Build the engine first: {RepositoryFiles.Cli} is missing.");
        string spacing = "0.0002697964817756191", tile = RepositoryFiles.SampleTile;
        string queries = System.IO.Path.Combine(System.IO.Path.GetTempPath(), $"terrainbench-queries-{Guid.NewGuid():N}.txt");
        File.WriteAllText(queries, $"{spacing}\n36.3 -111.5 36.35 -111.45 2 2\n36.3 -111.5 36.35 -111.45 1900:hae 2\n");
        try
        {
            var refusals = new (string Says, (int ExitCode, string Output) Run)[]
            {
                ("height is a height above the WGS84 ellipsoid with no geoid undulation",
                    Cli("viewshed", tile, "36", "-112", "36.3", "-111.5", "11", spacing, "1900:hae")),
                ("targetHeight is a height above the WGS84 ellipsoid with no geoid undulation",
                    Cli("viewshed", tile, "36", "-112", "36.3", "-111.5", "11", spacing, "2", "1.3333333333333333", "nearest", "1900:hae")),
                ("hA is a height above the WGS84 ellipsoid with no geoid undulation",
                    Cli("fresnel", tile, "36", "-112", "36.3", "-111.5", "36.35", "-111.45", spacing, "1900:hae", "2", "2400")),
                ("query 1: hA is a height above the WGS84 ellipsoid with no geoid undulation",
                    Cli("batch", tile, "36", "-112", queries)),
            };
            foreach (var (says, run) in refusals)
            {
                Assert.Equal(1, run.ExitCode);
                Assert.Contains(says, run.Output);
            }
        }
        finally
        {
            File.Delete(queries);
        }
    }

    [Fact]
    public void Through_terrainbench_one_eye_in_three_datums_gets_one_answer_and_a_target_can_be_placed_by_altitude()
    {
        using var tile = Engine.OpenTile(RepositoryFiles.SampleTile, 36, -112).Value;
        var vm = new LineOfSightViewModel(() => tile);
        var results = new List<(Verdict Verdict, PathAnalysis Analysis, string[] Rows)>();
        foreach (var (observer, target) in ThreeWays(tile))
        {
            Type(vm.ObserverHeight, observer);
            Type(vm.TargetHeight, target);
            Assert.True(vm.CanCheck);
            vm.Check();
            results.Add((vm.Verdict, vm.Analysis!, vm.Details.Select(r => $"{r.Label}: {r.Value}").ToArray()));
        }

        Assert.Equal(Verdict.Blocked, results[0].Verdict);
        foreach (var result in results)
        {
            Assert.Equal(results[0].Verdict, result.Verdict);
            Assert.Equal(results[0].Rows, result.Rows);
            Assert.Equal(results[0].Analysis.ObserverEyeHeightM!.Value, result.Analysis.ObserverEyeHeightM!.Value, 1e-9);
            Assert.Equal(results[0].Analysis.TargetEyeHeightM!.Value, result.Analysis.TargetEyeHeightM!.Value, 1e-9);
        }
        Assert.Contains(results[0].Rows, row => row.StartsWith("Target Eye: ") && row.EndsWith(" m above mean sea level"));

        // A target placed by altitude: an aircraft 5,000 m above sea level, which stays at that altitude when moved.
        Type(vm.TargetHeight, new Height(5000, HeightDatum.AboveSeaLevel));
        vm.PlaceTarget(36.4, -111.4);
        Assert.Equal(5000, vm.Analysis!.TargetEyeHeightM);
        Assert.Contains(vm.Details, r => r.Label == "Target Eye" && r.Value == "5000.00 m above mean sea level");
        Assert.Equal(Verdict.Visible, vm.Verdict);

        // Above the ellipsoid with no undulation: refused at the field, with what to do, and never sent.
        vm.TargetHeight.Datum = HeightDatum.AboveEllipsoid;
        vm.TargetHeight.Undulation.Text = "";
        Assert.Equal(PlainWords.UndulationRequired, vm.TargetHeight.Undulation.Error);
        Assert.False(vm.CanCheck);
    }

    private static void Type(HeightInput input, Height height)
    {
        input.Datum = height.Datum;
        input.Metres.Text = height.ValueM.ToString("R", CultureInfo.InvariantCulture);
        input.Undulation.Text = height.GeoidUndulationM?.ToString("R", CultureInfo.InvariantCulture) ?? "";
    }

    private static (int ExitCode, string Output) Los(double spacingDeg, string observerHeight, string targetHeight) =>
        Cli("los", RepositoryFiles.SampleTile, "36", "-112", "36.3", "-111.5", "36.35", "-111.45",
            spacingDeg.ToString("R", CultureInfo.InvariantCulture), observerHeight, targetHeight);

    private static (int ExitCode, string Output) Cli(params string[] arguments)
    {
        var start = new ProcessStartInfo(RepositoryFiles.Cli) { RedirectStandardOutput = true, UseShellExecute = false, WorkingDirectory = System.IO.Path.GetTempPath() };
        foreach (var argument in arguments) start.ArgumentList.Add(argument);
        using var process = Process.Start(start)!;
        string output = process.StandardOutput.ReadToEnd();
        process.WaitForExit();
        return (process.ExitCode, output);
    }

    private static string Line(string output, string label) =>
        output.Split('\n').Select(l => l.TrimEnd('\r')).Single(l => l.StartsWith(label + ": "));

    /// <summary>The metres on a "Observer eye: 1937.0000000000002 m above mean sea level" line, checking its datum.</summary>
    private static double Eye(string output, string label)
    {
        string line = Line(output, label);
        Assert.EndsWith(" m above mean sea level", line);
        return double.Parse(line[(label.Length + 2)..line.IndexOf(" m ", StringComparison.Ordinal)], CultureInfo.InvariantCulture);
    }
}
