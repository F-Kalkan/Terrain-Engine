using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Media;
using Avalonia.Media.Imaging;
using TerrainBench.Engine;
using TerrainBench.Presentation;

namespace TerrainBench.Controls;

public enum MapMarker
{
    Observer,
    Target,
    ViewshedObserver,
}

public sealed class MapPointEventArgs(double latitudeDeg, double longitudeDeg, MapMarker? marker = null, bool target = false) : EventArgs
{
    public double LatitudeDeg { get; } = latitudeDeg;
    public double LongitudeDeg { get; } = longitudeDeg;
    public MapMarker? Marker { get; } = marker;

    /// <summary>Ctrl was held: the click places the target rather than the observer.</summary>
    public bool Target { get; } = target;
}

/// <summary>
/// The tile, north up, with the viewshed over it, the line-of-sight markers, the blocking point and
/// a scale bar. A click places the observer and Ctrl + click the target; a marker can be dragged.
/// The mouse wheel zooms towards the pointer and a right-drag moves the zoomed map. Resting the
/// pointer on a marker or the blocking point shows what is known about it. With keyboard focus, the
/// arrow keys move a crosshair (Shift for bigger steps) and Enter places the observer there (Ctrl +
/// Enter the target), so the map works without a mouse too. Every number it draws comes from the engine.
/// </summary>
public sealed class MapView : Control
{
    public const double MinZoom = 1;
    public const double MaxZoom = 20;
    public const double WheelStep = 1.10;
    public const double ButtonStep = 1.25;

    public static readonly StyledProperty<TilePosts?> PostsProperty = AvaloniaProperty.Register<MapView, TilePosts?>(nameof(Posts));
    public static readonly StyledProperty<TileInfo?> TileProperty = AvaloniaProperty.Register<MapView, TileInfo?>(nameof(Tile));
    public static readonly StyledProperty<double> WidthMProperty = AvaloniaProperty.Register<MapView, double>(nameof(WidthM));
    public static readonly StyledProperty<ViewshedMap?> ViewshedProperty = AvaloniaProperty.Register<MapView, ViewshedMap?>(nameof(Viewshed));
    public static readonly StyledProperty<bool[]?> HighlightsProperty = AvaloniaProperty.Register<MapView, bool[]?>(nameof(Highlights));
    public static readonly StyledProperty<bool> ShowViewshedProperty = AvaloniaProperty.Register<MapView, bool>(nameof(ShowViewshed));
    public static readonly StyledProperty<bool> ShowPathProperty = AvaloniaProperty.Register<MapView, bool>(nameof(ShowPath), true);
    public static readonly StyledProperty<PathAnalysis?> AnalysisProperty = AvaloniaProperty.Register<MapView, PathAnalysis?>(nameof(Analysis));
    public static readonly StyledProperty<double?> ObserverLatitudeProperty = AvaloniaProperty.Register<MapView, double?>(nameof(ObserverLatitude));
    public static readonly StyledProperty<double?> ObserverLongitudeProperty = AvaloniaProperty.Register<MapView, double?>(nameof(ObserverLongitude));
    public static readonly StyledProperty<double?> TargetLatitudeProperty = AvaloniaProperty.Register<MapView, double?>(nameof(TargetLatitude));
    public static readonly StyledProperty<double?> TargetLongitudeProperty = AvaloniaProperty.Register<MapView, double?>(nameof(TargetLongitude));
    public static readonly StyledProperty<double?> ViewshedObserverLatitudeProperty = AvaloniaProperty.Register<MapView, double?>(nameof(ViewshedObserverLatitude));
    public static readonly StyledProperty<double?> ViewshedObserverLongitudeProperty = AvaloniaProperty.Register<MapView, double?>(nameof(ViewshedObserverLongitude));

    public static readonly StyledProperty<bool[]?> HiddenLayersProperty = AvaloniaProperty.Register<MapView, bool[]?>(nameof(HiddenLayers));
    public static readonly StyledProperty<double> ViewshedOpacityProperty = AvaloniaProperty.Register<MapView, double>(nameof(ViewshedOpacity), 1.0);
    public static readonly StyledProperty<bool> ShowViewshedLayerProperty = AvaloniaProperty.Register<MapView, bool>(nameof(ShowViewshedLayer), true);
    public static readonly StyledProperty<string?> ViewshedRadiusLabelProperty = AvaloniaProperty.Register<MapView, string?>(nameof(ViewshedRadiusLabel));
    public static readonly StyledProperty<double?> ProfileHoverLatitudeProperty = AvaloniaProperty.Register<MapView, double?>(nameof(ProfileHoverLatitude));
    public static readonly StyledProperty<double?> ProfileHoverLongitudeProperty = AvaloniaProperty.Register<MapView, double?>(nameof(ProfileHoverLongitude));

    /// <summary>
    /// Set while a marker is dragged when checking the path live couldn't keep up: the map then draws
    /// a plain line to the dragged marker and waits for the drop.
    /// </summary>
    public static readonly StyledProperty<bool> LiveDragPausedProperty = AvaloniaProperty.Register<MapView, bool>(nameof(LiveDragPaused));

    /// <summary>At most this often a dragged marker reports where it is.</summary>
    public static readonly TimeSpan DragReportInterval = TimeSpan.FromMilliseconds(33);
    /// <summary>1 shows the whole tile; 2 shows it twice as large, and so on up to <see cref="MaxZoom"/>.</summary>
    public static readonly StyledProperty<double> ZoomProperty = AvaloniaProperty.Register<MapView, double>(nameof(Zoom), 1.0,
        coerce: (_, value) => double.IsFinite(value) ? Math.Clamp(value, MinZoom, MaxZoom) : MinZoom);

    private static readonly Color ObserverColour = Color.Parse("#0072B2");
    private static readonly Color TargetColour = Color.Parse("#E69F00");
    private static readonly Color BlockingColour = Color.Parse("#A8322A");

    private enum Hovered { None, Observer, Target, ViewshedObserver, Blocking }

    private ElevationImage? _elevation;
    private WriteableBitmap? _overlay;
    private MapMarker? _dragging;
    private DateTime _lastDragReport;
    private Point _dragPoint;
    private (double Lat, double Lon)? _keyboardCursor;

    /// <summary>The crosshair shows while the arrow keys drive the map and hides once the mouse does; it keeps its place meanwhile.</summary>
    private bool _keyboardCursorShown;
    private Vector _pan;
    private bool _panning;
    private Point _panStart;
    private Vector _panOrigin;
    private Hovered _hovered;

    static MapView()
    {
        FocusableProperty.OverrideDefaultValue<MapView>(true);
        AffectsRender<MapView>(PostsProperty, TileProperty, WidthMProperty, ViewshedProperty, HighlightsProperty, ShowViewshedProperty, ShowPathProperty,
            AnalysisProperty, ObserverLatitudeProperty, ObserverLongitudeProperty, TargetLatitudeProperty, TargetLongitudeProperty,
            ViewshedObserverLatitudeProperty, ViewshedObserverLongitudeProperty, ZoomProperty, ViewshedOpacityProperty, ShowViewshedLayerProperty,
            ViewshedRadiusLabelProperty, ProfileHoverLatitudeProperty, ProfileHoverLongitudeProperty, LiveDragPausedProperty);
    }

    public TilePosts? Posts { get => GetValue(PostsProperty); set => SetValue(PostsProperty, value); }
    public TileInfo? Tile { get => GetValue(TileProperty); set => SetValue(TileProperty, value); }
    public double WidthM { get => GetValue(WidthMProperty); set => SetValue(WidthMProperty, value); }
    public ViewshedMap? Viewshed { get => GetValue(ViewshedProperty); set => SetValue(ViewshedProperty, value); }
    public bool[]? Highlights { get => GetValue(HighlightsProperty); set => SetValue(HighlightsProperty, value); }
    public bool ShowViewshed { get => GetValue(ShowViewshedProperty); set => SetValue(ShowViewshedProperty, value); }
    public bool ShowPath { get => GetValue(ShowPathProperty); set => SetValue(ShowPathProperty, value); }
    public PathAnalysis? Analysis { get => GetValue(AnalysisProperty); set => SetValue(AnalysisProperty, value); }
    public double? ObserverLatitude { get => GetValue(ObserverLatitudeProperty); set => SetValue(ObserverLatitudeProperty, value); }
    public double? ObserverLongitude { get => GetValue(ObserverLongitudeProperty); set => SetValue(ObserverLongitudeProperty, value); }
    public double? TargetLatitude { get => GetValue(TargetLatitudeProperty); set => SetValue(TargetLatitudeProperty, value); }
    public double? TargetLongitude { get => GetValue(TargetLongitudeProperty); set => SetValue(TargetLongitudeProperty, value); }
    public double? ViewshedObserverLatitude { get => GetValue(ViewshedObserverLatitudeProperty); set => SetValue(ViewshedObserverLatitudeProperty, value); }
    public double? ViewshedObserverLongitude { get => GetValue(ViewshedObserverLongitudeProperty); set => SetValue(ViewshedObserverLongitudeProperty, value); }
    public double Zoom { get => GetValue(ZoomProperty); set => SetValue(ZoomProperty, value); }
    public bool[]? HiddenLayers { get => GetValue(HiddenLayersProperty); set => SetValue(HiddenLayersProperty, value); }
    public double ViewshedOpacity { get => GetValue(ViewshedOpacityProperty); set => SetValue(ViewshedOpacityProperty, value); }
    public bool ShowViewshedLayer { get => GetValue(ShowViewshedLayerProperty); set => SetValue(ShowViewshedLayerProperty, value); }
    public string? ViewshedRadiusLabel { get => GetValue(ViewshedRadiusLabelProperty); set => SetValue(ViewshedRadiusLabelProperty, value); }
    public double? ProfileHoverLatitude { get => GetValue(ProfileHoverLatitudeProperty); set => SetValue(ProfileHoverLatitudeProperty, value); }
    public double? ProfileHoverLongitude { get => GetValue(ProfileHoverLongitudeProperty); set => SetValue(ProfileHoverLongitudeProperty, value); }
    public bool LiveDragPaused { get => GetValue(LiveDragPausedProperty); set => SetValue(LiveDragPausedProperty, value); }

    /// <summary>A line-of-sight marker being dragged, where it is now; reported at most every <see cref="DragReportInterval"/>.</summary>
    public event EventHandler<MapPointEventArgs>? MarkerDragging;

    /// <summary>A click (or Enter at the keyboard crosshair) at a coordinate.</summary>
    public event EventHandler<MapPointEventArgs>? MapClicked;

    /// <summary>A marker dragged and released at a coordinate.</summary>
    public event EventHandler<MapPointEventArgs>? MarkerDropped;

    public event EventHandler<MapPointEventArgs>? CursorMoved;

    public event EventHandler? CursorLeft;

    /// <summary>The part of the control the map is drawn in.</summary>
    private Rect Viewport => new Rect(Bounds.Size).Deflate(8);

    /// <summary>The tile's frame at zoom 1: the largest rectangle with its proportions that fits.</summary>
    private MapFrame? FitFrame => Tile is { } tile && Posts is { } posts ? MapFrame.Fit(Viewport, tile, posts.PostsPerSide) : null;

    /// <summary>The tile's frame as drawn, zoomed and moved; null without a tile.</summary>
    public MapFrame? Frame
    {
        get
        {
            if (FitFrame is not { } fit) return null;
            var area = fit.Area;
            double width = area.Width * Zoom, height = area.Height * Zoom;
            return fit with { Area = new Rect(area.Center.X - width / 2 + _pan.X, area.Center.Y - height / 2 + _pan.Y, width, height) };
        }
    }

    /// <summary>Zooms in one button step, towards the middle of the map.</summary>
    public void ZoomIn() => ZoomAt(Viewport.Center, Zoom * ButtonStep);

    /// <summary>Zooms out one button step, from the middle of the map.</summary>
    public void ZoomOut() => ZoomAt(Viewport.Center, Zoom / ButtonStep);

    /// <summary>Changes the zoom keeping the coordinate under <paramref name="anchor"/> where it is on screen.</summary>
    public void ZoomAt(Point anchor, double zoom)
    {
        if (FitFrame is not { } fit || Frame is not { } frame) return;
        zoom = Math.Clamp(zoom, MinZoom, MaxZoom);
        if (!frame.Area.Contains(anchor)) anchor = Viewport.Center;

        double rx = (anchor.X - frame.Area.X) / frame.Area.Width;
        double ry = (anchor.Y - frame.Area.Y) / frame.Area.Height;
        double width = fit.Area.Width * zoom, height = fit.Area.Height * zoom;
        double left = anchor.X - rx * width, top = anchor.Y - ry * height;
        _pan = new Vector(left - (fit.Area.Center.X - width / 2), top - (fit.Area.Center.Y - height / 2));
        SetCurrentValue(ZoomProperty, zoom);
        ClampPan();
        InvalidateVisual();
    }

    protected override void OnPropertyChanged(AvaloniaPropertyChangedEventArgs change)
    {
        base.OnPropertyChanged(change);
        if (change.Property == PostsProperty)
        {
            _elevation?.Bitmap.Dispose();
            _elevation = Posts is null ? null : MapPainter.ElevationBitmap(Posts);
            _keyboardCursor = null;
            _pan = default;
            SetCurrentValue(ZoomProperty, 1.0);
        }
        else if (change.Property == ViewshedProperty || change.Property == HighlightsProperty || change.Property == HiddenLayersProperty)
        {
            _overlay?.Dispose();
            _overlay = Viewshed is null ? null : MapPainter.ViewshedBitmap(Viewshed, Highlights, HiddenLayers);
        }
    }

    protected override Size ArrangeOverride(Size finalSize)
    {
        var size = base.ArrangeOverride(finalSize);
        ClampPan();
        return size;
    }

    public override void Render(DrawingContext context)
    {
        context.FillRectangle(Brushes.Transparent, new Rect(Bounds.Size));
        if (Frame is not { } frame || _elevation is null)
        {
            var hint = Text("Open a tile to see the terrain here.", 14, Resource("BodyTextBrush", Brushes.Gray));
            context.DrawText(hint, new Point((Bounds.Width - hint.Width) / 2, (Bounds.Height - hint.Height) / 2));
            return;
        }

        var visible = frame.Area.Intersect(Viewport);
        using (context.PushClip(Viewport))
        {
            MapPainter.DrawTile(context, frame, _elevation.Bitmap);
            if (ShowViewshed && ShowViewshedLayer && _dragging != MapMarker.ViewshedObserver && Viewshed is { } map && _overlay is not null)
            {
                MapPainter.DrawViewshed(context, frame, map, _overlay, ViewshedOpacity, ViewshedRadiusLabel);
            }
            context.DrawRectangle(null, new Pen(new SolidColorBrush(Color.FromArgb(160, 0, 0, 0)), 1), frame.Area);

            if (ShowViewshed)
            {
                DrawMarker(context, frame, MapMarker.ViewshedObserver, ViewshedObserverLatitude, ViewshedObserverLongitude, "Observer", ObserverColour, _hovered == Hovered.ViewshedObserver);
            }
            else if (ShowPath)
            {
                if (_dragging is MapMarker.Observer or MapMarker.Target && LiveDragPaused) DrawDragPreview(context, frame);
                else DrawPath(context, frame);
                DrawProfileHover(context, frame);
                DrawMarker(context, frame, MapMarker.Observer, ObserverLatitude, ObserverLongitude, "Observer", ObserverColour, _hovered == Hovered.Observer);
                DrawMarker(context, frame, MapMarker.Target, TargetLatitude, TargetLongitude, "Target", TargetColour, _hovered == Hovered.Target, labelAbove: true);
            }

            if (_keyboardCursor is { } cursor && IsFocused && _keyboardCursorShown)
            {
                var at = frame.ToScreen(cursor.Lat, cursor.Lon);
                foreach (var p in new[] { new Pen(Brushes.Black, 4), new Pen(Brushes.White, 2) })
                {
                    context.DrawLine(p, at + new Vector(-12, 0), at + new Vector(12, 0));
                    context.DrawLine(p, at + new Vector(0, -12), at + new Vector(0, 12));
                }
            }
        }

        DrawScaleBar(context, frame, visible);
        DrawNorthAndRange(context, visible);
        DrawHoverCard(context, frame);

        if (IsFocused)
        {
            context.DrawRectangle(null, new Pen(Resource("AccentBrush", Brushes.SteelBlue), 2), new Rect(Bounds.Size).Deflate(1));
        }
    }

    private void DrawPath(DrawingContext context, MapFrame frame)
    {
        if (Analysis is not { Samples.Count: > 1 } analysis) return;

        var halo = new Pen(new SolidColorBrush(Color.FromArgb(235, 255, 255, 255)), 4.5, lineCap: PenLineCap.Round, lineJoin: PenLineJoin.Round);
        var clear = new Pen(new SolidColorBrush(ObserverColour), 2, lineCap: PenLineCap.Round, lineJoin: PenLineJoin.Round);

        var blocking = !analysis.IsVisible ? analysis.Blocking : null;
        if (blocking is null)
        {
            var whole = Line(frame, analysis.Samples.Select(s => (s.LatitudeDeg, s.LongitudeDeg)));
            context.DrawGeometry(null, halo, whole);
            context.DrawGeometry(null, clear, whole);
            return;
        }

        // Seen up to the blocking point, hidden past it.
        var seen = analysis.Samples.Where(s => s.DistanceM <= blocking.DistanceM).Select(s => (s.LatitudeDeg, s.LongitudeDeg))
            .Append((blocking.LatitudeDeg, blocking.LongitudeDeg)).ToList();
        var hidden = analysis.Samples.Where(s => s.DistanceM > blocking.DistanceM).Select(s => (s.LatitudeDeg, s.LongitudeDeg))
            .Prepend((blocking.LatitudeDeg, blocking.LongitudeDeg)).ToList();

        var seenLine = Line(frame, seen);
        context.DrawGeometry(null, halo, seenLine);
        context.DrawGeometry(null, clear, seenLine);
        if (hidden.Count > 1)
        {
            var hiddenLine = Line(frame, hidden);
            context.DrawGeometry(null, new Pen(new SolidColorBrush(Color.FromArgb(200, 255, 255, 255)), 4, lineCap: PenLineCap.Round), hiddenLine);
            context.DrawGeometry(null, new Pen(new SolidColorBrush(BlockingColour), 2, new DashStyle([2.5, 2], 0)), hiddenLine);
        }

        var at = frame.ToScreen(blocking.LatitudeDeg, blocking.LongitudeDeg);
        bool hot = _hovered == Hovered.Blocking;
        context.DrawEllipse(new SolidColorBrush(BlockingColour, 0.3), null, at, hot ? 15 : 12, hot ? 15 : 12);
        context.DrawEllipse(new SolidColorBrush(BlockingColour), new Pen(Brushes.White, 2), at, hot ? 7.5 : 6.5, hot ? 7.5 : 6.5);
    }

    /// <summary>A plain line from the marker left in place to the one being dragged, while live checking is paused.</summary>
    private void DrawDragPreview(DrawingContext context, MapFrame frame)
    {
        var (lat, lon) = _dragging == MapMarker.Observer ? (TargetLatitude, TargetLongitude) : (ObserverLatitude, ObserverLongitude);
        if (lat is not double a || lon is not double b) return;
        var from = frame.ToScreen(a, b);
        context.DrawLine(new Pen(new SolidColorBrush(Color.FromArgb(200, 255, 255, 255)), 4, lineCap: PenLineCap.Round), from, _dragPoint);
        context.DrawLine(new Pen(new SolidColorBrush(ObserverColour, 0.8), 2, new DashStyle([3, 2], 0), PenLineCap.Round), from, _dragPoint);
    }

    /// <summary>The point the pointer is on in the profile chart, so a hill there can be found on the map.</summary>
    private void DrawProfileHover(DrawingContext context, MapFrame frame)
    {
        if (ProfileHoverLatitude is not double lat || ProfileHoverLongitude is not double lon) return;
        var at = frame.ToScreen(lat, lon);
        context.DrawEllipse(new SolidColorBrush(Color.FromArgb(90, 0, 0, 0)), null, at, 9, 9);
        context.DrawEllipse(Brushes.White, new Pen(new SolidColorBrush(Color.Parse("#0F2438")), 2), at, 5, 5);
    }

    private static StreamGeometry Line(MapFrame frame, IEnumerable<(double Lat, double Lon)> points)
    {
        var geometry = new StreamGeometry();
        using var g = geometry.Open();
        bool first = true;
        foreach (var (lat, lon) in points)
        {
            var p = frame.ToScreen(lat, lon);
            if (first) g.BeginFigure(p, false);
            else g.LineTo(p);
            first = false;
        }
        if (!first) g.EndFigure(false);
        return geometry;
    }

    /// <summary>A coloured dot in a white ring with a soft halo, and its name on a label: below it, or above for the target so close points keep both labels readable.</summary>
    private void DrawMarker(DrawingContext context, MapFrame frame, MapMarker marker, double? latitude, double? longitude, string label, Color colour, bool hot, bool labelAbove = false)
    {
        if (latitude is not double lat || longitude is not double lon) return;
        bool dragged = _dragging == marker;
        var at = dragged ? _dragPoint : frame.ToScreen(lat, lon);
        double scale = hot || dragged ? 1.2 : 1;

        context.DrawEllipse(new SolidColorBrush(colour, dragged ? 0.4 : 0.25), null, at, 16 * scale, 16 * scale);
        context.DrawEllipse(Brushes.White, null, at, 10 * scale, 10 * scale);
        context.DrawEllipse(new SolidColorBrush(colour), null, at, 6 * scale, 6 * scale);

        var text = Text(label, 11, Brushes.White, FontWeight.SemiBold);
        double labelHeight = text.Height + 4;
        double top = labelAbove ? at.Y - 17 * scale - 2 - labelHeight : at.Y + 17 * scale + 2;
        var box = new Rect(at.X - text.Width / 2 - 7, top, text.Width + 14, labelHeight);
        context.FillRectangle(new SolidColorBrush(Color.Parse("#D90F2438")), box, (float)(box.Height / 2));
        context.DrawText(text, new Point(box.X + 7, box.Y + 2));
    }

    private void DrawScaleBar(DrawingContext context, MapFrame frame, Rect visible)
    {
        if (WidthM <= 0 || visible.Width <= 0) return;
        var (_, pixels, label) = ScaleBar.Choose(WidthM / frame.Area.Width, Math.Min(180, visible.Width / 3));
        if (pixels <= 0) return;

        var start = new Point(visible.X + 12, visible.Bottom - 16);
        var end = start + new Vector(pixels, 0);
        var text = Text(label, 12, Brushes.Black, FontWeight.SemiBold);
        context.FillRectangle(new SolidColorBrush(Color.FromArgb(215, 255, 255, 255)), new Rect(start.X - 6, start.Y - text.Height - 6, pixels + 12, text.Height + 14), 4);
        var pen = new Pen(Brushes.Black, 2);
        context.DrawLine(pen, start, end);
        context.DrawLine(pen, start + new Vector(0, -5), start + new Vector(0, 3));
        context.DrawLine(pen, end + new Vector(0, -5), end + new Vector(0, 3));
        context.DrawText(text, new Point(start.X + (pixels - text.Width) / 2, start.Y - text.Height - 3));
    }

    private void DrawNorthAndRange(DrawingContext context, Rect visible)
    {
        if (_elevation is null || visible.Width <= 0) return;
        var north = Text("N ↑", 13, Brushes.Black, FontWeight.Bold);
        var range = Text($"{Format.WholeMetres(_elevation.MinM)} – {Format.WholeMetres(_elevation.MaxM)}", 12, Brushes.Black);
        double width = Math.Max(north.Width, range.Width) + 12;
        var box = new Rect(visible.Right - width - 8, visible.Y + 8, width, north.Height + range.Height + 10);
        context.FillRectangle(new SolidColorBrush(Color.FromArgb(215, 255, 255, 255)), box, 4);
        context.DrawText(north, new Point(box.X + 6, box.Y + 4));
        context.DrawText(range, new Point(box.X + 6, box.Y + 6 + north.Height));
    }

    /// <summary>What is known about the marker or blocking point under the pointer, on a small card beside it.</summary>
    private void DrawHoverCard(DrawingContext context, MapFrame frame)
    {
        if (_hovered == Hovered.None || _dragging is not null || _panning) return;

        var body = Resource("BodyTextBrush", Brushes.Black);
        var muted = Resource("MutedTextBrush", Brushes.Gray);
        Point anchor;
        string title;
        IBrush titleBrush;
        var lines = new List<(string Text, IBrush Brush)>();

        switch (_hovered)
        {
            case Hovered.Blocking when Analysis?.Blocking is { } blocking:
                anchor = frame.ToScreen(blocking.LatitudeDeg, blocking.LongitudeDeg);
                title = "Blocked";
                titleBrush = Resource("BlockedTextBrush", Brushes.Red);
                lines.Add((PlainWords.BlockingSentence(blocking), body));
                lines.Add(($"Terrain height: {Format.Metres(blocking.ElevationM)} above mean sea level", muted));
                lines.Add(($"{Format.Degrees(blocking.LatitudeDeg)}, {Format.Degrees(blocking.LongitudeDeg)}", muted));
                break;
            case Hovered.Observer when ObserverLatitude is double lat && ObserverLongitude is double lon:
                anchor = frame.ToScreen(lat, lon);
                title = "Observer";
                titleBrush = body;
                lines.Add(($"{Format.Degrees(lat)}, {Format.Degrees(lon)}", body));
                AddHeights(lines, lat, lon, Analysis is { Samples.Count: > 0 } a ? (a.Samples[0], a.ObserverEyeHeightM) : null, muted);
                break;
            case Hovered.Target when TargetLatitude is double tlat && TargetLongitude is double tlon:
                anchor = frame.ToScreen(tlat, tlon);
                title = "Target";
                titleBrush = body;
                lines.Add(($"{Format.Degrees(tlat)}, {Format.Degrees(tlon)}", body));
                AddHeights(lines, tlat, tlon, Analysis is { Samples.Count: > 0 } t ? (t.Samples[^1], t.TargetEyeHeightM) : null, muted);
                break;
            case Hovered.ViewshedObserver when ViewshedObserverLatitude is double lat && ViewshedObserverLongitude is double lon:
                anchor = frame.ToScreen(lat, lon);
                title = "Viewshed Observer";
                titleBrush = body;
                lines.Add(($"{Format.Degrees(lat)}, {Format.Degrees(lon)}", body));
                break;
            default:
                return;
        }

        const double maxTextWidth = 250, padding = 12;
        var titleText = Text(title, 14, titleBrush, FontWeight.Bold);
        var formatted = lines.Select(l => Wrapped(l.Text, 12, l.Brush, maxTextWidth)).ToList();
        double width = Math.Max(titleText.Width, formatted.Count == 0 ? 0 : formatted.Max(f => f.Width)) + padding * 2;
        double height = titleText.Height + formatted.Sum(f => f.Height + 3) + padding * 2;

        var bounds = new Rect(Bounds.Size).Deflate(4);
        double x = anchor.X + 22;
        if (x + width > bounds.Right) x = anchor.X - 22 - width;
        double y = Math.Clamp(anchor.Y - height / 2, bounds.Top, Math.Max(bounds.Top, bounds.Bottom - height));
        var card = new Rect(Math.Max(bounds.Left, x), y, width, height);

        context.DrawRectangle(Resource("CardBrush", Brushes.White), new Pen(Resource("DividerBrush", Brushes.LightGray), 1), new RoundedRect(card, 8),
            new BoxShadows(new BoxShadow { OffsetY = 4, Blur = 14, Color = Color.FromArgb(70, 0, 0, 0) }));

        double cy = card.Y + padding;
        context.DrawText(titleText, new Point(card.X + padding, cy));
        cy += titleText.Height + 3;
        foreach (var text in formatted)
        {
            context.DrawText(text, new Point(card.X + padding, cy));
            cy += text.Height + 3;
        }
    }

    /// <summary>Ground and eye height from the last analysis, when it was computed for this very point.</summary>
    private static void AddHeights(List<(string, IBrush)> lines, double lat, double lon, (PathSample Sample, double? EyeM)? end, IBrush brush)
    {
        if (end is not { } e || Math.Abs(e.Sample.LatitudeDeg - lat) > 1e-9 || Math.Abs(e.Sample.LongitudeDeg - lon) > 1e-9) return;
        lines.Add(($"Ground: {(e.Sample.ElevationM is double g ? Format.Metres(g) : "no data")}", brush));
        if (e.EyeM is double eye) lines.Add(($"Eye: {Format.Metres(eye)} above mean sea level", brush));
    }

    protected override void OnPointerPressed(PointerPressedEventArgs e)
    {
        base.OnPointerPressed(e);
        HideKeyboardCursor();
        Focus();
        if (Frame is not { } frame) return;

        var point = e.GetPosition(this);
        var properties = e.GetCurrentPoint(this).Properties;
        if (properties.IsRightButtonPressed)
        {
            _panning = true;
            _panStart = point;
            _panOrigin = _pan;
            Cursor = new Cursor(StandardCursorType.SizeAll);
            e.Pointer.Capture(this);
            e.Handled = true;
            return;
        }
        if (!properties.IsLeftButtonPressed) return;

        _dragging = MarkerAt(frame, point);
        _dragPoint = point;
        if (_dragging is not null)
        {
            SetCurrentValue(LiveDragPausedProperty, false);
            _lastDragReport = default;
            e.Pointer.Capture(this);
        }
        e.Handled = true;
    }

    protected override void OnPointerMoved(PointerEventArgs e)
    {
        base.OnPointerMoved(e);
        HideKeyboardCursor();
        if (Frame is not { } frame) return;

        var point = e.GetPosition(this);
        if (_panning)
        {
            _pan = _panOrigin + (point - _panStart);
            ClampPan();
            InvalidateVisual();
            return;
        }

        if (_dragging is not null)
        {
            _dragPoint = Clamp(frame, point);
            if (_dragging is MapMarker.Observer or MapMarker.Target && !LiveDragPaused && DateTime.UtcNow - _lastDragReport >= DragReportInterval)
            {
                _lastDragReport = DateTime.UtcNow;
                var (dragLat, dragLon) = frame.ToCoordinate(_dragPoint);
                MarkerDragging?.Invoke(this, new MapPointEventArgs(dragLat, dragLon, _dragging));
            }
            InvalidateVisual();
        }
        else
        {
            var hovered = HoveredAt(frame, point);
            if (hovered != _hovered)
            {
                _hovered = hovered;
                Cursor = hovered is Hovered.None or Hovered.Blocking ? null : new Cursor(StandardCursorType.Hand);
                InvalidateVisual();
            }
        }

        if (frame.Contains(point) && Viewport.Contains(point))
        {
            var (lat, lon) = frame.ToCoordinate(point);
            CursorMoved?.Invoke(this, new MapPointEventArgs(lat, lon));
        }
        else
        {
            CursorLeft?.Invoke(this, EventArgs.Empty);
        }
    }

    protected override void OnPointerReleased(PointerReleasedEventArgs e)
    {
        base.OnPointerReleased(e);
        if (_panning)
        {
            _panning = false;
            Cursor = null;
            e.Pointer.Capture(null);
            e.Handled = true;
            return;
        }
        if (Frame is not { } frame) return;

        var point = e.GetPosition(this);
        if (_dragging is { } marker)
        {
            _dragging = null;
            e.Pointer.Capture(null);
            var (lat, lon) = frame.ToCoordinate(Clamp(frame, point));
            MarkerDropped?.Invoke(this, new MapPointEventArgs(lat, lon, marker));
            InvalidateVisual();
        }
        else if (frame.Contains(point) && Viewport.Contains(point) && e.InitialPressMouseButton == MouseButton.Left)
        {
            var (lat, lon) = frame.ToCoordinate(point);
            MapClicked?.Invoke(this, new MapPointEventArgs(lat, lon, target: e.KeyModifiers.HasFlag(KeyModifiers.Control)));
        }
        e.Handled = true;
    }

    protected override void OnPointerWheelChanged(PointerWheelEventArgs e)
    {
        base.OnPointerWheelChanged(e);
        if (Frame is null || e.Delta.Y == 0) return;
        ZoomAt(e.GetPosition(this), Zoom * Math.Pow(WheelStep, e.Delta.Y));
        e.Handled = true;
    }

    protected override void OnPointerExited(PointerEventArgs e)
    {
        base.OnPointerExited(e);
        CursorLeft?.Invoke(this, EventArgs.Empty);
        if (_hovered != Hovered.None)
        {
            _hovered = Hovered.None;
            InvalidateVisual();
        }
    }

    protected override void OnKeyDown(KeyEventArgs e)
    {
        base.OnKeyDown(e);
        if (Tile is not { } tile) return;

        var cursor = _keyboardCursor ?? ((tile.SouthWestLatitudeDeg + tile.NorthEastLatitudeDeg) / 2, (tile.SouthWestLongitudeDeg + tile.NorthEastLongitudeDeg) / 2);
        double step = (e.KeyModifiers.HasFlag(KeyModifiers.Shift) ? 0.05 : 0.005) / Zoom * (tile.NorthEastLatitudeDeg - tile.SouthWestLatitudeDeg);
        switch (e.Key)
        {
            case Key.Up: cursor.Item1 += step; break;
            case Key.Down: cursor.Item1 -= step; break;
            case Key.Left: cursor.Item2 -= step; break;
            case Key.Right: cursor.Item2 += step; break;
            case Key.Add:
            case Key.OemPlus:
                ZoomIn();
                e.Handled = true;
                return;
            case Key.Subtract:
            case Key.OemMinus:
                ZoomOut();
                e.Handled = true;
                return;
            case Key.Enter:
            case Key.Space:
                MapClicked?.Invoke(this, new MapPointEventArgs(cursor.Item1, cursor.Item2, target: e.KeyModifiers.HasFlag(KeyModifiers.Control)));
                e.Handled = true;
                return;
            default:
                return;
        }

        cursor.Item1 = Math.Clamp(cursor.Item1, tile.SouthWestLatitudeDeg, tile.NorthEastLatitudeDeg);
        cursor.Item2 = Math.Clamp(cursor.Item2, tile.SouthWestLongitudeDeg, tile.NorthEastLongitudeDeg);
        _keyboardCursor = cursor;
        _keyboardCursorShown = true;
        KeepInView(cursor.Item1, cursor.Item2);
        CursorMoved?.Invoke(this, new MapPointEventArgs(cursor.Item1, cursor.Item2));
        InvalidateVisual();
        e.Handled = true;
    }

    protected override void OnGotFocus(FocusChangedEventArgs e)
    {
        base.OnGotFocus(e);
        InvalidateVisual();
    }

    protected override void OnLostFocus(FocusChangedEventArgs e)
    {
        base.OnLostFocus(e);
        InvalidateVisual();
    }

    private void HideKeyboardCursor()
    {
        if (!_keyboardCursorShown) return;
        _keyboardCursorShown = false;
        InvalidateVisual();
    }

    /// <summary>A zoomed map moves so the keyboard crosshair stays on screen.</summary>
    private void KeepInView(double lat, double lon)
    {
        if (Frame is not { } frame) return;
        var view = Viewport.Deflate(24);
        var at = frame.ToScreen(lat, lon);
        double dx = at.X < view.Left ? view.Left - at.X : at.X > view.Right ? view.Right - at.X : 0;
        double dy = at.Y < view.Top ? view.Top - at.Y : at.Y > view.Bottom ? view.Bottom - at.Y : 0;
        _pan += new Vector(dx, dy);
        ClampPan();
    }

    /// <summary>The zoomed tile always covers the area the whole tile covers at zoom 1: no moving it off screen.</summary>
    private void ClampPan()
    {
        if (FitFrame is not { } fit)
        {
            _pan = default;
            return;
        }
        double maxX = fit.Area.Width * (Zoom - 1) / 2, maxY = fit.Area.Height * (Zoom - 1) / 2;
        _pan = new Vector(Math.Clamp(_pan.X, -maxX, maxX), Math.Clamp(_pan.Y, -maxY, maxY));
    }

    private IEnumerable<(Hovered Kind, MapMarker? Marker, double? Lat, double? Lon)> Markers()
    {
        if (ShowViewshed)
        {
            yield return (Hovered.ViewshedObserver, MapMarker.ViewshedObserver, ViewshedObserverLatitude, ViewshedObserverLongitude);
        }
        else if (ShowPath)
        {
            yield return (Hovered.Target, MapMarker.Target, TargetLatitude, TargetLongitude);
            yield return (Hovered.Observer, MapMarker.Observer, ObserverLatitude, ObserverLongitude);
        }
    }

    private MapMarker? MarkerAt(MapFrame frame, Point point)
    {
        foreach (var (_, marker, lat, lon) in Markers())
        {
            if (lat is double a && lon is double b && Near(frame.ToScreen(a, b), point, 12)) return marker;
        }
        return null;
    }

    private Hovered HoveredAt(MapFrame frame, Point point)
    {
        if (!Viewport.Contains(point)) return Hovered.None;
        foreach (var (kind, _, lat, lon) in Markers())
        {
            if (lat is double a && lon is double b && Near(frame.ToScreen(a, b), point, 12)) return kind;
        }
        if (!ShowViewshed && ShowPath && Analysis is { IsVisible: false, Blocking: { } blocking }
            && Near(frame.ToScreen(blocking.LatitudeDeg, blocking.LongitudeDeg), point, 10))
        {
            return Hovered.Blocking;
        }
        return Hovered.None;
    }

    private static bool Near(Point a, Point b, double radius) => Math.Abs(a.X - b.X) <= radius && Math.Abs(a.Y - b.Y) <= radius;

    private Point Clamp(MapFrame frame, Point point)
    {
        var area = frame.Area.Intersect(Viewport);
        return new Point(Math.Clamp(point.X, area.Left, area.Right), Math.Clamp(point.Y, area.Top, area.Bottom));
    }

    private IBrush Resource(string key, IBrush fallback) =>
        this.TryFindResource(key, ActualThemeVariant, out var brush) && brush is IBrush b ? b : fallback;

    private static FormattedText Text(string text, double size, IBrush brush, FontWeight weight = FontWeight.Normal) =>
        new(text, Format.Invariant, FlowDirection.LeftToRight, new Typeface(FontFamily.Default, FontStyle.Normal, weight), size, brush);

    private static FormattedText Wrapped(string text, double size, IBrush brush, double maxWidth)
    {
        var formatted = Text(text, size, brush);
        if (formatted.Width > maxWidth) formatted.MaxTextWidth = maxWidth;
        return formatted;
    }
}
