#pragma once
#include <optional>
#include "TerrainProfile.h"
#include "VerticalDatum.h"
#include <string>

enum class TerrainFeatureType
{
    Unknown,
    LocalPeak,
    RisingSlope,
    FallingSlope,
    Plateau
};

// Complexity: O(1). Thread-safety: pure function, safe to call concurrently.
inline std::string TerrainFeatureTypeToString(TerrainFeatureType type)
{
    switch (type)
    {
    case TerrainFeatureType::LocalPeak: return "local peak / ridge";
    case TerrainFeatureType::RisingSlope: return "rising slope";
    case TerrainFeatureType::FallingSlope: return "falling slope";
    case TerrainFeatureType::Plateau: return "plateau / flat ground";
    default: return "unknown";
    }
}

// Complexity: O(1) -- looks only at index-1, index and index+1.
// Thread-safety: pure function over its inputs, safe to call concurrently.
inline TerrainFeatureType ClassifyBlockingFeature(const std::vector<ProfileSample>& profile, int index)
{
    if (index <= 0 || index >= (int)profile.size() - 1) return TerrainFeatureType::Unknown;
    if (!profile[index - 1].elevationM.has_value() || !profile[index + 1].elevationM.has_value()) return TerrainFeatureType::Unknown;

    // Tolerance scales with sample spacing so the same physical grade
    // classifies the same way regardless of profile resolution -- a fixed
    // metre tolerance would call a real slope "flat" once spacing got fine
    // enough that consecutive elevation deltas shrank below it (and read the
    // opposite way at coarse spacing). 1% grade is the "essentially flat"
    // threshold; the tiny floor only guards against zero spacing.
    const double flatGradeThreshold = 0.01;
    double spacingInM = profile[index].distanceFromStartM - profile[index - 1].distanceFromStartM;
    double spacingOutM = profile[index + 1].distanceFromStartM - profile[index].distanceFromStartM;
    double epsilonInM = spacingInM * flatGradeThreshold;
    double epsilonOutM = spacingOutM * flatGradeThreshold;
    if (epsilonInM < 1e-6) epsilonInM = 1e-6;
    if (epsilonOutM < 1e-6) epsilonOutM = 1e-6;

    double beforeM = *profile[index - 1].elevationM;
    double atM = *profile[index].elevationM;
    double afterM = *profile[index + 1].elevationM;

    double risingInM = atM - beforeM;   // positive: still climbing into this point
    double risingOutM = afterM - atM;   // positive: still climbing past this point

    if (risingInM > epsilonInM && risingOutM < -epsilonOutM) return TerrainFeatureType::LocalPeak;
    if (std::abs(risingInM) <= epsilonInM && std::abs(risingOutM) <= epsilonOutM) return TerrainFeatureType::Plateau;
    if (risingInM > epsilonInM || risingOutM > epsilonOutM) return TerrainFeatureType::RisingSlope;
    return TerrainFeatureType::FallingSlope;
}

// Distinct reasons a computation might not have a confident answer, instead of
// collapsing them into one "isDegraded" bool that could mean any of several
// unrelated things. IsOk(status) is true exactly when isVisible/
// minClearanceFraction is a real, confident answer; anything else says why it
// is not, rather than just that it is not.
enum class ComputationStatus
{
    Ok,
    EmptyOrSingleSampleProfile, // profile.size() < 2 -- nothing to walk
    EndpointMissing,            // the observer's or target's own elevation is void
    VoidInProfile,              // some interior sample along the path is void
    DatumRejected,              // observerHeightAgl/targetHeightAgl isn't HeightAboveGround, or the terrain's datum is Unknown
    NothingEvaluated            // Fresnel only: no interior sample existed to test at all
};

// Complexity: O(1). Thread-safety: pure function, safe to call concurrently.
inline bool IsOk(ComputationStatus status)
{
    return status == ComputationStatus::Ok;
}

// Complexity: O(1). Thread-safety: pure function, safe to call concurrently.
inline std::string ComputationStatusToString(ComputationStatus status)
{
    switch (status)
    {
    case ComputationStatus::Ok: return "ok";
    case ComputationStatus::EmptyOrSingleSampleProfile: return "empty or single-sample profile";
    case ComputationStatus::EndpointMissing: return "endpoint elevation missing";
    case ComputationStatus::VoidInProfile: return "void encountered in profile";
    case ComputationStatus::DatumRejected: return "datum rejected";
    case ComputationStatus::NothingEvaluated: return "nothing evaluated";
    default: return "unknown";
    }
}

struct LineOfSightResult
{
    bool isVisible = false;
    ComputationStatus status = ComputationStatus::Ok;
    std::optional<GeoPoint> blockingPoint;
    std::optional<double> blockingElevationM;
    double clearanceDeficitM = 0.0;
    TerrainFeatureType blockingFeature = TerrainFeatureType::Unknown;
};

// === FRAME-SAFE LINE-OF-SIGHT PATH (INTEGRATION-READINESS.md section 7/10) ===
// Pair with GetTerrainProfile's in-place overload (TerrainProfile.h): fill a
// caller-owned buffer there, pass it here by const&. Neither step allocates,
// so a per-frame query (one sensor, one target, one frame) costs one O(samples)
// pass and nothing else -- the shape a real-time host needs.
//
// observerHeightAgl/targetHeightAgl must be tagged HeightAboveGround -- that is
// the only datum this function's arithmetic (terrain elevation + a relative
// offset) is valid for. terrainDatum is the sampler's own declared datum
// (IElevationSampler::GetDatum()); Unknown means the terrain values themselves
// can't be trusted, so the query is rejected as a value (status = DatumRejected),
// never silently guessed.
//
// Complexity: O(profile.size()) -- one pass over the samples, no allocation of
// its own (the caller-owned profile is taken by const&).
// Thread-safety: pure function over its inputs (reads profile, never mutates
// it), safe to call concurrently from multiple threads as long as no thread is
// concurrently mutating the same profile buffer.
inline LineOfSightResult ComputeLineOfSight(const std::vector<ProfileSample>& profile, DatumHeight observerHeightAgl, DatumHeight targetHeightAgl, VerticalDatum terrainDatum, double k = 4.0 / 3.0)
{
    const double R = 6371000.0;

    LineOfSightResult result;
    result.isVisible = true;
    result.clearanceDeficitM = 0;
    double worstDeficitM = -999999;

    if (observerHeightAgl.datum != VerticalDatum::HeightAboveGround || targetHeightAgl.datum != VerticalDatum::HeightAboveGround || terrainDatum == VerticalDatum::Unknown)
    {
        result.status = ComputationStatus::DatumRejected;
        return result;
    }

    if (profile.size() < 2)
    {
        result.status = ComputationStatus::EmptyOrSingleSampleProfile;
        return result;
    }

    if (!profile.front().elevationM.has_value() || !profile.back().elevationM.has_value())
    {
        result.status = ComputationStatus::EndpointMissing;
        return result;
    }

    double totalDistanceM = profile.back().distanceFromStartM;

    double observerEyeHeightM = *profile.front().elevationM + observerHeightAgl.valueM;
    double targetEyeHeightM = *profile.back().elevationM + targetHeightAgl.valueM;

    for (size_t i = 0; i < profile.size(); i++)
    {
        if (!profile[i].elevationM.has_value())
        {
            result.status = ComputationStatus::VoidInProfile;
            continue;
        }

        double t = (double)i / (profile.size() - 1);
        double lineHeightM = observerEyeHeightM + t * (targetEyeHeightM - observerEyeHeightM);

        double d1M = profile[i].distanceFromStartM;
        double d2M = totalDistanceM - d1M;
        double curvatureDropM = (d1M * d2M) / (2 * k * R);

        double correctedElevationM = *profile[i].elevationM + curvatureDropM;
        double deficitM = correctedElevationM - lineHeightM;

        if (deficitM > worstDeficitM)
        {
            worstDeficitM = deficitM;
            if (deficitM > 0)
            {
                result.isVisible = false;
                result.blockingPoint = profile[i].point;
                result.blockingElevationM = profile[i].elevationM;
                result.clearanceDeficitM = deficitM;
                result.blockingFeature = ClassifyBlockingFeature(profile, (int)i);
            }
        }
    }

    return result;
}

struct FresnelClearanceResult
{
    double minClearanceFraction = 0.0;
    std::optional<GeoPoint> worstPoint;
    ComputationStatus status = ComputationStatus::Ok;
};

// === FRAME-SAFE LINE-OF-SIGHT PATH (INTEGRATION-READINESS.md section 7/10) ===
// Same shape and same guarantee as ComputeLineOfSight above: pair with
// GetTerrainProfile's in-place overload and this allocates nothing per call.
//
// Complexity: O(profile.size()) -- one pass over the samples, no allocation of
// its own (the caller-owned profile is taken by const&).
// Thread-safety: pure function over its inputs, safe to call concurrently from
// multiple threads as long as no thread is concurrently mutating the same
// profile buffer.
inline FresnelClearanceResult ComputeFresnelClearance(const std::vector<ProfileSample>& profile, DatumHeight observerHeightAgl, DatumHeight targetHeightAgl, VerticalDatum terrainDatum, double frequencyHz, double k = 4.0 / 3.0)
{
    const double R = 6371000.0;
    const double c = 299792458.0; // speed of light, m/s
    double wavelengthM = c / frequencyHz;

    FresnelClearanceResult result;
    double bestKnownFraction = 1e18;

    if (observerHeightAgl.datum != VerticalDatum::HeightAboveGround || targetHeightAgl.datum != VerticalDatum::HeightAboveGround || terrainDatum == VerticalDatum::Unknown)
    {
        result.status = ComputationStatus::DatumRejected;
        return result;
    }

    if (profile.size() < 2)
    {
        result.status = ComputationStatus::EmptyOrSingleSampleProfile;
        return result;
    }

    if (!profile.front().elevationM.has_value() || !profile.back().elevationM.has_value())
    {
        result.status = ComputationStatus::EndpointMissing;
        return result;
    }

    double totalDistanceM = profile.back().distanceFromStartM;

    double observerEyeHeightM = *profile.front().elevationM + observerHeightAgl.valueM;
    double targetEyeHeightM = *profile.back().elevationM + targetHeightAgl.valueM;

    for (size_t i = 0; i < profile.size(); i++)
    {
        if (!profile[i].elevationM.has_value())
        {
            result.status = ComputationStatus::VoidInProfile;
            continue;
        }

        double t = (double)i / (profile.size() - 1);
        double d1M = profile[i].distanceFromStartM;
        double d2M = totalDistanceM - d1M;

        if (d1M <= 0 || d2M <= 0) continue; // Fresnel radius is 0 at the antennas themselves

        double lineHeightM = observerEyeHeightM + t * (targetEyeHeightM - observerEyeHeightM);
        double curvatureDropM = (d1M * d2M) / (2 * k * R);
        double correctedElevationM = *profile[i].elevationM + curvatureDropM;

        double clearanceM = lineHeightM - correctedElevationM; // positive = clear of terrain
        double fresnelRadiusM = sqrt(wavelengthM * d1M * d2M / totalDistanceM);
        double fraction = clearanceM / fresnelRadiusM;

        if (fraction < bestKnownFraction)
        {
            bestKnownFraction = fraction;
            result.worstPoint = profile[i].point;
        }
    }

    if (bestKnownFraction >= 1e18)
    {
        // Only overwrite if the loop hasn't already flagged the more specific
        // VoidInProfile cause -- both can be true for the same profile
        // (a void plus a too-short remainder), and VoidInProfile is more
        // actionable to a caller than "nothing evaluated".
        if (result.status == ComputationStatus::Ok)
        {
            result.status = ComputationStatus::NothingEvaluated;
        }
        result.minClearanceFraction = 0.0;
        return result;
    }

    result.minClearanceFraction = bestKnownFraction;
    return result;
}

struct BatchLineOfSightQuery
{
    GeoPoint observer;
    DatumHeight observerHeightAgl;
    GeoPoint target;
    DatumHeight targetHeightAgl;
};

// === BATCH PATH ===
// Not frame-safe: builds and discards one GetTerrainProfile vector per query
// (the BATCH PATH overload of GetTerrainProfile, TerrainProfile.h), same as the
// viewsheds in Viewshed.h. Intended for a one-off batch of queries (a CLI
// command, a test, a planning pass over many observer/target pairs at once),
// not for a per-frame sensor loop -- that caller wants ComputeLineOfSight
// directly, paired with a reused scratch buffer, above.
//
// Complexity: O(queries.size() * average profile length) -- one GetTerrainProfile
// plus one ComputeLineOfSight per query.
// Thread-safety: single-thread-only. Builds one profile at a time in a local,
// per-call vector and calls the non-thread-affine sampler sequentially; not
// internally synchronised for concurrent calls against the same sampler.
inline std::vector<LineOfSightResult> ComputeBatchLineOfSight(const std::vector<BatchLineOfSightQuery>& queries, double spacingDeg, IElevationSampler& sampler, double k = 4.0 / 3.0)
{
    std::vector<LineOfSightResult> results;
    results.reserve(queries.size());
    VerticalDatum terrainDatum = sampler.GetDatum();
    for (const auto& q : queries)
    {
        std::vector<ProfileSample> profile = GetTerrainProfile(q.observer, q.target, spacingDeg, sampler);
        results.push_back(ComputeLineOfSight(profile, q.observerHeightAgl, q.targetHeightAgl, terrainDatum, k));
    }
    return results;
}
