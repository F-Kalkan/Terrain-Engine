#pragma once
#include <optional>

// A closed set of vertical reference frames a height or elevation might be
// expressed in. Never guess which one a bare number means -- Unknown is the
// honest answer when a source hasn't declared one, and callers must treat
// Unknown as unusable rather than assuming it is safe to combine with anything.
enum class VerticalDatum
{
    EllipsoidalHae,     // height above the WGS84 reference ellipsoid
    OrthometricMsl,     // height above the geoid ("mean sea level")
    PressureAltitude,
    HeightAboveGround,  // AGL -- relative to the local terrain surface
    Unknown
};

struct DatumHeight
{
    double valueM = 0.0;
    VerticalDatum datum = VerticalDatum::Unknown;
};

// Converts a height between the ellipsoidal (HAE) and orthometric (MSL)
// datums using the local geoid undulation -- the only conversion pair with a
// defined formula here. Undulation is REQUIRED, never defaulted to zero,
// because "zero undulation" silently asserts the point sits exactly on the
// geoid, which is true almost nowhere. Any other datum pair (or two equal
// datums aside) has no defined conversion and returns nullopt as a value,
// never a guess.
//
// Complexity: O(1). Thread-safety: pure function, no shared state -- safe to
// call concurrently from any number of threads.
inline std::optional<double> ConvertHeightBetweenDatums(double valueM, VerticalDatum from, VerticalDatum to, double geoidUndulationM)
{
    if (from == to) return valueM;

    if (from == VerticalDatum::OrthometricMsl && to == VerticalDatum::EllipsoidalHae)
    {
        return valueM + geoidUndulationM;
    }
    if (from == VerticalDatum::EllipsoidalHae && to == VerticalDatum::OrthometricMsl)
    {
        return valueM - geoidUndulationM;
    }

    return std::nullopt;
}