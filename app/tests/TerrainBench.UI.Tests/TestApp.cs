using Avalonia;
using Avalonia.Headless;
using Avalonia.Headless.XUnit;
using TerrainBench.UI.Tests;

[assembly: AvaloniaTestApplication(typeof(TestApp))]

namespace TerrainBench.UI.Tests;

/// <summary>
/// The real <see cref="App"/> on the headless platform with Skia drawing on, so the window lays out
/// and paints as it does on screen. Without a desktop lifetime the app composes nothing and only
/// loads its styles; each test builds its own window.
/// </summary>
public static class TestApp
{
    public static AppBuilder BuildAvaloniaApp() =>
        AppBuilder.Configure<App>()
            .UseSkia()
            .UseHeadless(new AvaloniaHeadlessPlatformOptions { UseHeadlessDrawing = false });
}
