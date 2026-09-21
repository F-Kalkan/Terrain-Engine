using System.ComponentModel;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Interactivity;
using Avalonia.Markup.Xaml;
using Avalonia.Media;
using Avalonia.Threading;
using CommunityToolkit.Mvvm.Input;
using TerrainBench.Controls;
using TerrainBench.ViewModels;
using ShapePath = Avalonia.Controls.Shapes.Path;

namespace TerrainBench.Views;

public partial class MainWindow : Window
{
    private const double MinPanelWidth = 360;
    private const double MinProfileHeight = 120;

    /// <summary>A check slower than this stops the live updates of a drag; the path is checked once more on the drop.</summary>
    private static readonly TimeSpan LiveCheckBudget = TimeSpan.FromMilliseconds(60);

    private readonly ColumnDefinition _panelColumn;
    private readonly RowDefinition _profileRow;
    private readonly Canvas _tourOverlay;
    private readonly Border _tourCard;
    private (Size Window, Rect? Hole, TourLayout Layout)? _tourLaidOut;
    private MainViewModel? _subscribed;

    public MainWindow()
    {
        AvaloniaXamlLoader.Load(this);

        _panelColumn = this.FindControl<Grid>("Workspace")!.ColumnDefinitions[2];
        _profileRow = this.FindControl<Grid>("MapArea")!.RowDefinitions[2];

        var map = this.FindControl<MapView>("Map")!;
        map.MapClicked += (_, e) => ViewModel?.OnMapClicked(e.LatitudeDeg, e.LongitudeDeg, e.Target);
        map.MarkerDropped += (_, e) =>
        {
            switch (e.Marker)
            {
                case MapMarker.Observer: ViewModel?.LineOfSight.PlaceObserver(e.LatitudeDeg, e.LongitudeDeg); break;
                case MapMarker.Target: ViewModel?.LineOfSight.PlaceTarget(e.LatitudeDeg, e.LongitudeDeg); break;
                case MapMarker.ViewshedObserver: ViewModel?.Viewshed.PlaceObserver(e.LatitudeDeg, e.LongitudeDeg); break;
            }
        };
        map.CursorMoved += (_, e) => ViewModel?.Terrain.UpdateCursor(e.LatitudeDeg, e.LongitudeDeg);
        map.CursorLeft += (_, _) => ViewModel?.Terrain.ClearCursor();

        // A line-of-sight marker being dragged checks the path live, as long as checking keeps up.
        map.MarkerDragging += (_, e) =>
        {
            if (ViewModel?.LineOfSight is not { } los) return;
            if (e.Marker == MapMarker.Observer) los.PlaceObserver(e.LatitudeDeg, e.LongitudeDeg);
            else if (e.Marker == MapMarker.Target) los.PlaceTarget(e.LatitudeDeg, e.LongitudeDeg);
            if (los.LastCheckDuration > LiveCheckBudget) map.LiveDragPaused = true;
        };

        // The point under the pointer on the profile, marked on the map too.
        var chart = this.FindControl<ProfileChart>("Chart")!;
        chart.HoverChanged += (_, sample) =>
        {
            map.ProfileHoverLatitude = sample?.LatitudeDeg;
            map.ProfileHoverLongitude = sample?.LongitudeDeg;
        };
        this.FindControl<Button>("ResetProfileZoomButton")!.Click += (_, _) => chart.ResetZoom();

        // Window-wide keys: Ctrl+1 moves to the map, Ctrl+2 to Ctrl+5 open a panel, Ctrl+6 moves to the
        // profile, and F5 runs whatever the open panel runs.
        var focusMap = new RelayCommand(() => map.Focus());
        AddKey(Key.D1, KeyModifiers.Control, focusMap);
        AddKey(Key.NumPad1, KeyModifiers.Control, focusMap);
        MainTab[] tabs = [MainTab.Terrain, MainTab.LineOfSight, MainTab.Viewshed, MainTab.About];
        for (int i = 0; i < tabs.Length; i++)
        {
            var tab = tabs[i];
            AddKey(Key.D2 + i, KeyModifiers.Control, new RelayCommand(() => ViewModel?.ShowTab(tab)));
            AddKey(Key.NumPad2 + i, KeyModifiers.Control, new RelayCommand(() => ViewModel?.ShowTab(tab)));
        }
        var focusProfile = new RelayCommand(() =>
        {
            if (ViewModel is null) return;
            ViewModel.OpenProfile();
            Dispatcher.UIThread.Post(() => chart.Focus(), DispatcherPriority.Loaded);
        });
        AddKey(Key.D6, KeyModifiers.Control, focusProfile);
        AddKey(Key.NumPad6, KeyModifiers.Control, focusProfile);
        AddKey(Key.F5, KeyModifiers.None, new AsyncRelayCommand(() => ViewModel?.RunActivePanelAsync() ?? Task.CompletedTask));

        // Escape closes the tour, before whatever has the focus sees the key.
        AddHandler(KeyDownEvent, (_, e) =>
        {
            if (e.Key != Key.Escape || ViewModel?.Tour is not { IsOpen: true } tour) return;
            tour.Close();
            e.Handled = true;
        }, RoutingStrategies.Tunnel);

        // The tour follows the control it points at as the window resizes, panels scroll and results appear.
        _tourOverlay = this.FindControl<Canvas>("TourOverlay")!;
        _tourCard = this.FindControl<Border>("TourCard")!;
        LayoutUpdated += (_, _) => LayOutTour();

        this.FindControl<Button>("ZoomInButton")!.Click += (_, _) => map.ZoomIn();
        this.FindControl<Button>("ZoomOutButton")!.Click += (_, _) => map.ZoomOut();

        // Remember the sizes the panel and the profile were dragged to.
        this.FindControl<Border>("SidePanel")!.SizeChanged += (_, e) =>
        {
            if (ViewModel is { } vm && e.NewSize.Width >= MinPanelWidth) vm.PanelWidth = e.NewSize.Width;
        };
        this.FindControl<Border>("ProfilePanel")!.SizeChanged += (_, e) =>
        {
            if (ViewModel is { IsProfileOpen: true } vm && e.NewSize.Height >= MinProfileHeight) vm.ProfileHeight = e.NewSize.Height;
        };

        // On a small window neither the panel nor the profile may squeeze the map away.
        SizeChanged += (_, e) =>
        {
            _panelColumn.MaxWidth = Math.Max(MinPanelWidth, e.NewSize.Width * 0.45);
            _profileRow.MaxHeight = Math.Max(MinProfileHeight, e.NewSize.Height * 0.35);
        };

        DataContextChanged += (_, _) =>
        {
            if (_subscribed is not null)
            {
                _subscribed.PropertyChanged -= OnViewModelChanged;
                _subscribed.Tour.PropertyChanged -= OnTourChanged;
            }
            _subscribed = ViewModel;
            if (_subscribed is null) return;
            _subscribed.PropertyChanged += OnViewModelChanged;
            _subscribed.Tour.PropertyChanged += OnTourChanged;
            if (_subscribed.Tour.IsOpen) FocusTour();
            _panelColumn.MinWidth = MinPanelWidth;
            _panelColumn.Width = new GridLength(_subscribed.PanelWidth);
            ArrangeProfile();
        };
    }

    private MainViewModel? ViewModel => DataContext as MainViewModel;

    private void AddKey(Key key, KeyModifiers modifiers, System.Windows.Input.ICommand command) =>
        KeyBindings.Add(new KeyBinding { Gesture = new KeyGesture(key, modifiers), Command = command });

    private void OnViewModelChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (e.PropertyName == nameof(MainViewModel.IsProfileOpen)) ArrangeProfile();
    }

    private void OnTourChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (e.PropertyName is not (nameof(TourViewModel.IsOpen) or nameof(TourViewModel.Index)) || ViewModel is not { } vm) return;
        if (vm.Tour.IsOpen) FocusTour();
        else Dispatcher.UIThread.Post(() => this.FindControl<MapView>("Map")!.Focus(), DispatcherPriority.Loaded);
    }

    /// <summary>
    /// A new page: its control is scrolled into view and, if the page waits for it to be used, given the
    /// focus, so Enter does what a click would; a page with a Next button focuses that.
    /// </summary>
    private void FocusTour() =>
        Dispatcher.UIThread.Post(() =>
        {
            if (ViewModel?.Tour is not { IsOpen: true } tour) return;
            var target = TourTarget();
            target?.BringIntoView();
            if (!tour.HasNext && target is { Focusable: true }) target.Focus(NavigationMethod.Tab);
            else this.FindControl<Button>(tour.HasNext ? "TourNextButton" : "TourSkipButton")!.Focus(NavigationMethod.Tab);
            LayOutTour();
        }, DispatcherPriority.Loaded);

    /// <summary>The control the current page points at, when it is on screen.</summary>
    private Control? TourTarget() =>
        ViewModel?.Tour.Current.Target is { } name && this.FindControl<Control>(name) is { IsEffectivelyVisible: true } target ? target : null;

    /// <summary>Dims the window around the page's control, rings it, and places the card with its arrow.</summary>
    private void LayOutTour()
    {
        if (ViewModel?.Tour is not { IsOpen: true }) return;
        var window = _tourOverlay.Bounds.Size;
        if (window.Width <= 0 || window.Height <= 0) return;

        Rect? hole = null;
        if (TourTarget() is { } target && target.TranslatePoint(default, _tourOverlay) is { } origin)
            hole = new Rect(origin, target.Bounds.Size).Inflate(6).Intersect(new Rect(window));

        _tourCard.Measure(new Size(_tourCard.Width, double.PositiveInfinity));
        var layout = TourPlacement.Place(window, _tourCard.DesiredSize, hole);

        // Setting what is already set would lay the window out again, and come straight back here.
        if (_tourLaidOut == (window, hole, layout)) return;
        _tourLaidOut = (window, hole, layout);

        var full = new RectangleGeometry(new Rect(window));
        this.FindControl<ShapePath>("TourScrim")!.Data = hole is { } gap
            ? new CombinedGeometry(GeometryCombineMode.Exclude, full, new RectangleGeometry(gap))
            : full;

        var ring = this.FindControl<Border>("TourRing")!;
        ring.IsVisible = hole is not null;
        if (hole is { } around)
        {
            Canvas.SetLeft(ring, around.X - 3);
            Canvas.SetTop(ring, around.Y - 3);
            ring.Width = around.Width + 6;
            ring.Height = around.Height + 6;
        }

        var arrow = this.FindControl<ShapePath>("TourArrow")!;
        arrow.IsVisible = layout.ArrowFrom is not null;
        if (layout is { ArrowFrom: { } from, ArrowTo: { } to }) arrow.Data = TourPlacement.Arrow(from, to);

        Canvas.SetLeft(_tourCard, layout.Card.X);
        Canvas.SetTop(_tourCard, layout.Card.Y);
    }

    /// <summary>An open profile takes its remembered height under the map; a closed one gives the map the room.</summary>
    private void ArrangeProfile()
    {
        if (ViewModel is not { } vm) return;
        _profileRow.MinHeight = vm.IsProfileOpen ? MinProfileHeight : 0;
        _profileRow.Height = new GridLength(vm.IsProfileOpen ? vm.ProfileHeight : 0);
    }
}
