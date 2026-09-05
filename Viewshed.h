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
ViewshedResult ComputeViewshedNaive(GeoPoint observer, double observerHeight, int gridRows, int gridCols, double spacing, IElevationSampler& sampler)
{
    ViewshedResult result;
    result.visible.resize(gridRows, std::vector<std::optional<bool>>(gridCols, std::nullopt));

    for (int row = 0; row < gridRows; row++)
    {
        for (int col = 0; col < gridCols; col++)
        {
            
            GeoPoint target{ 
                (double)row, 
                (double)col 
            };
            
            double distance = sqrt(pow(target.latitude - observer.latitude, 2) + pow(target.longitude - observer.longitude, 2));

            if (distance == 0)
            {
                result.visible[row][col] = true;
                continue;
            }

            std::vector<ProfileSample> profile = GetTerrainProfile(observer, target, spacing, sampler);
            LineOfSightResult los = ComputeLineOfSight(profile, observerHeight, 0, distance);
            
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
ViewshedResult ComputeViewshedFast(GeoPoint observer, double observerHeight, int gridRows, int gridCols, double spacing, IElevationSampler& sampler)
{
    ViewshedResult result;
    result.visible.resize(gridRows, std::vector<std::optional<bool>>(gridCols, std::nullopt));

    const double k = 4.0 / 3.0;
    const double R = 6371000.0;

    auto observerElevation = sampler.GetElevation(observer.latitude, observer.longitude);
    
    if (!observerElevation.has_value())
    {
        return result;
    }
    double observerEyeHeight = *observerElevation + observerHeight;

    std::vector<GeoPoint> boundaryCells;
    
    for (int col = 0; col < gridCols; col++)
    {
        boundaryCells.push_back(GeoPoint{ 0, (double)col });
        boundaryCells.push_back(GeoPoint{ (double)(gridRows - 1), (double)col });
    }
    
    for (int row = 0; row < gridRows; row++)
    {
        boundaryCells.push_back(GeoPoint{ (double)row, 0 });
        boundaryCells.push_back(GeoPoint{ (double)row, (double)(gridCols - 1) });
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
            int row = (int)round(profile[i].point.latitude);
            int col = (int)round(profile[i].point.longitude);
            
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

    int obsRow = (int)round(observer.latitude);
    int obsCol = (int)round(observer.longitude);
    
    if (obsRow >= 0 && obsRow < gridRows && obsCol >= 0 && obsCol < gridCols)
    {
        result.visible[obsRow][obsCol] = true;
    }

    return result;
}