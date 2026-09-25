#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>
#include "Viewshed.h"

// The minimum visible height: for one observer, the lowest height above a cell's
// ground at which a target standing there is seen -- 0 where the ground itself is.
// The viewshed answers "is a target this high seen?" for one height; this answers it
// for every height at once.
//
// Why it has an exact answer. A target is seen when its curvature-adjusted slope
// (CurvatureAdjustedSlope, Viewshed.h) is at least that of every point in front of it:
//
//     (T - eye) / D  -  D / (2 k R)  >=  S,   S = the largest s(d) in front of it
//
// Solved for the target's eye height T, the lowest one seen is
//
//     T = eye + D * S + D^2 / (2 k R),        and above the ground g: max(0, T - g).
//
// ComputeLineOfSight makes the same comparison, written as a sight line against
// the terrain; the two only part in the last bits of a double. So the formula is
// used as an estimate, and the answer is then settled by ComputeLineOfSight itself:
// whether a target is seen can only go from "no" to "yes" as it rises, so there is
// one smallest double at which it first says "yes", and that is the answer. A
// viewshed asked about a target of height H then sees exactly the cells whose answer
// is at most H. On a smooth sphere the formula has a closed form, derived in
// docs/ENGINE.md, "Minimum visible height", which the tests hold both versions to.

// The smallest height h >= 0, as a double, at which seen(h) holds -- given that it
// doesn't at 0, and that once it holds it holds at every greater height. Starts from
// estimateM and walks to the exact answer. Non-negative doubles are ordered like
// their bit patterns, so "the next double up" is the next integer: the walk doubles
// its step until it has the answer between two heights, then halves the gap between
// them. Infinity when no height up to 10^300 m is seen.
//
// Complexity: O(log of how far estimateM is from the answer, in doubles) calls of seen.
// Thread-safety: pure function over its inputs, as safe as seen is.
template <class Seen>
double SmallestHeightSeen(Seen&& seen, double estimateM)
{
    auto bitsOf = [](double value) { std::uint64_t bits; std::memcpy(&bits, &value, sizeof bits); return bits; };
    auto valueOf = [](std::uint64_t bits) { double value; std::memcpy(&value, &bits, sizeof value); return value; };

    const double ceilingM = 1e300;
    const std::uint64_t ceiling = bitsOf(ceilingM);
    std::uint64_t notSeen = bitsOf(0.0);
    std::uint64_t isSeen = ceiling;

    if (!(estimateM > 0.0 && estimateM < ceilingM))
    {
        if (!seen(ceilingM)) return INFINITY;
    }
    else if (seen(estimateM))
    {
        // Seen at the estimate: step down, doubling the step, until a height isn't.
        isSeen = bitsOf(estimateM);
        for (std::uint64_t step = 1; step < isSeen - notSeen; step *= 2)
        {
            std::uint64_t probe = isSeen - step;
            if (!seen(valueOf(probe)))
            {
                notSeen = probe;
                break;
            }
            isSeen = probe;
        }
    }
    else
    {
        // Not seen at the estimate: step up, doubling the step, until a height is.
        notSeen = bitsOf(estimateM);
        for (std::uint64_t step = 1;; step *= 2)
        {
            if (step >= ceiling - notSeen)
            {
                if (!seen(ceilingM)) return INFINITY;
                break;
            }
            std::uint64_t probe = notSeen + step;
            if (seen(valueOf(probe)))
            {
                isSeen = probe;
                break;
            }
            notSeen = probe;
        }
    }

    while (isSeen - notSeen > 1)
    {
        std::uint64_t middle = notSeen + (isSeen - notSeen) / 2;
        if (seen(valueOf(middle))) isSeen = middle;
        else notSeen = middle;
    }
    return valueOf(isSeen);
}

// The formula above, over a whole profile: the lowest target eye height, in the
// terrain's datum, that the curvature-adjusted slope of every sample leaves seen.
// A sample at distance 0 (the observer's own) doesn't depend on the target: ground
// there above the observer's eye hides every target, and the answer is infinity.
// The target's own sample gives T >= its ground. Every sample must have an elevation.
//
// Complexity: O(profile.size()). Thread-safety: pure function, safe to call concurrently.
inline double LowestTargetEyeHeightM(const std::vector<ProfileSample>& profile, double observerEyeHeightM, double k)
{
    double totalDistanceM = profile.back().distanceFromStartM;
    double lowestM = -INFINITY;
    for (const ProfileSample& sample : profile)
    {
        double dM = sample.distanceFromStartM;
        if (dM <= 0)
        {
            if (*sample.elevationM > observerEyeHeightM) return INFINITY;
            continue;
        }
        double slope = CurvatureAdjustedSlope(*sample.elevationM, observerEyeHeightM, dM, k);
        lowestM = (std::max)(lowestM, observerEyeHeightM + totalDistanceM * slope + totalDistanceM * totalDistanceM / (2 * k * EarthRadiusM));
    }
    return lowestM;
}

struct MinimumVisibleHeightAnswer
{
    // ComputeLineOfSight's own status for the path: anything but Ok means no height is known.
    ComputationStatus status = ComputationStatus::Ok;
    InputProblem inputProblem = InputProblem::None; // set with status InvalidInput

    // With status Ok: the lowest height above the target's ground at which it is seen --
    // 0 when the ground itself is, infinity when no height is. NaN otherwise.
    double heightAboveGroundM = NAN;

    // With status Ok: the ground under the target, in the profile's datum. The same
    // answer as an absolute height is groundM + heightAboveGroundM.
    double groundM = NAN;
};

// === THE EXACT PER-PATH ANSWER ===
// The lowest height above the ground at the profile's far end at which a target
// there is seen from observerHeight, decided by ComputeLineOfSight itself: at any
// height h >= 0, ComputeLineOfSight(profile, observerHeight, h above ground, k) says
// "visible" exactly when h >= the answer. A profile ComputeLineOfSight can't answer
// (a void, a datum it can't use, an input it refuses) has no answer, with its status.
//
// Complexity: O(profile.size()) per ComputeLineOfSight call: one when the ground is
// seen, a few more when it isn't. Allocates nothing.
// Thread-safety: pure function over its inputs, safe to call concurrently.
inline MinimumVisibleHeightAnswer MinimumVisibleHeightAlongProfile(const std::vector<ProfileSample>& profile, DatumHeight observerHeight, double k = 4.0 / 3.0)
{
    MinimumVisibleHeightAnswer answer;
    auto lineOfSightAt = [&](double heightAboveGroundM) {
        return ComputeLineOfSight(profile, observerHeight, DatumHeight{ heightAboveGroundM, VerticalDatum::HeightAboveGround }, k);
    };

    LineOfSightResult atGround = lineOfSightAt(0.0);
    answer.status = atGround.status;
    answer.inputProblem = atGround.inputProblem;
    if (!IsOk(atGround.status)) return answer;

    answer.groundM = *profile.back().elevationM;
    if (atGround.isVisible)
    {
        answer.heightAboveGroundM = 0.0;
        return answer;
    }

    // Ok means the terrain's datum and the observer's height can be put together.
    VerticalDatum terrainDatum = *ProfileElevationDatum(profile);
    double observerEyeHeightM = *EyeHeightInTerrainDatum(observerHeight, *profile.front().elevationM, terrainDatum);
    double estimateM = LowestTargetEyeHeightM(profile, observerEyeHeightM, k) - answer.groundM;
    answer.heightAboveGroundM = SmallestHeightSeen([&](double heightM) { return lineOfSightAt(heightM).isVisible; }, estimateM);
    return answer;
}

struct MinimumVisibleHeightResult
{
    // The viewshed of the ground itself, cell for cell what ComputeViewshedNaive (or
    // ComputeViewshedFast, for the fast version) gives for a target 0 m above ground:
    // Visible where the ground is seen, NotVisible where only a target standing above
    // it is, and NotCovered, Degraded or DataNotGiven, for the reasons the viewshed gives them,
    // where there is no confident answer.
    std::vector<std::vector<CellVisibility>> state;

    // Where state is confident: the lowest height above the cell's ground, in metres,
    // at which a target standing on the cell's centre is seen. 0 where the ground
    // itself is; infinity where no height is -- an observer whose eye is below the
    // ground under it sees nothing. NaN where state isn't confident.
    std::vector<std::vector<double>> heightAboveGroundM;

    // Where state is confident: the ground under the cell's centre, in terrainDatum --
    // the terrain the height was computed from. NaN elsewhere.
    std::vector<std::vector<double>> groundM;
    VerticalDatum terrainDatum = VerticalDatum::Unknown;

    // As for ViewshedResult.
    bool cancelled = false;
    InputProblem inputProblem = InputProblem::None;
};

// The same answer as an absolute height in the result's terrainDatum: the ground
// the cell's answer was computed on, plus the height above it. NaN where the cell
// has no confident answer; infinity where no height is seen.
//
// Complexity: O(1). Thread-safety: pure function, safe to call concurrently.
inline double MinimumVisibleAbsoluteHeightM(const MinimumVisibleHeightResult& result, int row, int col)
{
    return result.groundM[row][col] + result.heightAboveGroundM[row][col];
}

// The viewshed for a target standing heightAboveGroundM above every cell: a cell with
// a confident answer is Visible exactly when its minimum visible height is at most
// that; the rest keep their state. For the exact version this is, cell for cell,
// ComputeViewshedNaive with that target height; for the fast version,
// ComputeViewshedFast's. A height that isn't a finite number, or is below the ground,
// is refused with the reason and an empty grid.
//
// Complexity: O(cells). Thread-safety: pure function, safe to call concurrently.
inline ViewshedResult ViewshedAtTargetHeight(const MinimumVisibleHeightResult& result, double heightAboveGroundM)
{
    ViewshedResult viewshed;
    if (!std::isfinite(heightAboveGroundM)) viewshed.inputProblem = InputProblem::HeightNotFinite;
    else if (heightAboveGroundM < 0.0) viewshed.inputProblem = InputProblem::HeightBelowGround;
    if (viewshed.inputProblem != InputProblem::None) return viewshed;

    viewshed.cancelled = result.cancelled;
    viewshed.visible = result.state;
    for (size_t row = 0; row < viewshed.visible.size(); row++)
    {
        for (size_t col = 0; col < viewshed.visible[row].size(); col++)
        {
            CellVisibility& cell = viewshed.visible[row][col];
            if (IsConfident(cell))
            {
                cell = heightAboveGroundM >= result.heightAboveGroundM[row][col] ? CellVisibility::Visible : CellVisibility::NotVisible;
            }
        }
    }
    return viewshed;
}

// A grid of the given size with no answer in it yet: every cell NotCovered, no heights.
inline void LayOutMinimumVisibleHeightGrid(MinimumVisibleHeightResult& result, int gridRows, int gridCols, VerticalDatum terrainDatum)
{
    result.state.assign(gridRows, std::vector<CellVisibility>(gridCols, CellVisibility::NotCovered));
    result.heightAboveGroundM.assign(gridRows, std::vector<double>(gridCols, NAN));
    result.groundM.assign(gridRows, std::vector<double>(gridCols, NAN));
    result.terrainDatum = terrainDatum;
}

// === BATCH PATH: THE EXACT REFERENCE ===
// Every cell answered by MinimumVisibleHeightAlongProfile along its own exact line,
// on the same grid and the same profile ComputeViewshedNaive uses for it -- so
// ViewshedAtTargetHeight gives, for any target height, exactly the cells
// ComputeViewshedNaive gives for it. This is the reference the fast version below is
// measured against, the way the naive viewshed is the fast viewshed's.
//
// Complexity: O(gridRows * gridCols * samples per profile), like ComputeViewshedNaive: one
// line-of-sight pass for a cell whose ground is seen, a median of 10-16 for the rest,
// each cheaper than building the profile. Measured, 1.0-1.6 times naive's time. Each
// thread reuses one profile buffer for every cell it answers.
// Threads (optional): as ComputeViewshedNaive -- 1 by default, 0 for one per hardware
// thread; each cell is answered on its own, so the grid is the same at any thread count.
// Progress (optional): as ComputeViewshedNaive.
inline MinimumVisibleHeightResult ComputeMinimumVisibleHeightReference(GeoPoint observer, DatumHeight observerHeight, int gridRows, int gridCols, double spacingDeg, IElevationSampler& sampler, double k = 4.0 / 3.0, const ViewshedProgress& progress = nullptr, int threadCount = 1)
{
    MinimumVisibleHeightResult result;

    // As the viewsheds: no rows or no columns is an empty answer, a request they
    // would refuse is refused.
    if (gridRows <= 0 || gridCols <= 0) return result;
    result.inputProblem = CheckViewshedRequest(observer, observerHeight, gridRows, spacingDeg, k, DatumHeight{ 0.0, VerticalDatum::HeightAboveGround });
    if (result.inputProblem != InputProblem::None) return result;

    LayOutMinimumVisibleHeightGrid(result, gridRows, gridCols, sampler.GetDatum());

    int centerRow = gridRows / 2;
    int centerCol = gridCols / 2;
    double lonSpacingDeg = LongitudeSpacingForLatitude(spacingDeg, observer.latitudeDeg);

    // The observer's own cell, as naive answers it: seen from any height when
    // something is known about the observer, and otherwise no confident answer, for
    // the reason naive gives.
    ElevationSample observerGround = sampler.Sample(observer.latitudeDeg, observer.longitudeDeg);
    bool observerDatumOk = CanExpressInTerrainDatum(observerHeight, sampler.GetDatum());
    CellVisibility observerCell = observerGround.data != ElevationData::Present ? NoAnswerFor(observerGround.data)
        : observerDatumOk ? CellVisibility::Visible : CellVisibility::Degraded;

    // With no ground known under the observer, every cell is answered for that gap, as
    // ComputeViewshedNaive answers it.
    if (observerGround.data != ElevationData::Present)
    {
        result.state.assign(gridRows, std::vector<CellVisibility>(gridCols, observerDatumOk ? NoAnswerFor(observerGround.data) : CellVisibility::Degraded));
        result.state[centerRow][centerCol] = observerCell;
        if (progress) progress(1.0);
        return result;
    }

    // One profile buffer per thread, reused for every cell that thread answers.
    std::vector<std::vector<ProfileSample>> profiles(ThreadsFor(threadCount));
    bool finished = ForEachRowOnThreads(gridRows, threadCount, progress, [&](int row, int thread) {
        std::vector<ProfileSample>& profile = profiles[thread];
        for (int col = 0; col < gridCols; col++)
        {
            if (row == centerRow && col == centerCol)
            {
                result.state[row][col] = observerCell;
                if (observerCell == CellVisibility::Visible)
                {
                    result.heightAboveGroundM[row][col] = 0.0;
                    result.groundM[row][col] = observerGround.elevationM;
                }
                continue;
            }

            GeoPoint target{ observer.latitudeDeg + (row - centerRow) * spacingDeg, observer.longitudeDeg + (col - centerCol) * lonSpacingDeg };
            GetTerrainProfile(observer, target, spacingDeg, sampler, profile);
            MinimumVisibleHeightAnswer answer = MinimumVisibleHeightAlongProfile(profile, observerHeight, k);
            if (!IsOk(answer.status))
            {
                result.state[row][col] = answer.status == ComputationStatus::DataNotGiven ? CellVisibility::DataNotGiven : CellVisibility::Degraded;
                continue;
            }
            result.state[row][col] = answer.heightAboveGroundM == 0.0 ? CellVisibility::Visible : CellVisibility::NotVisible;
            result.heightAboveGroundM[row][col] = answer.heightAboveGroundM;
            result.groundM[row][col] = answer.groundM;
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

// === BATCH PATH: THE FAST VERSION ===
// Every cell answered against the horizon ComputeViewshedFast finds in front of it
// (AnswerEachCellFromFastHorizons, Viewshed.h): the formula above gives the estimate,
// and ComputeViewshedFast's own test -- a target's slope against that horizon -- settles
// it, so ViewshedAtTargetHeight gives, for any target height, exactly the cells
// ComputeViewshedFast gives for it. How far it is from the reference is measured the
// way the fast viewshed is measured against naive: see docs/ENGINE.md.
//
// Complexity: ComputeViewshedFast's, plus a few O(1) tests per cell whose ground isn't
// seen. Memory: ComputeViewshedFast's rays, and two doubles and a state per cell.
// Thread-safety: single-thread-only; order-independent, as ComputeViewshedFast.
// Progress (optional): as ComputeViewshedFast reports it.
inline MinimumVisibleHeightResult ComputeMinimumVisibleHeightFast(GeoPoint observer, DatumHeight observerHeight, int gridRows, int gridCols, double spacingDeg, IElevationSampler& sampler, double k = 4.0 / 3.0, const ViewshedProgress& progress = nullptr)
{
    MinimumVisibleHeightResult result;

    if (gridRows <= 0 || gridCols <= 0) return result;
    result.inputProblem = CheckViewshedRequest(observer, observerHeight, gridRows, spacingDeg, k, DatumHeight{ 0.0, VerticalDatum::HeightAboveGround });
    if (result.inputProblem != InputProblem::None) return result;

    VerticalDatum terrainDatum = sampler.GetDatum();
    LayOutMinimumVisibleHeightGrid(result, gridRows, gridCols, terrainDatum);

    // As ComputeViewshedFast: nothing known about the observer, nothing known about any cell
    // -- Degraded for a height with no datum, otherwise for the reason its ground is missing.
    ElevationSample observerGround = sampler.Sample(observer.latitudeDeg, observer.longitudeDeg);
    bool observerDatumOk = CanExpressInTerrainDatum(observerHeight, terrainDatum);
    if (!observerDatumOk || observerGround.data != ElevationData::Present)
    {
        CellVisibility everyCell = !observerDatumOk ? CellVisibility::Degraded : NoAnswerFor(observerGround.data);
        result.state.assign(gridRows, std::vector<CellVisibility>(gridCols, everyCell));
        result.state[gridRows / 2][gridCols / 2] = observerGround.data != ElevationData::Present ? NoAnswerFor(observerGround.data) : CellVisibility::Degraded;
        if (progress) progress(1.0);
        return result;
    }

    // An eye below the ground under it sees nothing, from any height: the reference's line of
    // sight is blocked by that ground at its first sample, as in ComputeViewshedFast.
    double observerEyeHeightM = *EyeHeightInTerrainDatum(observerHeight, observerGround.elevationM, terrainDatum);
    bool eyeAboveGround = observerEyeHeightM >= observerGround.elevationM;
    double twoKR = 2 * k * EarthRadiusM;
    bool finished = AnswerEachCellFromFastHorizons(observer, observerEyeHeightM, gridRows, gridCols, spacingDeg, sampler, k, progress, result.state,
        [&](int row, int col, double groundM, double dM, double horizon) {
            // ComputeViewshedFast's test, for a target heightM above this cell's ground.
            auto seen = [&](double heightM) {
                double targetM = *EyeHeightInTerrainDatum(DatumHeight{ heightM, VerticalDatum::HeightAboveGround }, groundM, terrainDatum);
                return CurvatureAdjustedSlope(targetM, observerEyeHeightM, dM, k) >= horizon;
            };
            double heightM = !eyeAboveGround ? INFINITY
                : seen(0.0) ? 0.0 : SmallestHeightSeen(seen, observerEyeHeightM + dM * horizon + dM * dM / twoKR - groundM);
            result.state[row][col] = heightM == 0.0 ? CellVisibility::Visible : CellVisibility::NotVisible;
            result.heightAboveGroundM[row][col] = heightM;
            result.groundM[row][col] = groundM;
        });
    if (!finished)
    {
        result.cancelled = true;
        return result;
    }

    int centerRow = gridRows / 2;
    int centerCol = gridCols / 2;
    result.state[centerRow][centerCol] = CellVisibility::Visible;
    result.heightAboveGroundM[centerRow][centerCol] = 0.0;
    result.groundM[centerRow][centerCol] = observerGround.elevationM;

    if (progress) progress(1.0);
    return result;
}
