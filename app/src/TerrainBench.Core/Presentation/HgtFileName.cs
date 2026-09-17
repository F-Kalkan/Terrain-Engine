using System.Text.RegularExpressions;

namespace TerrainBench.Presentation;

/// <summary>Reads a tile's south-west corner from the standard SRTM file name, e.g. N36W112.hgt.</summary>
public static partial class HgtFileName
{
    [GeneratedRegex(@"^(?<ns>[NS])(?<lat>\d{2})(?<ew>[EW])(?<lon>\d{3})", RegexOptions.IgnoreCase)]
    private static partial Regex CornerPattern();

    public static bool TryParseCorner(string? path, out double southWestLatitudeDeg, out double southWestLongitudeDeg)
    {
        southWestLatitudeDeg = 0;
        southWestLongitudeDeg = 0;
        if (string.IsNullOrWhiteSpace(path)) return false;

        var match = CornerPattern().Match(System.IO.Path.GetFileName(path));
        if (!match.Success) return false;

        int latitude = int.Parse(match.Groups["lat"].Value, Format.Invariant);
        int longitude = int.Parse(match.Groups["lon"].Value, Format.Invariant);
        southWestLatitudeDeg = match.Groups["ns"].Value.Equals("S", StringComparison.OrdinalIgnoreCase) ? -latitude : latitude;
        southWestLongitudeDeg = match.Groups["ew"].Value.Equals("W", StringComparison.OrdinalIgnoreCase) ? -longitude : longitude;
        return true;
    }
}
