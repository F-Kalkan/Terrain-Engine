#pragma once
#include <vector>
#include "IElevationSampler.h"
#include <cmath>
#include <optional>


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
};

// Mean Earth radius, in metres, modelled as a sphere. Declared once here --
// the header every distance and curvature calculation in this library already
// depends on -- so LineOfSight.h, Viewshed.h and the great-circle functions
// below all share this exact value instead of each redeclaring their own local
// copy of the same constant.
inline constexpr double EarthRadiusM = 6371000.0;

inline constexpr double DegToRad = 3.14159265358979323846 / 180.0;

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

// === FRAME-SAFE LINE-OF-SIGHT PATH (INTEGRATION-READINESS.md section 7/10) ===
// Fills outProfile in place instead of returning a freshly allocated vector.
// outProfile.clear() drops its elements but keeps its underlying storage, so a
// caller that owns one buffer and reuses it every frame allocates nothing here
// past the first call whose profile is the longest it will ever need -- exactly
// the per-frame line-of-sight query INTEGRATION-READINESS.md asks for. This is
// the profile-building half of the frame-safe path; pair it with
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
inline void GetTerrainProfile(GeoPoint startPoint, GeoPoint endPoint, double spacingDeg, IElevationSampler& sampler, std::vector<ProfileSample>& outProfile)
{
    outProfile.clear();

    double totalDistanceM = GreatCircleDistanceM(startPoint, endPoint);
    double centralAngleRad = totalDistanceM / EarthRadiusM;
    double metersPerDegree = EarthRadiusM * DegToRad;

    int sampleCount = (int)(totalDistanceM / (spacingDeg * metersPerDegree));
    if (sampleCount < 1) sampleCount = 1;

    for (int i = 0; i <= sampleCount; i++)
    {
        double t = (double)i / sampleCount;
        GeoPoint current = GreatCircleInterpolate(startPoint, endPoint, t, centralAngleRad);

        ProfileSample sample;
        sample.point = current;
        sample.elevationM = sampler.GetElevation(current.latitudeDeg, current.longitudeDeg);
        sample.distanceFromStartM = t * totalDistanceM;

        outProfile.push_back(sample);
    }
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
inline std::vector<ProfileSample> GetTerrainProfile(GeoPoint startPoint, GeoPoint endPoint, double spacingDeg, IElevationSampler& sampler)
{
    std::vector<ProfileSample> result;
    GetTerrainProfile(startPoint, endPoint, spacingDeg, sampler, result);
    return result;
}
