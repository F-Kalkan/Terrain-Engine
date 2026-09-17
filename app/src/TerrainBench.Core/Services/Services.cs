using System.Text.Json;
using TerrainBench.Engine;

namespace TerrainBench.Services;

/// <summary>File pickers; implemented by the app, faked in tests.</summary>
public interface IDialogService
{
    /// <summary>Asks for an .hgt tile to open; null when the person cancels.</summary>
    Task<string?> PickTileAsync();

    /// <summary>Asks where to save a file; null when the person cancels.</summary>
    Task<string?> PickSaveFileAsync(string title, string suggestedName, string extension, string typeName);
}

public interface IClipboardService
{
    Task SetTextAsync(string text);
}

/// <summary>Draws the map, with or without the viewshed over it, into a PNG file.</summary>
public interface IMapExporter
{
    Task SavePngAsync(string path, TilePosts posts, ViewshedMap? viewshed, bool[]? disagreements, TileInfo tile);
}

/// <summary>What the app remembers between runs: the last tile and every parameter as typed.</summary>
public sealed class AppSettings
{
    public string? TilePath { get; set; }
    public string SouthWestLatitude { get; set; } = "";
    public string SouthWestLongitude { get; set; } = "";
    public Dictionary<string, string> Fields { get; set; } = new();
    public Interpolation LineOfSightInterpolation { get; set; } = Interpolation.Nearest;
    public Interpolation ViewshedInterpolation { get; set; } = Interpolation.Nearest;
    public ViewshedAlgorithm ViewshedAlgorithm { get; set; } = ViewshedAlgorithm.Fast;
    public string SelectedTab { get; set; } = "Terrain";
    public bool ProfileOpen { get; set; } = true;
    public double ProfileHeight { get; set; } = 260;
    public double PanelWidth { get; set; } = 440;
    public bool ProfileShowTerrain { get; set; } = true;
    public bool ProfileShowCurvature { get; set; } = true;
    public bool ProfileShowSightLine { get; set; } = true;
    public bool ProfileShowFresnel { get; set; } = true;
    public double ViewshedOpacity { get; set; } = 100;
    public bool[]? HiddenViewshedLayers { get; set; }
}

public interface ISettingsStore
{
    AppSettings? Load();

    void Save(AppSettings settings);
}

/// <summary>Settings as JSON in a file; a missing or unreadable file reads as no settings.</summary>
public sealed class JsonSettingsStore(string path) : ISettingsStore
{
    private static readonly JsonSerializerOptions Options = new() { WriteIndented = true };

    public static string DefaultPath =>
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "TerrainBench", "settings.json");

    public AppSettings? Load()
    {
        try
        {
            return File.Exists(path) ? JsonSerializer.Deserialize<AppSettings>(File.ReadAllText(path), Options) : null;
        }
        catch (Exception ex) when (ex is IOException or JsonException or UnauthorizedAccessException)
        {
            return null;
        }
    }

    public void Save(AppSettings settings)
    {
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            File.WriteAllText(path, JsonSerializer.Serialize(settings, Options));
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            // Remembering settings is a convenience; failing to must never take the app down.
        }
    }
}
