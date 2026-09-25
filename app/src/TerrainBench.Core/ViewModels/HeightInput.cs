using CommunityToolkit.Mvvm.ComponentModel;
using TerrainBench.Engine;
using TerrainBench.Presentation;

namespace TerrainBench.ViewModels;

/// <summary>
/// A height a person types, and what it is measured from: the ground, sea level, or the WGS84 ellipsoid with
/// the geoid undulation at the point. The number's unit and allowed range follow the choice, and the
/// undulation field is asked for only when the ellipsoid is chosen -- where it is then required, so a height
/// above the ellipsoid never reaches the engine without it.
/// </summary>
public sealed partial class HeightInput : ObservableObject
{
    private readonly string _whose;
    private readonly string _aboveGroundHint;
    private readonly string _altitudeHint;

    /// <param name="key">Name the height is remembered under; the datum and undulation add "-datum" and "-undulation".</param>
    /// <param name="whose">"The observer" or "The target", for the messages.</param>
    public HeightInput(string key, string label, string whose, string defaultText, string aboveGroundHint, string? altitudeHint = null)
    {
        _whose = whose;
        _aboveGroundHint = aboveGroundHint;
        _altitudeHint = altitudeHint ?? PlainWords.AltitudeExplanation;
        DatumKey = key + "-datum";
        Metres = new NumberField(key, label, PlainWords.HeightUnit(HeightDatum.AboveGround), defaultText, CheckMetres,
            hint: aboveGroundHint, shortLabel: "Height");
        Undulation = new NumberField(key + "-undulation", label + " Geoid Undulation", "m, geoid above the ellipsoid", "",
            Rules.Between("The geoid undulation", -200, 200), hint: PlainWords.UndulationExplanation, shortLabel: "Geoid Undulation",
            inUse: () => NeedsUndulation, requiredMessage: PlainWords.UndulationRequired);

        Metres.Changed += (_, _) => Changed?.Invoke(this, EventArgs.Empty);
        Undulation.Changed += (_, _) => Changed?.Invoke(this, EventArgs.Empty);
    }

    /// <summary>The number of metres.</summary>
    public NumberField Metres { get; }

    /// <summary>The geoid's height above the ellipsoid at the point; asked for only above the ellipsoid.</summary>
    public NumberField Undulation { get; }

    /// <summary>Both number fields, for checking, remembering and restoring with the rest.</summary>
    public IReadOnlyList<NumberField> Fields => [Metres, Undulation];

    /// <summary>Name the datum is remembered under between runs.</summary>
    public string DatumKey { get; }

    public IReadOnlyList<string> DatumChoices => PlainWords.DatumChoices;

    /// <summary>What a screen reader calls the Measured From list, e.g. "Observer Height Measured From".</summary>
    public string DatumLabel => Metres.Label + " Measured From";

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(DatumIndex), nameof(NeedsUndulation))]
    private HeightDatum _datum = HeightDatum.AboveGround;

    /// <summary>The Measured From list's selection, in <see cref="HeightDatum"/> order.</summary>
    public int DatumIndex
    {
        get => (int)Datum;
        set
        {
            if (value >= 0 && value <= (int)HeightDatum.AboveEllipsoid) Datum = (HeightDatum)value;
        }
    }

    public bool NeedsUndulation => Datum == HeightDatum.AboveEllipsoid;

    public bool IsValid => Metres.IsValid && Undulation.IsValid;

    /// <summary>The height as the engine takes it; only while <see cref="IsValid"/>.</summary>
    public Height Height => new(Metres.Value!.Value, Datum, NeedsUndulation ? Undulation.Value : null);

    /// <summary>Raised when the number, the undulation or the datum changes.</summary>
    public event EventHandler? Changed;

    partial void OnDatumChanged(HeightDatum value)
    {
        Metres.Unit = PlainWords.HeightUnit(value);
        Metres.Hint = value switch
        {
            HeightDatum.AboveSeaLevel => _altitudeHint,
            HeightDatum.AboveEllipsoid => PlainWords.EllipsoidHeightExplanation,
            _ => _aboveGroundHint,
        };
        Metres.Validate();
        Undulation.Validate();
        Changed?.Invoke(this, EventArgs.Empty);
    }

    /// <summary>Above the ground, 0 to 100000 m: nothing stands in the ground. As an altitude, -1000 to 100000 m.</summary>
    private string? CheckMetres(double value) => Datum == HeightDatum.AboveGround
        ? Rules.HeightAboveGround(_whose)(value)
        : Rules.Between($"{_whose} height", -1000, 100000)(value);

    /// <summary>The height as typed, for the report: "2 m above ground", "1915.4 m above the ellipsoid, geoid undulation -21.6 m".</summary>
    public string AsTyped => $"{Metres.Text} {PlainWords.HeightUnit(Datum)}" + (NeedsUndulation ? $", geoid undulation {Undulation.Text} m" : string.Empty);

    /// <summary>Trades heights with another input: number, datum and undulation.</summary>
    public void SwapWith(HeightInput other)
    {
        (Datum, other.Datum) = (other.Datum, Datum);
        (Metres.Text, other.Metres.Text) = (other.Metres.Text, Metres.Text);
        (Undulation.Text, other.Undulation.Text) = (other.Undulation.Text, Undulation.Text);
    }

    public void Remember(IDictionary<string, string> saved)
    {
        foreach (var field in Fields) saved[field.Key] = field.Text;
        saved[DatumKey] = Datum.ToString();
    }

    public void Restore(IReadOnlyDictionary<string, string> saved)
    {
        if (saved.TryGetValue(DatumKey, out var datum) && Enum.TryParse(datum, out HeightDatum parsed) && Enum.IsDefined(parsed)) Datum = parsed;
        foreach (var field in Fields)
        {
            if (saved.TryGetValue(field.Key, out var text)) field.Text = text;
        }
    }
}
