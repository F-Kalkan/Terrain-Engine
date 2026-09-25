using CommunityToolkit.Mvvm.ComponentModel;
using TerrainBench.Presentation;

namespace TerrainBench.ViewModels;

/// <summary>
/// One number a person types: its label, its unit, what it is measured from, and the rule it must
/// satisfy. The text is checked as it changes, so a mistake is shown at the field -- saying what is
/// allowed -- before anything reaches the engine.
/// </summary>
public sealed partial class NumberField : ObservableObject
{
    private readonly Func<double, string?> _rule;
    private readonly Func<bool>? _inUse;
    private readonly string? _requiredMessage;

    /// <param name="inUse">
    /// For a field only some choices call for, such as a geoid undulation: while it returns false the field
    /// is set aside -- no value, no error -- and while it returns true an empty field is an error that says
    /// <paramref name="requiredMessage"/>.
    /// </param>
    public NumberField(string key, string label, string unit, string defaultText, Func<double, string?> rule, bool optional = false, string? hint = null, string? shortLabel = null,
        Func<bool>? inUse = null, string? requiredMessage = null)
    {
        Key = key;
        Label = label;
        DisplayLabel = shortLabel ?? label;
        _unit = unit;
        _hint = hint;
        IsOptional = optional;
        _rule = rule;
        _inUse = inUse;
        _requiredMessage = requiredMessage;
        _text = defaultText;
        Validate();
    }

    /// <summary>Name the value is remembered under between runs.</summary>
    public string Key { get; }

    /// <summary>The full name, e.g. "Observer Latitude": what a screen reader announces and errors are about.</summary>
    public string Label { get; }

    /// <summary>The name shown beside the box; shorter under a heading that already says whose it is, e.g. "Latitude" under "Observer".</summary>
    public string DisplayLabel { get; }

    /// <summary>The unit and, for a height, what it is measured from, e.g. "m above ground".</summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(HasUnit))]
    private string _unit;

    public bool HasUnit => Unit.Length > 0;

    /// <summary>What the field means, shown when the pointer rests on the ? beside its name.</summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(HasHint))]
    private string? _hint;

    public bool HasHint => Hint is not null;

    public bool IsOptional { get; }

    [ObservableProperty]
    private string _text;

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(HasError))]
    private string? _error;

    public bool HasError => Error is not null;

    /// <summary>The parsed value; null while the text is invalid, or empty in an optional field.</summary>
    [ObservableProperty]
    private double? _value;

    public bool IsValid => Error is null;

    public event EventHandler? Changed;

    partial void OnTextChanged(string value)
    {
        Validate();
        Changed?.Invoke(this, EventArgs.Empty);
    }

    /// <summary>Checks the current text again, for rules that depend on something else, like the open tile.</summary>
    public void Validate()
    {
        if (_inUse is not null && !_inUse())
        {
            Value = null;
            Error = null;
            return;
        }

        if (_inUse is not null && _requiredMessage is not null && string.IsNullOrWhiteSpace(Text))
        {
            Value = null;
            Error = _requiredMessage;
            return;
        }

        if (IsOptional && string.IsNullOrWhiteSpace(Text))
        {
            Value = null;
            Error = null;
            return;
        }

        if (!Format.TryParse(Text, out double value) || !double.IsFinite(value))
        {
            Value = null;
            Error = $"Enter a number{(IsOptional ? ", or leave it empty" : string.Empty)}.";
            return;
        }

        Value = value;
        Error = _rule(value);
    }

    /// <summary>Sets the text from a value, e.g. when a marker is placed on the map.</summary>
    public void SetValue(double value, string format = "0.000000") => Text = value.ToString(format, Format.Invariant);
}

/// <summary>Reusable field rules; each returns null when the value is allowed, or what is.</summary>
public static class Rules
{
    public static Func<double, string?> Positive(string what, double max = double.MaxValue) => value =>
        value <= 0 ? $"{what} must be greater than 0." :
        value > max ? $"{what} must be at most {Format.Number(max)}." : null;

    public static Func<double, string?> Between(string what, double min, double max) => value =>
        value < min || value > max ? $"{what} must be between {Format.Number(min)} and {Format.Number(max)}." : null;

    public static Func<double, string?> HeightAboveGround(string what) => Between($"{what} height", 0, 100000);
}
