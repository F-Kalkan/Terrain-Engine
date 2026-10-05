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
    Degraded,       // no confident answer: a void in the source under the cell or on the way to it, or
                    // a height that can't be put on the terrain's datum
    Visible,
    NotVisible,
    DataNotGiven    // no confident answer yet: ground under the cell or on the way to it the sampler was
                    // never given, and no void known -- given that ground, the cell may be answered
};

// The state of a cell with no confident answer, from what kept it from one. A void, once
// known, wins over data not given, as in ComputeLineOfSight: nothing loaded will fill it.
// Complexity: O(1). Thread-safety: pure function, safe to call concurrently.
inline CellVisibility NoAnswerFor(ElevationData data)
{
    return data == ElevationData::NotGiven ? CellVisibility::DataNotGiven : CellVisibility::Degraded;
}

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
// ThreadsFor(threadCount) threads, each taking the next row not yet taken
// (ForEachBlockOnThreads, a row at a time). Progress is reported on the calling thread only --
// 0 before any thread starts, then before each later row it takes, with the fraction of rows
// taken so far. A progress report that returns false stops every thread before its next row,
// and false is returned. At one thread this is a plain loop over the rows, reporting before each.
//
// Thread-safety: rowWork runs on several threads at once; it must write only its own row.
template <class RowWork>
bool ForEachRowOnThreads(int gridRows, int threadCount, const ViewshedProgress& progress, RowWork&& rowWork)
{
    BlockProgress rowsTaken;
    if (progress) rowsTaken = [&](size_t rows) { return progress((double)rows / gridRows); };
    return ForEachBlockOnThreads((size_t)(std::max)(gridRows, 0), 1, ThreadsFor(threadCount), rowsTaken,
        [&](size_t row, size_t, int thread) { rowWork((int)row, thread); }).finished;
}

// === THE NAIVE VIEWSHED, READING LESS ===
// One cell of the naive viewshed -- exactly what GetTerrainProfile and ComputeLineOfSight give
// for it -- reading only the terrain that can decide it, where the sampler can put a ceiling on
// a box of the ground (IElevationSampler::CeilingIn). The line's samples are taken in stretches:
// a stretch whose ceiling, raised by the Earth's curvature where that is worst along it, stays
// clearly below the sight line cannot block it and isn't read; any other is halved, down to eight
// samples, which are read and judged one by one with ComputeLineOfSight's own arithmetic
// (DeficitM). The first sample that blocks ends the walk, once the ceiling on the rest of the
// line shows no void or missing data that would turn the answer into none.
//
// Every judgement that decides the answer is made on a sample read exactly as the exact path
// reads it; a ceiling only ever rules a stretch out, with a margin far above rounding. Anything
// else -- a gap that may be there, an end in the ground, a height with no datum, a sampler with
// no ceilings -- returns nullopt, and the caller takes the exact path. So the answer is the
// exact path's, to the bit, and the tests hold it to that.
//
// Complexity: at worst the exact path's O(samples), plus the ceilings; far less where the
// terrain sits below the line or blocks it early. Allocates nothing.
// Thread-safety: as safe as the sampler's Sample and CeilingIn.
inline std::optional<CellVisibility> NaiveCellByCeilings(GeoPoint observer, GeoPoint target, double spacingDeg, IElevationSampler& sampler,
    const DatumHeight& observerHeight, const DatumHeight& targetHeight, double k)
{
    if (CheckProfileRequest(observer, target, spacingDeg) != InputProblem::None) return std::nullopt;
    if (CheckLineOfSightInputs(observerHeight, targetHeight, k) != InputProblem::None) return std::nullopt;

    // The samples GetTerrainProfile would take: the same count, points and distances.
    const double totalDistanceM = GreatCircleDistanceM(observer, target);
    const double centralAngleRad = totalDistanceM / EarthRadiusM;
    const int sampleCount = (int)ProfileIntervals(observer, target, spacingDeg);
    if (sampleCount < 2) return std::nullopt;
    const GreatCircleArc arc(observer, target, centralAngleRad);
    auto pointAt = [&](int i) { return arc.At((double)i / sampleCount); };
    auto distanceAt = [&](int i) { return (double)i / sampleCount * totalDistanceM; };

    GeoPoint first = pointAt(0), last = pointAt(sampleCount);
    ElevationSample firstGround = sampler.Sample(first.latitudeDeg, first.longitudeDeg);
    ElevationSample lastGround = sampler.Sample(last.latitudeDeg, last.longitudeDeg);
    if (firstGround.data != ElevationData::Present || lastGround.data != ElevationData::Present) return std::nullopt;
    const VerticalDatum datum = sampler.GetDatum();
    std::optional<double> observerEyeM = EyeHeightInTerrainDatum(observerHeight, firstGround.elevationM, datum);
    std::optional<double> targetEyeM = EyeHeightInTerrainDatum(targetHeight, lastGround.elevationM, datum);
    if (!observerEyeM || !targetEyeM) return std::nullopt;
    auto deficitAt = [&](int i, double elevationM) { return DeficitM(elevationM, *observerEyeM, *targetEyeM, distanceAt(i), totalDistanceM, k); };
    if (deficitAt(0, firstGround.elevationM) > 0 || deficitAt(sampleCount, lastGround.elevationM) > 0) return std::nullopt; // an end in the ground

    // The ceiling on every point sampled between two points of the arc: the box around them,
    // widened in latitude by the most the arc can bow out past its ends -- (arc angle)^2 / 8
    // times tan(latitude), doubled -- as longitude only ever moves one way along it.
    auto ceilingBetween = [&](GeoPoint a, GeoPoint b, int samplesApart) {
        double angle = centralAngleRad * samplesApart / sampleCount;
        double highestLatitude = (std::min)(89.9, (std::max)(std::abs(a.latitudeDeg), std::abs(b.latitudeDeg)) + angle / DegToRad);
        double bowDeg = angle * angle / 4 * std::tan(highestLatitude * DegToRad) / DegToRad + 1e-9;
        return sampler.CeilingIn((std::min)(a.latitudeDeg, b.latitudeDeg) - bowDeg, (std::max)(a.latitudeDeg, b.latitudeDeg) + bowDeg,
            (std::min)(a.longitudeDeg, b.longitudeDeg), (std::max)(a.longitudeDeg, b.longitudeDeg));
    };
    // The most the curvature, less the sight line, can add over distances da..db: a downward
    // parabola in the distance, so at its vertex, or at the nearer end.
    const double slope = (*targetEyeM - *observerEyeM) / totalDistanceM;
    auto worstRiseM = [&](double da, double db) {
        double d = (std::min)(db, (std::max)(da, totalDistanceM / 2 - slope * k * EarthRadiusM));
        return CurvatureDropM(d, totalDistanceM - d, k) - SightLineHeightM(*observerEyeM, *targetEyeM, d, totalDistanceM);
    };
    const double margin = 1e-6; // metres: far above rounding, far below anything that matters

    struct Stretch { int first, last; GeoPoint from, to; };
    Stretch pending[16];
    const int widest = 256, narrowest = 8;
    int blockedAt = -1;
    for (int start = 1; start < sampleCount && blockedAt < 0; start += widest)
    {
        int end = (std::min)(start + widest - 1, sampleCount - 1);
        int count = 0;
        pending[count++] = Stretch{ start, end, pointAt(start), pointAt(end) };
        while (count > 0 && blockedAt < 0)
        {
            Stretch s = pending[--count];
            std::optional<HeightCeiling> ceiling = ceilingBetween(s.from, s.to, s.last - s.first);
            if (!ceiling || ceiling->mayHaveGap) return std::nullopt;
            if (ceiling->highestM + worstRiseM(distanceAt(s.first), distanceAt(s.last)) < -margin) continue;
            if (s.last - s.first + 1 > narrowest)
            {
                int middle = (s.first + s.last) / 2;
                pending[count++] = Stretch{ middle + 1, s.last, pointAt(middle + 1), s.to };
                pending[count++] = Stretch{ s.first, middle, s.from, pointAt(middle) };
                continue;
            }
            for (int i = s.first; i <= s.last; i++)
            {
                GeoPoint point = i == s.first ? s.from : i == s.last ? s.to : pointAt(i);
                ElevationSample ground = sampler.Sample(point.latitudeDeg, point.longitudeDeg);
                if (ground.data != ElevationData::Present) return std::nullopt;
                if (deficitAt(i, ground.elevationM) > 0)
                {
                    blockedAt = i;
                    break;
                }
            }
        }
    }
    if (blockedAt < 0) return CellVisibility::Visible;

    // Blocked -- unless a void or data not given lies further on, which makes it no answer.
    std::optional<HeightCeiling> rest = ceilingBetween(pointAt(blockedAt), last, sampleCount - blockedAt);
    if (!rest || rest->mayHaveGap) return std::nullopt;
    return CellVisibility::NotVisible;
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
// Complexity: O(gridRows * gridCols * samples per profile) at worst -- a
// GetTerrainProfile + ComputeLineOfSight per cell -- and far less where the sampler puts
// ceilings on the ground (CeilingIn): each cell then reads only the part of its line that
// can decide it (NaiveCellByCeilings), with the same answer. This is the exact-per-cell
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
    // observer: ground under it that is given and isn't void, and a height that can be
    // put on the terrain's datum. Otherwise it has no confident answer, like every
    // other cell -- the same answer ComputeViewshedFast gives.
    ElevationSample observerGround = sampler.Sample(observer.latitudeDeg, observer.longitudeDeg);
    bool observerDatumOk = CanExpressInTerrainDatum(observerHeight, sampler.GetDatum());
    CellVisibility observerCell = observerGround.data != ElevationData::Present ? NoAnswerFor(observerGround.data)
        : observerDatumOk ? CellVisibility::Visible : CellVisibility::Degraded;

    // With no ground known under the observer, every line starts from a gap, and every
    // cell is answered for that gap -- Degraded if the heights have no datum to go on
    // first, as ComputeLineOfSight checks that first -- rather than for whatever else each
    // line crosses. ComputeViewshedFast answers the same, never casting a ray.
    if (observerGround.data != ElevationData::Present)
    {
        bool datumsOk = observerDatumOk && CanExpressInTerrainDatum(targetHeight, sampler.GetDatum());
        result.visible.assign(gridRows, std::vector<CellVisibility>(gridCols, datumsOk ? NoAnswerFor(observerGround.data) : CellVisibility::Degraded));
        result.visible[centerRow][centerCol] = observerCell;
        if (progress) progress(1.0);
        return result;
    }

    // Where the sampler can put ceilings on the ground, each cell reads only the terrain that can
    // decide it (NaiveCellByCeilings), and takes the exact path below whenever that can't be sure.
    const bool readLess = sampler.CeilingIn(observer.latitudeDeg, observer.latitudeDeg, observer.longitudeDeg, observer.longitudeDeg).has_value();
    std::vector<std::vector<ProfileSample>> profiles(ThreadsFor(threadCount));

    bool finished = ForEachRowOnThreads(gridRows, threadCount, progress, [&](int row, int thread) {
        for (int col = 0; col < gridCols; col++)
        {
            if (row == centerRow && col == centerCol)
            {
                result.visible[row][col] = observerCell;
                continue;
            }

            GeoPoint target{ observer.latitudeDeg + (row - centerRow) * spacingDeg, observer.longitudeDeg + (col - centerCol) * lonSpacingDeg };

            if (readLess)
            {
                if (std::optional<CellVisibility> cell = NaiveCellByCeilings(observer, target, spacingDeg, sampler, observerHeight, targetHeight, k))
                {
                    result.visible[row][col] = *cell;
                    continue;
                }
            }

            std::vector<ProfileSample>& profile = profiles[thread];
            GetTerrainProfile(observer, target, spacingDeg, sampler, profile);
            LineOfSightResult los = ComputeLineOfSight(profile, observerHeight, targetHeight, k);

            if (!IsOk(los.status))
            {
                result.visible[row][col] = los.status == ComputationStatus::DataNotGiven ? CellVisibility::DataNotGiven : CellVisibility::Degraded;
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
// rather than on it. A cell with no ground under its centre, or whose rays met a gap
// before reaching it, is marked in `cells` and not visited: Degraded when either gap is a
// void in the source, DataNotGiven when every gap is ground the sampler wasn't given.
//
// answerCell(row, col, groundM, dM, horizon) is called for every other cell, with the
// ground under its centre, its distance from the observer and the horizon in front of
// it (-infinity when no terrain stands in front of it). ComputeViewshedFast asks it
// whether a target is seen; ComputeMinimumVisibleHeightFast (MinimumVisibleHeight.h),
// how high a target must stand to be seen.
//
// Returns false, leaving the cells partly answered, when progress asked it to stop.
// Progress: 0 first, then before every block of 16 rays and every grid row of the answering pass
// the calling thread takes. `cells` must already be gridRows x gridCols.
//
// Complexity: O(gridRows + gridCols) rays, each O(samples per profile) to cast;
// then O(log rays + 1) per cell. Memory: one float per ray sample, about
// 4 * gridSize * gridSize * 0.7 of them -- ~45 MB for a 30 km radius at 30 m.
//
// Threads: threadCount at a time, 1 by default, 0 for one per hardware thread. Rays are
// taken sixteen at a time, each cast on its own into its own place, and gathered in the same
// order as on one thread before they are sorted; cells are then taken a row at a time
// (ForEachRowOnThreads), each answered on its own from the finished rays. Nothing depends on
// which thread did what, or in what order, so every cell is the same, bit for bit, at any
// thread count. answerCell runs on several threads at once and must write only its own cell;
// the sampler is read from every thread at once and must allow it, as every sampler in this
// library does. Progress is reported on the calling thread only.
template <class AnswerCell>
bool AnswerEachCellFromFastHorizons(GeoPoint observer, double observerEyeHeightM, int gridRows, int gridCols, double spacingDeg, IElevationSampler& sampler, double k, const ViewshedProgress& progress, std::vector<std::vector<CellVisibility>>& cells, AnswerCell&& answerCell, int threadCount = 1)
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
        double firstVoidM = INFINITY;       // where the ray first meets a void in the source
        double firstNotGivenM = INFINITY;   // where it first meets ground the sampler wasn't given
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
    const int threads = ThreadsFor(threadCount);

    // Every ray into its own place, sixteen at a time from a shared counter
    // (ForEachBlockOnThreads), each thread with its own profile buffer. The calling thread
    // reports progress before each block it takes.
    std::vector<Ray> cast(ends.size());
    std::vector<char> wasCast(ends.size(), 0);
    std::vector<std::vector<ProfileSample>> profiles(threads);
    BlockProgress raysTaken;
    if (progress) raysTaken = [&](size_t rays) { return progress(rays / workUnits); };
    bool allCast = ForEachBlockOnThreads(ends.size(), 16, threads, raysTaken, [&](size_t first, size_t last, int thread) {
        std::vector<ProfileSample>& profile = profiles[thread];
        for (size_t i = first; i < last; i++)
        {
            auto [row, col] = ends[i];
            if (row == centerRow && col == centerCol) continue;

            GetTerrainProfile(observer, cellCentre(row, col), spacingDeg, sampler, profile);
            Ray& ray = cast[i];
            ray.direction = directionOf(row, col);
            ray.stepM = profile.back().distanceFromStartM / (profile.size() - 1);
            ray.horizon.reserve(profile.size() - 1);
            double horizon = -INFINITY;
            for (size_t s = 1; s < profile.size(); s++)
            {
                double dM = profile[s].distanceFromStartM;
                if (profile[s].dataNotGiven)
                {
                    ray.firstNotGivenM = (std::min)(ray.firstNotGivenM, dM);
                }
                else if (!profile[s].elevationM.has_value())
                {
                    ray.firstVoidM = (std::min)(ray.firstVoidM, dM);
                }
                else
                {
                    horizon = (std::max)(horizon, CurvatureAdjustedSlope(*profile[s].elevationM, observerEyeHeightM, dM, k));
                }
                ray.horizon.push_back((float)horizon);
            }
            wasCast[i] = 1;
        }
    }).finished;
    if (!allCast) return false;

    // Gathered in the order one thread would have cast them.
    std::vector<Ray> rays;
    rays.reserve(ends.size());
    for (size_t i = 0; i < ends.size(); i++)
    {
        if (wasCast[i]) rays.push_back(std::move(cast[i]));
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
    ViewshedProgress rowProgress;
    if (progress) rowProgress = [&](double rowsTaken) { return progress((ends.size() + rowsTaken * gridRows) / workUnits); };
    return ForEachRowOnThreads(gridRows, threadCount, rowProgress, [&](int row, int) {
        for (int col = 0; col < gridCols; col++)
        {
            if (row == centerRow && col == centerCol) continue;

            GeoPoint centre = cellCentre(row, col);
            ElevationSample ground = sampler.Sample(centre.latitudeDeg, centre.longitudeDeg);
            if (rays.empty())
            {
                cells[row][col] = NoAnswerFor(ground.data == ElevationData::Present ? ElevationData::Void : ground.data);
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

            // No ground under the centre, or a ray either side that met a gap before reaching
            // the cell: no confident answer -- a void if either gap is one, else data not given.
            double dM = GreatCircleDistanceM(observer, centre);
            bool notGiven = ground.data == ElevationData::NotGiven || rays[a].firstNotGivenM < dM || rays[b].firstNotGivenM < dM;
            bool hole = ground.data == ElevationData::Void || rays[a].firstVoidM < dM || rays[b].firstVoidM < dM;
            if (notGiven || hole)
            {
                cells[row][col] = hole ? CellVisibility::Degraded : CellVisibility::DataNotGiven;
                continue;
            }

            double ha = horizonBefore(rays[a], dM - halfCellM);
            double hb = horizonBefore(rays[b], dM - halfCellM);
            double horizon = std::isinf(ha) || std::isinf(hb) ? (std::max)(ha, hb) : ha + t * (hb - ha);

            answerCell(row, col, ground.elevationM, dM, horizon);
        }
    });
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
// Threads (optional): as for AnswerEachCellFromFastHorizons -- the same cells at any thread count.
// Progress (optional): as AnswerEachCellFromFastHorizons reports it, then once at the end.
// Target height (optional): as for ComputeViewshedNaive.
inline ViewshedResult ComputeViewshedFast(GeoPoint observer, DatumHeight observerHeight, int gridRows, int gridCols, double spacingDeg, IElevationSampler& sampler, double k = 4.0 / 3.0, const ViewshedProgress& progress = nullptr, DatumHeight targetHeight = DatumHeight{ 0.0, VerticalDatum::HeightAboveGround }, int threadCount = 1)
{
    ViewshedResult result;

    // Same as naive: a grid with no rows or no columns comes back empty.
    if (gridRows <= 0 || gridCols <= 0) return result;

    // An input it can't use is refused, with the reason, rather than laid out into a grid.
    result.inputProblem = CheckViewshedRequest(observer, observerHeight, gridRows, spacingDeg, k, targetHeight);
    if (result.inputProblem != InputProblem::None) return result;

    // Naive delegates these checks to ComputeLineOfSight per cell; fast does its own
    // curvature math and never calls it, so it checks here, and answers as naive's
    // line of sight would. A height that can't be put on the terrain's datum leaves every
    // cell Degraded; otherwise ground under the observer that is a void, or wasn't given,
    // leaves every cell without an answer for that reason. The observer's own cell is
    // Visible when its ground is known and its height has a datum -- as in naive.
    VerticalDatum terrainDatum = sampler.GetDatum();
    ElevationSample observerGround = sampler.Sample(observer.latitudeDeg, observer.longitudeDeg);
    bool observerDatumOk = CanExpressInTerrainDatum(observerHeight, terrainDatum);
    bool targetDatumOk = CanExpressInTerrainDatum(targetHeight, terrainDatum);

    int centerRow = gridRows / 2;
    int centerCol = gridCols / 2;

    if (!observerDatumOk || !targetDatumOk || observerGround.data != ElevationData::Present)
    {
        CellVisibility everyCell = !observerDatumOk || !targetDatumOk ? CellVisibility::Degraded : NoAnswerFor(observerGround.data);
        result.visible.assign(gridRows, std::vector<CellVisibility>(gridCols, everyCell));
        result.visible[centerRow][centerCol] = observerGround.data != ElevationData::Present ? NoAnswerFor(observerGround.data)
            : observerDatumOk ? CellVisibility::Visible : CellVisibility::Degraded;
        if (progress) progress(1.0);
        return result;
    }

    result.visible.resize(gridRows, std::vector<CellVisibility>(gridCols, CellVisibility::NotCovered));

    // An eye below the ground under it, or a target below the ground under it -- possible for a
    // height above sea level or the ellipsoid -- is walled in by that ground: ComputeLineOfSight
    // finds it blocking at the path's own end, and so naive hides the cell. The horizons leave
    // both ends' own ground out, so the fast test asks it here.
    double observerEyeHeightM = *EyeHeightInTerrainDatum(observerHeight, observerGround.elevationM, terrainDatum);
    bool eyeAboveGround = observerEyeHeightM >= observerGround.elevationM;
    bool finished = AnswerEachCellFromFastHorizons(observer, observerEyeHeightM, gridRows, gridCols, spacingDeg, sampler, k, progress, result.visible,
        [&](int row, int col, double groundM, double dM, double horizon) {
            double targetM = *EyeHeightInTerrainDatum(targetHeight, groundM, terrainDatum);
            bool isVisible = eyeAboveGround && targetM >= groundM && CurvatureAdjustedSlope(targetM, observerEyeHeightM, dM, k) >= horizon;
            result.visible[row][col] = isVisible ? CellVisibility::Visible : CellVisibility::NotVisible;
        }, threadCount);
    if (!finished)
    {
        result.cancelled = true;
        return result;
    }

    result.visible[centerRow][centerCol] = CellVisibility::Visible;

    if (progress) progress(1.0);
    return result;
}
