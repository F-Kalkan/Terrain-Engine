#pragma once
#include <vector>
#include "LineOfSight.h"
#include <cmath>
#include <optional>


struct ViewshedResult
{
    std::vector<std::vector<std::optional<bool>>> visible;
};

//Naive ViewShed
inline ViewshedResult ComputeViewshedNaive(GeoPoint observer, double observerHeight, int gridRows, int gridCols, double spacing, IElevationSampler& sampler, double k = 4.0 / 3.0)
{
    ViewshedResult result;
    result.visible.resize(gridRows, std::vector<std::optional<bool>>(gridCols, std::nullopt));

    int centerRow = gridRows / 2;
    int centerCol = gridCols / 2;

    for (int row = 0; row < gridRows; row++)
    {
        for (int col = 0; col < gridCols; col++)
        {
            GeoPoint target{ observer.latitude + (row - centerRow) * spacing, observer.longitude + (col - centerCol) * spacing };
            double distance = sqrt(pow(target.latitude - observer.latitude, 2) + pow(target.longitude - observer.longitude, 2));

            if (distance == 0)
            {
                result.visible[row][col] = true;
                continue;
            }

            std::vector<ProfileSample> profile = GetTerrainProfile(observer, target, spacing, sampler);
            LineOfSightResult los = ComputeLineOfSight(profile, observerHeight, 0, distance, k);

            if (los.isDegraded)
            {
                result.visible[row][col] = std::nullopt;
            }
            
            else
            {
                result.visible[row][col] = los.isVisible;
            }
        }
    }

    return result;
}

//Fast ViewShed
inline ViewshedResult ComputeViewshedFast(GeoPoint observer, double observerHeight, int gridRows, int gridCols, double spacing, IElevationSampler& sampler, double k = 4.0 / 3.0)
{
    ViewshedResult result;
    result.visible.resize(gridRows, std::vector<std::optional<bool>>(gridCols, std::nullopt));

    const double R = 6371000.0;

    int centerRow = gridRows / 2;
    int centerCol = gridCols / 2;

    auto observerElevation = sampler.GetElevation(observer.latitude, observer.longitude);
    
    if (!observerElevation.has_value())
    {
        return result;
    }
    double observerEyeHeight = *observerElevation + observerHeight;

    std::vector<GeoPoint> boundaryCells;
    
    for (int col = 0; col < gridCols; col++)
    {
        boundaryCells.push_back(GeoPoint{ observer.latitude + (0 - centerRow) * spacing, observer.longitude + (col - centerCol) * spacing });
        boundaryCells.push_back(GeoPoint{ observer.latitude + (gridRows - 1 - centerRow) * spacing, observer.longitude + (col - centerCol) * spacing });
    }
    
    for (int row = 0; row < gridRows; row++)
    {
        boundaryCells.push_back(GeoPoint{ observer.latitude + (row - centerRow) * spacing, observer.longitude + (0 - centerCol) * spacing });
        boundaryCells.push_back(GeoPoint{ observer.latitude + (row - centerRow) * spacing, observer.longitude + (gridCols - 1 - centerCol) * spacing });
    }

    for (const auto& target : boundaryCells)
    {
        double totalDistance = sqrt(pow(target.latitude - observer.latitude, 2) + pow(target.longitude - observer.longitude, 2));
        
        if (totalDistance == 0) continue;

        std::vector<ProfileSample> profile = GetTerrainProfile(observer, target, spacing, sampler);

        double maxSlope = -1e18;
        int lastRow = -9999;
        int lastCol = -9999;

        for (int i = 1; i < profile.size(); i++)
        {
            int row = (int)round((profile[i].point.latitude - observer.latitude) / spacing) + centerRow;
            int col = (int)round((profile[i].point.longitude - observer.longitude) / spacing) + centerCol;
            if (row < 0 || row >= gridRows || col < 0 || col >= gridCols) continue;

            if (row == lastRow && col == lastCol) continue;
            
            lastRow = row;
            lastCol = col;

            double t = (double)i / (profile.size() - 1);
            double d = t * totalDistance;

            if (!profile[i].elevation.has_value())
            {
                result.visible[row][col] = std::nullopt;
                continue;
            }

            double dRemain = totalDistance - d;
            double curvatureDrop = (d * dRemain) / (2 * k * R);
            double pointHeight = *profile[i].elevation + curvatureDrop;

            double slope = (pointHeight - observerEyeHeight) / d;

            bool isVisible = slope >= maxSlope;
            
            if (isVisible)
            {
                maxSlope = slope;
            }

            result.visible[row][col] = isVisible;
        }
    }

    result.visible[centerRow][centerCol] = true;

    return result;
}