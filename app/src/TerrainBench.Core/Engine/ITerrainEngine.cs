namespace TerrainBench.Engine;

/// <summary>
/// The terrain engine as the app sees it. Every elevation, distance, visibility and clearance the
/// app shows comes through here; nothing above this interface computes terrain geometry.
/// </summary>
public interface ITerrainEngine
{
    /// <summary>The engine's version, e.g. "1.0.0".</summary>
    string Version { get; }

    /// <summary>The commit the engine was built from, or "unknown".</summary>
    string Commit { get; }

    EngineResult<ITile> OpenTile(string path, double southWestLatitudeDeg, double southWestLongitudeDeg);

    EngineResult<double> DistanceM(double latitude1Deg, double longitude1Deg, double latitude2Deg, double longitude2Deg);

    /// <summary>The spacing in degrees the engine samples at for a spacing in metres.</summary>
    EngineResult<double> SpacingToDegrees(double spacingM);
}

/// <summary>An open .hgt tile. Dispose closes it; calls after that fail as an invalid handle.</summary>
public interface ITile : IDisposable
{
    string Path { get; }

    TileInfo Info { get; }

    /// <summary>Elevation in metres above mean sea level, or null over a void.</summary>
    EngineResult<double?> Elevation(double latitudeDeg, double longitudeDeg, Interpolation interpolation);

    EngineResult<TilePosts> CopyPosts();

    EngineResult<PathAnalysis> AnalyzePath(PathQuery query);

    /// <summary>
    /// Runs on the calling thread; call it off the UI thread. Progress reports come from that thread
    /// too, so pass a <see cref="Progress{T}"/> created on the UI thread to see them there.
    /// </summary>
    EngineResult<ViewshedMap> Viewshed(ViewshedQuery query, IProgress<double>? progress, CancellationToken cancellation);
}
