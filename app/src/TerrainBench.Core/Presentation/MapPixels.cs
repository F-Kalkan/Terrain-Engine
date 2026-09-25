using TerrainBench.Engine;

namespace TerrainBench.Presentation;

/// <summary>
/// Colours pixels for the map: the tile by elevation and the viewshed by cell state. Pure
/// presentation -- every elevation and every cell state comes from the engine; nothing here knows
/// a distance or a slope. Images are BGRA, premultiplied, row 0 at the top (north).
/// </summary>
public static class MapPixels
{
    /// <summary>Pixel colour of a post with no data.</summary>
    public static readonly Rgba NoData = new(96, 96, 104, 255);

    // Visible is the answer people look for, so it is the brightest; not visible only darkens the
    // terrain a little, so the ground stays readable. No confident answer is a strong purple and not
    // reached a grey, so all four states stay distinct on the map, beyond the tile's edge too.
    public static readonly Rgba VisibleColour = new(0, 180, 216, 205);
    public static readonly Rgba NotVisibleColour = new(0, 0, 0, 64);
    public static readonly Rgba DegradedColour = new(176, 64, 214, 185);
    public static readonly Rgba NotReachedColour = new(170, 170, 170, 120);
    public static readonly Rgba DisagreementColour = new(255, 214, 0, 235);

    /// <summary>
    /// Past the open tile: a slate blue, apart from the purple of a hole in the tile's own data.
    /// The map draws only the part of a run on the tile, so this shows mostly along its edge.
    /// </summary>
    public static readonly Rgba DataNotGivenColour = new(84, 110, 150, 175);

    /// <summary>Index of the comparison highlights in a hidden-layers array, after the five cell states.</summary>
    public const int HighlightLayer = 5;

    /// <summary>
    /// The minimum-visible-height legend, low to high: each band holds the cells whose height is
    /// above the previous band's limit and at most its own. The ground seen is the viewshed's
    /// visible colour; then green through yellow and orange to red and a dark wine for the heights
    /// that take a mast or a tower, and near-black for cells no height makes visible.
    /// </summary>
    public static readonly IReadOnlyList<(string Name, double UpToM, Rgba Colour)> HeightBands =
    [
        ("Ground Seen (0 m)", 0, VisibleColour),
        ("Up to 2 m", 2, new Rgba(102, 187, 106, 205)),
        ("2 to 10 m", 10, new Rgba(212, 225, 87, 205)),
        ("10 to 30 m", 30, new Rgba(255, 193, 7, 210)),
        ("30 to 100 m", 100, new Rgba(251, 110, 0, 215)),
        ("100 to 300 m", 300, new Rgba(211, 47, 47, 220)),
        ("Over 300 m", double.MaxValue, new Rgba(110, 20, 60, 225)),
        ("Not Seen at Any Height", double.PositiveInfinity, new Rgba(0, 0, 0, 150)),
    ];

    /// <summary>Index of the first height band in a hidden-layers array, after the highlights.</summary>
    public const int FirstHeightLayer = HighlightLayer + 1;

    /// <summary>How many layers a hidden-layers array covers: the four states, the highlights and every height band.</summary>
    public static int LayerCount => FirstHeightLayer + HeightBands.Count;

    /// <summary>The band a minimum visible height falls in, or -1 for NaN (no confident answer).</summary>
    public static int HeightBand(double heightM)
    {
        if (double.IsNaN(heightM)) return -1;
        for (int band = 0; band < HeightBands.Count; band++)
        {
            if (heightM <= HeightBands[band].UpToM) return band;
        }
        return HeightBands.Count - 1;
    }

    /// <summary>
    /// The tile as an image, one pixel per post, coloured along a low-to-high ramp between the
    /// tile's own lowest and highest elevations. Returns the image and those two elevations.
    /// </summary>
    public static (byte[] Bgra, float MinM, float MaxM) Elevation(TilePosts posts)
    {
        float min = float.MaxValue, max = float.MinValue;
        for (int i = 0; i < posts.ElevationsM.Length; i++)
        {
            if (posts.Valid[i] == 0) continue;
            min = Math.Min(min, posts.ElevationsM[i]);
            max = Math.Max(max, posts.ElevationsM[i]);
        }
        if (min > max) { min = 0; max = 1; }
        float range = Math.Max(max - min, 1f);

        var bgra = new byte[posts.ElevationsM.Length * 4];
        for (int i = 0; i < posts.ElevationsM.Length; i++)
        {
            var colour = posts.Valid[i] == 0 ? NoData : Ramp((posts.ElevationsM[i] - min) / range);
            Write(bgra, i, colour);
        }

        return (bgra, min, max);
    }

    /// <summary>
    /// A viewshed as an image, one pixel per cell, flipped so north is up. When
    /// <paramref name="highlights"/> is given, those cells are painted as highlights instead. A
    /// minimum-visible-height map colours each confident cell by its <see cref="HeightBands"/> band.
    /// <paramref name="hidden"/>, indexed by <see cref="CellState"/>, then <see cref="HighlightLayer"/>,
    /// then the height bands from <see cref="FirstHeightLayer"/>, leaves those layers out.
    /// </summary>
    public static byte[] Viewshed(ViewshedMap map, bool[]? highlights = null, bool[]? hidden = null)
    {
        bool Hidden(int layer) => hidden is not null && layer < hidden.Length && hidden[layer];

        var bgra = new byte[map.Rows * map.Cols * 4];
        for (int row = 0; row < map.Rows; row++)
        {
            int imageRow = map.Rows - 1 - row;
            for (int col = 0; col < map.Cols; col++)
            {
                int cell = row * map.Cols + col;
                var state = map.Cells[cell];
                int band = map.HeightsM is { } heights ? HeightBand(heights[cell]) : -1;
                Rgba colour;
                if (highlights is not null && highlights[cell] && !Hidden(HighlightLayer)) colour = DisagreementColour;
                else if (band >= 0)
                {
                    if (Hidden(FirstHeightLayer + band)) continue;
                    colour = HeightBands[band].Colour;
                }
                else if (Hidden((int)state)) continue;
                else colour = Colour(state);
                Write(bgra, imageRow * map.Cols + col, colour);
            }
        }
        return bgra;
    }

    public static Rgba Colour(CellState state) => state switch
    {
        CellState.Visible => VisibleColour,
        CellState.NotVisible => NotVisibleColour,
        CellState.Degraded => DegradedColour,
        CellState.DataNotGiven => DataNotGivenColour,
        _ => NotReachedColour,
    };

    /// <summary>Low ground green, through tan and brown, to pale grey on the highest ground.</summary>
    public static Rgba Ramp(float t)
    {
        t = Math.Clamp(t, 0f, 1f);
        (float Stop, Rgba Colour)[] stops =
        [
            (0.00f, new Rgba(46, 125, 50, 255)),
            (0.35f, new Rgba(198, 184, 120, 255)),
            (0.70f, new Rgba(141, 93, 60, 255)),
            (1.00f, new Rgba(238, 236, 230, 255)),
        ];

        for (int i = 1; i < stops.Length; i++)
        {
            if (t > stops[i].Stop) continue;
            float local = (t - stops[i - 1].Stop) / (stops[i].Stop - stops[i - 1].Stop);
            return Rgba.Lerp(stops[i - 1].Colour, stops[i].Colour, local);
        }
        return stops[^1].Colour;
    }

    private static void Write(byte[] bgra, int pixel, Rgba colour)
    {
        int o = pixel * 4;
        float a = colour.A / 255f;
        bgra[o] = (byte)(colour.B * a);
        bgra[o + 1] = (byte)(colour.G * a);
        bgra[o + 2] = (byte)(colour.R * a);
        bgra[o + 3] = colour.A;
    }
}

public readonly record struct Rgba(byte R, byte G, byte B, byte A)
{
    public static Rgba Lerp(Rgba from, Rgba to, float t) => new(
        (byte)(from.R + (to.R - from.R) * t),
        (byte)(from.G + (to.G - from.G) * t),
        (byte)(from.B + (to.B - from.B) * t),
        (byte)(from.A + (to.A - from.A) * t));
}
