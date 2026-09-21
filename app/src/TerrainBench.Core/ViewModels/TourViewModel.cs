using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;

namespace TerrainBench.ViewModels;

/// <summary>What a tour page waits for before it moves on by itself.</summary>
public enum TourGoal
{
    /// <summary>Nothing to do: the page has a Next button.</summary>
    None,
    TileOpen,
    ObserverPlaced,
    TargetPlaced,
    ViewshedTab,
    ViewshedResult,
    AboutTab,
}

/// <summary>
/// One page of the first-run tour. <paramref name="Target"/> names the control it points at, or is
/// null for a page in the middle of the window; <paramref name="Goal"/> is what moves it on.
/// </summary>
public sealed record TourStep(string Title, string Body, string? Target = null, TourGoal Goal = TourGoal.None);

/// <summary>
/// The tour shown on first start. It points at the real controls and has the user use them: a page
/// with a goal moves on when the goal is reached, and one whose goal already holds is skipped.
/// Closing it any way counts as seen; the About panel can open it again.
/// </summary>
public sealed partial class TourViewModel : ObservableObject
{
    public static IReadOnlyList<TourStep> Steps { get; } =
    [
        new("Welcome to TerrainBench",
            "TerrainBench answers three questions about real terrain: the ground along a path, whether one point can see " +
            "another, and everything an observer can see around them. This tour has you try each one; it takes a minute."),
        new("Open a Tile",
            "Everything starts from a tile of elevation data. Open the sample tile, a stretch of the Grand Canyon; " +
            "Open Tile… in the toolbar opens your own .hgt files later.",
            "SamplePromptButton", TourGoal.TileOpen),
        new("Place the Observer",
            "Click anywhere on the map to stand the observer there. The line of sight to the target is checked at once.",
            "Map", TourGoal.ObserverPlaced),
        new("Place the Target",
            "Now hold Ctrl and click somewhere else to move the target. With the keyboard, Enter places the observer " +
            "and Ctrl + Enter the target, at the crosshair the arrow keys move.",
            "Map", TourGoal.TargetPlaced),
        new("Read the Answer",
            "Here is the answer: Visible, Blocked or No Confident Answer, and why. When the path is blocked, it says where " +
            "and by how much. The profile under the map draws the ground between the two points; drag a marker and both follow.",
            "LineOfSightResult"),
        new("Try a Viewshed",
            "A viewshed asks what the observer can see all around. Open the Viewshed panel.",
            "ViewshedTabButton", TourGoal.ViewshedTab),
        new("Run It",
            "Run the viewshed from the observer on the map. Target Height above asks about a person or a mast " +
            "instead of the ground itself.",
            "RunViewshedButton", TourGoal.ViewshedResult),
        new("Read the Colours",
            "Cyan is visible, darkened is out of sight, and purple has no confident answer. Click a row to hide or show " +
            "those cells. Rest the pointer on any ? to learn what a setting does.",
            "ViewshedLegend"),
        new("Shortcuts and This Tour",
            "The About panel lists every mouse and keyboard shortcut, and Show the Tour Again brings this tour back. " +
            "Open it to finish.",
            "AboutTabButton", TourGoal.AboutTab),
    ];

    /// <summary>Whether a goal already holds, asked when a page is reached; set by the window's view model.</summary>
    public Func<TourGoal, bool> Holds { get; set; } = _ => false;

    [ObservableProperty]
    private bool _isOpen;

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(Current), nameof(Progress), nameof(HasNext), nameof(NextText))]
    [NotifyCanExecuteChangedFor(nameof(BackCommand))]
    private int _index;

    public TourStep Current => Steps[Index];

    public string Progress => $"Step {Index + 1} of {Steps.Count}";

    /// <summary>A page without a goal moves on by its button; one with a goal, by doing it.</summary>
    public bool HasNext => Current.Goal == TourGoal.None;

    public string NextText => Index == Steps.Count - 1 ? "Finish" : "Next";

    /// <summary>Raised whenever the tour closes: at its end, by Skip or by Escape.</summary>
    public event EventHandler? Closed;

    /// <summary>Opens the tour at its first page.</summary>
    [RelayCommand]
    public void Open()
    {
        Index = 0;
        IsOpen = true;
    }

    [RelayCommand(CanExecute = nameof(CanGoBack))]
    public void Back()
    {
        // Back over the pages whose goal holds, as forward skips them.
        int index = Index - 1;
        while (index > 0 && Steps[index].Goal != TourGoal.None && Holds(Steps[index].Goal)) index--;
        Index = index;
    }

    public bool CanGoBack => Index > 0;

    [RelayCommand]
    public void Next() => Advance();

    /// <summary>Something the tour may be waiting for happened; the page waiting for it moves on.</summary>
    public void Reached(TourGoal goal)
    {
        if (IsOpen && goal != TourGoal.None && Current.Goal == goal) Advance();
    }

    [RelayCommand]
    public void Close()
    {
        if (!IsOpen) return;
        IsOpen = false;
        Closed?.Invoke(this, EventArgs.Empty);
    }

    /// <summary>The next page whose goal doesn't already hold; past the last, the tour closes.</summary>
    private void Advance()
    {
        int index = Index + 1;
        while (index < Steps.Count && Steps[index].Goal != TourGoal.None && Holds(Steps[index].Goal)) index++;
        if (index >= Steps.Count) Close();
        else Index = index;
    }
}
