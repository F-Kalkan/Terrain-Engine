namespace TerrainBench.Engine;

/// <summary>Why an engine call didn't produce an answer.</summary>
public enum EngineErrorKind
{
    InvalidArgument = 1,
    InvalidHandle = 2,
    LoadFailed = 3,
    OutsideTile = 4,
    Cancelled = 5,
    OutOfMemory = 6,
    Internal = 7,

    /// <summary>TerrainEngineApi.dll couldn't be loaded at all.</summary>
    EngineMissing = 100,
}

/// <summary>A failed engine call: what kind of failure, and a sentence a user can read.</summary>
public sealed record EngineError(EngineErrorKind Kind, string Message);

/// <summary>Either a value or an <see cref="EngineError"/>; engine calls never throw.</summary>
public sealed class EngineResult<T>
{
    private readonly T? _value;

    private EngineResult(T? value, EngineError? error)
    {
        _value = value;
        Error = error;
    }

    public EngineError? Error { get; }

    public bool IsOk => Error is null;

    public T Value => IsOk ? _value! : throw new InvalidOperationException($"No value: {Error!.Message}");

    public static EngineResult<T> Ok(T value) => new(value, null);

    public static EngineResult<T> Fail(EngineError error) => new(default, error);

    public static EngineResult<T> Fail(EngineErrorKind kind, string message) => new(default, new EngineError(kind, message));
}

public enum Interpolation
{
    Nearest = 0,
    Bilinear = 1,
}

/// <summary>Why a line of sight or Fresnel clearance has, or hasn't, a confident answer.</summary>
public enum ComputationStatus
{
    Ok = 0,
    EmptyOrSingleSampleProfile = 1,
    EndpointMissing = 2,
    VoidInProfile = 3,
    DatumRejected = 4,
    NothingEvaluated = 5,
    InvalidInput = 6,
}

public enum TerrainFeature
{
    Unknown = 0,
    LocalPeak = 1,
    RisingSlope = 2,
    FallingSlope = 3,
    Plateau = 4,
}

public enum CellState : byte
{
    NotCovered = 0,
    Degraded = 1,
    Visible = 2,
    NotVisible = 3,
}

public enum ViewshedAlgorithm
{
    Fast = 0,
    Naive = 1,
}

/// <summary>A tile's extent, grid and data quality, as the engine reports them.</summary>
public sealed record TileInfo(
    double SouthWestLatitudeDeg,
    double SouthWestLongitudeDeg,
    double NorthEastLatitudeDeg,
    double NorthEastLongitudeDeg,
    int PostsPerSide,
    int VoidCount,
    double PostSpacingArcsec,
    double PostSpacingNorthSouthM,
    double PostSpacingEastWestM);

/// <summary>
/// Every post of a tile, row-major, row 0 the northern edge and column 0 the western edge. A post
/// whose <see cref="Valid"/> entry is 0 holds no data.
/// </summary>
public sealed record TilePosts(int PostsPerSide, float[] ElevationsM, byte[] Valid);

/// <summary>One observer-to-target query: heights above ground, spacing in metres.</summary>
public sealed record PathQuery(
    double ObserverLatitudeDeg,
    double ObserverLongitudeDeg,
    double ObserverHeightAboveGroundM,
    double TargetLatitudeDeg,
    double TargetLongitudeDeg,
    double TargetHeightAboveGroundM,
    double SpacingM,
    double RefractionK,
    Interpolation Interpolation,
    double FrequencyMHz = 0);

/// <summary>One sample along a path, carrying everything a chart of the result needs.</summary>
public sealed record PathSample(
    double LatitudeDeg,
    double LongitudeDeg,
    double DistanceM,
    double? ElevationM,
    double? CurvatureCorrectedElevationM,
    double? SightLineHeightM,
    double FirstFresnelRadiusM);

public sealed record BlockingPoint(
    double LatitudeDeg,
    double LongitudeDeg,
    double ElevationM,
    double DistanceM,
    int SampleIndex,
    double ClearanceDeficitM,
    TerrainFeature Feature);

public sealed record FresnelClearance(
    ComputationStatus Status,
    double MinClearanceFraction,
    int WorstSampleIndex,
    double WavelengthM);

/// <summary>A path's profile, line of sight and, when a frequency was given, Fresnel clearance.</summary>
public sealed record PathAnalysis(
    double SpacingDeg,
    double TotalDistanceM,
    double? ObserverEyeHeightM,
    double? TargetEyeHeightM,
    ComputationStatus LineOfSightStatus,
    bool IsVisible,
    BlockingPoint? Blocking,
    FresnelClearance? Fresnel,
    IReadOnlyList<PathSample> Samples);

public sealed record ViewshedQuery(
    double ObserverLatitudeDeg,
    double ObserverLongitudeDeg,
    double ObserverHeightAboveGroundM,
    double RadiusKm,
    double SpacingM,
    double RefractionK,
    Interpolation Interpolation,
    ViewshedAlgorithm Algorithm,
    double TargetHeightAboveGroundM = 0);

/// <summary>
/// A viewshed grid: <see cref="Cells"/> is row-major with row 0 the southernmost row, and cell
/// (row, col) is centred on (<see cref="SouthWestCellLatitudeDeg"/> + row * <see cref="SpacingDeg"/>,
/// <see cref="SouthWestCellLongitudeDeg"/> + col * <see cref="ColStepDeg"/>).
/// <para>
/// From a minimum-visible-height run, <see cref="HeightsM"/> holds, in the same order, the lowest
/// height above each cell's ground at which a target there is seen: 0 where the ground itself is,
/// infinity where no height is, NaN where there is no confident answer. <see cref="Cells"/> is then
/// the viewshed of the ground itself. Null from a viewshed run.
/// </para>
/// </summary>
public sealed record ViewshedMap(
    int Rows,
    int Cols,
    int ObserverRow,
    int ObserverCol,
    double SpacingDeg,
    double ColStepDeg,
    double SouthWestCellLatitudeDeg,
    double SouthWestCellLongitudeDeg,
    CellState[] Cells,
    double[]? HeightsM = null)
{
    public CellState At(int row, int col) => Cells[row * Cols + col];

    /// <summary>The cell's minimum visible height, or null for a viewshed run.</summary>
    public double? HeightAt(int row, int col) => HeightsM?[row * Cols + col];
}
