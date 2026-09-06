#pragma once
#include <optional>
#include "TerrainProfile.h"
#include <string>

enum class TerrainFeatureType
{
    Unknown,
    LocalPeak,
    RisingSlope,
    FallingSlope,
    Plateau
};

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

inline TerrainFeatureType ClassifyBlockingFeature(const std::vector<ProfileSample>& profile, int index)
{
    if (index <= 0 || index >= (int)profile.size() - 1) return TerrainFeatureType::Unknown;
    if (!profile[index - 1].elevation.has_value() || !profile[index + 1].elevation.has_value()) return TerrainFeatureType::Unknown;

    // Tolerance scales with sample spacing so the same physical grade
    // classifies the same way regardless of profile resolution -- a fixed
    // metre tolerance would call a real slope "flat" once spacing got fine
    // enough that consecutive elevation deltas shrank below it (and read the
    // opposite way at coarse spacing). 1% grade is the "essentially flat"
    // threshold; the tiny floor only guards against zero spacing.
    const double flatGradeThreshold = 0.01;
    double spacingIn = profile[index].distanceFromStartM - profile[index - 1].distanceFromStartM;
    double spacingOut = profile[index + 1].distanceFromStartM - profile[index].distanceFromStartM;
    double epsilonIn = spacingIn * flatGradeThreshold;
    double epsilonOut = spacingOut * flatGradeThreshold;
    if (epsilonIn < 1e-6) epsilonIn = 1e-6;
    if (epsilonOut < 1e-6) epsilonOut = 1e-6;

    double before = *profile[index - 1].elevation;
    double at = *profile[index].elevation;
    double after = *profile[index + 1].elevation;

    double risingIn = at - before;   // positive: still climbing into this point
    double risingOut = after - at;   // positive: still climbing past this point

    if (risingIn > epsilonIn && risingOut < -epsilonOut) return TerrainFeatureType::LocalPeak;
    if (std::abs(risingIn) <= epsilonIn && std::abs(risingOut) <= epsilonOut) return TerrainFeatureType::Plateau;
    if (risingIn > epsilonIn || risingOut > epsilonOut) return TerrainFeatureType::RisingSlope;
    return TerrainFeatureType::FallingSlope;
}

struct LineOfSightResult
{
    bool isVisible = false;
    bool isDegraded = false;
    std::optional<GeoPoint> blockingPoint;
    std::optional<double> blockingElevation;
    double clearanceDeficit = 0.0;
    TerrainFeatureType blockingFeature = TerrainFeatureType::Unknown;
};

inline LineOfSightResult ComputeLineOfSight(const std::vector<ProfileSample>& profile, double hA, double hB, double k = 4.0 / 3.0) 
{
    const double R = 6371000.0;

    LineOfSightResult result;
    result.isVisible = true;
    result.isDegraded = false;
    result.clearanceDeficit = 0;
    double worstDeficit = -999999;

    if (profile.size() < 2)
    {
        result.isDegraded = true;
        return result;
    }

    if (!profile.front().elevation.has_value() || !profile.back().elevation.has_value())
    {
        result.isDegraded = true;
        return result;
    }

    double totalDistance = profile.back().distanceFromStartM;

    double observerEyeHeight = *profile.front().elevation + hA;
    double targetEyeHeight = *profile.back().elevation + hB;

    for (size_t i = 0; i < profile.size(); i++)
    {
        if (!profile[i].elevation.has_value())
        {
            result.isDegraded = true;
            continue;
        }

        double t = (double)i / (profile.size() - 1);
        double lineHeight = observerEyeHeight + t * (targetEyeHeight - observerEyeHeight);

        double d1 = profile[i].distanceFromStartM;
        double d2 = totalDistance - d1;
        double curvatureDrop = (d1 * d2) / (2 * k * R);

        double correctedElevation = *profile[i].elevation + curvatureDrop;
        double deficit = correctedElevation - lineHeight;

        if (deficit > worstDeficit)
        {
            worstDeficit = deficit;
            if (deficit > 0)
            {
                result.isVisible = false;
                result.blockingPoint = profile[i].point;
                result.blockingElevation = profile[i].elevation;
                result.clearanceDeficit = deficit;
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
    bool isDegraded = false;
};

inline FresnelClearanceResult ComputeFresnelClearance(const std::vector<ProfileSample>& profile, double hA, double hB, double frequencyHz, double k = 4.0 / 3.0) 
{
    const double R = 6371000.0;
    const double c = 299792458.0; // speed of light, m/s
    double wavelength = c / frequencyHz;

    FresnelClearanceResult result;
    double bestKnownFraction = 1e18;

    if (profile.size() < 2)
    {
        result.isDegraded = true;
        return result;
    }

    if (!profile.front().elevation.has_value() || !profile.back().elevation.has_value())
    {
        result.isDegraded = true;
        return result;
    }

    double totalDistance = profile.back().distanceFromStartM;

    double observerEyeHeight = *profile.front().elevation + hA;
    double targetEyeHeight = *profile.back().elevation + hB;

    for (size_t i = 0; i < profile.size(); i++)
    {
        if (!profile[i].elevation.has_value())
        {
            result.isDegraded = true;
            continue;
        }

        double t = (double)i / (profile.size() - 1);
        double d1 = profile[i].distanceFromStartM;
        double d2 = totalDistance - d1;

        if (d1 <= 0 || d2 <= 0) continue; // Fresnel radius is 0 at the antennas themselves

        double lineHeight = observerEyeHeight + t * (targetEyeHeight - observerEyeHeight);
        double curvatureDrop = (d1 * d2) / (2 * k * R);
        double correctedElevation = *profile[i].elevation + curvatureDrop;

        double clearance = lineHeight - correctedElevation; // positive = clear of terrain
        double fresnelRadius = sqrt(wavelength * d1 * d2 / totalDistance);
        double fraction = clearance / fresnelRadius;

        if (fraction < bestKnownFraction)
        {
            bestKnownFraction = fraction;
            result.worstPoint = profile[i].point;
        }
    }

    if (bestKnownFraction >= 1e18)
    {
        result.isDegraded = true;
        result.minClearanceFraction = 0.0;
        return result;
    }

    result.minClearanceFraction = bestKnownFraction;
    return result;
}

struct BatchLineOfSightQuery
{
    GeoPoint observer;
    double observerHeight = 0.0;
    GeoPoint target;
    double targetHeight = 0.0;
};

inline std::vector<LineOfSightResult> ComputeBatchLineOfSight(const std::vector<BatchLineOfSightQuery>& queries, double spacing, IElevationSampler& sampler, double k = 4.0 / 3.0)
{
    std::vector<LineOfSightResult> results;
    results.reserve(queries.size());
    for (const auto& q : queries)
    {
        std::vector<ProfileSample> profile = GetTerrainProfile(q.observer, q.target, spacing, sampler);
        results.push_back(ComputeLineOfSight(profile, q.observerHeight, q.targetHeight, k));
    }
    return results;
}