using Avalonia;
using Avalonia.Media;

namespace TerrainBench.Controls;

/// <summary>Where the tour's card goes, and its arrow, for a page pointing at <c>Hole</c>.</summary>
public readonly record struct TourLayout(Point Card, Point? ArrowFrom, Point? ArrowTo);

/// <summary>
/// Places the tour's card beside the control a page points at, with room for an arrow between them:
/// to the side with space, else below or above, else inside a control too big to go round (the map),
/// pointing down into it. A page that points at nothing gets the card in the middle.
/// </summary>
public static class TourPlacement
{
    /// <summary>The arrow's length: the gap between the card and the control.</summary>
    public const double Gap = 64;

    private const double Margin = 12;
    private const double Inset = 24;

    public static TourLayout Place(Size window, Size card, Rect? hole)
    {
        if (hole is not { } h)
            return new TourLayout(new Point((window.Width - card.Width) / 2, (window.Height - card.Height) / 2), null, null);

        double ClampX(double x) => Math.Clamp(x, Margin, Math.Max(Margin, window.Width - card.Width - Margin));
        double ClampY(double y) => Math.Clamp(y, Margin, Math.Max(Margin, window.Height - card.Height - Margin));

        // The arrow's point along the card's edge: facing the control's middle, kept off the card's corners and inside the control.
        double AlongY(Point c) => Math.Clamp(Math.Clamp(h.Center.Y, c.Y + Inset, c.Y + card.Height - Inset), h.Top + 8, Math.Max(h.Top + 8, h.Bottom - 8));
        double AlongX(Point c) => Math.Clamp(Math.Clamp(h.Center.X, c.X + Inset, c.X + card.Width - Inset), h.Left + 8, Math.Max(h.Left + 8, h.Right - 8));

        // Beside a control taller than the card, the card lines up with its top, leaving what is under it in sight.
        double SideY() => ClampY(h.Height > card.Height ? h.Top : h.Center.Y - card.Height / 2);

        TourLayout? Left()
        {
            var c = new Point(h.Left - Gap - card.Width, SideY());
            if (c.X < Margin) return null;
            double y = AlongY(c);
            return new TourLayout(c, new Point(c.X + card.Width + 6, y), new Point(h.Left - 4, y));
        }

        TourLayout? Right()
        {
            var c = new Point(h.Right + Gap, SideY());
            if (c.X + card.Width > window.Width - Margin) return null;
            double y = AlongY(c);
            return new TourLayout(c, new Point(c.X - 6, y), new Point(h.Right + 4, y));
        }

        TourLayout? Below()
        {
            var c = new Point(ClampX(h.Center.X - card.Width / 2), h.Bottom + Gap);
            if (c.Y + card.Height > window.Height - Margin) return null;
            double x = AlongX(c);
            return new TourLayout(c, new Point(x, c.Y - 6), new Point(x, h.Bottom + 4));
        }

        TourLayout? Above()
        {
            var c = new Point(ClampX(h.Center.X - card.Width / 2), h.Top - Gap - card.Height);
            if (c.Y < Margin) return null;
            double x = AlongX(c);
            return new TourLayout(c, new Point(x, c.Y + card.Height + 6), new Point(x, h.Top - 4));
        }

        // A control on the right has its room on the left, and the other way round.
        var sides = h.Center.X > window.Width / 2
            ? new Func<TourLayout?>[] { Left, Below, Above, Right }
            : [Right, Below, Above, Left];
        foreach (var side in sides)
        {
            if (side() is { } layout) return layout;
        }

        // Too big to go round: inside it, near its top, pointing down into it.
        var inside = new Point(ClampX(h.Center.X - card.Width / 2), ClampY(h.Top + Inset));
        double cx = inside.X + card.Width / 2;
        return new TourLayout(inside, new Point(cx, inside.Y + card.Height + 6), new Point(cx, inside.Y + card.Height + 6 + Gap));
    }

    /// <summary>A line with a filled head at <paramref name="to"/>.</summary>
    public static Geometry Arrow(Point from, Point to)
    {
        var delta = to - from;
        double length = Math.Sqrt(delta.X * delta.X + delta.Y * delta.Y);
        var geometry = new StreamGeometry();
        if (length < 1) return geometry;

        var along = delta / length;
        var across = new Vector(-along.Y, along.X);
        var neck = to - along * 16;
        using var context = geometry.Open();
        context.BeginFigure(from, false);
        context.LineTo(neck);
        context.EndFigure(false);
        context.BeginFigure(to, true);
        context.LineTo(neck + across * 10);
        context.LineTo(neck - across * 10);
        context.EndFigure(true);
        return geometry;
    }
}
