using Avalonia;
using Avalonia.Controls;
using Avalonia.Headless;
using Avalonia.Headless.XUnit;
using Avalonia.Input;
using TerrainBench.Controls;
using TerrainBench.ViewModels;
using Xunit;
using ShapePath = Avalonia.Controls.Shapes.Path;

namespace TerrainBench.UI.Tests;

/// <summary>The first-run tour: it points at the real controls, and using them walks it.</summary>
public class TourTests
{
    [AvaloniaFact]
    public void The_tour_is_walked_by_using_what_each_page_points_at()
    {
        using var app = new Harness(tour: true);
        Assert.True(app.Find<Canvas>("TourOverlay").IsEffectivelyVisible);
        Assert.Equal("Welcome to TerrainBench", app.Find<TextBlock>("TourTitle").Text);
        Assert.False(app.Find<ShapePath>("TourArrow").IsVisible);
        Assert.True(app.Find<Button>("TourNextButton").IsFocused);
        app.Click(app.Find<Button>("TourNextButton"));

        // Open a tile: the ring and the arrow are on the sample prompt, and only it takes a click.
        AssertPointsAt(app, "SamplePromptButton");
        Assert.False(app.Find<Button>("TourNextButton").IsVisible);
        ClickThrough(app, app.Find<Button>("OpenSampleButton"));
        Assert.False(app.ViewModel.Terrain.IsLoaded);
        ClickThrough(app, app.Find<Button>("SamplePromptButton"));
        Assert.True(app.ViewModel.Terrain.IsLoaded);

        // The observer, then the target, by clicks on the map below the card.
        AssertPointsAt(app, "Map");
        ClickMap(app, 36.25, -111.7);
        Assert.Equal(36.25, app.ViewModel.LineOfSight.ObserverLatitude.Value!.Value, 0.002);
        Assert.Equal(TourGoal.TargetPlaced, app.ViewModel.Tour.Current.Goal);
        ClickMap(app, 36.3, -111.4, RawInputModifiers.Control);
        Assert.Equal(36.3, app.ViewModel.LineOfSight.TargetLatitude.Value!.Value, 0.002);

        AssertPointsAt(app, "LineOfSightResult");
        app.Click(app.Find<Button>("TourNextButton"));

        AssertPointsAt(app, "ViewshedTabButton");
        ClickThrough(app, app.Find<Button>("ViewshedTabButton"));
        AssertPointsAt(app, "RunViewshedButton");
        ClickThrough(app, app.Find<Button>("RunViewshedButton"));
        app.WaitUntil(() => app.ViewModel.Viewshed.HasResult && !app.ViewModel.Viewshed.IsRunning, TimeSpan.FromSeconds(60), "the viewshed");

        AssertPointsAt(app, "ViewshedLegend");
        app.Click(app.Find<Button>("TourNextButton"));
        AssertPointsAt(app, "AboutTabButton");
        ClickThrough(app, app.Find<Button>("AboutTabButton"));

        Assert.False(app.ViewModel.Tour.IsOpen);
        Assert.False(app.Find<Canvas>("TourOverlay").IsVisible);
        Assert.True(app.Find<MapView>("Map").IsFocused);
        app.ViewModel.SaveSettings();
        Assert.True(app.Settings.Saved!.TourSeen);
    }

    [AvaloniaFact]
    public void The_keyboard_alone_walks_the_tour_and_escape_ends_it()
    {
        using var app = new Harness(tour: true);

        Press(app, Key.Enter);
        Assert.True(app.Find<Button>("SamplePromptButton").IsFocused);
        Press(app, Key.Enter);
        Assert.True(app.ViewModel.Terrain.IsLoaded);

        Assert.True(app.Find<MapView>("Map").IsFocused);
        Press(app, Key.Enter);
        Assert.Equal(TourGoal.TargetPlaced, app.ViewModel.Tour.Current.Goal);
        Press(app, Key.Right);
        Press(app, Key.Right);
        Press(app, Key.Enter, RawInputModifiers.Control);
        Assert.Equal("LineOfSightResult", app.ViewModel.Tour.Current.Target);
        Assert.True(app.Find<Button>("TourNextButton").IsFocused);

        Press(app, Key.Escape);
        Assert.False(app.ViewModel.Tour.IsOpen);
    }

    [AvaloniaFact]
    public void Show_the_tour_again_skips_opening_a_tile_when_one_is_open()
    {
        using var app = new Harness();
        Assert.False(app.ViewModel.Tour.IsOpen);
        app.Click(app.Find<Button>("SamplePromptButton"));

        app.SelectTab(MainTab.About);
        app.Click(app.Find<Button>("ShowTourButton"));
        Assert.Equal("Welcome to TerrainBench", app.Find<TextBlock>("TourTitle").Text);
        app.Click(app.Find<Button>("TourNextButton"));

        Assert.Equal(TourGoal.ObserverPlaced, app.ViewModel.Tour.Current.Goal);
        AssertPointsAt(app, "Map");
    }

    [Fact]
    public void The_card_goes_beside_a_control_with_room_for_its_arrow_and_inside_one_too_big_to_go_round()
    {
        var window = new Size(1360, 880);
        var card = new Size(400, 220);

        // A button on the right: the card to its left, the arrow from the card to the button.
        var right = new Rect(1200, 400, 120, 40);
        var beside = TourPlacement.Place(window, card, right);
        Assert.Equal(right.Left - TourPlacement.Gap - card.Width, beside.Card.X);
        Assert.True(beside.ArrowTo!.Value.X < right.Left && beside.ArrowTo.Value.X > beside.ArrowFrom!.Value.X);
        Assert.InRange(beside.ArrowTo.Value.Y, right.Top, right.Bottom);

        // A tall result card on the right: the card lines up with its top rather than its middle.
        var tall = new Rect(940, 290, 400, 540);
        var high = TourPlacement.Place(window, card, tall);
        Assert.Equal(tall.Top, high.Card.Y);
        Assert.InRange(high.ArrowTo!.Value.Y, tall.Top, tall.Bottom);

        // A button near the top left: the card to its right.
        var left = new Rect(20, 60, 160, 36);
        var other = TourPlacement.Place(window, card, left);
        Assert.Equal(left.Right + TourPlacement.Gap, other.Card.X);
        Assert.True(other.Card.Y >= 0);

        // The map, nearly the whole window: inside it, pointing down into it.
        var map = new Rect(0, 60, 900, 700);
        var inside = TourPlacement.Place(window, card, map);
        Assert.True(map.Contains(new Rect(inside.Card, card)));
        Assert.True(inside.ArrowTo!.Value.Y > inside.ArrowFrom!.Value.Y);
        Assert.True(map.Contains(inside.ArrowTo.Value));

        // Nothing to point at: in the middle, with no arrow.
        var middle = TourPlacement.Place(window, card, null);
        Assert.Equal(new Point(480, 330), middle.Card);
        Assert.Null(middle.ArrowFrom);
    }

    /// <summary>The ring is round the control, the arrow ends at it, and the card is on screen and clear of it unless it had to go inside.</summary>
    private static void AssertPointsAt(Harness app, string name)
    {
        app.Settle();
        Assert.Equal(name, app.ViewModel.Tour.Current.Target);
        var overlay = app.Find<Canvas>("TourOverlay");
        var target = app.Find<Control>(name);
        var bounds = new Rect(target.TranslatePoint(default, overlay)!.Value, target.Bounds.Size);

        var ring = app.Find<Border>("TourRing");
        var ringBounds = new Rect(Canvas.GetLeft(ring), Canvas.GetTop(ring), ring.Width, ring.Height);
        Assert.True(ring.IsVisible);
        Assert.True(ringBounds.Contains(bounds.Intersect(new Rect(overlay.Bounds.Size))), $"The ring {ringBounds} misses {name} at {bounds}.");

        var arrow = app.Find<ShapePath>("TourArrow");
        Assert.True(arrow.IsVisible);
        Assert.True(ringBounds.Inflate(8).Intersects(arrow.Data!.Bounds), $"The arrow {arrow.Data!.Bounds} doesn't reach {name}.");

        var card = app.Find<Border>("TourCard");
        var cardBounds = new Rect(Canvas.GetLeft(card), Canvas.GetTop(card), card.Bounds.Width, card.Bounds.Height);
        Assert.True(new Rect(overlay.Bounds.Size).Contains(cardBounds), $"The card {cardBounds} is off screen.");
        if (name != "Map") Assert.False(cardBounds.Intersects(bounds), $"The card {cardBounds} covers {name}.");
    }

    /// <summary>A real click at a control's middle: it reaches the control only through the gap in the dimming.</summary>
    private static void ClickThrough(Harness app, Control control)
    {
        var centre = control.TranslatePoint(new Point(control.Bounds.Width / 2, control.Bounds.Height / 2), app.Window)!.Value;
        app.Window.MouseMove(centre);
        app.Window.MouseDown(centre, MouseButton.Left);
        app.Window.MouseUp(centre, MouseButton.Left);
        app.Settle();
    }

    private static void ClickMap(Harness app, double lat, double lon, RawInputModifiers modifiers = RawInputModifiers.None)
    {
        var map = app.Find<MapView>("Map");
        var point = map.TranslatePoint(map.Frame!.Value.ToScreen(lat, lon), app.Window)!.Value;
        app.Window.MouseMove(point);
        app.Window.MouseDown(point, MouseButton.Left, modifiers);
        app.Window.MouseUp(point, MouseButton.Left, modifiers);
        app.Settle();
    }

    private static void Press(Harness app, Key key, RawInputModifiers modifiers = RawInputModifiers.None)
    {
        var physical = key switch
        {
            Key.Enter => PhysicalKey.Enter,
            Key.Escape => PhysicalKey.Escape,
            Key.Right => PhysicalKey.ArrowRight,
            _ => PhysicalKey.None,
        };
        app.Window.KeyPress(key, modifiers, physical, null);
        app.Settle();
    }
}
