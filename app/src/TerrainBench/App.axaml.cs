using System.Reflection;
using Avalonia;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Markup.Xaml;
using TerrainBench.Engine;
using TerrainBench.Services;
using TerrainBench.ViewModels;
using TerrainBench.Views;

namespace TerrainBench;

/// <summary>Composition root: loads the engine, wires the services and opens the window.</summary>
public partial class App : Application
{
    public override void Initialize() => AvaloniaXamlLoader.Load(this);

    public override void OnFrameworkInitializationCompleted()
    {
        if (ApplicationLifetime is IClassicDesktopStyleApplicationLifetime desktop)
        {
            var window = new MainWindow();
            var viewModel = CreateViewModel(window);
            window.DataContext = viewModel;
            window.Closing += (_, _) => viewModel.SaveSettings();
            desktop.Exit += (_, _) => viewModel.Dispose();
            desktop.MainWindow = window;
            viewModel.Start();
        }

        base.OnFrameworkInitializationCompleted();
    }

    internal static MainViewModel CreateViewModel(MainWindow window, ISettingsStore? settings = null) => new(
        NativeTerrainEngine.Load(),
        new AvaloniaDialogs(window),
        new AvaloniaClipboard(window),
        settings ?? new JsonSettingsStore(JsonSettingsStore.DefaultPath),
        new PngMapExporter(),
        AppVersion,
        Path.Combine(AppContext.BaseDirectory, "Samples", "N36W112.hgt"));

    /// <summary>The version stamped into the build: from the tag in a release, the project's own otherwise.</summary>
    public static string AppVersion
    {
        get
        {
            var version = typeof(App).Assembly.GetCustomAttribute<AssemblyInformationalVersionAttribute>()?.InformationalVersion ?? "0.0.0";
            int metadata = version.IndexOf('+');
            return metadata >= 0 ? version[..metadata] : version;
        }
    }
}
