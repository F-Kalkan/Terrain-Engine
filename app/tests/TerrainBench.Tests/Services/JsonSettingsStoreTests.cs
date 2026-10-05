using TerrainBench.Services;
using Xunit;

namespace TerrainBench.Tests.Services;

/// <summary>
/// The settings file, on disk: written whole or not at all, and never left behind half-written.
/// </summary>
public class JsonSettingsStoreTests : IDisposable
{
    private readonly string _folder = Path.Combine(Path.GetTempPath(), $"terrainbench-settings-{Guid.NewGuid():N}");
    private string SettingsPath => Path.Combine(_folder, "settings.json");

    public void Dispose()
    {
        try { Directory.Delete(_folder, recursive: true); } catch (IOException) { }
    }

    [Fact]
    public void Saved_settings_read_back_and_leave_nothing_beside_them()
    {
        var store = new JsonSettingsStore(SettingsPath);
        store.Save(new AppSettings { SelectedTab = "Viewshed", PanelWidth = 512, TourSeen = true });
        store.Save(new AppSettings { SelectedTab = "Line of sight", PanelWidth = 480, TourSeen = true });

        var loaded = store.Load();
        Assert.NotNull(loaded);
        Assert.Equal("Line of sight", loaded.SelectedTab);
        Assert.Equal(480, loaded.PanelWidth);
        Assert.Equal(["settings.json"], Directory.GetFiles(_folder).Select(Path.GetFileName));
    }

    [Fact]
    public void A_half_written_file_left_by_an_earlier_crash_does_not_stop_the_next_save()
    {
        Directory.CreateDirectory(_folder);
        File.WriteAllText(SettingsPath + ".tmp", "{ \"SelectedTab\": \"Vie");
        var store = new JsonSettingsStore(SettingsPath);

        store.Save(new AppSettings { SelectedTab = "Profile" });

        Assert.Equal("Profile", store.Load()?.SelectedTab);
        Assert.False(File.Exists(SettingsPath + ".tmp"));
    }

    [Fact]
    public void A_save_that_cannot_be_written_leaves_the_previous_settings_as_they_were()
    {
        var store = new JsonSettingsStore(SettingsPath);
        store.Save(new AppSettings { SelectedTab = "Viewshed", PanelWidth = 512 });
        string before = File.ReadAllText(SettingsPath);
        // Where the new settings would be written first, a folder: the write fails, as on a full disk.
        Directory.CreateDirectory(SettingsPath + ".tmp");

        store.Save(new AppSettings { SelectedTab = "Profile", PanelWidth = 300 });

        Assert.Equal(before, File.ReadAllText(SettingsPath));
        Assert.Equal("Viewshed", store.Load()?.SelectedTab);
    }
}
