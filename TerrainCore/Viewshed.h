#pragma once
#include <vector>
#include "LineOfSight.h"
#include <cmath>
#include <optional>

// A single optional<bool> cannot say why a cell has no confident answer -- "no ray
// ever reached this cell" and "a ray reached it but crossed a void partway" are
// different situations that call for different handling by a caller, so they get
// their own explicit states instead of collapsing into one nullopt.
enum class CellVisibility
{
    NotCovered,
    Degraded,
    Visible,
    NotVisible
};

inline bool IsConfident(CellVisibility v)
{
    return v == CellVisibility::Visible || v == CellVisibility::NotVisible;
}

// A degree of longitude covers less real ground than a degree of latitude away
// from the equator, shrinking by a factor of cos(latitude). A viewshed grid
// that steps by the same number of degrees on both axes is therefore an
// ellipse in real-world terms -- narrower east-west -- not the circle its
// "radius" implies. Widening the longitude step by 1/cos(latitude) makes a
// column step cover the same real distance as a row step, at any latitude.
inline double LongitudeSpacingForLatitude(double spacingDeg, double latitudeDeg)
{
    const double degToRad = 3.14159265358979323846 / 180.0;
    return spacingDeg / cos(latitudeDeg * degToRad);
}

struct ViewshedResult
{
    std::vector<std::vector<CellVisibility>> visible;
};

//Naive ViewShed
inline ViewshedResult ComputeViewshedNaive(GeoPoint observer, double observerHeight, int gridRows, int gridCols, double spacing, IElevationSampler& sampler, double k = 4.0 / 3.0)
{
    ViewshedResult result;
    result.visible.resize(gridRows, std::vector<CellVisibility>(gridCols, CellVisibility::NotCovered));

    int centerRow = gridRows / 2;
    int centerCol = gridCols / 2;
    double lonSpacing = LongitudeSpacingForLatitude(spacing, observer.latitude);

    for (int row = 0; row < gridRows; row++)
    {
        for (int col = 0; col < gridCols; col++)
        {
            if (row == centerRow && col == centerCol)
            {
                result.visible[row][col] = CellVisibility::Visible;
                continue;
            }

            GeoPoint target{ observer.latitude + (row - centerRow) * spacing, observer.longitude + (col - centerCol) * lonSpacing };

            std::vector<ProfileSample> profile = GetTerrainProfile(observer, target, spacing, sampler);
            LineOfSightResult los = ComputeLineOfSight(profile, observerHeight, 0, k);

            if (los.isDegraded)
            {
                result.visible[row][col] = CellVisibility::Degraded;
            }

            else
            {
                result.visible[row][col] = los.isVisible ? CellVisibility::Visible : CellVisibility::NotVisible;
            }
        }
    }

    return result;
}

//Fast ViewShed
inline ViewshedResult ComputeViewshedFast(GeoPoint observer, double observerHeight, int gridRows, int gridCols, double spacing, IElevationSampler& sampler, double k = 4.0 / 3.0)
{
    ViewshedResult result;
    result.visible.resize(gridRows, std::vector<CellVisibility>(gridCols, CellVisibility::NotCovered));

    const double R = 6371000.0;

    int centerRow = gridRows / 2;
    int centerCol = gridCols / 2;
    double lonSpacing = LongitudeSpacingForLatitude(spacing, observer.latitude);

    auto observerElevation = sampler.GetElevation(observer.latitude, observer.longitude);

    if (!observerElevation.has_value())
    {
        return result;
    }
    double observerEyeHeight = *observerElevation + observerHeight;

    std::vector<GeoPoint> boundaryCells;

    for (int col = 0; col < gridCols; col++)
    {
        boundaryCells.push_back(GeoPoint{ observer.latitude + (0 - centerRow) * spacing, observer.longitude + (col - centerCol) * lonSpacing });
        boundaryCells.push_back(GeoPoint{ observer.latitude + (gridRows - 1 - centerRow) * spacing, observer.longitude + (col - centerCol) * lonSpacing });
    }

    for (int row = 0; row < gridRows; row++)
    {
        boundaryCells.push_back(GeoPoint{ observer.latitude + (row - centerRow) * spacing, observer.longitude + (0 - centerCol) * lonSpacing });
        boundaryCells.push_back(GeoPoint{ observer.latitude + (row - centerRow) * spacing, observer.longitude + (gridCols - 1 - centerCol) * lonSpacing });
    }

    for (const auto& target : boundaryCells)
    {
        std::vector<ProfileSample> profile = GetTerrainProfile(observer, target, spacing, sampler);

        double totalDistance = profile.back().distanceFromStartM;

        if (totalDistance == 0) continue;

        double maxSlope = -1e18;
        int lastRow = -9999;
        int lastCol = -9999;
        bool degraded = false;

        for (size_t i = 1; i < profile.size(); i++)
        {
            int row = (int)round((profile[i].point.latitude - observer.latitude) / spacing) + centerRow;
            int col = (int)round((profile[i].point.longitude - observer.longitude) / lonSpacing) + centerCol;
            if (row < 0 || row >= gridRows || col < 0 || col >= gridCols) continue;

            if (row == lastRow && col == lastCol) continue;

            lastRow = row;
            lastCol = col;

            if (!profile[i].elevation.has_value())
            {
                degraded = true;
            }

            if (degraded)
            {
                result.visible[row][col] = CellVisibility::Degraded;
                continue;
            }

            double d = profile[i].distanceFromStartM;

            double dRemain = totalDistance - d;
            double curvatureDrop = (d * dRemain) / (2 * k * R);
            double pointHeight = *profile[i].elevation + curvatureDrop;

            double slope = (pointHeight - observerEyeHeight) / d;

            bool isVisible = slope >= maxSlope;

            if (isVisible)
            {
                maxSlope = slope;
            }

            result.visible[row][col] = isVisible ? CellVisibility::Visible : CellVisibility::NotVisible;
        }
    }

    result.visible[centerRow][centerCol] = CellVisibility::Visible;

    return result;
}