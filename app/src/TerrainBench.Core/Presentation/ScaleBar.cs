namespace TerrainBench.Presentation;

/// <summary>
/// Picks a round length for a map scale bar. The metres a pixel covers come from the engine
/// (a great-circle distance across the map); this only chooses 1, 2 or 5 times a power of ten.
/// </summary>
public static class ScaleBar
{
    public static (double LengthM, double Pixels, string Label) Choose(double metresPerPixel, double maxPixels)
    {
        if (!(metresPerPixel > 0) || !(maxPixels > 0)) return (0, 0, string.Empty);

        double maxMetres = metresPerPixel * maxPixels;
        double power = Math.Pow(10, Math.Floor(Math.Log10(maxMetres)));
        double length = power;
        foreach (double step in new[] { 5.0, 2.0, 1.0 })
        {
            if (step * power <= maxMetres)
            {
                length = step * power;
                break;
            }
        }

        string label = length >= 1000
            ? (length / 1000).ToString("0.###", Format.Invariant) + " km"
            : length.ToString("0.###", Format.Invariant) + " m";
        return (length, length / metresPerPixel, label);
    }
}
