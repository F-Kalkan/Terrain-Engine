using Avalonia;
using Avalonia.Automation;
using Avalonia.Controls;
using Avalonia.Headless;
using Avalonia.Headless.XUnit;
using Avalonia.Input;
using Avalonia.Media;
using Avalonia.Styling;
using Avalonia.VisualTree;
using TerrainBench.Controls;
using TerrainBench.ViewModels;
using Xunit;

namespace TerrainBench.UI.Tests;

public class AccessibilityTests
{
    [AvaloniaFact]
    public void Every_control_on_every_tab_can_be_reached_by_keyboard_and_says_what_it_is()
    {
        using var app = new Harness();
        app.Click(app.Find<Button>("SamplePromptButton"));

        foreach (var tab in Enum.GetValues<MainTab>())
        {
            app.SelectTab(tab);
            // The app's own controls; parts inside a control's template (a scroll bar's arrow
            // buttons, say) belong to that control and are reached through it.
            var interactive = app.Window.GetVisualDescendants().OfType<Control>()
                .Where(c => c.IsEffectivelyVisible && c.TemplatedParent is null && c is Button or TextBox or ComboBox or CheckBox or MapView)
                .ToList();

            Assert.NotEmpty(interactive);
            foreach (var control in interactive)
            {
                Assert.True(control.Focusable, $"{Describe(control)} on {tab} can't take keyboard focus.");
                Assert.True(control is not InputElement input || input.IsTabStop, $"{Describe(control)} on {tab} is skipped by Tab.");

                switch (control)
                {
                    case Button button:
                        Assert.True(button.Content is string { Length: > 0 } || ToolTip.GetTip(button) is string { Length: > 0 },
                            $"A button on {tab} has neither text nor a tooltip.");
                        break;
                    case TextBox or ComboBox or MapView:
                        Assert.False(string.IsNullOrWhiteSpace(AutomationProperties.GetName(control)), $"{Describe(control)} on {tab} has no accessible name.");
                        break;
                }
            }
        }
    }

    [AvaloniaFact]
    public void Tab_walks_from_the_toolbar_into_the_line_of_sight_fields_and_the_map_answers_arrows_and_enter()
    {
        using var app = new Harness();
        app.Click(app.Find<Button>("SamplePromptButton"));
        app.SelectTab(MainTab.LineOfSight);

        var reached = new HashSet<Control>();
        app.Find<Button>("OpenTileButton").Focus();
        for (int i = 0; i < 60; i++)
        {
            if (app.Window.FocusManager?.GetFocusedElement() is Control focused) reached.Add(focused);
            app.Window.KeyPress(Key.Tab, RawInputModifiers.None, PhysicalKey.Tab, null);
            app.Settle();
        }

        Assert.Contains(app.Find<MapView>("Map"), reached);
        Assert.Contains(app.FieldBox(app.ViewModel.LineOfSight.ObserverLatitude), reached);
        Assert.Contains(app.Find<Button>("CheckButton"), reached);

        // On the focused map: move the crosshair and place the observer with Enter.
        var map = app.Find<MapView>("Map");
        map.Focus();
        app.Window.KeyPress(Key.Up, RawInputModifiers.None, PhysicalKey.ArrowUp, null);
        app.Window.KeyPress(Key.Enter, RawInputModifiers.None, PhysicalKey.Enter, null);
        app.Settle();

        Assert.Equal(36.505, app.ViewModel.LineOfSight.ObserverLatitude.Value!.Value, 1e-6);
        Assert.Equal(-111.5, app.ViewModel.LineOfSight.ObserverLongitude.Value!.Value, 1e-6);
    }

    [AvaloniaTheory]
    [InlineData("Light")]
    [InlineData("Dark")]
    public void Every_text_colour_reads_at_4_5_to_1_or_better_in_both_themes(string variant)
    {
        var theme = variant == "Light" ? ThemeVariant.Light : ThemeVariant.Dark;
        var app = Application.Current!;

        Color Resolve(string key)
        {
            Assert.True(app.TryGetResource(key, theme, out var value), $"{key} isn't defined for {variant}.");
            return ((ISolidColorBrush)value!).Color;
        }

        string[] backgrounds =
        [
            "WindowBackgroundBrush", "PanelBackgroundBrush", "PanelHeaderBrush", "CardBrush", "StatusBackgroundBrush", "BannerBackgroundBrush",
            "VisibleCardBrush", "BlockedCardBrush", "NoAnswerCardBrush",
        ];
        string[] texts =
        [
            "BodyTextBrush", "MutedTextBrush", "HeadingTextBrush", "ErrorTextBrush", "WarningTextBrush",
            "VisibleTextBrush", "BlockedTextBrush", "NoAnswerTextBrush",
        ];

        var pairs = backgrounds.SelectMany(background => texts.Select(text => (text, background))).ToList();
        // The navy frame, and the accent buttons.
        pairs.Add(("ToolbarForegroundBrush", "ToolbarBackgroundBrush"));
        pairs.Add(("ToolbarForegroundBrush", "ToolbarButtonBrush"));
        pairs.Add(("ToolbarForegroundBrush", "ToolbarButtonHoverBrush"));
        pairs.Add(("ToolbarForegroundBrush", "AccentBrush"));
        pairs.Add(("OnAccentBrush", "AccentBrush"));
        pairs.Add(("OnAccentBrush", "AccentHoverBrush"));
        // The verdict badge's glyph is drawn in the card colour on the verdict colour.
        pairs.Add(("VisibleCardBrush", "VisibleTextBrush"));
        pairs.Add(("BlockedCardBrush", "BlockedTextBrush"));
        pairs.Add(("NoAnswerCardBrush", "NoAnswerTextBrush"));

        foreach (var (text, background) in pairs)
        {
            double ratio = Contrast(Resolve(text), Resolve(background));
            Assert.True(ratio >= 4.5, $"{text} on {background} in {variant} is {ratio:0.00}:1.");
        }
    }

    [AvaloniaFact]
    public void Screenshots_in_both_themes_when_asked_for()
    {
        // Set TERRAINBENCH_SCREENSHOTS to a folder to write the README's screenshots.
        var folder = Environment.GetEnvironmentVariable("TERRAINBENCH_SCREENSHOTS");
        if (string.IsNullOrWhiteSpace(folder)) return;
        Directory.CreateDirectory(folder);

        foreach (var (name, theme) in new[] { ("light", ThemeVariant.Light), ("dark", ThemeVariant.Dark) })
        {
            Application.Current!.RequestedThemeVariant = theme;
            using var app = new Harness();
            app.Click(app.Find<Button>("SamplePromptButton"));
            app.SelectTab(MainTab.LineOfSight);
            app.ViewModel.LineOfSight.Frequency.Text = "2400";
            app.Click(app.Find<Button>("CheckButton"));
            // Scroll to the bottom of the answer so the whole result card set is on screen.
            app.Find<StackPanel>("LineOfSightResult").BringIntoView();
            app.Settle();
            app.Window.CaptureRenderedFrame()!.Save(Path.Combine(folder, $"line-of-sight-{name}.png"), Avalonia.Media.Imaging.PngBitmapEncoderOptions.Default);

            app.Find<ScrollViewer>("LineOfSightPanel").Offset = default;
            app.Settle();
            app.Window.CaptureRenderedFrame()!.Save(Path.Combine(folder, $"line-of-sight-fields-{name}.png"), Avalonia.Media.Imaging.PngBitmapEncoderOptions.Default);

            var chart = app.Find<ProfileChart>("Chart");
            chart.ZoomAt(chart.Bounds.Width * 0.55, 3);
            app.Window.MouseMove(chart.TranslatePoint(new Point(chart.Bounds.Width * 0.55, chart.Bounds.Height / 2), app.Window)!.Value);
            app.Settle();
            app.Window.CaptureRenderedFrame()!.Save(Path.Combine(folder, $"profile-hover-{name}.png"), Avalonia.Media.Imaging.PngBitmapEncoderOptions.Default);
            chart.ResetZoom();

            // Zoomed in on the path, with the pointer resting on the blocking point.
            var map = app.Find<MapView>("Map");
            var blocking = app.ViewModel.LineOfSight.Analysis!.Blocking!;
            map.ZoomAt(map.Frame!.Value.ToScreen(blocking.LatitudeDeg, blocking.LongitudeDeg), 6);
            app.Settle();
            var hover = map.TranslatePoint(map.Frame!.Value.ToScreen(blocking.LatitudeDeg, blocking.LongitudeDeg), app.Window)!.Value;
            app.Window.MouseMove(hover);
            app.Settle();
            app.Window.CaptureRenderedFrame()!.Save(Path.Combine(folder, $"map-zoom-hover-{name}.png"), Avalonia.Media.Imaging.PngBitmapEncoderOptions.Default);
            app.Window.MouseMove(new Point(2, 2));
            map.ZoomAt(map.Frame!.Value.Area.Center, 1);

            app.SelectTab(MainTab.About);
            app.Settle();
            app.Window.CaptureRenderedFrame()!.Save(Path.Combine(folder, $"about-{name}.png"), Avalonia.Media.Imaging.PngBitmapEncoderOptions.Default);

            app.SelectTab(MainTab.Viewshed);
            app.FieldBox(app.ViewModel.Viewshed.RadiusKm).Text = "30";
            app.ViewModel.Viewshed.Comparison = ViewshedComparison.PreviousRun;
            app.Click(app.Find<Button>("RunViewshedButton"));
            app.WaitUntil(() => app.ViewModel.Viewshed.HasResult && !app.ViewModel.Viewshed.IsRunning, TimeSpan.FromSeconds(60), "the viewshed");
            app.Find<TextBlock>("ViewshedSummary").BringIntoView();
            app.Settle();
            app.Window.CaptureRenderedFrame()!.Save(Path.Combine(folder, $"viewshed-{name}.png"), Avalonia.Media.Imaging.PngBitmapEncoderOptions.Default);

            // The same viewshed with curvature switched off, compared with the run above.
            app.FieldBox(app.ViewModel.Viewshed.RefractionK).Text = "1e12";
            app.Click(app.Find<Button>("RunViewshedButton"));
            app.WaitUntil(() => app.ViewModel.Viewshed.Highlights is not null && !app.ViewModel.Viewshed.IsRunning, TimeSpan.FromSeconds(60), "the second viewshed");
            app.Find<TextBlock>("ViewshedSummary").BringIntoView();
            app.Settle();
            app.Window.CaptureRenderedFrame()!.Save(Path.Combine(folder, $"viewshed-k-change-{name}.png"), Avalonia.Media.Imaging.PngBitmapEncoderOptions.Default);

            // Near the tile's west edge, where part of the disc has no data to answer from.
            app.FieldBox(app.ViewModel.Viewshed.RefractionK).Text = "4/3";
            app.ViewModel.Viewshed.Comparison = ViewshedComparison.None;
            app.ViewModel.Viewshed.ObserverLongitude.Text = "-111.9";
            app.Click(app.Find<Button>("RunViewshedButton"));
            app.WaitUntil(() => app.ViewModel.Viewshed.HasResult && !app.ViewModel.Viewshed.IsRunning && !app.ViewModel.Viewshed.IsObserverMoved, TimeSpan.FromSeconds(60), "the edge viewshed");
            app.Find<ItemsControl>("ViewshedLegend").BringIntoView();
            app.Settle();
            app.Window.CaptureRenderedFrame()!.Save(Path.Combine(folder, $"viewshed-edge-{name}.png"), Avalonia.Media.Imaging.PngBitmapEncoderOptions.Default);
        }
        Application.Current!.RequestedThemeVariant = ThemeVariant.Default;
    }

    private static string Describe(Control control) => $"{control.GetType().Name} '{control.Name ?? AutomationProperties.GetName(control) ?? (control as ContentControl)?.Content?.ToString()}'";

    private static double Contrast(Color a, Color b)
    {
        double la = Luminance(a), lb = Luminance(b);
        return (Math.Max(la, lb) + 0.05) / (Math.Min(la, lb) + 0.05);
    }

    private static double Luminance(Color c)
    {
        static double Channel(byte v)
        {
            double s = v / 255.0;
            return s <= 0.03928 ? s / 12.92 : Math.Pow((s + 0.055) / 1.055, 2.4);
        }
        return 0.2126 * Channel(c.R) + 0.7152 * Channel(c.G) + 0.0722 * Channel(c.B);
    }
}
