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

LineOfSightResult ComputeLineOfSight(std::vector<ProfileSample> profile, double hA, double hB, double totalDistance)
{

    const double k = 4.0 / 3.0;
    const double R = 6371000.0;

    LineOfSightResult result;
    result.isVisible = true;
    result.clearanceDeficit = 0;
    double worstDeficit = -999999;

    double observerEyeHeight = profile.front().elevation + hA;
    double targetEyeHeight = profile.back().elevation + hB;

    

    for (int i = 0; i < profile.size(); i++)
    {
        double t = (double)i / (profile.size() - 1);

        double lineHeight = observerEyeHeight + t * (targetEyeHeight - observerEyeHeight);

        double d1 = t * totalDistance;
        double d2 = totalDistance - d1;
        double curvatureDrop = (d1 * d2) / (2 * k * R);
         
        double correctedElevation = profile[i].elevation + curvatureDrop;
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
            }
        }
    }

    return result;
}