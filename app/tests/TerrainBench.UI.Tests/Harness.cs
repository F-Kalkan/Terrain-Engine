using System.Diagnostics;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Headless;
using Avalonia.Input;
using Avalonia.Threading;
using Avalonia.VisualTree;
using TerrainBench.Engine;
using TerrainBench.Services;
using TerrainBench.ViewModels;
using TerrainBench.Views;
using Xunit;

namespace TerrainBench.UI.Tests;

/// <summary>A main window over the real engine, with the outside world (dialogs, clipboard, disk) faked.</summary>
internal sealed class Harness : IDisposable
{
    public Harness(bool firstStart = true)
    {
        Window = new MainWindow { Width = 1360, Height = 880 };
        Settings = new MemorySettings();
        ViewModel = new MainViewModel(
            NativeTerrainEngine.Load(),
            new NoDialogs(),
            Clipboard,
            Settings,
            new NoExporter(),
            "1.2.3",
            Path.Combine(AppContext.BaseDirectory, "Samples", "N36W112.hgt"));
        Window.DataContext = ViewModel;
        if (firstStart) ViewModel.Start();
        Window.Show();
        Settle();
    }

    public MainWindow Window { get; }

    public MainViewModel ViewModel { get; }

    public MemorySettings Settings { get; }

    public MemoryClipboard Clipboard { get; } = new();

    public T Find<T>(string name) where T : Control =>
        Window.GetVisualDescendants().OfType<T>().FirstOrDefault(c => c.Name == name)
        ?? throw new InvalidOperationException($"No {typeof(T).Name} named {name} is on screen.");

    public TextBox FieldBox(NumberField field) =>
        Window.GetVisualDescendants().OfType<TextBox>().Single(t => t.Name == "Input" && ReferenceEquals(t.DataContext, field));

    public void Click(Control control)
    {
        Assert.True(control.IsEffectivelyVisible, $"{control.Name} isn't visible.");

        // As a person would: scroll the control into view before clicking it.
        control.BringIntoView();
        Settle();
        var centre = control.TranslatePoint(new Point(control.Bounds.Width / 2, control.Bounds.Height / 2), Window)!.Value;
        Window.MouseMove(centre);
        Window.MouseDown(centre, MouseButton.Left);
        Window.MouseUp(centre, MouseButton.Left);
        Settle();
    }

    public void SelectTab(MainTab tab)
    {
        ViewModel.SelectedTab = tab;
        Settle();
    }

    public void Settle()
    {
        for (int i = 0; i < 3; i++)
        {
            Dispatcher.UIThread.RunJobs();
            AvaloniaHeadlessPlatform.ForceRenderTimerTick();
        }
    }

    public void WaitUntil(Func<bool> condition, TimeSpan timeout, string what)
    {
        var clock = Stopwatch.StartNew();
        while (!condition())
        {
            if (clock.Elapsed > timeout) throw new TimeoutException($"Timed out waiting for {what}.");
            Dispatcher.UIThread.RunJobs();
            Thread.Sleep(10);
        }
        Settle();
    }

    public void Dispose()
    {
        Window.Close();
        ViewModel.Dispose();
    }

    private sealed class NoDialogs : IDialogService
    {
        public Task<string?> PickTileAsync() => Task.FromResult<string?>(null);

        public Task<string?> PickSaveFileAsync(string title, string suggestedName, string extension, string typeName) => Task.FromResult<string?>(null);
    }

    private sealed class NoExporter : IMapExporter
    {
        public Task SavePngAsync(string path, TilePosts posts, ViewshedMap? viewshed, bool[]? disagreements, TileInfo tile) => Task.CompletedTask;
    }
}

internal sealed class MemorySettings : ISettingsStore
{
    public AppSettings? Saved { get; set; }

    public AppSettings? Load() => Saved;

    public void Save(AppSettings settings) => Saved = settings;
}

internal sealed class MemoryClipboard : IClipboardService
{
    public string? Text { get; private set; }

    public Task SetTextAsync(string text)
    {
        Text = text;
        return Task.CompletedTask;
    }
}
