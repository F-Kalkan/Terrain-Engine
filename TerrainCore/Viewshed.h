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

// Complexity: O(1). Thread-safety: pure function, safe to call concurrently.
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
//
// Complexity: O(1). Thread-safety: pure function, safe to call concurrently.
inline double LongitudeSpacingForLatitude(double spacingDeg, double latitudeDeg)
{
    return spacingDeg / cos(latitudeDeg * DegToRad);
}

struct ViewshedResult
{
    std::vector<std::vector<CellVisibility>> visible;
};

// === BATCH VIEWSHED PATH (INTEGRATION-READINESS.md section 7/10) ===
// Both viewsheds below are the batch path, not the frame-safe one: they own
// the BATCH PATH overload of GetTerrainProfile (TerrainProfile.h), allocating a
// fresh profile per cell (Naive) or per ray (Fast). That is the correct budget
// for a viewshed -- it is a planning product, computed at load time or on
// request, not a per-frame cost -- so neither one should be called from a
// per-frame loop. A per-frame caller (one sensor checking one target) wants
// ComputeLineOfSight/ComputeFresnelClearance (LineOfSight.h) paired with
// GetTerrainProfile's in-place overload instead; see the frame-safe path
// documented there.
//
// Complexity: O(gridRows * gridCols * samples per profile) -- one full
// GetTerrainProfile + ComputeLineOfSight per cell. This is the exact-per-cell
// oracle Viewshed's fast path is checked against, not the frame-path algorithm;
// see INTEGRATION-READINESS.md section 7 ("viewshed is a planning product").
// Thread-safety: single-thread-only, as written -- but unlike ComputeViewshedFast
// below, it has no ordering hazard to fix first: each cell is written exactly
// once, by its own independent GetTerrainProfile/ComputeLineOfSight call, so no
// cell's result depends on any other cell's, or on visit order. It could be
// parallelised across rows or cells with no change to its reduction; it simply
// never has been, since nothing in this project calls it per frame either.
inline ViewshedResult ComputeViewshedNaive(GeoPoint observer, DatumHeight observerHeightAgl, int gridRows, int gridCols, double spacingDeg, IElevationSampler& sampler, double k = 4.0 / 3.0)
{
    ViewshedResult result;
    result.visible.resize(gridRows, std::vector<CellVisibility>(gridCols, CellVisibility::NotCovered));

    int centerRow = gridRows / 2;
    int centerCol = gridCols / 2;
    double lonSpacingDeg = LongitudeSpacingForLatitude(spacingDeg, observer.latitudeDeg);

    for (int row = 0; row < gridRows; row++)
    {
        for (int col = 0; col < gridCols; col++)
        {
            if (row == centerRow && col == centerCol)
            {
                result.visible[row][col] = CellVisibility::Visible;
                continue;
            }

            GeoPoint target{ observer.latitudeDeg + (row - centerRow) * spacingDeg, observer.longitudeDeg + (col - centerCol) * lonSpacingDeg };

            std::vector<ProfileSample> profile = GetTerrainProfile(observer, target, spacingDeg, sampler);
            LineOfSightResult los = ComputeLineOfSight(profile, observerHeightAgl, DatumHeight{ 0.0, VerticalDatum::HeightAboveGround }, sampler.GetDatum(), k);

            if (!IsOk(los.status))
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

// Complexity: O(gridRows + gridCols) rays cast from the boundary inward, each
// O(samples per profile) -- far below naive's O(gridRows * gridCols * samples),
// at the cost of being an approximation (see README's stated fast/naive
// tolerance) rather than an exact per-cell answer.
//
// Threading position (INTEGRATION-READINESS.md section 6): deliberately
// serial, not order-independent -- and this is a stated decision, not an
// oversight. The boundary rays below are cast and applied to the grid in a
// fixed, sequential order; where two rays visit the same cell (which happens
// near the observer, where rays converge), the later ray in that fixed order
// overwrites the earlier one ("last ray wins"). That rule is deterministic
// only because the ray order itself never varies -- naively parallelising this
// loop (e.g. one thread per boundary ray) would let completion order decide
// the result instead, breaking the determinism contract silently (same inputs,
// same state, same ordering => same output, every run, every machine).
//
// There is no per-frame pressure to change this: a viewshed call is a
// planning-time cost, computed at load time or on request (see
// INTEGRATION-READINESS.md section 7), so keeping the loop serial costs
// nothing today. If that changes, the reduction must become
// order-independent before the loop is threaded -- e.g. track the best
// (highest) slope seen per cell explicitly, applied under a lock or via
// atomic compare-and-swap, instead of "whichever ray touched this cell last,
// wins".
//
// Thread-safety: single-thread-only. Builds one profile at a time in a local,
// per-ray vector and calls the non-thread-affine sampler sequentially.
inline ViewshedResult ComputeViewshedFast(GeoPoint observer, DatumHeight observerHeightAgl, int gridRows, int gridCols, double spacingDeg, IElevationSampler& sampler, double k = 4.0 / 3.0)
{
    ViewshedResult result;
    result.visible.resize(gridRows, std::vector<CellVisibility>(gridCols, CellVisibility::NotCovered));

    // Naive delegates this same check to ComputeLineOfSight per cell; fast does
    // its own curvature math and never calls it, so it must check itself.
    if (observerHeightAgl.datum != VerticalDatum::HeightAboveGround || sampler.GetDatum() == VerticalDatum::Unknown)
    {
        for (int row = 0; row < gridRows; row++)
        {
            for (int col = 0; col < gridCols; col++)
            {
                result.visible[row][col] = CellVisibility::Degraded;
            }
        }
        return result;
    }

    const double R = EarthRadiusM;

    int centerRow = gridRows / 2;
    int centerCol = gridCols / 2;
    double lonSpacingDeg = LongitudeSpacingForLatitude(spacingDeg, observer.latitudeDeg);

    auto observerElevationM = sampler.GetElevation(observer.latitudeDeg, observer.longitudeDeg);

    if (!observerElevationM.has_value())
    {
        return result;
    }
    double observerEyeHeightM = *observerElevationM + observerHeightAgl.valueM;

    std::vector<GeoPoint> boundaryCells;

    for (int col = 0; col < gridCols; col++)
    {
        boundaryCells.push_back(GeoPoint{ observer.latitudeDeg + (0 - centerRow) * spacingDeg, observer.longitudeDeg + (col - centerCol) * lonSpacingDeg });
        boundaryCells.push_back(GeoPoint{ observer.latitudeDeg + (gridRows - 1 - centerRow) * spacingDeg, observer.longitudeDeg + (col - centerCol) * lonSpacingDeg });
    }

    for (int row = 0; row < gridRows; row++)
    {
        boundaryCells.push_back(GeoPoint{ observer.latitudeDeg + (row - centerRow) * spacingDeg, observer.longitudeDeg + (0 - centerCol) * lonSpacingDeg });
        boundaryCells.push_back(GeoPoint{ observer.latitudeDeg + (row - centerRow) * spacingDeg, observer.longitudeDeg + (gridCols - 1 - centerCol) * lonSpacingDeg });
    }

    for (const auto& target : boundaryCells)
    {
        std::vector<ProfileSample> profile = GetTerrainProfile(observer, target, spacingDeg, sampler);

        double totalDistanceM = profile.back().distanceFromStartM;

        if (totalDistanceM == 0) continue;

        double maxSlope = -1e18;
        int lastRow = -9999;
        int lastCol = -9999;
        bool degraded = false;

        for (size_t i = 1; i < profile.size(); i++)
        {
            int row = (int)round((profile[i].point.latitudeDeg - observer.latitudeDeg) / spacingDeg) + centerRow;
            int col = (int)round((profile[i].point.longitudeDeg - observer.longitudeDeg) / lonSpacingDeg) + centerCol;
            if (row < 0 || row >= gridRows || col < 0 || col >= gridCols) continue;

            if (row == lastRow && col == lastCol) continue;

            lastRow = row;
            lastCol = col;

            if (!profile[i].elevationM.has_value())
            {
                degraded = true;
            }

            if (degraded)
            {
                result.visible[row][col] = CellVisibility::Degraded;
                continue;
            }

            double dM = profile[i].distanceFromStartM;

            double dRemainM = totalDistanceM - dM;
            double curvatureDropM = (dM * dRemainM) / (2 * k * R);
            double pointHeightM = *profile[i].elevationM + curvatureDropM;

            double slope = (pointHeightM - observerEyeHeightM) / dM;

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
