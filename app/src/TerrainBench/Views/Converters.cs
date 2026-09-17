using System.Globalization;
using Avalonia.Data.Converters;
using Avalonia.Media;
using TerrainBench.Presentation;

namespace TerrainBench.Views;

public static class Converters
{
    /// <summary>A legend swatch's colour, as the map draws it.</summary>
    public static readonly IValueConverter RgbaToBrush = new FuncValueConverter<Rgba, IBrush>(c => new SolidColorBrush(Color.FromArgb(c.A, c.R, c.G, c.B)));

    /// <summary>A map zoom factor as a whole percentage, e.g. 1.5 as "150%".</summary>
    public static readonly IValueConverter ZoomPercent = new FuncValueConverter<double, string>(zoom => Math.Round(zoom * 100).ToString("0", CultureInfo.InvariantCulture) + "%");

    public static readonly IValueConverter Percent = new FuncValueConverter<double, string>(fraction => (fraction * 100).ToString("0", CultureInfo.InvariantCulture) + " %");
}
