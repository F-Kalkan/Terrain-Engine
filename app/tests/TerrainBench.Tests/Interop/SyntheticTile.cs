namespace TerrainBench.Tests.Interop;

/// <summary>
/// Writes a 3-arcsecond (1201 x 1201) .hgt tile with chosen posts, so the hand-checkable cases in
/// the engine's own Tests.h can be asked through the DLL, which only reads tiles from disk.
/// </summary>
internal sealed class SyntheticTile : IDisposable
{
    public const int Posts = 1201;

    private SyntheticTile(string path) => Path = path;

    public string Path { get; }

    /// <summary>Post spacing along a meridian, in degrees: a row step.</summary>
    public static double RowStepDeg => 1.0 / (Posts - 1);

    public static SyntheticTile Write(short background, Action<short[]>? edit = null)
    {
        var posts = new short[Posts * Posts];
        Array.Fill(posts, background);
        edit?.Invoke(posts);

        var bytes = new byte[posts.Length * 2];
        for (int i = 0; i < posts.Length; i++)
        {
            bytes[2 * i] = (byte)((posts[i] >> 8) & 0xFF);
            bytes[2 * i + 1] = (byte)(posts[i] & 0xFF);
        }

        var path = System.IO.Path.Combine(System.IO.Path.GetTempPath(), $"terrainbench-{Guid.NewGuid():N}.hgt");
        File.WriteAllBytes(path, bytes);
        return new SyntheticTile(path);
    }

    public static int Index(int row, int col) => row * Posts + col;

    /// <summary>Latitude of a post row in a tile whose south-west corner is at <paramref name="southDeg"/>.</summary>
    public static double RowLatitude(double southDeg, int row) => southDeg + 1.0 - row * RowStepDeg;

    public static double ColLongitude(double westDeg, int col) => westDeg + col * RowStepDeg;

    public void Dispose()
    {
        try { File.Delete(Path); } catch (IOException) { }
    }
}
