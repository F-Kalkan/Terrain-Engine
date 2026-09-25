#pragma once
#include <cmath>
#include "TerrainProfile.h"
#include "Viewshed.h"

// Which elevation data a query will read, asked before it runs, so a host that loads data
// by region can load exactly that region first. Each function here returns the box, in
// latitude and longitude, around every point the query reads an elevation at -- exactly
// those points, not an estimate of them: each box's edges are points the query reads. A
// reader that interpolates between posts needs the posts around the box too; the tile
// reader's PostsCovering says which (RealElevationSampler.h).

struct GeoExtent
{
    // Why the query would refuse its request, when it would; the box is then empty, as the
    // query reads nothing. None for a request the query takes, however little it reads.
    InputProblem inputProblem = InputProblem::None;

    bool empty = true;
    double southLatDeg = 0.0;
    double northLatDeg = 0.0;
    double westLonDeg = 0.0;
    double eastLonDeg = 0.0;

    void Include(GeoPoint point)
    {
        if (empty)
        {
            southLatDeg = northLatDeg = point.latitudeDeg;
            westLonDeg = eastLonDeg = point.longitudeDeg;
            empty = false;
            return;
        }
        southLatDeg = (std::min)(southLatDeg, point.latitudeDeg);
        northLatDeg = (std::max)(northLatDeg, point.latitudeDeg);
        westLonDeg = (std::min)(westLonDeg, point.longitudeDeg);
        eastLonDeg = (std::max)(eastLonDeg, point.longitudeDeg);
    }

    void Include(const GeoExtent& other)
    {
        if (other.empty) return;
        Include(GeoPoint{ other.southLatDeg, other.westLonDeg });
        Include(GeoPoint{ other.northLatDeg, other.eastLonDeg });
    }

    bool operator==(const GeoExtent& other) const
    {
        return inputProblem == other.inputProblem && empty == other.empty && (empty || (southLatDeg == other.southLatDeg && northLatDeg == other.northLatDeg
            && westLonDeg == other.westLonDeg && eastLonDeg == other.eastLonDeg));
    }
};

// The box around every point GetTerrainProfile(a, b, spacingDeg) reads, to the bit. The
// points are computed as GetTerrainProfile computes them, so the box is theirs, not a
// neighbour's. Along the great circle, longitude only ever moves one way, so its extremes
// are the two ends; latitude can turn once -- a great circle between two points on the
// same parallel bulges toward the pole -- so the samples either side of the turn are
// checked as well. With z = sin(latitude), the path's z(t) is
//     z_a sin((1 - t) w) / sin w  +  z_b sin(t w) / sin w      (w the path's central angle)
// whose derivative is zero where tan(t w) = (z_b - z_a cos w) / (z_a sin w).
// For a path GetTerrainProfile refuses, an empty box carrying its reason: it reads nothing.
//
// Complexity: O(1) -- at most six points computed, whatever the path's length.
// Thread-safety: pure function, safe to call concurrently.
inline GeoExtent ProfileExtent(GeoPoint a, GeoPoint b, double spacingDeg)
{
    GeoExtent extent;
    extent.inputProblem = CheckProfileRequest(a, b, spacingDeg);
    if (extent.inputProblem != InputProblem::None) return extent;

    double totalDistanceM = GreatCircleDistanceM(a, b);
    double centralAngleRad = totalDistanceM / EarthRadiusM;
    int sampleCount = (int)ProfileIntervals(a, b, spacingDeg);
    auto sampleAt = [&](int i) { return GreatCircleInterpolate(a, b, (double)i / sampleCount, centralAngleRad); };

    extent.Include(sampleAt(0));
    extent.Include(sampleAt(sampleCount));

    if (centralAngleRad >= 1e-12)
    {
        const double pi = 3.14159265358979323846;
        double za = std::sin(a.latitudeDeg * DegToRad);
        double zb = std::sin(b.latitudeDeg * DegToRad);
        double turn = std::atan2(zb - za * std::cos(centralAngleRad), za * std::sin(centralAngleRad));
        if (turn < 0) turn += pi;
        if (turn > 0 && turn < centralAngleRad)
        {
            int turnSample = (int)std::floor(turn / centralAngleRad * sampleCount);
            for (int i = turnSample - 1; i <= turnSample + 2; i++)
            {
                if (i >= 0 && i <= sampleCount) extent.Include(sampleAt(i));
            }
        }
    }
    return extent;
}

// Every point a line of sight from observer to target reads: its profile's.
//
// Complexity: O(1). Thread-safety: pure function, safe to call concurrently.
inline GeoExtent LineOfSightExtent(GeoPoint observer, GeoPoint target, double spacingDeg)
{
    return ProfileExtent(observer, target, spacingDeg);
}

// Why a viewshed would refuse to lay out its grid, if it would: its request as
// CheckViewshedRequest sees it, heights and k aside, since they don't change which points
// are read.
//
// Complexity: O(1). Thread-safety: pure function, safe to call concurrently.
inline InputProblem ViewshedGridProblem(GeoPoint observer, int gridRows, double spacingDeg)
{
    const DatumHeight ground{ 0.0, VerticalDatum::HeightAboveGround };
    return CheckViewshedRequest(observer, ground, gridRows, spacingDeg, 4.0 / 3.0, ground);
}

// Every point ComputeViewshedNaive -- or ComputeMinimumVisibleHeightReference, which reads the
// same -- reads over this grid: the observer's own point, and the profile to every cell.
// Empty for a grid with no cells, and for one the viewshed refuses, with the reason.
//
// Complexity: O(gridRows * gridCols). Thread-safety: pure function, safe to call concurrently.
inline GeoExtent NaiveViewshedExtent(GeoPoint observer, int gridRows, int gridCols, double spacingDeg)
{
    GeoExtent extent;
    extent.inputProblem = ViewshedGridProblem(observer, gridRows, spacingDeg);
    if (extent.inputProblem != InputProblem::None || gridRows <= 0 || gridCols <= 0) return extent;

    int centerRow = gridRows / 2;
    int centerCol = gridCols / 2;
    double lonSpacingDeg = LongitudeSpacingForLatitude(spacingDeg, observer.latitudeDeg);
    extent.Include(observer);
    for (int row = 0; row < gridRows; row++)
    {
        for (int col = 0; col < gridCols; col++)
        {
            if (row == centerRow && col == centerCol) continue;
            GeoPoint target{ observer.latitudeDeg + (row - centerRow) * spacingDeg, observer.longitudeDeg + (col - centerCol) * lonSpacingDeg };
            extent.Include(ProfileExtent(observer, target, spacingDeg));
        }
    }
    return extent;
}

// Every point ComputeViewshedFast -- or ComputeMinimumVisibleHeightFast -- reads over this
// grid: the observer's own point, the ray to every boundary cell, and every cell's centre.
// The centres need no reading of their own: they step evenly by row and by column, so the
// boundary cells' centres bound them, and each of those is where its ray ends. Empty for a
// grid with no cells, and for one the viewshed refuses, with the reason.
//
// Complexity: O(gridRows + gridCols). Thread-safety: pure function, safe to call concurrently.
inline GeoExtent FastViewshedExtent(GeoPoint observer, int gridRows, int gridCols, double spacingDeg)
{
    GeoExtent extent;
    extent.inputProblem = ViewshedGridProblem(observer, gridRows, spacingDeg);
    if (extent.inputProblem != InputProblem::None || gridRows <= 0 || gridCols <= 0) return extent;

    int centerRow = gridRows / 2;
    int centerCol = gridCols / 2;
    double lonSpacingDeg = LongitudeSpacingForLatitude(spacingDeg, observer.latitudeDeg);
    auto centre = [&](int row, int col) {
        return GeoPoint{ observer.latitudeDeg + (row - centerRow) * spacingDeg, observer.longitudeDeg + (col - centerCol) * lonSpacingDeg };
    };
    auto ray = [&](int row, int col) {
        if (row != centerRow || col != centerCol) extent.Include(ProfileExtent(observer, centre(row, col), spacingDeg));
    };

    extent.Include(observer);
    for (int col = 0; col < gridCols; col++)
    {
        ray(0, col);
        ray(gridRows - 1, col);
    }
    for (int row = 1; row < gridRows - 1; row++)
    {
        ray(row, 0);
        ray(row, gridCols - 1);
    }
    return extent;
}
