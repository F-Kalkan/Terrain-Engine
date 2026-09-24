#pragma once
#include <algorithm>
#include <vector>
#include "LineOfSight.h"
#include "Threads.h"
#include <cmath>
#include <functional>
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

    // True when a progress callback asked the computation to stop. The cells are then
    // only partly computed and must not be read as an answer.
    bool cancelled = false;

    // Set when an input was outside the viewshed's domain (CheckViewshedRequest); the
    // grid is then empty. None otherwise, an empty grid for no rows or no columns included.
    InputProblem inputProblem = InputProblem::None;
};

// Whether a viewshed can be computed: an observer on the Earth, heights and k a line
// of sight can use, a positive finite spacing, and a grid whose every row stays short
// of the poles -- LongitudeSpacingForLatitude divides by cos(latitude), and a row at or
// past a pole has no longitude to lay its columns along. A grid with no rows or no
// columns is not a problem: it has no cells to answer for.
//
// Complexity: O(1). Thread-safety: pure function, safe to call concurrently.
inline InputProblem CheckViewshedRequest(GeoPoint observer, const DatumHeight& observerHeight, int gridRows, double spacingDeg, double k, const DatumHeight& targetHeight)
{
    if (InputProblem point = CheckPoint(observer); point != InputProblem::None) return point;
    if (InputProblem inputs = CheckLineOfSightInputs(observerHeight, targetHeight, k); inputs != InputProblem::None) return inputs;
    if (!std::isfinite(spacingDeg) || spacingDeg <= 0.0) return InputProblem::SpacingNotPositive;
    int rowsFromCentre = gridRows <= 0 ? 0 : (std::max)(gridRows / 2, gridRows - 1 - gridRows / 2);
    if (!(std::abs(observer.latitudeDeg) + rowsFromCentre * spacingDeg < 90.0)) return InputProblem::GridBeyondPole;
    return InputProblem::None;
}

// Optional progress reporting for a long viewshed. Called with the fraction of the
// work done so far -- 0 before the first unit of work, 1 once all of it is done.
// Returning false stops the computation and marks the result cancelled; the final
// report of 1 comes after the work is finished, so its return value is ignored.
// Reporting never changes a computed cell.
using ViewshedProgress = std::function<bool(double fractionDone)>;

// Runs rowWork(row, thread) once for every row in [0, gridRows), spread over
// ThreadsFor(threadCount) threads, each taking the next row not yet taken. Progress is
// reported on the calling thread only -- before each row it takes, with the fraction of rows
// taken so far -- so a callback never runs on a thread its caller didn't start. A progress
// report that returns false stops every thread before its next row, and false is returned.
// At one thread this is a plain loop over the rows, reporting before each.
//
// Thread-safety: rowWork runs on several threads at once; it must write only its own row.
template <class RowWork>
bool ForEachRowOnThreads(int gridRows, int threadCount, const ViewshedProgress& progress, RowWork&& rowWork)
{
    std::atomic<int> nextRow{ 0 };
    std::atomic<bool> stopped{ false };
    RunOnThreads(ThreadsFor(threadCount), [&](int thread, const std::atomic<bool>& failed) {
        while (!stopped.load(std::memory_order_relaxed) && !failed.load(std::memory_order_relaxed))
        {
            int row = nextRow.fetch_add(1, std::memory_order_relaxed);
            if (row >= gridRows) return;
            if (thread == 0 && progress && !progress((double)row / gridRows))
            {
                stopped = true;
                return;
            }
            rowWork(row, thread);
        }
    });
    return !stopped;
}

// === BATCH VIEWSHED PATH ===
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
// oracle the fast viewshed is checked against, not a per-frame algorithm.
// Threads (optional): threadCount rows at a time, 1 by default, 0 for one per hardware
// thread (ForEachRowOnThreads). Each cell is written exactly once, by its own
// GetTerrainProfile and ComputeLineOfSight, so no cell depends on any other or on the order
// rows are taken in: the grid is the same, bit for bit, at any thread count. The sampler is
// read from every thread at once and must allow it, as every sampler in this library does.
// Progress (optional): reported on the calling thread before each row it takes, then once
// at the end.
// Target height (optional): every cell is asked whether a target that high can be seen -- a
// person, a vehicle, a mast. Usually above the ground under each cell; any datum the terrain can be
// put on works, as for the observer. 0 m above ground, the default, asks about the ground itself. A
// target height that can't be put on the terrain's datum leaves every cell but the observer's Degraded.
inline ViewshedResult ComputeViewshedNaive(GeoPoint observer, DatumHeight observerHeight, int gridRows, int gridCols, double spacingDeg, IElevationSampler& sampler, double k = 4.0 / 3.0, const ViewshedProgress& progress = nullptr, DatumHeight targetHeight = DatumHeight{ 0.0, VerticalDatum::HeightAboveGround }, int threadCount = 1)
{
    ViewshedResult result;

    // A grid with no rows or no columns has no cells to answer for, not even the
    // observer's own: return it empty rather than writing into it.
    if (gridRows <= 0 || gridCols <= 0) return result;

    // An input it can't use is refused, with the reason, rather than laid out into a grid.
    result.inputProblem = CheckViewshedRequest(observer, observerHeight, gridRows, spacingDeg, k, targetHeight);
    if (result.inputProblem != InputProblem::None) return result;

    result.visible.resize(gridRows, std::vector<CellVisibility>(gridCols, CellVisibility::NotCovered));

    int centerRow = gridRows / 2;
    int centerCol = gridCols / 2;
    double lonSpacingDeg = LongitudeSpacingForLatitude(spacingDeg, observer.latitudeDeg);

    // The observer's own cell is only Visible if something is known about the
    // observer: ground under it that isn't void, and a height that can be put on
    // the terrain's datum. Otherwise it is Degraded like every other cell --
    // the same answer ComputeViewshedFast gives.
    bool observerKnown = sampler.GetElevation(observer.latitudeDeg, observer.longitudeDeg).has_value()
        && CanExpressInTerrainDatum(observerHeight, sampler.GetDatum());

    bool finished = ForEachRowOnThreads(gridRows, threadCount, progress, [&](int row, int) {
        for (int col = 0; col < gridCols; col++)
        {
            if (row == centerRow && col == centerCol)
            {
                result.visible[row][col] = observerKnown ? CellVisibility::Visible : CellVisibility::Degraded;
                continue;
            }

            GeoPoint target{ observer.latitudeDeg + (row - centerRow) * spacingDeg, observer.longitudeDeg + (col - centerCol) * lonSpacingDeg };

            std::vector<ProfileSample> profile = GetTerrainProfile(observer, target, spacingDeg, sampler);
            LineOfSightResult los = ComputeLineOfSight(profile, observerHeight, targetHeight, k);

            if (!IsOk(los.status))
            {
                result.visible[row][col] = CellVisibility::Degraded;
            }

            else
            {
                result.visible[row][col] = los.isVisible ? CellVisibility::Visible : CellVisibility::NotVisible;
            }
        }
    });
    if (!finished)
    {
        result.cancelled = true;
        return result;
    }

    if (progress) progress(1.0);
    return result;
}

// A point at distance dM with height heightM is seen from an eye at eyeM when its
// curvature-adjusted slope is at least that of every point nearer the eye:
//
//     s(d) = (height - eye) / d  -  d / (2 k R)
//
// This is ComputeLineOfSight's own test rearranged. Raised by the drop
// d (D - d) / (2 k R) against the chord to a target D away, a point blocks when
// (height + d (D - d) / 2kR - eye) / d exceeds the target's (heightT - eye) / D; the
// D / 2kR term is common to both sides and cancels, leaving s(d) against s(D).
// Unlike the raw slope, s doesn't depend on how far away the target is, so one
// running maximum along a ray serves every target on it.
//
// Complexity: O(1). Thread-safety: pure function, safe to call concurrently.
inline double CurvatureAdjustedSlope(double heightM, double eyeM, double dM, double k)
{
    return (heightM - eyeM) / dM - dM / (2 * k * EarthRadiusM);
}

// The part of the fast viewshed that doesn't depend on the question asked of a cell.
// Casts a ray from the observer to every cell on the grid's boundary and records,
// along each, the horizon: the largest curvature-adjusted slope of the terrain so
// far. Then visits every cell but the observer's with the horizon in front of its
// centre, taken from the two rays either side of it and blended by the cell's
// direction between them. The ray's terrain within half a cell of the target is left
// out of its horizon: it lies in the target's own cell, beside the line to the centre
// rather than on it. A cell with no ground under its centre, or whose rays met a void
// before reaching it, is marked Degraded in `cells` and not visited.
//
// answerCell(row, col, groundM, dM, horizon) is called for every other cell, with the
// ground under its centre, its distance from the observer and the horizon in front of
// it (-infinity when no terrain stands in front of it). ComputeViewshedFast asks it
// whether a target is seen; ComputeMinimumVisibleHeightFast (MinimumVisibleHeight.h),
// how high a target must stand to be seen.
//
// Returns false, leaving the cells partly answered, when progress asked it to stop.
// Progress: reported before every 16th ray and before every grid row of the answering
// pass. `cells` must already be gridRows x gridCols.
//
// Complexity: O(gridRows + gridCols) rays, each O(samples per profile) to cast;
// then O(log rays + 1) per cell. Memory: one float per ray sample, about
// 4 * gridSize * gridSize * 0.7 of them -- ~45 MB for a 30 km radius at 30 m.
//
// Threading position: single-thread-only as written, but order-independent: every
// ray is cast on its own, and every cell is then answered on its own from the
// finished rays, so no cell's answer depends on the order anything was visited in.
// Either loop could be split across threads without changing a single cell.
// Thread-safety: single-thread-only. Calls the non-thread-affine sampler sequentially.
template <class AnswerCell>
bool AnswerEachCellFromFastHorizons(GeoPoint observer, double observerEyeHeightM, int gridRows, int gridCols, double spacingDeg, IElevationSampler& sampler, double k, const ViewshedProgress& progress, std::vector<std::vector<CellVisibility>>& cells, AnswerCell&& answerCell)
{
    int centerRow = gridRows / 2;
    int centerCol = gridCols / 2;

    double lonSpacingDeg = LongitudeSpacingForLatitude(spacingDeg, observer.latitudeDeg);
    auto cellCentre = [&](int row, int col) {
        return GeoPoint{ observer.latitudeDeg + (row - centerRow) * spacingDeg, observer.longitudeDeg + (col - centerCol) * lonSpacingDeg };
    };
    // A direction on the grid, measured in cells: rows and columns are the same length on the ground.
    auto directionOf = [&](int row, int col) { return std::atan2((double)(row - centerRow), (double)(col - centerCol)); };

    // One ray to each boundary cell. Samples are evenly spaced along it, sample i at
    // i * stepM, so only their horizon is kept: horizon[i - 1] covers samples 1 to i.
    struct Ray
    {
        double direction = 0;
        double stepM = 0;
        double firstVoidM = INFINITY;
        std::vector<float> horizon;
    };
    std::vector<std::pair<int, int>> ends;
    for (int col = 0; col < gridCols; col++)
    {
        ends.push_back({ 0, col });
        if (gridRows > 1) ends.push_back({ gridRows - 1, col });
    }
    for (int row = 1; row < gridRows - 1; row++)
    {
        ends.push_back({ row, 0 });
        if (gridCols > 1) ends.push_back({ row, gridCols - 1 });
    }

    const double workUnits = (double)ends.size() + gridRows;
    std::vector<Ray> rays;
    rays.reserve(ends.size());
    for (size_t i = 0; i < ends.size(); i++)
    {
        if (progress && i % 16 == 0 && !progress(i / workUnits)) return false;

        auto [row, col] = ends[i];
        if (row == centerRow && col == centerCol) continue;

        std::vector<ProfileSample> profile = GetTerrainProfile(observer, cellCentre(row, col), spacingDeg, sampler);
        Ray ray;
        ray.direction = directionOf(row, col);
        ray.stepM = profile.back().distanceFromStartM / (profile.size() - 1);
        ray.horizon.reserve(profile.size() - 1);
        double horizon = -INFINITY;
        for (size_t s = 1; s < profile.size(); s++)
        {
            double dM = profile[s].distanceFromStartM;
            if (!profile[s].elevationM.has_value())
            {
                ray.firstVoidM = (std::min)(ray.firstVoidM, dM);
            }
            else
            {
                horizon = (std::max)(horizon, CurvatureAdjustedSlope(*profile[s].elevationM, observerEyeHeightM, dM, k));
            }
            ray.horizon.push_back((float)horizon);
        }
        rays.push_back(std::move(ray));
    }
    // By direction, so the two rays either side of any cell are neighbours in the list.
    std::sort(rays.begin(), rays.end(), [](const Ray& a, const Ray& b) { return a.direction < b.direction; });

    // A ray's horizon over its samples strictly nearer than dM.
    auto horizonBefore = [](const Ray& ray, double dM) -> double {
        if (dM <= 0 || ray.stepM <= 0 || ray.horizon.empty()) return -INFINITY;
        size_t count = (std::min)((size_t)std::ceil(dM / ray.stepM) - 1, ray.horizon.size());
        return count == 0 ? -INFINITY : (double)ray.horizon[count - 1];
    };

    const double pi = 3.14159265358979323846;
    const double halfCellM = EarthRadiusM * DegToRad * spacingDeg / 2;
    for (int row = 0; row < gridRows; row++)
    {
        if (progress && !progress((ends.size() + row) / workUnits)) return false;

        for (int col = 0; col < gridCols; col++)
        {
            if (row == centerRow && col == centerCol) continue;

            GeoPoint centre = cellCentre(row, col);
            auto groundM = sampler.GetElevation(centre.latitudeDeg, centre.longitudeDeg);
            if (!groundM.has_value() || rays.empty())
            {
                cells[row][col] = CellVisibility::Degraded;
                continue;
            }

            // The rays either side: b the first at or past this direction, a the one before, wrapping round.
            double direction = directionOf(row, col);
            size_t b = std::lower_bound(rays.begin(), rays.end(), direction,
                [](const Ray& ray, double d) { return ray.direction < d; }) - rays.begin();
            size_t a = (b + rays.size() - 1) % rays.size();
            b %= rays.size();
            double span = rays[b].direction - rays[a].direction;
            if (span <= 0) span += 2 * pi;
            double past = direction - rays[a].direction;
            if (past < 0) past += 2 * pi;
            double t = (std::min)(1.0, past / span);

            double dM = GreatCircleDistanceM(observer, centre);
            if (rays[a].firstVoidM < dM || rays[b].firstVoidM < dM)
            {
                cells[row][col] = CellVisibility::Degraded;
                continue;
            }

            double ha = horizonBefore(rays[a], dM - halfCellM);
            double hb = horizonBefore(rays[b], dM - halfCellM);
            double horizon = std::isinf(ha) || std::isinf(hb) ? (std::max)(ha, hb) : ha + t * (hb - ha);

            answerCell(row, col, *groundM, dM, horizon);
        }
    }
    return true;
}

// The fast viewshed: every cell answered from its own centre -- its own ground, its
// own distance, the target height standing on it -- against the horizon
// AnswerEachCellFromFastHorizons finds in front of it. The terrain alone builds the
// horizon; the target, standing on the cell, is what is tested against it.
//
// Naive answers each cell along its own exact line; the rays here pass beside most
// cells, so this is an approximation. How far it is from naive, and where the two
// differ -- on naive's visibility edge, almost always -- is measured by
// ViewshedAgreement.h and tested at three observers on real terrain; see
// docs/ENGINE.md and NOTES.md, "A fast viewshed that asks naive's question".
//
// Complexity, memory and threading: those of AnswerEachCellFromFastHorizons.
// Thread-safety: single-thread-only. Calls the non-thread-affine sampler sequentially.
// Progress (optional): as AnswerEachCellFromFastHorizons reports it, then once at the end.
// Target height (optional): as for ComputeViewshedNaive.
inline ViewshedResult ComputeViewshedFast(GeoPoint observer, DatumHeight observerHeight, int gridRows, int gridCols, double spacingDeg, IElevationSampler& sampler, double k = 4.0 / 3.0, const ViewshedProgress& progress = nullptr, DatumHeight targetHeight = DatumHeight{ 0.0, VerticalDatum::HeightAboveGround })
{
    ViewshedResult result;

    // Same as naive: a grid with no rows or no columns comes back empty.
    if (gridRows <= 0 || gridCols <= 0) return result;

    // An input it can't use is refused, with the reason, rather than laid out into a grid.
    result.inputProblem = CheckViewshedRequest(observer, observerHeight, gridRows, spacingDeg, k, targetHeight);
    if (result.inputProblem != InputProblem::None) return result;

    // Naive delegates these checks to ComputeLineOfSight per cell; fast does its
    // own curvature math and never calls it, so it checks here. An observer whose
    // height can't be put on the terrain's datum, or who stands on a void, leaves
    // nothing known about any cell: every cell is Degraded, the observer's own
    // included -- exactly what naive reports for the same case.
    VerticalDatum terrainDatum = sampler.GetDatum();
    auto observerElevationM = sampler.GetElevation(observer.latitudeDeg, observer.longitudeDeg);
    if (!observerElevationM.has_value() || !CanExpressInTerrainDatum(observerHeight, terrainDatum))
    {
        result.visible.assign(gridRows, std::vector<CellVisibility>(gridCols, CellVisibility::Degraded));
        if (progress) progress(1.0);
        return result;
    }

    int centerRow = gridRows / 2;
    int centerCol = gridCols / 2;

    // A target height that can't be put on the terrain's datum leaves nothing known about
    // any cell but the observer's own -- what naive's per-cell ComputeLineOfSight reports.
    if (!CanExpressInTerrainDatum(targetHeight, terrainDatum))
    {
        result.visible.assign(gridRows, std::vector<CellVisibility>(gridCols, CellVisibility::Degraded));
        result.visible[centerRow][centerCol] = CellVisibility::Visible;
        if (progress) progress(1.0);
        return result;
    }

    result.visible.resize(gridRows, std::vector<CellVisibility>(gridCols, CellVisibility::NotCovered));

    double observerEyeHeightM = *EyeHeightInTerrainDatum(observerHeight, *observerElevationM, terrainDatum);
    bool finished = AnswerEachCellFromFastHorizons(observer, observerEyeHeightM, gridRows, gridCols, spacingDeg, sampler, k, progress, result.visible,
        [&](int row, int col, double groundM, double dM, double horizon) {
            double targetM = *EyeHeightInTerrainDatum(targetHeight, groundM, terrainDatum);
            bool isVisible = CurvatureAdjustedSlope(targetM, observerEyeHeightM, dM, k) >= horizon;
            result.visible[row][col] = isVisible ? CellVisibility::Visible : CellVisibility::NotVisible;
        });
    if (!finished)
    {
        result.cancelled = true;
        return result;
    }

    result.visible[centerRow][centerCol] = CellVisibility::Visible;

    if (progress) progress(1.0);
    return result;
}
