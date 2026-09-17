namespace TerrainBench.Tests.Interop;

/// <summary>Finds the repository the tests were built from: the sample tile and the CLI.</summary>
internal static class RepositoryFiles
{
    public static string Root
    {
        get
        {
            var directory = new DirectoryInfo(AppContext.BaseDirectory);
            while (directory is not null && !File.Exists(Path.Combine(directory.FullName, "TerrainEngine.sln")))
            {
                directory = directory.Parent;
            }

            return directory?.FullName
                ?? throw new InvalidOperationException("The tests must run from inside the TerrainEngine repository.");
        }
    }

    /// <summary>The copy of the sample tile placed next to the test binaries.</summary>
    public static string SampleTile => Path.Combine(AppContext.BaseDirectory, "Samples", "N36W112.hgt");

    public static string Cli => Path.Combine(Root, "x64", "Release", "TerrainEngine.exe");
}
