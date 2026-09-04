#pragma once
#include <vector>
#include "LineOfSight.h"
#include <cmath>

struct ViewshedResult
{
    std::vector<std::vector<bool>> visible;
};

ViewshedResult ComputeViewshedNaive(GeoPoint observer, double observerHeight, int gridRows, int gridCols, double spacing, IElevationSampler& sampler)
{
    ViewshedResult result;
    result.visible.resize(gridRows, std::vector<bool>(gridCols, false));

    for (int row = 0; row < gridRows; row++)
    {
        for (int col = 0; col < gridCols; col++)
        {
            GeoPoint target{ (double)row, (double)col };
            double distance = sqrt(pow(target.latitude - observer.latitude, 2) + pow(target.longitude - observer.longitude, 2));

            if (distance == 0)
            {
                result.visible[row][col] = true;
                continue;
            }

            std::vector<ProfileSample> profile = GetTerrainProfile(observer, target, spacing, sampler);
            LineOfSightResult los = ComputeLineOfSight(profile, observerHeight, 0, distance);
            result.visible[row][col] = los.isVisible;
        }
    }

    return result;
}