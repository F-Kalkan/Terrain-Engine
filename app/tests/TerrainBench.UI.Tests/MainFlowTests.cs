using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.Primitives;
using Avalonia.Headless;
using Avalonia.Headless.XUnit;
using Avalonia.Input;
using Avalonia.VisualTree;
using TerrainBench.Controls;
using TerrainBench.ViewModels;
using Xunit;

namespace TerrainBench.UI.Tests;

/// <summary>The main flows, clicked through in a real window over the real engine.</summary>
public class MainFlowTests
{
    [AvaloniaFact]
    public void A_first_start_offers_the_sample_tile_and_one_click_opens_it()
    {
        using var app = new Harness();

        var prompt = app.Find<Button>("SamplePromptButton");
        app.Click(prompt);

        Assert.True(app.ViewModel.Terrain.IsLoaded);
        Assert.False(prompt.IsEffectivelyVisible);
        Assert.NotNull(app.Find<MapView>("Map").Posts);
        Assert.Equal("1,201 × 1,201 posts", app.ViewModel.Terrain.GridText);
        Assert.Equal("Opened N36W112.hgt.", app.Find<TextBlock>("StatusText").Text);
    }

    [AvaloniaFact]
    public void Checking_the_readme_example_shows_blocked_with_the_reason_and_the_chart()
    {
        using var app = new Harness();
        app.Click(app.Find<Button>("SamplePromptButton"));
        app.SelectTab(MainTab.LineOfSight);

        app.Click(app.Find<Button>("CheckButton"));

        var verdict = app.Find<TextBlock>("VerdictText");
        Assert.True(verdict.IsEffectivelyVisible);
        Assert.Equal("Blocked", verdict.Text);
        Assert.Contains("blocked", verdict.Classes);
        Assert.Contains(app.ViewModel.LineOfSight.Details, row => row.Label == "Sight Line Falls Short By" && row.Value == "195.99 m");
        Assert.Contains(app.ViewModel.LineOfSight.Details, row => row.Label == "Terrain Feature" && row.Value == "A falling slope");
        Assert.True(app.Find<ProfileChart>("Chart").IsEffectivelyVisible);
        Assert.NotNull(app.Find<ProfileChart>("Chart").Analysis);
    }

    [AvaloniaFact]
    public void The_blocked_answer_is_a_card_with_its_obstruction_path_and_fresnel_zone()
    {
        using var app = new Harness();
        app.Click(app.Find<Button>("SamplePromptButton"));
        app.SelectTab(MainTab.LineOfSight);
        app.FieldBox(app.ViewModel.LineOfSight.Frequency).Text = "2400";
        app.Settle();

        app.Click(app.Find<Button>("CheckButton"));

        var card = app.Find<TextBlock>("VerdictText").GetVisualAncestors().OfType<Border>().First(b => b.Classes.Contains("resultCard"));
        Assert.Contains("blocked", card.Classes);
        Assert.DoesNotContain("visible", card.Classes);
        Assert.StartsWith("A falling slope", app.Find<TextBlock>("ReasonText").Text);
        Assert.True(app.Find<TextBlock>("ReasonText").IsEffectivelyVisible);

        var result = app.Find<StackPanel>("LineOfSightResult");
        var shown = result.GetVisualDescendants().OfType<TextBlock>().Where(t => t.IsEffectivelyVisible).Select(t => t.Text).ToList();
        Assert.Contains("OBSTRUCTION", shown);
        Assert.Contains("PATH", shown);
        Assert.Contains("Sight Line Falls Short By", shown);
        Assert.Contains("195.99 m", shown);
        Assert.Equal("Blocked", app.Find<TextBlock>("FresnelVerdictText").Text);
        Assert.True(app.Find<TextBlock>("FresnelVerdictText").IsEffectivelyVisible);
    }

    [AvaloniaFact]
    public void Each_side_tab_shows_only_its_panel_and_the_profile_closes_and_reopens_under_the_map()
    {
        using var app = new Harness();
        app.Click(app.Find<Button>("SamplePromptButton"));
        var panels = new[] { "TerrainPanel", "LineOfSightPanel", "ViewshedPanel", "AboutPanel" };
        var tabs = new[] { "TerrainTabButton", "LineOfSightTabButton", "ViewshedTabButton", "AboutTabButton" };

        for (int i = 0; i < tabs.Length; i++)
        {
            app.Click(app.Find<Button>(tabs[i]));
            Assert.Equal((MainTab)i, app.ViewModel.SelectedTab);
            Assert.Contains("selected", app.Find<Button>(tabs[i]).Classes);
            for (int j = 0; j < panels.Length; j++)
            {
                var panel = app.Window.GetVisualDescendants().OfType<ScrollViewer>().Single(s => s.Name == panels[j]);
                Assert.Equal(i == j, panel.IsEffectivelyVisible);
            }
        }

        // The tabs hang on the panel's edge over the map: no empty strip beside the map or the profile.
        var tabStrip = app.Find<StackPanel>("SideTabs");
        double tabsRight = tabStrip.TranslatePoint(new Point(tabStrip.Bounds.Width, 0), app.Window)!.Value.X;
        double panelLeft = app.Find<Border>("SidePanel").TranslatePoint(default, app.Window)!.Value.X;
        Assert.Equal(panelLeft + 1, tabsRight, 0.5);
        double splitterLeft = app.Find<GridSplitter>("PanelSplitter").TranslatePoint(default, app.Window)!.Value.X;
        Assert.Equal(splitterLeft, app.Find<MapView>("Map").TranslatePoint(new Point(app.Find<MapView>("Map").Bounds.Width, 0), app.Window)!.Value.X, 0.5);
        Assert.Equal(splitterLeft, app.Find<Border>("ProfilePanel").TranslatePoint(new Point(app.Find<Border>("ProfilePanel").Bounds.Width, 0), app.Window)!.Value.X, 0.5);

        // The profile stays whichever panel is open, and closes to a button that brings it back.
        var map = app.Find<MapView>("Map");
        double mapHeight = map.Bounds.Height;
        Assert.True(app.Find<ProfileChart>("Chart").IsEffectivelyVisible);

        app.Click(app.Find<Button>("CloseProfileButton"));
        Assert.False(app.ViewModel.IsProfileOpen);
        Assert.True(map.Bounds.Height > mapHeight + 150, "The map should take the profile's room.");

        app.Click(app.Find<Button>("OpenProfileButton"));
        Assert.True(app.Find<ProfileChart>("Chart").IsEffectivelyVisible);
        Assert.Equal(mapHeight, map.Bounds.Height, 1.0);
    }

    [AvaloniaFact]
    public void The_wheel_and_the_buttons_zoom_the_map_and_a_right_drag_moves_it()
    {
        using var app = new Harness();
        app.Click(app.Find<Button>("SamplePromptButton"));
        app.SelectTab(MainTab.LineOfSight);
        var map = app.Find<MapView>("Map");
        var fit = map.Frame!.Value.Area;
        var zoomText = app.Find<TextBlock>("ZoomText");
        Assert.Equal("100%", zoomText.Text);

        // The wheel zooms 10% a notch, keeping the coordinate under the pointer in place.
        var pointer = new Point(fit.X + fit.Width * 0.25, fit.Y + fit.Height * 0.25);
        var before = map.Frame!.Value.ToCoordinate(pointer);
        var inWindow = map.TranslatePoint(pointer, app.Window)!.Value;
        app.Window.MouseMove(inWindow);
        app.Window.MouseWheel(inWindow, new Vector(0, 3));
        app.Settle();
        Assert.Equal(Math.Pow(1.1, 3), map.Zoom, 1e-9);
        Assert.Equal("133%", zoomText.Text);
        var after = map.Frame!.Value.ToCoordinate(pointer);
        Assert.Equal(before.LatitudeDeg, after.LatitudeDeg, 1e-9);
        Assert.Equal(before.LongitudeDeg, after.LongitudeDeg, 1e-9);

        app.Click(app.Find<Button>("ZoomInButton"));
        Assert.Equal(Math.Pow(1.1, 3) * 1.25, map.Zoom, 1e-9);

        // A right-drag moves the map without placing anything.
        var observer = app.ViewModel.LineOfSight.ObserverLatitude.Text;
        double left = map.Frame!.Value.Area.X;
        var from = map.TranslatePoint(fit.Center, app.Window)!.Value;
        app.Window.MouseDown(from, MouseButton.Right);
        app.Window.MouseMove(from + new Vector(60, 0));
        app.Window.MouseUp(from + new Vector(60, 0), MouseButton.Right);
        app.Settle();
        Assert.Equal(left + 60, map.Frame!.Value.Area.X, 0.5);
        Assert.Equal(observer, app.ViewModel.LineOfSight.ObserverLatitude.Text);

        // Zooming all the way out puts the whole tile back where it was.
        for (int i = 0; i < 10; i++) app.Click(app.Find<Button>("ZoomOutButton"));
        Assert.Equal(1, map.Zoom);
        Assert.Equal("100%", zoomText.Text);
        Assert.Equal(fit, map.Frame!.Value.Area);
    }


    [AvaloniaFact]
    public void Number_boxes_keep_a_fixed_width_instead_of_stretching_with_the_panel()
    {
        using var app = new Harness();
        app.Click(app.Find<Button>("SamplePromptButton"));
        app.SelectTab(MainTab.LineOfSight);

        var box = app.FieldBox(app.ViewModel.LineOfSight.ObserverLatitude);
        Assert.Equal(240, box.Bounds.Width, 0.5);
    }

    [AvaloniaFact]
    public void A_spacing_of_zero_is_caught_at_the_field_and_the_check_is_disabled()
    {
        using var app = new Harness();
        app.Click(app.Find<Button>("SamplePromptButton"));
        app.SelectTab(MainTab.LineOfSight);

        app.FieldBox(app.ViewModel.LineOfSight.Spacing).Text = "0";
        app.Settle();

        var error = app.Window.GetVisualDescendants().OfType<TextBlock>()
            .Single(t => t.Classes.Contains("error") && ReferenceEquals(t.DataContext, app.ViewModel.LineOfSight.Spacing));
        Assert.True(error.IsEffectivelyVisible);
        Assert.Equal("The spacing must be greater than 0.", error.Text);
        Assert.False(app.Find<Button>("CheckButton").IsEffectivelyEnabled);
    }

    [AvaloniaFact]
    public void A_fast_viewshed_draws_its_legend_and_summary()
    {
        using var app = new Harness();
        app.Click(app.Find<Button>("SamplePromptButton"));
        app.SelectTab(MainTab.Viewshed);
        app.FieldBox(app.ViewModel.Viewshed.RadiusKm).Text = "2";
        app.Settle();

        app.Click(app.Find<Button>("RunViewshedButton"));
        app.WaitUntil(() => app.ViewModel.Viewshed.HasResult, TimeSpan.FromSeconds(30), "the viewshed");

        Assert.Equal(133, app.ViewModel.Viewshed.Map!.Rows);
        Assert.Equal(4, app.ViewModel.Viewshed.Legend.Count);
        Assert.StartsWith("Fast viewshed of 133 × 133 cells", app.Find<TextBlock>("ViewshedSummary").Text);
        Assert.True(app.Find<MapView>("Map").ShowViewshed);
    }

    [AvaloniaFact]
    public void A_naive_30_km_viewshed_shows_progress_keeps_the_window_responsive_and_cancels()
    {
        using var app = new Harness();
        app.Click(app.Find<Button>("SamplePromptButton"));
        app.SelectTab(MainTab.Viewshed);
        app.ViewModel.Viewshed.Algorithm = Engine.ViewshedAlgorithm.Naive;
        app.Settle();

        app.Click(app.Find<Button>("RunViewshedButton"));
        app.WaitUntil(() => app.ViewModel.Viewshed.Progress > 0, TimeSpan.FromSeconds(60), "the first progress report");

        Assert.True(app.ViewModel.Viewshed.IsRunning);
        Assert.Contains(app.Window.GetVisualDescendants().OfType<ProgressBar>(), bar => bar.IsEffectivelyVisible && bar.Value > 0);

        // The UI thread is free while it runs: the window still switches tabs and back.
        app.SelectTab(MainTab.About);
        Assert.Equal("1.2.3", app.Find<TextBlock>("AboutAppVersion").Text);
        app.SelectTab(MainTab.Viewshed);

        app.Click(app.Find<Button>("CancelViewshedButton"));
        app.WaitUntil(() => !app.ViewModel.Viewshed.IsRunning, TimeSpan.FromSeconds(30), "the viewshed to stop");

        Assert.StartsWith("Cancelled.", app.ViewModel.Viewshed.Error);
        Assert.Null(app.ViewModel.Viewshed.Map);
    }

    [AvaloniaFact]
    public void A_click_places_the_observer_and_a_ctrl_click_the_target()
    {
        using var app = new Harness();
        app.Click(app.Find<Button>("SamplePromptButton"));
        app.SelectTab(MainTab.LineOfSight);
        var map = app.Find<MapView>("Map");
        var frame = map.Frame!.Value;

        void ClickAt(double lat, double lon, RawInputModifiers modifiers = RawInputModifiers.None)
        {
            var point = map.TranslatePoint(frame.ToScreen(lat, lon), app.Window)!.Value;
            app.Window.MouseMove(point);
            app.Window.MouseDown(point, MouseButton.Left, modifiers);
            app.Window.MouseUp(point, MouseButton.Left, modifiers);
            app.Settle();
        }

        ClickAt(36.2, -111.8);
        ClickAt(36.25, -111.7, RawInputModifiers.Control);
        ClickAt(36.3, -111.6);

        Assert.Equal(36.3, app.ViewModel.LineOfSight.ObserverLatitude.Value!.Value, 0.002);
        Assert.Equal(-111.7, app.ViewModel.LineOfSight.TargetLongitude.Value!.Value, 0.002);
        Assert.NotNull(app.ViewModel.LineOfSight.Analysis);
    }

    [AvaloniaFact]
    public void While_a_marker_is_dragged_the_path_is_checked_live_before_the_drop()
    {
        using var app = new Harness();
        app.Click(app.Find<Button>("SamplePromptButton"));
        app.SelectTab(MainTab.LineOfSight);
        var map = app.Find<MapView>("Map");
        var frame = map.Frame!.Value;

        var from = map.TranslatePoint(frame.ToScreen(36.35, -111.45), app.Window)!.Value;
        var middle = map.TranslatePoint(frame.ToScreen(36.45, -111.35), app.Window)!.Value;
        app.Window.MouseMove(from);
        app.Window.MouseDown(from, MouseButton.Left);
        app.Window.MouseMove(middle);
        app.Settle();

        // Not dropped yet, and the target and its analysis already follow the pointer.
        Assert.Equal(36.45, app.ViewModel.LineOfSight.TargetLatitude.Value!.Value, 0.002);
        var analysis = app.ViewModel.LineOfSight.Analysis!;
        Assert.Equal(36.45, analysis.Samples[^1].LatitudeDeg, 0.002);
        Assert.False(map.LiveDragPaused);

        app.Window.MouseUp(middle, MouseButton.Left);
        app.Settle();
        Assert.Equal(36.45, app.ViewModel.LineOfSight.TargetLatitude.Value!.Value, 0.002);
    }

    [AvaloniaFact]
    public void The_profile_legend_turns_lines_off_and_the_chart_zooms_moves_and_reads_the_point_under_the_pointer()
    {
        using var app = new Harness();
        app.Click(app.Find<Button>("SamplePromptButton"));
        app.SelectTab(MainTab.LineOfSight);
        app.Click(app.Find<Button>("CheckButton"));
        var chart = app.Find<ProfileChart>("Chart");
        var map = app.Find<MapView>("Map");

        // Legend
        app.Click(app.Find<ToggleButton>("LegendCurvature"));
        Assert.False(app.ViewModel.ProfileShowCurvature);
        Assert.False(chart.ShowCurvature);
        Assert.Equal(0.4, app.Find<ToggleButton>("LegendCurvature").Opacity, 1e-9);
        app.Click(app.Find<ToggleButton>("LegendCurvature"));
        Assert.True(chart.ShowCurvature);

        // Zoom towards the middle, then move along the path.
        var total = app.ViewModel.LineOfSight.Analysis!.TotalDistanceM;
        var centre = chart.TranslatePoint(new Point(chart.Bounds.Width / 2, chart.Bounds.Height / 2), app.Window)!.Value;
        app.Window.MouseMove(centre);
        app.Window.MouseWheel(centre, new Vector(0, 5));
        app.Settle();
        Assert.True(chart.IsZoomed);
        Assert.True(app.Find<Button>("ResetProfileZoomButton").IsEffectivelyVisible);
        var (start, end) = chart.View;
        Assert.Equal(total / Math.Pow(1.1, 5), end - start, 1.0);

        app.Window.MouseDown(centre, MouseButton.Right);
        app.Window.MouseMove(centre + new Vector(-100, 0));
        app.Window.MouseUp(centre + new Vector(-100, 0), MouseButton.Right);
        app.Settle();
        Assert.True(chart.View.Start > start);
        Assert.Equal(end - start, chart.View.End - chart.View.Start, 1e-6);

        // The pointer on the chart marks the same point on the map.
        app.Window.MouseMove(centre);
        app.Settle();
        Assert.NotNull(map.ProfileHoverLatitude);

        app.Click(app.Find<Button>("ResetProfileZoomButton"));
        Assert.False(chart.IsZoomed);
        Assert.Equal((0.0, total), chart.View);
    }

    [AvaloniaFact]
    public void Plus_and_minus_zoom_the_focused_map_and_swapping_trades_the_points()
    {
        using var app = new Harness();
        app.Click(app.Find<Button>("SamplePromptButton"));
        app.SelectTab(MainTab.LineOfSight);
        var map = app.Find<MapView>("Map");
        map.Focus();

        app.Window.KeyPress(Key.Add, RawInputModifiers.None, PhysicalKey.NumPadAdd, null);
        app.Settle();
        Assert.Equal(1.25, map.Zoom, 1e-9);
        app.Window.KeyPress(Key.Subtract, RawInputModifiers.None, PhysicalKey.NumPadSubtract, null);
        app.Settle();
        Assert.Equal(1, map.Zoom, 1e-9);

        app.Click(app.Find<Button>("SwapPointsButton"));
        Assert.Equal("36.35", app.ViewModel.LineOfSight.ObserverLatitude.Text);
        Assert.Equal("36.3", app.ViewModel.LineOfSight.TargetLatitude.Text);
        Assert.NotNull(app.ViewModel.LineOfSight.Analysis);
    }

    [AvaloniaFact]
    public void A_viewshed_legend_entry_hides_its_cells_and_moving_the_observer_reruns_a_fast_viewshed()
    {
        using var app = new Harness();
        app.Click(app.Find<Button>("SamplePromptButton"));
        app.SelectTab(MainTab.Viewshed);
        app.FieldBox(app.ViewModel.Viewshed.RadiusKm).Text = "3";
        app.Settle();
        app.Click(app.Find<Button>("RunViewshedButton"));
        app.WaitUntil(() => app.ViewModel.Viewshed.HasResult && !app.ViewModel.Viewshed.IsRunning, TimeSpan.FromSeconds(30), "the viewshed");
        var map = app.Find<MapView>("Map");
        Assert.Equal("3 km", map.ViewshedRadiusLabel);

        var legendButton = app.Find<ItemsControl>("ViewshedLegend").GetVisualDescendants().OfType<Button>()
            .First(b => b.DataContext is LegendRow { Name: "Not Visible" });
        app.Click(legendButton);
        Assert.True(map.HiddenLayers![(int)Engine.CellState.NotVisible]);
        Assert.Contains("off", legendButton.Classes);

        // A left-click moves the observer: the old layer goes and a fast run redoes it.
        var firstMap = app.ViewModel.Viewshed.Map;
        var point = map.TranslatePoint(map.Frame!.Value.ToScreen(36.6, -111.6), app.Window)!.Value;
        app.Window.MouseMove(point);
        app.Window.MouseDown(point, MouseButton.Left);
        app.Window.MouseUp(point, MouseButton.Left);
        app.Settle();
        app.WaitUntil(() => !app.ViewModel.Viewshed.IsRunning && !ReferenceEquals(app.ViewModel.Viewshed.Map, firstMap), TimeSpan.FromSeconds(30), "the rerun");
        Assert.True(map.ShowViewshedLayer);
        Assert.False(app.ViewModel.Viewshed.NeedsRunAgain);

        // Another setting only fades it and asks for a run.
        app.FieldBox(app.ViewModel.Viewshed.RefractionK).Text = "1e12";
        app.Settle();
        Assert.True(app.Find<Border>("RunAgainNote").IsEffectivelyVisible);
        Assert.Equal(0.35, map.ViewshedOpacity, 1e-9);
    }

    [AvaloniaFact]
    public void Dragging_the_target_marker_moves_the_target_and_checks_again()
    {
        using var app = new Harness();
        app.Click(app.Find<Button>("SamplePromptButton"));
        app.SelectTab(MainTab.LineOfSight);
        var map = app.Find<MapView>("Map");
        var frame = map.Frame!.Value;

        var from = map.TranslatePoint(frame.ToScreen(36.35, -111.45), app.Window)!.Value;
        var to = map.TranslatePoint(frame.ToScreen(36.4, -111.3), app.Window)!.Value;
        app.Window.MouseMove(from);
        app.Window.MouseDown(from, Avalonia.Input.MouseButton.Left);
        app.Window.MouseMove(to);
        app.Window.MouseUp(to, Avalonia.Input.MouseButton.Left);
        app.Settle();

        Assert.Equal(36.4, app.ViewModel.LineOfSight.TargetLatitude.Value!.Value, 0.002);
        Assert.Equal(-111.3, app.ViewModel.LineOfSight.TargetLongitude.Value!.Value, 0.002);
        Assert.Equal(36.3, app.ViewModel.LineOfSight.ObserverLatitude.Value);
        Assert.NotNull(app.ViewModel.LineOfSight.Analysis);
    }

    [AvaloniaFact]
    public async Task The_map_and_a_viewshed_export_as_png_files_in_the_tiles_proportions()
    {
        using var app = new Harness();
        app.Click(app.Find<Button>("SamplePromptButton"));
        var tile = app.ViewModel.Terrain.Tile!;
        var viewshed = tile.Viewshed(new Engine.ViewshedQuery(36.5, -111.5, 2, 2, 30, 4.0 / 3.0, Engine.Interpolation.Nearest, Engine.ViewshedAlgorithm.Fast), null, default).Value;
        var path = Path.Combine(Path.GetTempPath(), $"terrainbench-{Guid.NewGuid():N}.png");

        try
        {
            await new Services.PngMapExporter().SavePngAsync(path, app.ViewModel.Terrain.Posts!, viewshed, null, tile.Info);

            using var image = new Avalonia.Media.Imaging.Bitmap(path);
            Assert.Equal(1201, image.PixelSize.Width);
            // One degree of latitude is longer on the ground than one of longitude at 36.5 N.
            double expectedHeight = 1201 / (tile.Info.PostSpacingEastWestM / tile.Info.PostSpacingNorthSouthM);
            Assert.Equal(expectedHeight, image.PixelSize.Height, 1.0);
        }
        finally
        {
            File.Delete(path);
        }
    }

    [AvaloniaFact]
    public void The_about_page_shows_the_app_version_and_the_engine_commit_and_the_window_has_its_icon()
    {
        using var app = new Harness();
        app.SelectTab(MainTab.About);

        Assert.Equal("1.2.3", app.Find<TextBlock>("AboutAppVersion").Text);
        Assert.Equal(app.ViewModel.About.EngineCommit, app.Find<SelectableTextBlock>("AboutCommit").Text);
        Assert.False(string.IsNullOrEmpty(app.ViewModel.About.EngineCommit));
        Assert.NotNull(app.Window.Icon);
    }
}
