using System.Text;
using TerrainBench.Engine;

namespace TerrainBench.Presentation;

/// <summary>The profile as CSV: one row per sample, invariant numbers, voids left empty.</summary>
public static class ProfileCsv
{
    /// <summary>
    /// The columns, each height's name saying what it is measured from -- the datum the engine returned the
    /// path's heights in -- so the file never leaves it to be guessed.
    /// </summary>
    public static string Header(HeightDatum heightsDatum)
    {
        string above = PlainWords.DatumColumn(heightsDatum);
        return $"distance_m,latitude_deg,longitude_deg,elevation_m_{above},curvature_corrected_elevation_m_{above},sight_line_height_m_{above},first_fresnel_radius_m";
    }

    public static string Write(PathAnalysis analysis)
    {
        var text = new StringBuilder();
        text.Append(Header(analysis.HeightsDatum)).Append("\r\n");
        foreach (var sample in analysis.Samples)
        {
            text.Append(Format.RoundTrip(sample.DistanceM)).Append(',')
                .Append(Format.RoundTrip(sample.LatitudeDeg)).Append(',')
                .Append(Format.RoundTrip(sample.LongitudeDeg)).Append(',')
                .Append(Optional(sample.ElevationM)).Append(',')
                .Append(Optional(sample.CurvatureCorrectedElevationM)).Append(',')
                .Append(Optional(sample.SightLineHeightM)).Append(',')
                .Append(Format.RoundTrip(sample.FirstFresnelRadiusM)).Append("\r\n");
        }
        return text.ToString();
    }

    private static string Optional(double? value) => value.HasValue ? Format.RoundTrip(value.Value) : string.Empty;
}

/// <summary>
/// The text "Copy results" puts on the clipboard: versions, tile, inputs and outputs, laid out to
/// paste straight into an issue. Numbers are invariant and round-trip, so a query can be repeated
/// exactly -- including with the command-line tool, whose spacing is in degrees.
/// </summary>
public sealed class ResultsReport
{
    private readonly StringBuilder _text = new();

    public ResultsReport(string appVersion, string engineVersion, string engineCommit)
    {
        _text.Append("TerrainBench ").Append(appVersion)
            .Append(" (engine ").Append(engineVersion).Append(", commit ").Append(engineCommit).Append(")\r\n");
    }

    public ResultsReport Section(string title)
    {
        _text.Append("\r\n## ").Append(title).Append("\r\n");
        return this;
    }

    public ResultsReport Line(string label, string value)
    {
        _text.Append(label).Append(": ").Append(value).Append("\r\n");
        return this;
    }

    public override string ToString() => _text.ToString();
}
