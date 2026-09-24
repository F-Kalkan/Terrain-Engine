#pragma once
#include <vector>
#include "IElevationSampler.h"
#include <cmath>
#include <optional>
#include <algorithm>
#include <climits>


struct GeoPoint
{
    double latitudeDeg = 0.0;
    double longitudeDeg = 0.0;
};

struct ProfileSample
{
    GeoPoint point;
    std::optional<double> elevationM;
    double distanceFromStartM = 0.0;
    VerticalDatum elevationDatum = VerticalDatum::Unknown; // the datum elevationM is expressed in
};

// Mean Earth radius, in metres, modelled as a sphere. Declared once here --
// the header every distance and curvature calculation in this library already
// depends on -- so LineOfSight.h, Viewshed.h and the great-circle functions
// below all share this exact value instead of each redeclaring their own local
// copy of the same constant.
inline constexpr double EarthRadiusM = 6371000.0;

inline constexpr double DegToRad = 3.14159265358979323846 / 180.0;

// Why an entry point refused its input: the value that says a question was outside
// the function's domain, instead of a confident answer to it or undefined behaviour.
enum class InputProblem
{
    None,
    SpacingNotPositive,         // a spacing that is zero, negative, NaN or infinite
    SpacingTooFine,             // a spacing so fine the path's sample count won't fit in an int
    CoordinateNotFinite,        // a latitude or longitude that is NaN or infinite
    LatitudeOutOfRange,         // a latitude beyond +-90 degrees
    HeightNotFinite,            // a height, or its geoid undulation, that is NaN or infinite
    CurvatureFactorNotPositive, // a refraction factor k that is zero, negative, NaN or infinite
    FrequencyNotPositive,       // a Fresnel frequency that is zero, negative, NaN or infinite
    GridBeyondPole,             // a viewshed grid whose rows would reach a pole or past it
    HeightBelowGround,          // a target height above ground that is negative: a target in the ground
    RadiusNotPositive,          // a radius that is zero, negative, NaN or infinite
};

// Complexity: O(1). Thread-safety: pure function, safe to call concurrently.
inline const char* InputProblemToString(InputProblem problem)
{
    switch (problem)
    {
    case InputProblem::None: return "none";
    case InputProblem::SpacingNotPositive: return "spacing is not a positive finite number";
    case InputProblem::SpacingTooFine: return "spacing is too fine: the sample count would overflow";
    case InputProblem::CoordinateNotFinite: return "a coordinate is not a finite number";
    case InputProblem::LatitudeOutOfRange: return "a latitude is beyond 90 degrees";
    case InputProblem::HeightNotFinite: return "a height or its undulation is not a finite number";
    case InputProblem::CurvatureFactorNotPositive: return "k is not a positive finite number";
    case InputProblem::FrequencyNotPositive: return "the frequency is not a positive finite number";
    case InputProblem::GridBeyondPole: return "the viewshed grid would reach a pole";
    case InputProblem::HeightBelowGround: return "a target height above ground is below the ground";
    case InputProblem::RadiusNotPositive: return "the radius is not a positive finite number";
    }
    return "unknown";
}

// A point on the Earth: both coordinates finite, the latitude within +-90 degrees.
// Longitude is not range-checked: any finite longitude names a meridian.
//
// Complexity: O(1). Thread-safety: pure function, safe to call concurrently.
inline InputProblem CheckPoint(GeoPoint point)
{
    if (!std::isfinite(point.latitudeDeg) || !std::isfinite(point.longitudeDeg)) return InputProblem::CoordinateNotFinite;
    if (std::abs(point.latitudeDeg) > 90.0) return InputProblem::LatitudeOutOfRange;
    return InputProblem::None;
}

// Great-circle (haversine) distance between two geodetic points, in metres,
// over EarthRadiusM's sphere.
//
// Complexity: O(1). Thread-safety: pure function, safe to call concurrently.
inline double GreatCircleDistanceM(GeoPoint a, GeoPoint b)
{
    double lat1Rad = a.latitudeDeg * DegToRad;
    double lat2Rad = b.latitudeDeg * DegToRad;
    double dLatRad = (b.latitudeDeg - a.latitudeDeg) * DegToRad;
    double dLonRad = (b.longitudeDeg - a.longitudeDeg) * DegToRad;

    double sinDLat2 = sin(dLatRad / 2.0);
    double sinDLon2 = sin(dLonRad / 2.0);
    double h = sinDLat2 * sinDLat2 + cos(lat1Rad) * cos(lat2Rad) * sinDLon2 * sinDLon2;
    if (h > 1.0) h = 1.0; // guard floating-point overshoot past 1 when a and b coincide

    double centralAngleRad = 2.0 * atan2(sqrt(h), sqrt(1.0 - h));
    return EarthRadiusM * centralAngleRad;
}

// The point at fractional distance t (0 = a, 1 = b) along the great-circle arc
// between a and b, given that arc's own central angle in radians -- passed in
// rather than recomputed so a caller walking many samples along one path (like
// GetTerrainProfile below) pays for the haversine calculation once, not once
// per sample.
//
// Complexity: O(1). Thread-safety: pure function, safe to call concurrently.
inline GeoPoint GreatCircleInterpolate(GeoPoint a, GeoPoint b, double t, double centralAngleRad)
{
    if (centralAngleRad < 1e-12)
    {
        return a; // a and b are (numerically) the same point -- nothing to interpolate
    }

    double lat1Rad = a.latitudeDeg * DegToRad;
    double lon1Rad = a.longitudeDeg * DegToRad;
    double lat2Rad = b.latitudeDeg * DegToRad;
    double lon2Rad = b.longitudeDeg * DegToRad;

    double sinAngle = sin(centralAngleRad);
    double coeffA = sin((1.0 - t) * centralAngleRad) / sinAngle;
    double coeffB = sin(t * centralAngleRad) / sinAngle;

    double x = coeffA * cos(lat1Rad) * cos(lon1Rad) + coeffB * cos(lat2Rad) * cos(lon2Rad);
    double y = coeffA * cos(lat1Rad) * sin(lon1Rad) + coeffB * cos(lat2Rad) * sin(lon2Rad);
    double z = coeffA * sin(lat1Rad) + coeffB * sin(lat2Rad);

    GeoPoint result;
    result.latitudeDeg = atan2(z, sqrt(x * x + y * y)) / DegToRad;
    result.longitudeDeg = atan2(y, x) / DegToRad;
    return result;
}

// The number of intervals a path is sampled in: the distance over the spacing,
// rounded UP so the effective spacing never exceeds the request, and at least one.
// Floor used to drop a sample whenever the count came out as 999.9999..., which a
// viewshed's axis rays hit by construction (floating-point noise, and a great
// circle a few mm shorter than the parallel) -- leaving a cell on the ray
// unvisited. The (1 - 1e-9) factor stops that noise adding a spurious extra sample
// instead. Infinite for a spacing that isn't positive; beyond INT_MAX for one too fine.
//
// Complexity: O(1). Thread-safety: pure function, safe to call concurrently.
inline double ProfileIntervals(GeoPoint startPoint, GeoPoint endPoint, double spacingDeg)
{
    double intervalsNeeded = GreatCircleDistanceM(startPoint, endPoint) / (spacingDeg * EarthRadiusM * DegToRad);
    return (std::max)(1.0, ceil(intervalsNeeded * (1.0 - 1e-9)));
}

// Whether GetTerrainProfile can sample this path: both ends on the Earth
// (CheckPoint), and a spacing that is a positive finite number of degrees, coarse
// enough that the interval count fits in an int. A spacing of zero or less once
// came back as a confident two-sample profile -- the endpoints alone, as if nothing
// stood between them.
//
// Complexity: O(1). Thread-safety: pure function, safe to call concurrently.
inline InputProblem CheckProfileRequest(GeoPoint startPoint, GeoPoint endPoint, double spacingDeg)
{
    if (InputProblem point = CheckPoint(startPoint); point != InputProblem::None) return point;
    if (InputProblem point = CheckPoint(endPoint); point != InputProblem::None) return point;
    if (!std::isfinite(spacingDeg) || spacingDeg <= 0.0) return InputProblem::SpacingNotPositive;
    if (!(ProfileIntervals(startPoint, endPoint, spacingDeg) <= (double)INT_MAX - 1)) return InputProblem::SpacingTooFine;
    return InputProblem::None;
}

// === FRAME-SAFE LINE-OF-SIGHT PATH ===
// Fills outProfile in place instead of returning a freshly allocated vector.
// outProfile.clear() drops its elements but keeps its underlying storage, so a
// caller that owns one buffer and reuses it every frame allocates nothing here
// past the first call whose profile is the longest it will ever need -- the
// shape a per-frame line-of-sight query needs. This is the profile-building half
// of the frame-safe path; pair it with
// ComputeLineOfSight/ComputeFresnelClearance (LineOfSight.h), which are the
// other half. The by-value overload below is the BATCH PATH equivalent, used
// where a per-call allocation is acceptable (batch, viewshed, tests).
//
// Sample points are walked along the true great-circle arc between startPoint
// and endPoint (GreatCircleInterpolate), not a straight line in degree-space --
// a degree of longitude is not the same ground distance as a degree of
// latitude, and a great circle between two points that share a latitude
// (other than the equator) bulges toward the pole rather than following that
// latitude. distanceFromStartM is exact by construction (t * the arc's own
// total length), not re-derived per sample.
//
// spacingDeg still means degrees, matching every other caller of this
// function (the viewsheds lay out their grid in degree steps too) -- it is
// converted to an equivalent metre spacing via EarthRadiusM to decide how many
// samples the path needs, rather than assuming a fixed metres-per-degree
// constant.
//
// Complexity: O(totalDistanceM / (spacingDeg in metres)), i.e. O(sample count).
// Thread-safety: caller-synchronised. Safe to call concurrently from multiple
// threads as long as each call passes its own outProfile and sampler is safe
// for concurrent GetElevation calls (IElevationSampler makes no guarantee of
// its own -- see each implementation).
// A request CheckProfileRequest refuses leaves outProfile empty and returns why;
// ComputeLineOfSight and ComputeFresnelClearance answer an empty profile with
// EmptyOrSingleSampleProfile, never Ok. Otherwise returns InputProblem::None.
inline InputProblem GetTerrainProfile(GeoPoint startPoint, GeoPoint endPoint, double spacingDeg, IElevationSampler& sampler, std::vector<ProfileSample>& outProfile)
{
    outProfile.clear();

    InputProblem problem = CheckProfileRequest(startPoint, endPoint, spacingDeg);
    if (problem != InputProblem::None) return problem;

    double totalDistanceM = GreatCircleDistanceM(startPoint, endPoint);
    double centralAngleRad = totalDistanceM / EarthRadiusM;
    int sampleCount = (int)ProfileIntervals(startPoint, endPoint, spacingDeg);

    for (int i = 0; i <= sampleCount; i++)
    {
        double t = (double)i / sampleCount;
        GeoPoint current = GreatCircleInterpolate(startPoint, endPoint, t, centralAngleRad);

        ProfileSample sample;
        sample.point = current;
        sample.elevationM = sampler.GetElevation(current.latitudeDeg, current.longitudeDeg);
        sample.distanceFromStartM = t * totalDistanceM;
        sample.elevationDatum = sampler.GetDatum();

        outProfile.push_back(sample);
    }
    return InputProblem::None;
}

// === BATCH PATH ===
// Owns its own buffer instead of borrowing one -- convenient for a one-off
// call (a CLI command, a test, a single query) but not for a per-frame loop,
// which should call the in-place overload above with a buffer it keeps alive
// across frames instead. ComputeBatchLineOfSight (LineOfSight.h) and the
// viewsheds (Viewshed.h) use this overload internally for the same reason:
// none of them are frame-safe callers to begin with.
//
// Complexity: O(totalDistanceM / (spacingDeg in metres)), same as the in-place
// overload -- this is a two-line wrapper that owns its own buffer instead of
// reusing one.
// Thread-safety: caller-synchronised, same conditions as the in-place overload.
// A request CheckProfileRequest refuses comes back empty; that function says why.
inline std::vector<ProfileSample> GetTerrainProfile(GeoPoint startPoint, GeoPoint endPoint, double spacingDeg, IElevationSampler& sampler)
{
    std::vector<ProfileSample> result;
    GetTerrainProfile(startPoint, endPoint, spacingDeg, sampler, result);
    return result;
}
