using TerrainBench.Engine;
using TerrainBench.Presentation;
using TerrainBench.ViewModels;
using Xunit;

namespace TerrainBench.Tests.ViewModels;

public class PresentationTests
{
    [Theory]
    [InlineData("36.5", 36.5)]
    [InlineData("  -111.45 ", -111.45)]
    [InlineData("1e3", 1000)]
    public void A_field_reads_a_number_typed_the_invariant_way(string text, double expected)
    {
        var field = new NumberField("k", "Value", "m", text, _ => null);
        Assert.True(field.IsValid);
        Assert.Equal(expected, field.Value);
    }

    [Fact]
    public void A_simple_fraction_is_read_exactly()
    {
        var field = new NumberField("k", "k", "no unit", "4/3", _ => null);
        Assert.Equal(4.0 / 3.0, field.Value);

        field.Text = "1/0";
        Assert.Equal("Enter a number.", field.Error);
    }

    [Fact]
    public void A_field_also_reads_a_decimal_comma_on_a_machine_that_writes_one()
    {
        using var turkish = new CultureScope("tr-TR");
        var field = new NumberField("k", "Value", "m", "36,5", _ => null);
        Assert.Equal(36.5, field.Value);
    }

    [Theory]
    [InlineData("")]
    [InlineData("abc")]
    [InlineData("12abc")]
    [InlineData("NaN")]
    public void A_field_that_is_not_a_number_says_so(string text)
    {
        var field = new NumberField("k", "Value", "m", text, _ => null);
        Assert.False(field.IsValid);
        Assert.Null(field.Value);
        Assert.Equal("Enter a number.", field.Error);
    }

    [Fact]
    public void An_optional_field_may_be_empty_but_not_wrong()
    {
        var field = new NumberField("f", "Frequency", "MHz", "", Rules.Positive("The frequency"), optional: true);
        Assert.True(field.IsValid);
        Assert.Null(field.Value);

        field.Text = "-5";
        Assert.Equal("The frequency must be greater than 0.", field.Error);

        field.Text = "x";
        Assert.Equal("Enter a number, or leave it empty.", field.Error);
    }

    [Fact]
    public void A_rule_says_what_is_allowed()
    {
        var field = new NumberField("h", "Height", "m above ground", "-2", Rules.HeightAboveGround("The observer"));
        Assert.Equal("The observer height must be between 0 and 100000.", field.Error);
    }

    [Theory]
    [InlineData("N36W112.hgt", 36, -112)]
    [InlineData(@"C:\tiles\s05e010.SRTMGL1.hgt", -5, 10)]
    [InlineData("N00E000.hgt", 0, 0)]
    public void The_corner_is_read_from_a_standard_file_name(string name, double lat, double lon)
    {
        Assert.True(HgtFileName.TryParseCorner(name, out double south, out double west));
        Assert.Equal(lat, south);
        Assert.Equal(lon, west);
    }

    [Theory]
    [InlineData("tile.hgt")]
    [InlineData("X36W112.hgt")]
    [InlineData("")]
    public void A_name_without_a_corner_is_not_guessed(string name)
    {
        Assert.False(HgtFileName.TryParseCorner(name, out _, out _));
    }

    [Fact]
    public void Profile_csv_uses_invariant_numbers_and_leaves_voids_empty_even_on_a_turkish_machine()
    {
        using var turkish = new CultureScope("tr-TR");
        var analysis = Analyses.NoConfidentAnswer(ComputationStatus.VoidInProfile);

        var lines = ProfileCsv.Write(analysis).Split("\r\n", StringSplitOptions.RemoveEmptyEntries);

        Assert.Equal(ProfileCsv.Header, lines[0]);
        Assert.Equal(4, lines.Length);
        Assert.Equal("1500.5,36.31,-111.49,,,1740,12.5", lines[2]);
        Assert.DoesNotContain(lines, line => line.Contains(";"));
    }

    [Theory]
    [InlineData(10.0, 300, 2000)]
    [InlineData(74.2, 400, 20000)]
    [InlineData(0.3, 200, 50)]
    public void The_scale_bar_picks_a_round_length_that_fits(double metresPerPixel, double maxPixels, double expected)
    {
        var (length, pixels, _) = ScaleBar.Choose(metresPerPixel, maxPixels);
        Assert.Equal(expected, length);
        Assert.True(pixels <= maxPixels);
    }

    [Fact]
    public void The_viewshed_image_is_north_up_and_marks_disagreements()
    {
        // Row 0 of the engine's grid is the south row, so it must land on the image's bottom row.
        var map = Maps.Of(CellState.Visible, CellState.NotVisible, CellState.Degraded, CellState.NotCovered);
        var image = MapPixels.Viewshed(map, highlights: [false, true, false, false]);

        Assert.Equal(PixelOf(MapPixels.DegradedColour), image[0..4]);          // top-left: engine row 1, col 0
        Assert.Equal(PixelOf(MapPixels.VisibleColour), image[8..12]);          // bottom-left: engine row 0, col 0
        Assert.Equal(PixelOf(MapPixels.DisagreementColour), image[12..16]);    // bottom-right: engine row 0, col 1
    }

    [Fact]
    public void Hidden_viewshed_layers_are_left_out_of_the_image_and_every_shown_state_has_its_own_colour()
    {
        var map = Maps.Of(CellState.Visible, CellState.NotVisible, CellState.Degraded, CellState.NotCovered);
        var hidden = new bool[MapPixels.HighlightLayer + 1];
        hidden[(int)CellState.NotVisible] = true;
        hidden[MapPixels.HighlightLayer] = true;

        var image = MapPixels.Viewshed(map, highlights: [true, false, false, false], hidden: hidden);

        Assert.Equal(PixelOf(MapPixels.VisibleColour), image[8..12]);   // a hidden highlight shows the cell's own state
        Assert.Equal(new byte[4], image[12..16]);                         // not visible, hidden
        Assert.Equal(PixelOf(MapPixels.NotReachedColour), image[4..8]);   // not reached, shown in grey
        Assert.Equal(PixelOf(MapPixels.DegradedColour), image[0..4]);
    }

    [Fact]
    public void A_cell_past_the_tile_has_its_own_colour_and_its_own_layer()
    {
        // Engine row 0: a hole in the tile's data, and a cell past its edge.
        var map = Maps.Of(CellState.Degraded, CellState.DataNotGiven, CellState.Visible, CellState.NotVisible);
        var hidden = new bool[MapPixels.LayerCount];

        var image = MapPixels.Viewshed(map, hidden: hidden);
        Assert.Equal(PixelOf(MapPixels.DegradedColour), image[8..12]);
        Assert.Equal(PixelOf(MapPixels.DataNotGivenColour), image[12..16]);
        Assert.NotEqual(MapPixels.DegradedColour, MapPixels.DataNotGivenColour);

        hidden[(int)CellState.DataNotGiven] = true;
        image = MapPixels.Viewshed(map, hidden: hidden);
        Assert.Equal(new byte[4], image[12..16]);
        Assert.Equal(PixelOf(MapPixels.DegradedColour), image[8..12]);
    }

    [Fact]
    public void A_minimum_visible_height_map_is_coloured_by_its_band_and_a_hidden_band_is_left_out()
    {
        // Engine row 0 (the image's bottom): the ground seen, and 12.5 m. Row 1: no confident
        // answer, and a cell no height makes visible.
        var map = new ViewshedMap(2, 2, 1, 1, 0.00027, 0.00034, 36.5, -111.5,
            [CellState.Visible, CellState.NotVisible, CellState.Degraded, CellState.NotVisible],
            [0, 12.5, double.NaN, double.PositiveInfinity]);
        var hidden = new bool[MapPixels.LayerCount];

        var image = MapPixels.Viewshed(map, hidden: hidden);

        Assert.Equal(PixelOf(MapPixels.HeightBands[0].Colour), image[8..12]);
        Assert.Equal(PixelOf(MapPixels.HeightBands[3].Colour), image[12..16]);
        Assert.Equal(PixelOf(MapPixels.DegradedColour), image[0..4]);
        Assert.Equal(PixelOf(MapPixels.HeightBands[^1].Colour), image[4..8]);

        hidden[MapPixels.FirstHeightLayer + 3] = true;
        Assert.Equal(new byte[4], MapPixels.Viewshed(map, hidden: hidden)[12..16]);
    }

    [Theory]
    [InlineData(0.0, 0)]
    [InlineData(1e-9, 1)]
    [InlineData(2.0, 1)]
    [InlineData(2.0000001, 2)]
    [InlineData(10.0, 2)]
    [InlineData(30.0, 3)]
    [InlineData(100.0, 4)]
    [InlineData(300.0, 5)]
    [InlineData(300.5, 6)]
    [InlineData(1e6, 6)]
    [InlineData(double.PositiveInfinity, 7)]
    [InlineData(double.NaN, -1)]
    public void A_height_falls_in_the_first_band_whose_limit_it_is_at_most(double heightM, int band)
    {
        Assert.Equal(band, MapPixels.HeightBand(heightM));
    }

    [Fact]
    public void The_height_legend_is_in_metres()
    {
        Assert.Equal(
            ["Ground Seen (0 m)", "Up to 2 m", "2 to 10 m", "10 to 30 m", "30 to 100 m", "100 to 300 m", "Over 300 m", "Not Seen at Any Height"],
            MapPixels.HeightBands.Select(b => b.Name));
    }

    private static byte[] PixelOf(Rgba colour)
    {
        float a = colour.A / 255f;
        return [(byte)(colour.B * a), (byte)(colour.G * a), (byte)(colour.R * a), colour.A];
    }
}
