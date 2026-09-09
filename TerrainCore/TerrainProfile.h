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
// Complexity: O(totalDistanceDeg / spacingDeg), i.e. O(sample count).
// Thread-safety: caller-synchronised. Safe to call concurrently from multiple
// threads as long as each call passes its own outProfile and sampler is safe
// for concurrent GetElevation calls (IElevationSampler makes no guarantee of
// its own -- see each implementation).
inline void GetTerrainProfile(GeoPoint startPoint, GeoPoint endPoint, double spacingDeg, IElevationSampler& sampler, std::vector<ProfileSample>& outProfile)
{
    outProfile.clear();

    const double metersPerDegreeLat = 111320.0;
    const double degToRad = 3.14159265358979323846 / 180.0;
    double midLatRad = ((startPoint.latitudeDeg + endPoint.latitudeDeg) / 2.0) * degToRad;
    double cosMidLat = cos(midLatRad);

    double totalDistanceDeg = sqrt(pow(endPoint.latitudeDeg - startPoint.latitudeDeg, 2) + pow(endPoint.longitudeDeg - startPoint.longitudeDeg, 2));

    int sampleCount = (int)(totalDistanceDeg / spacingDeg);
    if (sampleCount < 1) sampleCount = 1;

    for (int i = 0; i <= sampleCount; i++)
    {
        double t = (double)i / sampleCount;

        GeoPoint current;
        current.latitudeDeg = startPoint.latitudeDeg + t * (endPoint.latitudeDeg - startPoint.latitudeDeg);
        current.longitudeDeg = startPoint.longitudeDeg + t * (endPoint.longitudeDeg - startPoint.longitudeDeg);

        double dNorthM = (current.latitudeDeg - startPoint.latitudeDeg) * metersPerDegreeLat;
        double dEastM = (current.longitudeDeg - startPoint.longitudeDeg) * metersPerDegreeLat * cosMidLat;
        double distanceFromStartM = sqrt(dNorthM * dNorthM + dEastM * dEastM);

        ProfileSample sample;
        sample.point = current;
        sample.elevationM = sampler.GetElevation(current.latitudeDeg, current.longitudeDeg);
        sample.distanceFromStartM = distanceFromStartM;

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
// Complexity: O(totalDistanceDeg / spacingDeg), same as the in-place overload --
// this is a two-line wrapper that owns its own buffer instead of reusing one.
// Thread-safety: caller-synchronised, same conditions as the in-place overload.
inline std::vector<ProfileSample> GetTerrainProfile(GeoPoint startPoint, GeoPoint endPoint, double spacingDeg, IElevationSampler& sampler)
{
    std::vector<ProfileSample> result;
    GetTerrainProfile(startPoint, endPoint, spacingDeg, sampler, result);
    return result;
}
