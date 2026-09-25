using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Media;
using TerrainBench.Engine;
using TerrainBench.Presentation;

namespace TerrainBench.Controls;

/// <summary>
/// The path's profile against distance: terrain (with gaps where data is missing, never zero), the
/// terrain raised by Earth's curvature, the sight line, the first Fresnel zone and the blocking
/// point. Every value plotted comes from the engine's samples; this only lays them out.
/// The mouse wheel zooms along the distance axis towards the pointer, a right-drag moves along it
/// and a double-click shows the whole path again; from the keyboard, the arrow keys, + and −, Home and Esc do
/// the same (see OnKeyDown). Resting the pointer on the chart marks the nearest
/// sample on the terrain and reads its distance and elevation off the axes.
/// </summary>
public sealed class ProfileChart : Control
{
    /// <summary>The fewest samples a zoomed chart still shows.</summary>
    public const int MinVisibleSamples = 20;

    public static readonly StyledProperty<PathAnalysis?> AnalysisProperty = AvaloniaProperty.Register<ProfileChart, PathAnalysis?>(nameof(Analysis));
    public static readonly StyledProperty<bool> ShowTerrainProperty = AvaloniaProperty.Register<ProfileChart, bool>(nameof(ShowTerrain), true);
    public static readonly StyledProperty<bool> ShowCurvatureProperty = AvaloniaProperty.Register<ProfileChart, bool>(nameof(ShowCurvature), true);
    public static readonly StyledProperty<bool> ShowSightLineProperty = AvaloniaProperty.Register<ProfileChart, bool>(nameof(ShowSightLine), true);
    public static readonly StyledProperty<bool> ShowFresnelProperty = AvaloniaProperty.Register<ProfileChart, bool>(nameof(ShowFresnel), true);

    public static readonly DirectProperty<ProfileChart, bool> IsZoomedProperty =
        AvaloniaProperty.RegisterDirect<ProfileChart, bool>(nameof(IsZoomed), c => c.IsZoomed);

    private double _viewStart;
    private double _viewEnd;
    private bool _isZoomed;
    private bool _panning;
    private Point _panStart;
    private (double Start, double End) _panOrigin;
    private int? _hoverIndex;

    static ProfileChart()
    {
        FocusableProperty.OverrideDefaultValue<ProfileChart>(true);
        AffectsRender<ProfileChart>(AnalysisProperty, ShowTerrainProperty, ShowCurvatureProperty, ShowSightLineProperty, ShowFresnelProperty);
    }

    public PathAnalysis? Analysis { get => GetValue(AnalysisProperty); set => SetValue(AnalysisProperty, value); }
    public bool ShowTerrain { get => GetValue(ShowTerrainProperty); set => SetValue(ShowTerrainProperty, value); }
    public bool ShowCurvature { get => GetValue(ShowCurvatureProperty); set => SetValue(ShowCurvatureProperty, value); }
    public bool ShowSightLine { get => GetValue(ShowSightLineProperty); set => SetValue(ShowSightLineProperty, value); }
    public bool ShowFresnel { get => GetValue(ShowFresnelProperty); set => SetValue(ShowFresnelProperty, value); }

    /// <summary>True while only part of the path is shown.</summary>
    public bool IsZoomed
    {
        get => _isZoomed;
        private set => SetAndRaise(IsZoomedProperty, ref _isZoomed, value);
    }

    /// <summary>The sample under the pointer, or null when the pointer is off the chart.</summary>
    public event EventHandler<PathSample?>? HoverChanged;

    /// <summary>The distance range shown, in metres along the path.</summary>
    public (double Start, double End) View => (_viewStart, _viewEnd);

    public void ResetZoom()
    {
        _viewStart = 0;
        _viewEnd = TotalM;
        IsZoomed = false;
        InvalidateVisual();
    }

    /// <summary>Zooms along the distance axis keeping the distance at <paramref name="x"/> in place; a factor above 1 zooms in.</summary>
    public void ZoomAt(double x, double factor)
    {
        if (Analysis is not { Samples.Count: > 1 } analysis) return;
        var plot = PlotArea();
        double span = _viewEnd - _viewStart;
        double minSpan = Math.Min(TotalM, TotalM / (analysis.Samples.Count - 1) * MinVisibleSamples);
        double newSpan = Math.Clamp(span / factor, minSpan, TotalM);
        double anchor = _viewStart + Math.Clamp((x - plot.X) / plot.Width, 0, 1) * span;
        double t = span > 0 ? (anchor - _viewStart) / span : 0.5;
        SetView(anchor - t * newSpan, newSpan);
    }

    protected override void OnPropertyChanged(AvaloniaPropertyChangedEventArgs change)
    {
        base.OnPropertyChanged(change);
        if (change.Property == AnalysisProperty)
        {
            _hoverIndex = null;
            ResetZoom();
        }
    }

    private double TotalM => Math.Max(Analysis?.TotalDistanceM ?? 1, 1);

    private Rect PlotArea() => new(72, 12, Math.Max(Bounds.Width - 92, 10), Math.Max(Bounds.Height - 44, 10));

    private void SetView(double start, double span)
    {
        start = Math.Clamp(start, 0, TotalM - span);
        _viewStart = start;
        _viewEnd = start + span;
        IsZoomed = span < TotalM - 1e-6;
        InvalidateVisual();
    }

    public override void Render(DrawingContext context)
    {
        var muted = Brush("MutedTextBrush", Brushes.Gray);
        var area = new Rect(Bounds.Size);
        context.FillRectangle(Brushes.Transparent, area);

        if (Analysis is not { Samples.Count: > 1 } analysis)
        {
            var hint = Label("Check a line of sight to see its profile here.", 13, muted);
            context.DrawText(hint, new Point((area.Width - hint.Width) / 2, (area.Height - hint.Height) / 2));
            if (IsFocused) context.DrawRectangle(null, new Pen(Brush("AccentBrush", Brushes.SteelBlue), 2), area.Deflate(1));
            return;
        }
        if (_viewEnd <= _viewStart) { _viewStart = 0; _viewEnd = TotalM; }

        var samples = analysis.Samples;
        var (first, last) = VisibleRange(samples);

        // The height axis fits what is in view, whichever lines are switched off, so hiding a line never rescales the chart.
        var values = new List<double>();
        for (int i = first; i <= last; i++)
        {
            var s = samples[i];
            if (s.ElevationM is double e) values.Add(e);
            if (s.CurvatureCorrectedElevationM is double c) values.Add(c);
            if (s.SightLineHeightM is double h)
            {
                values.Add(h);
                if (s.FirstFresnelRadiusM > 0) { values.Add(h - s.FirstFresnelRadiusM); values.Add(h + s.FirstFresnelRadiusM); }
            }
        }
        if (values.Count == 0)
        {
            context.DrawText(Label("This part of the path has no terrain data to plot.", 13, muted), new Point(12, 12));
            return;
        }

        double minY = values.Min(), maxY = values.Max();
        double padY = Math.Max((maxY - minY) * 0.08, 5);
        minY -= padY;
        maxY += padY;

        var plot = PlotArea();
        Point P(double distance, double height) => new(
            plot.X + (distance - _viewStart) / (_viewEnd - _viewStart) * plot.Width,
            plot.Bottom - (height - minY) / (maxY - minY) * plot.Height);

        // Grid and axes
        var grid = new Pen(Brush("ChartGridBrush", Brushes.LightGray), 1);
        for (int i = 0; i <= 4; i++)
        {
            double height = minY + (maxY - minY) * i / 4;
            double y = P(_viewStart, height).Y;
            context.DrawLine(grid, new Point(plot.X, y), new Point(plot.Right, y));
            var tick = Label(Format.WholeMetres(height), 11, muted);
            context.DrawText(tick, new Point(plot.X - tick.Width - 6, y - tick.Height / 2));

            double distance = _viewStart + (_viewEnd - _viewStart) * i / 4;
            double x = P(distance, minY).X;
            var dtick = Label(Format.Distance(distance), 11, muted);
            context.DrawText(dtick, new Point(Math.Clamp(x - dtick.Width / 2, plot.X - 20, plot.Right - dtick.Width + 20), plot.Bottom + 4));
        }

        // What every height on the chart is measured from: the engine's datum, whatever the heights were typed in.
        context.DrawText(Label($"Heights in m {PlainWords.DatumWords(analysis.HeightsDatum)}", 11, muted), new Point(plot.X + 6, plot.Y + 2));

        using (context.PushClip(plot))
        {
            // Fresnel zone around the sight line
            if (ShowFresnel && analysis.Fresnel is not null && samples.Any(s => s.FirstFresnelRadiusM > 0 && s.SightLineHeightM is not null))
            {
                var withLine = samples.Where(s => s.SightLineHeightM is not null).ToList();
                var zone = new StreamGeometry();
                using (var g = zone.Open())
                {
                    g.BeginFigure(P(withLine[0].DistanceM, withLine[0].SightLineHeightM!.Value + withLine[0].FirstFresnelRadiusM), true);
                    foreach (var s in withLine.Skip(1)) g.LineTo(P(s.DistanceM, s.SightLineHeightM!.Value + s.FirstFresnelRadiusM));
                    foreach (var s in Enumerable.Reverse(withLine)) g.LineTo(P(s.DistanceM, s.SightLineHeightM!.Value - s.FirstFresnelRadiusM));
                    g.EndFigure(true);
                }
                context.DrawGeometry(Brush("ChartFresnelBrush", Brushes.LightBlue), null, zone);
            }

            // Terrain and curvature-corrected terrain, broken at every void
            if (ShowTerrain)
            {
                DrawRuns(context, samples, first, last, s => s.ElevationM, P, new Pen(Brush("ChartTerrainBrush", Brushes.Brown), 2), fill: Brush("ChartTerrainFillBrush", null), baseline: minY);
            }
            if (ShowCurvature)
            {
                DrawRuns(context, samples, first, last, s => s.CurvatureCorrectedElevationM, P, new Pen(Brush("ChartCorrectedBrush", Brushes.Tan), 1.5, dashStyle: DashStyle.Dash), fill: null, baseline: minY);
            }

            // Sight line
            var from = samples.FirstOrDefault(s => s.SightLineHeightM is not null);
            var to = samples.LastOrDefault(s => s.SightLineHeightM is not null);
            if (ShowSightLine && from is not null && to is not null)
            {
                context.DrawLine(new Pen(Brush("ChartSightLineBrush", Brushes.Blue), 2), P(from.DistanceM, from.SightLineHeightM!.Value), P(to.DistanceM, to.SightLineHeightM!.Value));
            }

            // Blocking point: always shown, it is the answer.
            if (analysis.Blocking is { } blocking && !analysis.IsVisible && blocking.SampleIndex >= 0 && samples[blocking.SampleIndex].CurvatureCorrectedElevationM is double corrected)
            {
                var at = P(blocking.DistanceM, corrected);
                var pen = new Pen(Brush("ChartBlockingBrush", Brushes.Red), 3);
                context.DrawLine(pen, at + new Vector(-6, -6), at + new Vector(6, 6));
                context.DrawLine(pen, at + new Vector(-6, 6), at + new Vector(6, -6));
            }
        }

        if (_hoverIndex is int index && index >= first && index <= last) DrawHover(context, samples[index], plot, P);

        if (IsFocused)
        {
            context.DrawRectangle(null, new Pen(Brush("AccentBrush", Brushes.SteelBlue), 2), area.Deflate(1));
        }
    }

    /// <summary>A point on the terrain with dashed lines to both axes, and its distance and elevation read off them.</summary>
    private void DrawHover(DrawingContext context, PathSample sample, Rect plot, Func<double, double, Point> toPoint)
    {
        var accent = Brush("AccentBrush", Brushes.SteelBlue);
        var onAccent = Brush("OnAccentBrush", Brushes.White);
        var dash = new Pen(Brush("BodyTextBrush", Brushes.Gray), 1, new DashStyle([4, 3], 0));
        double x = toPoint(sample.DistanceM, 0).X;

        if (sample.ElevationM is double elevation)
        {
            var at = toPoint(sample.DistanceM, elevation);
            context.DrawLine(dash, new Point(plot.X, at.Y), at);
            context.DrawLine(dash, at, new Point(x, plot.Bottom));
            context.DrawEllipse(onAccent, new Pen(accent, 2.5), at, 5, 5);
            AxisLabel(context, Format.Metres(elevation), new Point(plot.X - 4, at.Y), alignRight: true, accent, onAccent);
        }
        else
        {
            context.DrawLine(dash, new Point(x, plot.Y), new Point(x, plot.Bottom));
            AxisLabel(context, "no data", new Point(plot.X - 4, plot.Y + 10), alignRight: true, accent, onAccent);
        }
        AxisLabel(context, Format.Distance(sample.DistanceM), new Point(x, plot.Bottom + 12), alignRight: false, accent, onAccent);
    }

    private static void AxisLabel(DrawingContext context, string text, Point anchor, bool alignRight, IBrush background, IBrush foreground)
    {
        var label = new FormattedText(text, Format.Invariant, FlowDirection.LeftToRight, new Typeface(FontFamily.Default, FontStyle.Normal, FontWeight.SemiBold), 11, foreground);
        double width = label.Width + 12, height = label.Height + 4;
        var box = alignRight
            ? new Rect(anchor.X - width, anchor.Y - height / 2, width, height)
            : new Rect(anchor.X - width / 2, anchor.Y - height / 2, width, height);
        context.FillRectangle(background, box, 4);
        context.DrawText(label, new Point(box.X + 6, box.Y + 2));
    }

    /// <summary>
    /// With the chart focused: Enter or the arrow keys show the reading cursor and move it a sample at
    /// a time (Shift for ten), + and − zoom at the cursor, Home shows the whole path, Esc hides the cursor.
    /// </summary>
    protected override void OnKeyDown(KeyEventArgs e)
    {
        base.OnKeyDown(e);
        if (Analysis is not { Samples.Count: > 1 } analysis) return;
        var samples = analysis.Samples;
        int centre = NearestSample(samples, (_viewStart + _viewEnd) / 2);

        switch (e.Key)
        {
            case Key.Enter:
                if (_hoverIndex is null) SetHover(centre);
                break;
            case Key.Left:
            case Key.Right:
                int step = (e.KeyModifiers.HasFlag(KeyModifiers.Shift) ? 10 : 1) * (e.Key == Key.Left ? -1 : 1);
                int index = _hoverIndex is int current ? Math.Clamp(current + step, 0, samples.Count - 1) : centre;
                SetHover(index);
                KeepInView(samples[index].DistanceM);
                break;
            case Key.Add:
            case Key.OemPlus:
                ZoomAtDistance(samples[_hoverIndex ?? centre].DistanceM, MapView.ButtonStep);
                break;
            case Key.Subtract:
            case Key.OemMinus:
                ZoomAtDistance(samples[_hoverIndex ?? centre].DistanceM, 1 / MapView.ButtonStep);
                break;
            case Key.Home:
                ResetZoom();
                break;
            case Key.Escape when _hoverIndex is not null:
                SetHover(null);
                break;
            default:
                return;
        }
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

    private void ZoomAtDistance(double distanceM, double factor)
    {
        var plot = PlotArea();
        ZoomAt(plot.X + (distanceM - _viewStart) / (_viewEnd - _viewStart) * plot.Width, factor);
    }

    /// <summary>A zoomed chart moves so the keyboard cursor stays in view.</summary>
    private void KeepInView(double distanceM)
    {
        double span = _viewEnd - _viewStart;
        if (distanceM < _viewStart || distanceM > _viewEnd) SetView(distanceM - span / 2, span);
    }

    protected override void OnPointerWheelChanged(PointerWheelEventArgs e)
    {
        base.OnPointerWheelChanged(e);
        if (Analysis is null || e.Delta.Y == 0) return;
        ZoomAt(e.GetPosition(this).X, Math.Pow(MapView.WheelStep, e.Delta.Y));
        UpdateHover(e.GetPosition(this));
        e.Handled = true;
    }

    protected override void OnPointerPressed(PointerPressedEventArgs e)
    {
        base.OnPointerPressed(e);
        Focus();
        if (Analysis is null) return;
        var properties = e.GetCurrentPoint(this).Properties;
        if (properties.IsLeftButtonPressed && e.ClickCount == 2)
        {
            ResetZoom();
            e.Handled = true;
        }
        else if (properties.IsRightButtonPressed)
        {
            _panning = true;
            _panStart = e.GetPosition(this);
            _panOrigin = (_viewStart, _viewEnd);
            Cursor = new Cursor(StandardCursorType.SizeWestEast);
            e.Pointer.Capture(this);
            e.Handled = true;
        }
    }

    protected override void OnPointerMoved(PointerEventArgs e)
    {
        base.OnPointerMoved(e);
        var point = e.GetPosition(this);
        if (_panning)
        {
            double span = _panOrigin.End - _panOrigin.Start;
            double metresPerPixel = span / PlotArea().Width;
            SetView(_panOrigin.Start - (point.X - _panStart.X) * metresPerPixel, span);
        }
        UpdateHover(point);
    }

    protected override void OnPointerReleased(PointerReleasedEventArgs e)
    {
        base.OnPointerReleased(e);
        if (!_panning) return;
        _panning = false;
        Cursor = null;
        e.Pointer.Capture(null);
        e.Handled = true;
    }

    protected override void OnPointerExited(PointerEventArgs e)
    {
        base.OnPointerExited(e);
        SetHover(null);
    }

    private void UpdateHover(Point point)
    {
        var plot = PlotArea();
        if (Analysis is not { Samples.Count: > 1 } analysis || !plot.Inflate(new Thickness(0, 0, 0, 0)).Contains(point))
        {
            SetHover(null);
            return;
        }

        double distance = _viewStart + (point.X - plot.X) / plot.Width * (_viewEnd - _viewStart);
        SetHover(NearestSample(analysis.Samples, distance));
    }

    private void SetHover(int? index)
    {
        if (index == _hoverIndex) return;
        _hoverIndex = index;
        InvalidateVisual();
        HoverChanged?.Invoke(this, index is int i && Analysis is { } analysis ? analysis.Samples[i] : null);
    }

    private static int NearestSample(IReadOnlyList<PathSample> samples, double distance)
    {
        int lo = 0, hi = samples.Count - 1;
        while (lo < hi)
        {
            int mid = (lo + hi) / 2;
            if (samples[mid].DistanceM < distance) lo = mid + 1;
            else hi = mid;
        }
        if (lo > 0 && Math.Abs(samples[lo - 1].DistanceM - distance) <= Math.Abs(samples[lo].DistanceM - distance)) lo--;
        return lo;
    }

    /// <summary>The samples in view, plus one on each side so lines run to the plot's edges.</summary>
    private (int First, int Last) VisibleRange(IReadOnlyList<PathSample> samples) =>
        (Math.Max(0, NearestSample(samples, _viewStart) - 1), Math.Min(samples.Count - 1, NearestSample(samples, _viewEnd) + 1));

    private static void DrawRuns(DrawingContext context, IReadOnlyList<PathSample> samples, int first, int last, Func<PathSample, double?> value, Func<double, double, Point> toPoint, Pen pen, IBrush? fill, double baseline)
    {
        int i = first;
        while (i <= last)
        {
            while (i <= last && value(samples[i]) is null) i++;
            int start = i;
            while (i <= last && value(samples[i]) is not null) i++;
            if (i - start < 1) continue;

            var run = new List<PathSample>(i - start);
            for (int j = start; j < i; j++) run.Add(samples[j]);
            if (fill is not null && run.Count > 1)
            {
                var area = new StreamGeometry();
                using (var g = area.Open())
                {
                    g.BeginFigure(toPoint(run[0].DistanceM, baseline), true);
                    foreach (var s in run) g.LineTo(toPoint(s.DistanceM, value(s)!.Value));
                    g.LineTo(toPoint(run[^1].DistanceM, baseline));
                    g.EndFigure(true);
                }
                context.DrawGeometry(fill, null, area);
            }

            var line = new StreamGeometry();
            using (var g = line.Open())
            {
                g.BeginFigure(toPoint(run[0].DistanceM, value(run[0])!.Value), false);
                foreach (var s in run.Skip(1)) g.LineTo(toPoint(s.DistanceM, value(s)!.Value));
                g.EndFigure(false);
            }
            context.DrawGeometry(null, pen, line);
        }
    }

    private IBrush Brush(string key, IBrush? fallback) =>
        this.TryFindResource(key, ActualThemeVariant, out var found) && found is IBrush brush ? brush : fallback ?? Brushes.Transparent;

    private static FormattedText Label(string text, double size, IBrush brush) =>
        new(text, Format.Invariant, FlowDirection.LeftToRight, Typeface.Default, size, brush);
}
