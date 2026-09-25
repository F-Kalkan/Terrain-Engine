using CommunityToolkit.Mvvm.ComponentModel;
using TerrainBench.Engine;
using TerrainBench.Presentation;

namespace TerrainBench.ViewModels;

/// <summary>The open tile: loading it, its facts, its posts for the map, and the readout under the cursor.</summary>
public sealed partial class TerrainViewModel : ObservableObject, IDisposable
{
    private readonly ITerrainEngine? _engine;

    public TerrainViewModel(ITerrainEngine? engine)
    {
        _engine = engine;
        SouthWestLatitude = new NumberField("sw-lat", "South-West Corner Latitude", "degrees", "", Rules.Between("The corner latitude", -90, 89), hint: PlainWords.CornerExplanation);
        SouthWestLongitude = new NumberField("sw-lon", "South-West Corner Longitude", "degrees", "", Rules.Between("The corner longitude", -180, 179), hint: PlainWords.CornerExplanation);
    }

    public NumberField SouthWestLatitude { get; }

    public NumberField SouthWestLongitude { get; }

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(IsLoaded), nameof(FileName), nameof(ExtentText), nameof(GridText), nameof(ResolutionText), nameof(VoidText))]
    private ITile? _tile;

    [ObservableProperty]
    private TilePosts? _posts;

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(FileName))]
    private string? _tilePath;

    [ObservableProperty]
    private string? _loadError;

    [ObservableProperty]
    private string _cursorText = "Move the pointer over the map to read a position and elevation.";

    /// <summary>Metres one map-width covers at the tile's middle latitude, from the engine; 0 without a tile.</summary>
    [ObservableProperty]
    private double _widthM;

    public bool IsLoaded => Tile is not null;

    public string FileName => TilePath is null ? "No tile open" : Path.GetFileName(TilePath);

    public string ExtentText => Tile is null ? "—" :
        $"Latitude {Format.Number(Tile.Info.SouthWestLatitudeDeg)}° to {Format.Number(Tile.Info.NorthEastLatitudeDeg)}°, longitude {Format.Number(Tile.Info.SouthWestLongitudeDeg)}° to {Format.Number(Tile.Info.NorthEastLongitudeDeg)}°";

    public string GridText => Tile is null ? "—" : $"{Format.Count(Tile.Info.PostsPerSide)} × {Format.Count(Tile.Info.PostsPerSide)} posts";

    public string ResolutionText => Tile is null ? "—" :
        $"{Format.Number(Tile.Info.PostSpacingArcsec)} arcseconds: {Format.Metres(Tile.Info.PostSpacingNorthSouthM)} north–south, {Format.Metres(Tile.Info.PostSpacingEastWestM)} east–west at mid-tile";

    public string VoidText => Tile is null ? "—" : $"{Format.Count(Tile.Info.VoidCount)} posts with no data";

    /// <summary>Raised after a tile opens or closes.</summary>
    public event EventHandler? TileChanged;

    /// <summary>
    /// Fills the corner from a standard file name. Returns false when the name doesn't state one,
    /// leaving whatever was typed.
    /// </summary>
    public bool FillCornerFromName(string path)
    {
        if (!HgtFileName.TryParseCorner(path, out double lat, out double lon)) return false;
        SouthWestLatitude.SetValue(lat, "0");
        SouthWestLongitude.SetValue(lon, "0");
        return true;
    }

    /// <summary>Opens a tile with the corner fields as they stand. Returns the error, or null on success.</summary>
    public string? Open(string path)
    {
        if (_engine is null) return LoadError = "The terrain engine isn't available.";
        if (!SouthWestLatitude.IsValid || !SouthWestLongitude.IsValid || SouthWestLatitude.Value is null || SouthWestLongitude.Value is null)
        {
            return LoadError = "Fix the south-west corner first: " + (SouthWestLatitude.Error ?? SouthWestLongitude.Error ?? "enter both numbers.");
        }

        var result = _engine.OpenTile(path, SouthWestLatitude.Value.Value, SouthWestLongitude.Value.Value);
        if (!result.IsOk) return LoadError = result.Error!.Message;

        var posts = result.Value.CopyPosts();
        if (!posts.IsOk)
        {
            result.Value.Dispose();
            return LoadError = posts.Error!.Message;
        }

        var info = result.Value.Info;
        double middle = (info.SouthWestLatitudeDeg + info.NorthEastLatitudeDeg) / 2;
        var width = _engine.DistanceM(middle, info.SouthWestLongitudeDeg, middle, info.NorthEastLongitudeDeg);

        Tile?.Dispose();
        TilePath = path;
        Posts = posts.Value;
        WidthM = width.IsOk ? width.Value : 0;
        Tile = result.Value;
        LoadError = null;
        TileChanged?.Invoke(this, EventArgs.Empty);
        return null;
    }

    /// <summary>Opens the tile again with the corner as now typed.</summary>
    public string? Reopen() => TilePath is null ? LoadError = "Open a tile first." : Open(TilePath);

    public void UpdateCursor(double latitudeDeg, double longitudeDeg)
    {
        if (Tile is null) return;
        var elevation = Tile.Elevation(latitudeDeg, longitudeDeg, Interpolation.Nearest);
        string height = !elevation.IsOk ? "outside the tile"
            : elevation.Value is double metres ? Format.WholeMetres(metres) + " " + PlainWords.DatumWords(Tile.Info.ElevationDatum)
            : "no data";
        CursorText = $"Latitude {Format.Degrees(latitudeDeg)}, longitude {Format.Degrees(longitudeDeg)}, elevation {height}";
    }

    public void ClearCursor() => CursorText = IsLoaded ? "Move the pointer over the map to read a position and elevation." : string.Empty;

    public void Dispose() => Tile?.Dispose();
}
