using TerrainBench.Engine;

namespace TerrainBench.Presentation;

/// <summary>The engine's statuses and feature kinds, in words a person reads.</summary>
public static class PlainWords
{
    public static string WhyNoConfidentAnswer(ComputationStatus status) => status switch
    {
        ComputationStatus.EndpointMissing => "The tile has a hole in its data under the observer or the target.",
        ComputationStatus.VoidInProfile => "The path crosses a hole in the tile's data, so part of the terrain between the points is unknown.",
        ComputationStatus.DataNotGiven =>
            "Part of the path needs terrain past the edge of the open tile, where there's no data. With bilinear interpolation this also happens on the tile's north or east edge.",
        ComputationStatus.EmptyOrSingleSampleProfile => "The path has no length to sample.",
        ComputationStatus.DatumRejected => "The heights couldn't be put on the terrain's vertical datum.",
        ComputationStatus.NothingEvaluated => "The path is too short to have any point between its ends to evaluate.",
        ComputationStatus.InvalidInput => "One of the inputs isn't a number the engine can use.",
        _ => "The engine reported no confident answer.",
    };

    public static string Feature(TerrainFeature feature) => feature switch
    {
        TerrainFeature.LocalPeak => "A local peak or ridge",
        TerrainFeature.RisingSlope => "A rising slope",
        TerrainFeature.FallingSlope => "A falling slope",
        TerrainFeature.Plateau => "Flat ground (a plateau)",
        _ => "Unknown",
    };

    public static string Interpolation(Engine.Interpolation mode) => mode == Engine.Interpolation.Bilinear ? "Bilinear" : "Nearest";

    public static string Algorithm(ViewshedAlgorithm algorithm) => algorithm == ViewshedAlgorithm.Naive ? "Naive" : "Fast";

    /// <summary>The one-line answer for a blocked path, as the result card and the map's tooltip say it.</summary>
    public static string BlockingSentence(BlockingPoint blocking) =>
        $"{Feature(blocking.Feature)} {Format.Distance(blocking.DistanceM)} from the observer rises {Format.Metres(blocking.ClearanceDeficitM)} above the sight line.";

    public const string RefractionExplanation =
        "k stretches the Earth's radius to model how the air bends light. 4/3 is a standard atmosphere, 1 means no refraction (the Earth's bare curvature), and a very large value such as 1e12 switches curvature off, as if the Earth were flat.";

    public const string PointsExplanation =
        "Left-click the map to place the observer and Ctrl + left-click to place the target, or type the coordinates. Drag a marker to move it; right-drag moves the map and the mouse wheel zooms.";

    public const string HeightExplanation = "The height of the eye or antenna above the terrain at that point.";

    /// <summary>What each height datum is called in the Measured From list, in <see cref="HeightDatum"/> order.</summary>
    public static IReadOnlyList<string> DatumChoices { get; } = ["Above the Ground", "Above Sea Level", "Above the WGS84 Ellipsoid"];

    /// <summary>What a height in the datum is measured from, to follow a number: "1,937.00 m above mean sea level".</summary>
    public static string DatumWords(HeightDatum datum) => datum switch
    {
        HeightDatum.AboveSeaLevel => "above mean sea level",
        HeightDatum.AboveEllipsoid => "above the WGS84 ellipsoid",
        _ => "above ground",
    };

    /// <summary>Under a minimum-visible-height legend: what its metres are measured from.</summary>
    public static string HeightsLegendNote(HeightDatum datum) =>
        datum == HeightDatum.AboveGround ? "Heights in m above each cell's own ground" : $"Heights in m {DatumWords(datum)}";

    /// <summary>A CSV column name's datum part: "above_mean_sea_level", "above_wgs84_ellipsoid", "above_ground".</summary>
    public static string DatumColumn(HeightDatum datum) => datum switch
    {
        HeightDatum.AboveSeaLevel => "above_mean_sea_level",
        HeightDatum.AboveEllipsoid => "above_wgs84_ellipsoid",
        _ => "above_ground",
    };

    /// <summary>The unit beside a height field: "m above ground", "m above sea level", "m above the ellipsoid".</summary>
    public static string HeightUnit(HeightDatum datum) => datum switch
    {
        HeightDatum.AboveSeaLevel => "m above sea level",
        HeightDatum.AboveEllipsoid => "m above the ellipsoid",
        _ => "m above ground",
    };

    /// <summary>A height as the report gives it: "2 m above ground", "1915.4 m above the WGS84 ellipsoid, where the geoid is -21.6 m above it".</summary>
    public static string Describe(Height height) =>
        $"{Format.Number(height.ValueM)} m {DatumWords(height.Datum)}" +
        (height.Datum == HeightDatum.AboveEllipsoid && height.GeoidUndulationM is double n ? $", where the geoid is {Format.Number(n)} m above it" : string.Empty);

    /// <summary>A height as the command line takes it: "2", "1937:msl", "1915.4:hae:-21.6".</summary>
    public static string CommandLine(Height height) => height.Datum switch
    {
        HeightDatum.AboveSeaLevel => Format.RoundTrip(height.ValueM) + ":msl",
        HeightDatum.AboveEllipsoid => $"{Format.RoundTrip(height.ValueM)}:hae:{Format.RoundTrip(height.GeoidUndulationM ?? double.NaN)}",
        _ => Format.RoundTrip(height.ValueM),
    };

    public const string AltitudeExplanation =
        "The height above mean sea level: one altitude, whatever the ground below it -- an aircraft's, or a summit's from the map.";

    public const string EllipsoidHeightExplanation =
        "The height above the WGS84 ellipsoid, as a GPS receiver reports it. It differs from the height above sea level by the geoid undulation, which it needs.";

    public const string DatumExplanation =
        "What the height is measured from. Above the ground, it rides on the terrain under the point; above sea level or the ellipsoid, it is one altitude, whatever the ground below. A height above the ellipsoid, as GPS gives it, needs the geoid undulation there too.";

    public const string UndulationExplanation =
        "How far the geoid (mean sea level) lies above the WGS84 ellipsoid at this point, from a geoid model such as EGM2008; negative where it lies below. A height above the ellipsoid is this much more than the same height above sea level. The engine has no geoid model of its own, so it can't guess this.";

    public const string UndulationRequired =
        "A height above the ellipsoid needs the geoid undulation at this point to be put on the tile's sea level. Enter it, or measure the height from sea level instead.";

    public const string TargetAltitudeExplanation =
        "Every cell is asked whether something at this one altitude above it can be seen -- an aircraft at a fixed altitude, say. Where the ground rises above it, the target is in the ground there and hidden.";

    public const string PathSpacingExplanation =
        "How far apart the engine samples the terrain along the path. Smaller spacing catches narrower ridges but takes longer; the tile's own post spacing, on the Terrain panel, is a good start.";

    public const string CellSpacingExplanation =
        "The size of one viewshed cell on the ground. Smaller cells show more detail, but halving the spacing makes four times as many cells.";

    public const string RadiusExplanation = "How far from the observer the viewshed reaches.";

    public const string TargetHeightExplanation =
        "Every cell is asked whether something this high above its ground can be seen: 0 for the ground itself, about 1.8 for a person standing there, a mast's height for a radio link. The terrain in between stays as it is.";

    public const string FrequencyExplanation =
        "Enter a radio frequency to also check the first Fresnel zone: the space around the sight line a radio link needs kept clear. Leave it empty to skip.";

    public const string InterpolationExplanation =
        "How the elevation between posts is read. Nearest takes the closest post as it is; bilinear blends the four posts around the point into a smoother surface, and needs all four to have data.";

    public const string AlgorithmExplanation =
        "Fast casts rays from the observer to the grid's edge and answers the cells along each ray. Naive checks every cell with its own line of sight, on every core: it's the reference, and takes about half a minute at 30 km on 8 cores.";

    public const string ShowExplanation =
        "Visibility marks which cells a target of the given height can be seen at. Minimum visible height colours every cell by how high above its ground a target there must stand to be seen: 0 where the ground itself is. It answers every target height at once, so the target height isn't used.";

    public const string ComparisonExplanation =
        "Marks cells in yellow. Fast vs. naive runs both algorithms and marks where they disagree. Previous run marks every cell that changed since the last run, which shows what changing one setting, such as k, did.";

    public const string CornerExplanation =
        "The tile's south-west corner is read from its file name (N36W112 is 36, -112). Change it if the file is named differently, then reopen.";
}
