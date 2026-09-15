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

// A height together with the datum it's measured in. geoidUndulationM is the
// local geoid-ellipsoid separation at the height's own position, needed only to
// move an absolute height between EllipsoidalHae and OrthometricMsl. It stays
// absent -- never zero -- unless the caller supplies it, because "zero" would
// silently assert the point sits exactly on the geoid.
struct DatumHeight
{
    double valueM = 0.0;
    VerticalDatum datum = VerticalDatum::Unknown;
    std::optional<double> geoidUndulationM;
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

// Terrain can only be described in an absolute elevation datum. HeightAboveGround
// is relative to the terrain itself, and PressureAltitude isn't a geometric
// height at all.
//
// Complexity: O(1). Thread-safety: pure function, safe to call concurrently.
inline bool IsTerrainElevationDatum(VerticalDatum datum)
{
    return datum == VerticalDatum::OrthometricMsl || datum == VerticalDatum::EllipsoidalHae;
}

// An eye height as an absolute height in the terrain's own datum, or nullopt
// when that can't be done honestly: terrain not in an elevation datum, a height
// in PressureAltitude or Unknown, or an EllipsoidalHae<->OrthometricMsl move
// with no geoid undulation to make it with. groundElevationM is the terrain under
// that height, in terrainDatum, and is used only for HeightAboveGround.
//
// Complexity: O(1). Thread-safety: pure function, safe to call concurrently.
inline std::optional<double> EyeHeightInTerrainDatum(const DatumHeight& height, double groundElevationM, VerticalDatum terrainDatum)
{
    if (!IsTerrainElevationDatum(terrainDatum)) return std::nullopt;
    if (height.datum == VerticalDatum::HeightAboveGround) return groundElevationM + height.valueM;
    if (!IsTerrainElevationDatum(height.datum)) return std::nullopt;
    if (height.datum == terrainDatum) return height.valueM;
    if (!height.geoidUndulationM.has_value()) return std::nullopt;
    return ConvertHeightBetweenDatums(height.valueM, height.datum, terrainDatum, *height.geoidUndulationM);
}

// Whether EyeHeightInTerrainDatum can succeed. That never depends on the ground
// elevation, so it can be decided before the ground under the height is known.
//
// Complexity: O(1). Thread-safety: pure function, safe to call concurrently.
inline bool CanExpressInTerrainDatum(const DatumHeight& height, VerticalDatum terrainDatum)
{
    return EyeHeightInTerrainDatum(height, 0.0, terrainDatum).has_value();
}