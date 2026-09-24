using TerrainBench.Engine;
using TerrainBench.Services;

namespace TerrainBench.Tests.ViewModels;

/// <summary>An engine whose answers a test chooses, so view models can be checked without the DLL.</summary>
internal sealed class FakeEngine : ITerrainEngine
{
    public string Version => "9.9.9";

    public string Commit => "abc1234";

    public Func<string, double, double, EngineResult<ITile>>? OnOpen { get; set; }

    public List<string> Opened { get; } = [];

    public EngineResult<ITile> OpenTile(string path, double southWestLatitudeDeg, double southWestLongitudeDeg)
    {
        Opened.Add(path);
        return OnOpen?.Invoke(path, southWestLatitudeDeg, southWestLongitudeDeg)
            ?? EngineResult<ITile>.Ok(new FakeTile(path, southWestLatitudeDeg, southWestLongitudeDeg));
    }

    public EngineResult<double> DistanceM(double latitude1Deg, double longitude1Deg, double latitude2Deg, double longitude2Deg) =>
        EngineResult<double>.Ok(89_000);

    public EngineResult<double> SpacingToDegrees(double spacingM) => EngineResult<double>.Ok(spacingM / 111_194.92664455873);
}

internal sealed class FakeTile(string path, double south, double west) : ITile
{
    public string Path { get; } = path;

    public TileInfo Info { get; } = new(south, west, south + 1, west + 1, 1201, 0, 3, 92.66, 74.49);

    public bool Disposed { get; private set; }

    public List<PathQuery> PathQueries { get; } = [];

    public List<ViewshedQuery> ViewshedQueries { get; } = [];

    public Func<PathQuery, EngineResult<PathAnalysis>> OnAnalyze { get; set; } = _ => EngineResult<PathAnalysis>.Ok(Analyses.Visible());

    public Func<ViewshedQuery, IProgress<double>?, CancellationToken, EngineResult<ViewshedMap>> OnViewshed { get; set; } =
        (query, progress, _) =>
        {
            progress?.Report(0);
            progress?.Report(1);
            return EngineResult<ViewshedMap>.Ok(Maps.Of(CellState.Visible, CellState.NotVisible, CellState.Degraded, CellState.NotCovered));
        };

    public EngineResult<double?> Elevation(double latitudeDeg, double longitudeDeg, Interpolation interpolation) =>
        latitudeDeg < Info.SouthWestLatitudeDeg || latitudeDeg > Info.NorthEastLatitudeDeg
            ? EngineResult<double?>.Fail(EngineErrorKind.OutsideTile, "The point is outside the open tile.")
            : EngineResult<double?>.Ok(1935);

    public EngineResult<TilePosts> CopyPosts() => EngineResult<TilePosts>.Ok(new TilePosts(2, [1, 2, 3, 4], [1, 1, 1, 0]));

    public EngineResult<PathAnalysis> AnalyzePath(PathQuery query)
    {
        PathQueries.Add(query);
        return OnAnalyze(query);
    }

    public EngineResult<ViewshedMap> Viewshed(ViewshedQuery query, IProgress<double>? progress, CancellationToken cancellation)
    {
        lock (ViewshedQueries) ViewshedQueries.Add(query);
        return OnViewshed(query, progress, cancellation);
    }

    public List<ViewshedQuery> HeightQueries { get; } = [];

    public Func<ViewshedQuery, IProgress<double>?, CancellationToken, EngineResult<ViewshedMap>> OnMinimumVisibleHeight { get; set; } =
        (query, progress, _) =>
        {
            progress?.Report(0);
            progress?.Report(1);
            return EngineResult<ViewshedMap>.Ok(Maps.WithHeights(0, 12.5, double.NaN, double.NaN));
        };

    public EngineResult<ViewshedMap> MinimumVisibleHeight(ViewshedQuery query, IProgress<double>? progress, CancellationToken cancellation)
    {
        lock (HeightQueries) HeightQueries.Add(query);
        return OnMinimumVisibleHeight(query, progress, cancellation);
    }

    public void Dispose() => Disposed = true;
}

internal static class Analyses
{
    public static PathAnalysis Visible() => new(0.00027, 6800, 1753, 1666, ComputationStatus.Ok, true, null, null, Samples());

    public static PathAnalysis Blocked() => new(0.0002697964817756191, 7139, 1753, 1666, ComputationStatus.Ok, false,
        new BlockingPoint(36.32773366451538, -111.47227683251644, 1900, 3959.8, 132, 195.9932532157111, TerrainFeature.FallingSlope),
        new FresnelClearance(ComputationStatus.Ok, -13.204864828261176, 132, 0.1249),
        Samples());

    public static PathAnalysis NoConfidentAnswer(ComputationStatus status) => new(0.00027, 44000, null, null, status, true, null, null, Samples(voidAt: 1));

    public static PathSample[] Samples(int voidAt = -1) =>
    [
        new(36.3, -111.5, 0, 1751, 1751, 1753, 0),
        new(36.31, -111.49, 1500.5, voidAt == 1 ? null : 1800.25, voidAt == 1 ? null : 1800.4, 1740, 12.5),
        new(36.32, -111.48, 3000, 1660, 1660.5, 1700, 0),
    ];
}

internal static class Maps
{
    public static ViewshedMap Of(params CellState[] cells) => new(2, 2, 1, 1, 0.00027, 0.00034, 36.5, -111.5, cells);

    /// <summary>A 2x2 minimum-visible-height map: each cell's state follows from its height, NaN meaning no confident answer.</summary>
    public static ViewshedMap WithHeights(params double[] heightsM) => new(2, 2, 1, 1, 0.00027, 0.00034, 36.5, -111.5,
        heightsM.Select(h => double.IsNaN(h) ? CellState.Degraded : h == 0 ? CellState.Visible : CellState.NotVisible).ToArray(), heightsM);
}

internal sealed class FakeDialogs : IDialogService
{
    public string? TileToPick { get; set; }

    public string? SavePath { get; set; }

    public Task<string?> PickTileAsync() => Task.FromResult(TileToPick);

    public Task<string?> PickSaveFileAsync(string title, string suggestedName, string extension, string typeName) => Task.FromResult(SavePath);
}

internal sealed class FakeClipboard : IClipboardService
{
    public string? Text { get; private set; }

    public Task SetTextAsync(string text)
    {
        Text = text;
        return Task.CompletedTask;
    }
}

internal sealed class MemorySettings : ISettingsStore
{
    public AppSettings? Saved { get; set; }

    public AppSettings? Load() => Saved;

    public void Save(AppSettings settings) => Saved = settings;
}

internal sealed class FakeExporter : IMapExporter
{
    public List<(string Path, bool WithViewshed)> Saved { get; } = [];

    public Task SavePngAsync(string path, TilePosts posts, ViewshedMap? viewshed, bool[]? disagreements, TileInfo tile)
    {
        Saved.Add((path, viewshed is not null));
        return Task.CompletedTask;
    }
}

/// <summary>Runs a block with the thread's culture swapped, e.g. to prove numbers don't pick up a decimal comma.</summary>
internal sealed class CultureScope : IDisposable
{
    private readonly System.Globalization.CultureInfo _culture = System.Globalization.CultureInfo.CurrentCulture;

    public CultureScope(string name) => System.Globalization.CultureInfo.CurrentCulture = new System.Globalization.CultureInfo(name);

    public void Dispose() => System.Globalization.CultureInfo.CurrentCulture = _culture;
}
