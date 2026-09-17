using System.ComponentModel;
using Avalonia.Controls;
using Avalonia.Markup.Xaml;
using TerrainBench.Controls;
using TerrainBench.ViewModels;

namespace TerrainBench.Views;

public partial class MainWindow : Window
{
    private const double MinPanelWidth = 360;
    private const double MinProfileHeight = 120;

    /// <summary>A check slower than this stops the live updates of a drag; the path is checked once more on the drop.</summary>
    private static readonly TimeSpan LiveCheckBudget = TimeSpan.FromMilliseconds(60);

    private readonly ColumnDefinition _panelColumn;
    private readonly RowDefinition _profileRow;
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
            if (_subscribed is not null) _subscribed.PropertyChanged -= OnViewModelChanged;
            _subscribed = ViewModel;
            if (_subscribed is null) return;
            _subscribed.PropertyChanged += OnViewModelChanged;
            _panelColumn.MinWidth = MinPanelWidth;
            _panelColumn.Width = new GridLength(_subscribed.PanelWidth);
            ArrangeProfile();
        };
    }

    private MainViewModel? ViewModel => DataContext as MainViewModel;

    private void OnViewModelChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (e.PropertyName == nameof(MainViewModel.IsProfileOpen)) ArrangeProfile();
    }

    /// <summary>An open profile takes its remembered height under the map; a closed one gives the map the room.</summary>
    private void ArrangeProfile()
    {
        if (ViewModel is not { } vm) return;
        _profileRow.MinHeight = vm.IsProfileOpen ? MinProfileHeight : 0;
        _profileRow.Height = new GridLength(vm.IsProfileOpen ? vm.ProfileHeight : 0);
    }
}
