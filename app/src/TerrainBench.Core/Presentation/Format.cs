using System.Globalization;

namespace TerrainBench.Presentation;

/// <summary>
/// Every number the app shows or writes, formatted the same way on every machine. Culture-specific
/// formatting would put a decimal comma into copied results and CSV files on, say, a Turkish or
/// German Windows, and the command-line tool would then read "0,00027" as garbage.
/// </summary>
public static class Format
{
    public static readonly CultureInfo Invariant = CultureInfo.InvariantCulture;

    public static string Degrees(double value) => value.ToString("0.000000", Invariant) + "°";

    public static string Coordinate(double value) => value.ToString("0.000000", Invariant);

    public static string Metres(double value) => value.ToString("0.00", Invariant) + " m";

    public static string WholeMetres(double value) => value.ToString("0", Invariant) + " m";

    public static string Distance(double metres) =>
        metres >= 1000 ? (metres / 1000).ToString("0.00", Invariant) + " km" : metres.ToString("0", Invariant) + " m";

    public static string Number(double value, string format = "0.###") => value.ToString(format, Invariant);

    public static string Count(int value) => value.ToString("N0", Invariant);

    public static string Percent(double fraction) => (fraction * 100).ToString("0.00", Invariant) + " %";

    public static string Milliseconds(TimeSpan elapsed) =>
        elapsed.TotalSeconds >= 10 ? elapsed.TotalSeconds.ToString("0.0", Invariant) + " s" : elapsed.TotalMilliseconds.ToString("0", Invariant) + " ms";

    /// <summary>The shortest text that reads back as exactly the same double.</summary>
    public static string RoundTrip(double value) => value.ToString("R", Invariant);

    /// <summary>
    /// Reads a number typed by a person. The invariant form ("36.5") always works; the machine's own
    /// form ("36,5" on a Turkish Windows) is accepted too, so nobody has to know which one is expected.
    /// </summary>
    public static bool TryParse(string? text, out double value)
    {
        value = 0;
        if (string.IsNullOrWhiteSpace(text)) return false;

        // A simple fraction, like k = 4/3, is read exactly: 4.0 / 3.0 is the very double the engine
        // uses as its default, which a typed 1.333333 would not be.
        int slash = text.IndexOf('/');
        if (slash >= 0)
        {
            if (!TryParseNumber(text[..slash], out double numerator) || !TryParseNumber(text[(slash + 1)..], out double denominator) || denominator == 0)
            {
                return false;
            }
            value = numerator / denominator;
            return double.IsFinite(value);
        }

        return TryParseNumber(text, out value);
    }

    private static bool TryParseNumber(string text, out double value)
    {
        const NumberStyles styles = NumberStyles.Float;
        return double.TryParse(text.Trim(), styles, Invariant, out value)
            || double.TryParse(text.Trim(), styles, CultureInfo.CurrentCulture, out value);
    }
}
