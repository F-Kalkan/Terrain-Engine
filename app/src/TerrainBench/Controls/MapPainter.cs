using System.Runtime.InteropServices;
using Avalonia;
using Avalonia.Media;
using Avalonia.Media.Imaging;
using Avalonia.Platform;
using TerrainBench.Engine;
using TerrainBench.Presentation;

namespace TerrainBench.Controls;

/// <summary>
/// Where the tile's square degree lands on screen, and the conversion between a screen point and a
/// latitude and longitude. This is laying out a picture, not terrain geometry: the tile spans one
/// degree each way, north at the top, and its on-screen proportions come from the post spacings the
/// engine reported.
/// </summary>
public readonly record struct MapFrame(Rect Area, TileInfo Tile, int PostsPerSide)
{
    public double North => Tile.NorthEastLatitudeDeg;
    public double South => Tile.SouthWestLatitudeDeg;
    public double West => Tile.SouthWestLongitudeDeg;
    public double East => Tile.NorthEastLongitudeDeg;

    public Point ToScreen(double latitudeDeg, double longitudeDeg) => new(
        Area.X + (longitudeDeg - West) / (East - West) * Area.Width,
        Area.Y + (North - latitudeDeg) / (North - South) * Area.Height);

    public (double LatitudeDeg, double LongitudeDeg) ToCoordinate(Point point) => (
        North - (point.Y - Area.Y) / Area.Height * (North - South),
        West + (point.X - Area.X) / Area.Width * (East - West));

    public bool Contains(Point point) => Area.Inflate(0.5).Contains(point);

    /// <summary>The largest rectangle with the tile's proportions that fits, centred.</summary>
    public static MapFrame Fit(Rect bounds, TileInfo tile, int postsPerSide)
    {
        double aspect = tile.PostSpacingNorthSouthM > 0 ? tile.PostSpacingEastWestM / tile.PostSpacingNorthSouthM : 1;
        double width = bounds.Width;
        double height = width / aspect;
        if (height > bounds.Height)
        {
            height = bounds.Height;
            width = height * aspect;
        }
        return new MapFrame(new Rect(bounds.X + (bounds.Width - width) / 2, bounds.Y + (bounds.Height - height) / 2, width, height), tile, postsPerSide);
    }
}

public sealed record ElevationImage(WriteableBitmap Bitmap, float MinM, float MaxM);

public static class MapPainter
{
    public static ElevationImage ElevationBitmap(TilePosts posts)
    {
        var (bgra, min, max) = MapPixels.Elevation(posts);
        return new ElevationImage(ToBitmap(bgra, posts.PostsPerSide, posts.PostsPerSide), min, max);
    }

    public static WriteableBitmap ViewshedBitmap(ViewshedMap map, bool[]? highlights, bool[]? hidden = null) =>
        ToBitmap(MapPixels.Viewshed(map, highlights, hidden), map.Cols, map.Rows);

    /// <summary>
    /// The disc the viewshed was asked for, on screen: centred on the observer's cell and reaching the
    /// grid's outer edge. Its east-west radius follows the grid's own column step, which the engine
    /// widened so a column covers the same ground as a row -- so on the ground it is a circle.
    /// </summary>
    public static (Point Centre, double RadiusX, double RadiusY) ViewshedDisc(MapFrame frame, ViewshedMap map)
    {
        double lat = map.SouthWestCellLatitudeDeg + map.ObserverRow * map.SpacingDeg;
        double lon = map.SouthWestCellLongitudeDeg + map.ObserverCol * map.ColStepDeg;
        double halfRows = Math.Max(map.ObserverRow, map.Rows - 1 - map.ObserverRow) + 0.5;
        double halfCols = Math.Max(map.ObserverCol, map.Cols - 1 - map.ObserverCol) + 0.5;
        double radiusY = halfRows * map.SpacingDeg / (frame.North - frame.South) * frame.Area.Height;
        double radiusX = halfCols * map.ColStepDeg / (frame.East - frame.West) * frame.Area.Width;
        return (frame.ToScreen(lat, lon), radiusX, radiusY);
    }

    /// <summary>
    /// Posts sit on the tile's edges, so each pixel is centred on its post: the image reaches half a
    /// post beyond the degree on every side.
    /// </summary>
    public static void DrawTile(DrawingContext context, MapFrame frame, Bitmap elevation)
    {
        double halfPostX = frame.Area.Width / (frame.PostsPerSide - 1) / 2;
        double halfPostY = frame.Area.Height / (frame.PostsPerSide - 1) / 2;
        var destination = frame.Area.Inflate(new Thickness(halfPostX, halfPostY));
        using (context.PushClip(frame.Area))
        using (context.PushRenderOptions(new RenderOptions { BitmapInterpolationMode = BitmapInterpolationMode.None }))
        {
            context.DrawImage(elevation, new Rect(elevation.Size), destination);
        }
    }

    /// <summary>
    /// Each cell centred on its own coordinate, as the engine laid the grid out, inside the disc of
    /// the requested radius, with a dashed ring (and, when given, its radius) around it.
    /// </summary>
    public static void DrawViewshed(DrawingContext context, MapFrame frame, ViewshedMap map, Bitmap overlay, double opacity = 1, string? radiusLabel = null)
    {
        double northCentre = map.SouthWestCellLatitudeDeg + (map.Rows - 1) * map.SpacingDeg;
        double eastCentre = map.SouthWestCellLongitudeDeg + (map.Cols - 1) * map.ColStepDeg;
        var topLeft = frame.ToScreen(northCentre + map.SpacingDeg / 2, map.SouthWestCellLongitudeDeg - map.ColStepDeg / 2);
        var bottomRight = frame.ToScreen(map.SouthWestCellLatitudeDeg - map.SpacingDeg / 2, eastCentre + map.ColStepDeg / 2);
        var (centre, rx, ry) = ViewshedDisc(frame, map);
        var disc = new EllipseGeometry(new Rect(centre.X - rx, centre.Y - ry, rx * 2, ry * 2));

        // A 30 km grid has far more cells than the map has pixels. Picking one cell per pixel would
        // drop scattered cells -- a few thousand changed by k among four million -- so a shrinking
        // overlay is averaged instead, and those cells still tint the pixels they fall in.
        var destination = new Rect(topLeft, bottomRight);
        var mode = destination.Width < overlay.PixelSize.Width ? BitmapInterpolationMode.HighQuality : BitmapInterpolationMode.None;
        // Not clipped to the tile: cells the engine answered beyond its edge (no confident answer, for want of data)
        // are part of the result and must show, not be cut away with the terrain.
        using (context.PushClip(new Rect(topLeft, bottomRight).Union(new Rect(centre.X - rx, centre.Y - ry, rx * 2, ry * 2)).Inflate(4)))
        {
            using (context.PushGeometryClip(disc))
            using (context.PushOpacity(Math.Clamp(opacity, 0, 1)))
            using (context.PushRenderOptions(new RenderOptions { BitmapInterpolationMode = mode, EdgeMode = EdgeMode.Antialias }))
            {
                context.DrawImage(overlay, new Rect(overlay.Size), destination);
            }

            context.DrawGeometry(null, new Pen(new SolidColorBrush(Color.FromArgb(110, 0, 0, 0)), 3), disc);
            context.DrawGeometry(null, new Pen(Brushes.White, 1.5, new DashStyle([5, 3], 0)), disc);

            if (radiusLabel is not null)
            {
                var text = new FormattedText(radiusLabel, Format.Invariant, FlowDirection.LeftToRight, new Typeface(FontFamily.Default, FontStyle.Normal, FontWeight.SemiBold), 11, Brushes.White);
                // On the ring, up and to the right of the observer.
                var at = new Point(centre.X + rx * Math.Cos(Math.PI / 4), centre.Y - ry * Math.Sin(Math.PI / 4));
                var box = new Rect(at.X - text.Width / 2 - 7, at.Y - text.Height / 2 - 2, text.Width + 14, text.Height + 4);
                context.FillRectangle(new SolidColorBrush(Color.Parse("#D90F2438")), box, (float)(box.Height / 2));
                context.DrawText(text, new Point(box.X + 7, box.Y + 2));
            }
        }
    }

    private static WriteableBitmap ToBitmap(byte[] bgra, int width, int height)
    {
        var bitmap = new WriteableBitmap(new PixelSize(width, height), new Vector(96, 96), PixelFormat.Bgra8888, AlphaFormat.Premul);
        using var buffer = bitmap.Lock();
        int stride = buffer.RowBytes;
        for (int row = 0; row < height; row++)
        {
            Marshal.Copy(bgra, row * width * 4, buffer.Address + row * stride, width * 4);
        }
        return bitmap;
    }
}
