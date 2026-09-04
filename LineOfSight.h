#pragma once
#include <optional>
#include "TerrainProfile.h"

struct LineOfSightResult
{
    bool isVisible;
    std::optional<GeoPoint> blockingPoint;
    std::optional<double> blockingElevation;
    double clearanceDeficit;
};

LineOfSightResult ComputeLineOfSight(std::vector<ProfileSample> profile, double hA, double hB)
{
    LineOfSightResult result;
    result.isVisible = true;
    result.clearanceDeficit = 0;


    double observerEyeHeight = profile.front().elevation + hA;
    double targetEyeHeight = profile.back().elevation + hB;

    double worstDeficit = -999999;

    for (int i = 0; i < profile.size(); i++)
    {
        double t = (double)i / (profile.size() - 1);

        double lineHeight = observerEyeHeight + t * (targetEyeHeight - observerEyeHeight);

        double deficit = profile[i].elevation - lineHeight;

        if (deficit > worstDeficit)
        {
            worstDeficit = deficit;

            if (deficit > 0)
            {
                result.isVisible = false;
                result.blockingPoint = profile[i].point;
                result.blockingElevation = profile[i].elevation;
                result.clearanceDeficit = deficit;
            }
        }
    }

    return result;
}