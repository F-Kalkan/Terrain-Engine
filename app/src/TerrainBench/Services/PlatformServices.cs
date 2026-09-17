using Avalonia;
using Avalonia.Controls;
using Avalonia.Input.Platform;
using Avalonia.Media;
using Avalonia.Media.Imaging;
using Avalonia.Platform.Storage;
using TerrainBench.Controls;
using TerrainBench.Engine;

namespace TerrainBench.Services;

public sealed class AvaloniaDialogs(TopLevel owner) : IDialogService
{
    public async Task<string?> PickTileAsync()
    {
        var files = await owner.StorageProvider.OpenFilePickerAsync(new FilePickerOpenOptions
        {
            Title = "Open an SRTM tile",
            AllowMultiple = false,
            FileTypeFilter = [new FilePickerFileType("SRTM tile (.hgt)") { Patterns = ["*.hgt"] }, FilePickerFileTypes.All],
        });
        return files.Count == 0 ? null : files[0].TryGetLocalPath();
    }

    public async Task<string?> PickSaveFileAsync(string title, string suggestedName, string extension, string typeName)
    {
        var file = await owner.StorageProvider.SaveFilePickerAsync(new FilePickerSaveOptions
        {
            Title = title,
            SuggestedFileName = suggestedName,
            DefaultExtension = extension,
            FileTypeChoices = [new FilePickerFileType(typeName) { Patterns = [$"*.{extension}"] }],
        });
        return file?.TryGetLocalPath();
    }
}

public sealed class AvaloniaClipboard(TopLevel owner) : IClipboardService
{
    public async Task SetTextAsync(string text)
    {
        if (owner.Clipboard is { } clipboard) await clipboard.SetTextAsync(text);
    }
}

/// <summary>Draws the map exactly as the map view does, into a PNG file.</summary>
public sealed class PngMapExporter : IMapExporter
{
    public Task SavePngAsync(string path, TilePosts posts, ViewshedMap? viewshed, bool[]? disagreements, TileInfo tile)
    {
        var elevation = MapPainter.ElevationBitmap(posts);
        using var overlay = viewshed is null ? null : MapPainter.ViewshedBitmap(viewshed, disagreements);

        // One pixel per post across; the height follows the tile's real ground shape.
        double aspect = tile.PostSpacingNorthSouthM > 0 ? tile.PostSpacingEastWestM / tile.PostSpacingNorthSouthM : 1;
        int width = posts.PostsPerSide;
        int height = Math.Max(1, (int)Math.Round(posts.PostsPerSide / aspect));

        using var target = new RenderTargetBitmap(new PixelSize(width, height));
        using (var context = target.CreateDrawingContext())
        {
            var frame = new MapFrame(new Rect(0, 0, width, height), tile, posts.PostsPerSide);
            MapPainter.DrawTile(context, frame, elevation.Bitmap);
            if (overlay is not null && viewshed is not null) MapPainter.DrawViewshed(context, frame, viewshed, overlay);
        }

        elevation.Bitmap.Dispose();
        target.Save(path, PngBitmapEncoderOptions.Default);
        return Task.CompletedTask;
    }
}
