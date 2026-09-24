using System.Runtime.InteropServices;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Headless;
using Avalonia.Headless.XUnit;
using Avalonia.Input;
using Avalonia.VisualTree;
using TerrainBench.Controls;
using TerrainBench.Engine;
using TerrainBench.ViewModels;
using Xunit;

namespace TerrainBench.UI.Tests;

/// <summary>The window-wide shortcuts, the keyboard on the map and the profile, and what the viewshed layer really paints.</summary>
public class KeyboardAndViewshedPixelTests
{
    [AvaloniaFact]
    public void Ctrl_and_a_number_moves_to_the_map_or_opens_a_panel_and_F5_runs_the_open_panel_only()
    {
        using var app = new Harness();
        app.Click(app.Find<Button>("SamplePromptButton"));
        app.Find<Button>("OpenTileButton").Focus();

        Press(app, Key.D3, RawInputModifiers.Control, PhysicalKey.Digit3);
        Assert.Equal(MainTab.LineOfSight, app.ViewModel.SelectedTab);
        Press(app, Key.F5, RawInputModifiers.None, PhysicalKey.F5);
        Assert.NotNull(app.ViewModel.LineOfSight.Analysis);

        Press(app, Key.D4, RawInputModifiers.Control, PhysicalKey.Digit4);
        Assert.Equal(MainTab.Viewshed, app.ViewModel.SelectedTab);
        app.FieldBox(app.ViewModel.Viewshed.RadiusKm).Text = "2";
        app.Find<Button>("OpenTileButton").Focus();
        Press(app, Key.F5, RawInputModifiers.None, PhysicalKey.F5);
        app.WaitUntil(() => app.ViewModel.Viewshed.HasResult && !app.ViewModel.Viewshed.IsRunning, TimeSpan.FromSeconds(30), "the viewshed");

        // On the About panel F5 does nothing.
        Press(app, Key.D5, RawInputModifiers.Control, PhysicalKey.Digit5);
        Assert.Equal(MainTab.About, app.ViewModel.SelectedTab);
        var map = app.ViewModel.Viewshed.Map;
        var analysis = app.ViewModel.LineOfSight.Analysis;
        Press(app, Key.F5, RawInputModifiers.None, PhysicalKey.F5);
        Assert.Same(map, app.ViewModel.Viewshed.Map);
        Assert.Same(analysis, app.ViewModel.LineOfSight.Analysis);

        Press(app, Key.D2, RawInputModifiers.Control, PhysicalKey.Digit2);
        Assert.Equal(MainTab.Terrain, app.ViewModel.SelectedTab);

        Press(app, Key.D1, RawInputModifiers.Control, PhysicalKey.Digit1);
        Assert.True(app.Find<MapView>("Map").IsFocused);
    }

    [AvaloniaFact]
    public void Ctrl_6_moves_to_the_profile_where_the_keys_read_zoom_and_reset_it()
    {
        using var app = new Harness();
        app.Click(app.Find<Button>("SamplePromptButton"));
        app.SelectTab(MainTab.LineOfSight);
        app.Click(app.Find<Button>("CloseProfileButton"));
        app.Click(app.Find<Button>("CheckButton"));
        var samples = app.ViewModel.LineOfSight.Analysis!.Samples;

        Press(app, Key.D6, RawInputModifiers.Control, PhysicalKey.Digit6);
        var chart = app.Find<ProfileChart>("Chart");
        Assert.True(app.ViewModel.IsProfileOpen);
        Assert.True(chart.IsFocused);

        var map = app.Find<MapView>("Map");
        Press(app, Key.Enter, RawInputModifiers.None, PhysicalKey.Enter);
        Assert.NotNull(map.ProfileHoverLatitude);
        int middle = samples.ToList().FindIndex(s => s.LatitudeDeg == map.ProfileHoverLatitude);
        Assert.True(middle > 10);

        Press(app, Key.Right, RawInputModifiers.None, PhysicalKey.ArrowRight);
        Assert.Equal(samples[middle + 1].LatitudeDeg, map.ProfileHoverLatitude);
        Press(app, Key.Left, RawInputModifiers.Shift, PhysicalKey.ArrowLeft);
        Assert.Equal(samples[middle - 9].LatitudeDeg, map.ProfileHoverLatitude);

        Press(app, Key.Add, RawInputModifiers.None, PhysicalKey.NumPadAdd);
        Assert.True(chart.IsZoomed);
        Press(app, Key.Home, RawInputModifiers.None, PhysicalKey.Home);
        Assert.False(chart.IsZoomed);
        Press(app, Key.Escape, RawInputModifiers.None, PhysicalKey.Escape);
        Assert.Null(map.ProfileHoverLatitude);
    }

    [AvaloniaFact]
    public void Ctrl_and_an_arrow_moves_the_zoomed_map()
    {
        using var app = new Harness();
        app.Click(app.Find<Button>("SamplePromptButton"));
        var map = app.Find<MapView>("Map");
        map.Focus();
        for (int i = 0; i < 4; i++) Press(app, Key.Add, RawInputModifiers.None, PhysicalKey.NumPadAdd);
        double left = map.Frame!.Value.Area.X;

        Press(app, Key.Left, RawInputModifiers.Control, PhysicalKey.ArrowLeft);

        Assert.Equal(left + 80, map.Frame!.Value.Area.X, 0.5);
    }

    [AvaloniaFact]
    public void Cells_beyond_the_tile_edge_are_neither_drawn_nor_counted_and_a_legend_entry_hides_its_cells_at_once()
    {
        using var app = new Harness();
        app.Click(app.Find<Button>("SamplePromptButton"));
        app.SelectTab(MainTab.Viewshed);
        // Near the west edge: the 30 km disc reaches past the tile, where there is no data.
        app.ViewModel.Viewshed.ObserverLatitude.Text = "36.5";
        app.ViewModel.Viewshed.ObserverLongitude.Text = "-111.9";
        app.Settle();
        app.Click(app.Find<Button>("RunViewshedButton"));
        app.WaitUntil(() => app.ViewModel.Viewshed.HasResult && !app.ViewModel.Viewshed.IsRunning, TimeSpan.FromSeconds(60), "the viewshed");

        // The legend counts only what the map draws: every cell with no confident answer lies past the edge
        // here, so it counts none, and the summary says part of the circle lies beyond the tile.
        Assert.Equal("0", app.ViewModel.Viewshed.Legend.Single(r => r.Layer == (int)CellState.Degraded).Count);
        Assert.EndsWith(ViewshedViewModel.BeyondTileSentence, app.ViewModel.Viewshed.Summary);

        // The map leaves them off: beyond the edge is the window's own background.
        var map = app.Find<MapView>("Map");
        var beyondEdge = map.TranslatePoint(map.Frame!.Value.ToScreen(36.5, -112.12), app.Window)!.Value;
        var outside = PixelAt(app, beyondEdge);
        Assert.False(IsPurple(outside), $"Expected nothing drawn beyond the tile's edge, got {outside}.");

        // Inside the tile, hiding the cells out of sight takes their shading off at once.
        var insideTile = map.TranslatePoint(map.Frame!.Value.ToScreen(36.35, -111.85), app.Window)!.Value;
        Assert.Equal(CellState.NotVisible, CellStateAt(app.ViewModel.Viewshed.Map!, 36.35, -111.85));
        var shaded = PixelAt(app, insideTile);
        var legendButton = app.Find<ItemsControl>("ViewshedLegend").GetVisualDescendants().OfType<Button>()
            .First(b => b.DataContext is LegendRow { Layer: (int)CellState.NotVisible });
        app.Click(legendButton);
        var bare = PixelAt(app, insideTile);
        Assert.True(bare.R + bare.G + bare.B > shaded.R + shaded.G + shaded.B + 30, $"Expected the ground brighter once its shading is hidden: {shaded} then {bare}.");
    }

    [AvaloniaFact]
    public void The_minimum_visible_height_paints_cells_by_band_and_a_hidden_band_uncovers_the_ground()
    {
        using var app = new Harness();
        app.Click(app.Find<Button>("SamplePromptButton"));
        app.SelectTab(MainTab.Viewshed);
        // An observer on the north rim: the ground seen nearby, and to the west a wide stretch a
        // target must stand tens to hundreds of metres tall to be seen in.
        app.ViewModel.Viewshed.ObserverLatitude.Text = "36.86361";
        app.ViewModel.Viewshed.ObserverLongitude.Text = "-111.30861";
        app.ViewModel.Viewshed.RadiusKm.Text = "10";
        app.Find<ComboBox>("ViewshedShowChoice").SelectedIndex = 1;
        app.Settle();

        // It answers every target height at once, so the target height field is set aside.
        Assert.False(app.FieldBox(app.ViewModel.Viewshed.TargetHeight).IsEffectivelyEnabled);
        app.Click(app.Find<Button>("RunViewshedButton"));
        app.WaitUntil(() => app.ViewModel.Viewshed.HasResult && !app.ViewModel.Viewshed.IsRunning, TimeSpan.FromSeconds(60), "the minimum visible height");
        var heights = app.ViewModel.Viewshed.Map!;
        Assert.NotNull(heights.HeightsM);
        Assert.Contains(app.ViewModel.Viewshed.Legend, row => row.Name == "Ground Seen (0 m)" && row.Count != "0");

        // At this zoom a 30 m cell is a fraction of a pixel, so look where a wide patch of cells all
        // need 30 m or more: the map paints it in the red end of the legend.
        int[] tall = [4, 5, 6];
        var map = app.Find<MapView>("Map");
        var (lat, lon) = UniformPatch(heights, band => tall.Contains(band));
        var point = map.TranslatePoint(map.Frame!.Value.ToScreen(lat, lon), app.Window)!.Value;
        var painted = PixelAt(app, point);
        Assert.True(painted.R > painted.G + 40 && painted.R > painted.B + 30, $"Expected the red end of the legend, got {painted}.");

        // Hiding those bands in the legend uncovers the terrain under them at once.
        foreach (var button in app.Find<ItemsControl>("ViewshedLegend").GetVisualDescendants().OfType<Button>()
                     .Where(b => b.DataContext is LegendRow row && tall.Contains(row.Layer - Presentation.MapPixels.FirstHeightLayer)).ToList())
        {
            app.Click(button);
        }
        var bare = PixelAt(app, point);
        Assert.False(bare.R > bare.G + 40 && bare.R > bare.B + 30, $"Expected the terrain once those bands are hidden, got {bare}.");
    }

    /// <summary>
    /// The centre of a 51 x 51 block of cells -- 1.5 km at 30 m -- all in bands the test accepts, inside
    /// the drawn circle, north-west of the observer: clear of its marker and label, the ring round the
    /// circle, and the radius label and the elevation legend to the north-east -- so a pixel there shows
    /// only them.
    /// </summary>
    private static (double LatitudeDeg, double LongitudeDeg) UniformPatch(ViewshedMap map, Func<int, bool> accepts)
    {
        const int half = 25;
        double radius = map.ObserverRow, inside = 0.75 * radius, clear = 0.3 * radius;
        for (int row = half; row < map.Rows - half; row += 5)
        {
            for (int col = half; col < map.Cols - half; col += 5)
            {
                int dr = row - map.ObserverRow, dc = col - map.ObserverCol;
                if (dr * dr + dc * dc > inside * inside || dr < clear || dc > -clear) continue;
                bool uniform = true;
                for (int r = row - half; r <= row + half && uniform; r++)
                {
                    for (int c = col - half; c <= col + half && uniform; c++)
                    {
                        uniform = accepts(Presentation.MapPixels.HeightBand(map.HeightAt(r, c)!.Value));
                    }
                }
                if (uniform) return (map.SouthWestCellLatitudeDeg + row * map.SpacingDeg, map.SouthWestCellLongitudeDeg + col * map.ColStepDeg);
            }
        }
        throw new Xunit.Sdk.XunitException("No patch of cells all in the bands asked for.");
    }

    private static bool IsPurple((byte R, byte G, byte B) pixel) => pixel.R > pixel.G + 40 && pixel.B > pixel.G + 40;

    private static CellState CellStateAt(ViewshedMap map, double latitudeDeg, double longitudeDeg)
    {
        int row = (int)Math.Round((latitudeDeg - map.SouthWestCellLatitudeDeg) / map.SpacingDeg);
        int col = (int)Math.Round((longitudeDeg - map.SouthWestCellLongitudeDeg) / map.ColStepDeg);
        return map.At(row, col);
    }

    private static void Press(Harness app, Key key, RawInputModifiers modifiers, PhysicalKey physical)
    {
        app.Window.KeyPress(key, modifiers, physical, null);
        app.Settle();
    }

    private static (byte R, byte G, byte B) PixelAt(Harness app, Point point)
    {
        app.Settle();
        using var frame = app.Window.CaptureRenderedFrame()!;
        using var buffer = frame.Lock();
        int x = (int)Math.Round(point.X), y = (int)Math.Round(point.Y);
        int offset = y * buffer.RowBytes + x * 4;
        byte first = Marshal.ReadByte(buffer.Address, offset);
        byte g = Marshal.ReadByte(buffer.Address, offset + 1);
        byte third = Marshal.ReadByte(buffer.Address, offset + 2);
        // The frame says which way round its channels are.
        return buffer.Format == Avalonia.Platform.PixelFormat.Rgba8888 ? (first, g, third) : (third, g, first);
    }
}
