using TerrainBench.Engine;

namespace TerrainBench.Presentation;

/// <summary>The engine's statuses and feature kinds, in words a person reads.</summary>
public static class PlainWords
{
    public static string WhyNoConfidentAnswer(ComputationStatus status) => status switch
    {
        ComputationStatus.EndpointMissing =>
            "There's no terrain data under the observer or the target. With bilinear interpolation this also happens on the tile's north or east edge.",
        ComputationStatus.VoidInProfile => "The path crosses missing data, so part of the terrain between the points is unknown.",
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
        "Fast casts rays from the observer to the grid's edge and answers the cells along each ray. Naive checks every cell with its own line of sight: it's the reference, and takes minutes at 30 km.";

    public const string ShowExplanation =
        "Visibility marks which cells a target of the given height can be seen at. Minimum visible height colours every cell by how high above its ground a target there must stand to be seen: 0 where the ground itself is. It answers every target height at once, so the target height isn't used.";

    public const string ComparisonExplanation =
        "Marks cells in yellow. Fast vs. naive runs both algorithms and marks where they disagree. Previous run marks every cell that changed since the last run, which shows what changing one setting, such as k, did.";

    public const string CornerExplanation =
        "The tile's south-west corner is read from its file name (N36W112 is 36, -112). Change it if the file is named differently, then reopen.";
}
