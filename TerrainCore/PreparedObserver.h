#pragma once
#include <cmath>
#include <vector>
#include "Viewshed.h"

// Many answers per second from a fixed observer. A ground observer stays put while its
// targets move on every update: anywhere within a radius, on the ground or in the air.
// Answering each with its own line of sight re-reads the same terrain every time.
// PrepareObserver reads it once: rays from the observer in evenly spaced directions, each
// keeping its horizon -- the largest curvature-adjusted slope of the terrain so far
// (CurvatureAdjustedSlope, Viewshed.h) -- one float per sample. QueryTarget then answers a
// target from the two rays either side of it, blended by its direction between them, as
// the fast viewshed answers a cell: a target is seen when its own slope is at least the
// horizon in front of it. That is a direction, a distance, two table reads and a
// comparison, and allocates nothing.
//
// ComputeLineOfSight stays the reference: its answer along the target's own line is the
// exact one, and how far QueryTarget is from it is measured -- see docs/ENGINE.md.

// Where a direction on the Earth points from start: the initial great-circle bearing to
// end, in radians clockwise from north, in [0, 2 pi).
//
// Complexity: O(1). Thread-safety: pure function, safe to call concurrently.
inline double InitialBearingRad(GeoPoint start, GeoPoint end)
{
    const double twoPi = 2 * 3.14159265358979323846;
    double lat1 = start.latitudeDeg * DegToRad, lat2 = end.latitudeDeg * DegToRad;
    double dLon = (end.longitudeDeg - start.longitudeDeg) * DegToRad;
    double bearing = std::atan2(std::sin(dLon) * std::cos(lat2), std::cos(lat1) * std::sin(lat2) - std::sin(lat1) * std::cos(lat2) * std::cos(dLon));
    return bearing < 0 ? bearing + twoPi : bearing;
}

// The point distanceM along the great circle leaving start at bearingRad.
//
// Complexity: O(1). Thread-safety: pure function, safe to call concurrently.
inline GeoPoint GreatCircleDestination(GeoPoint start, double bearingRad, double distanceM)
{
    double angle = distanceM / EarthRadiusM;
    double lat1 = start.latitudeDeg * DegToRad, lon1 = start.longitudeDeg * DegToRad;
    double lat2 = std::asin(std::sin(lat1) * std::cos(angle) + std::cos(lat1) * std::sin(angle) * std::cos(bearingRad));
    double lon2 = lon1 + std::atan2(std::sin(bearingRad) * std::sin(angle) * std::cos(lat1), std::cos(angle) - std::sin(lat1) * std::sin(lat2));
    return GeoPoint{ lat2 / DegToRad, lon2 / DegToRad };
}

struct PreparedObserver
{
    GeoPoint observer{};
    double observerEyeM = 0.0;          // in terrainDatum
    double k = 4.0 / 3.0;
    double radiusM = 0.0;
    double stepM = 0.0;                 // sample spacing along every ray
    double leaveOutM = 0.0;             // terrain this close in front of a target is left out of its horizon
    int rayCount = 0;
    int samplesPerRay = 0;
    VerticalDatum terrainDatum = VerticalDatum::Unknown;
    IElevationSampler* sampler = nullptr; // not owned; reads the ground under each target

    // False when nothing is known about the observer -- its ground is a void or wasn't given,
    // or its height can't be put on the terrain's datum: every target then gets
    // unknownObserverAnswer, DataNotGiven for ground not given and Degraded otherwise, as in
    // a viewshed.
    bool observerKnown = false;
    CellVisibility unknownObserverAnswer = CellVisibility::Degraded;

    // Ray r points at bearing 2 pi r / rayCount. horizon[r * samplesPerRay + i] is its
    // horizon over samples 1 to i + 1, sample j lying j * stepM from the observer.
    std::vector<float> horizon;

    // Per ray, how far out its first void lies, and the first ground it wasn't given;
    // infinity when it has none.
    std::vector<float> firstVoidM;
    std::vector<float> firstNotGivenM;

    bool cancelled = false;
    InputProblem inputProblem = InputProblem::None;
};

struct TargetAnswer
{
    // Visible or NotVisible; NotCovered for a target beyond the prepared radius; DataNotGiven
    // where ground under the target or on the way to it wasn't given to the sampler; Degraded
    // where the answer isn't confident -- a void on the way or under the target, a height
    // that can't be put on the terrain's datum, nothing known about the observer.
    CellVisibility state = CellVisibility::NotCovered;
    InputProblem inputProblem = InputProblem::None; // a target QueryTarget refuses: state Degraded
};

// The rays a radius needs so that, at the radius, neighbouring rays are no further apart
// than the spacing: 2 pi radius / spacing, rounded up.
//
// Complexity: O(1). Thread-safety: pure function, safe to call concurrently.
inline int RaysForRadius(double radiusM, double spacingM)
{
    return (int)std::ceil(2 * 3.14159265358979323846 * radiusM / spacingM);
}

// === PREPARATION: ONCE PER OBSERVER ===
// Casts rayCount rays of radiusM from the observer, sampled at spacingDeg, and keeps each
// one's horizon. rayCount 0 picks RaysForRadius at the spacing. The sampler is kept to read
// the ground under targets later and must outlive the result.
//
// Complexity: O(rayCount * radius / spacing) samples. Memory: one float per sample --
// ~70 MB for 50 km at 30 m, with 10,472 rays.
// Thread-safety: single-thread-only; calls the sampler sequentially.
// Progress (optional): reported before every 64th ray, then once at the end.
inline PreparedObserver PrepareObserver(GeoPoint observer, DatumHeight observerHeight, double radiusM, double spacingDeg, IElevationSampler& sampler, double k = 4.0 / 3.0, int rayCount = 0, const ViewshedProgress& progress = nullptr)
{
    PreparedObserver prepared;
    prepared.observer = observer;
    prepared.k = k;
    prepared.radiusM = radiusM;
    prepared.sampler = &sampler;
    prepared.terrainDatum = sampler.GetDatum();

    prepared.inputProblem = CheckPoint(observer);
    if (prepared.inputProblem == InputProblem::None) prepared.inputProblem = CheckLineOfSightInputs(observerHeight, DatumHeight{ 0.0, VerticalDatum::HeightAboveGround }, k);
    if (prepared.inputProblem == InputProblem::None && !(std::isfinite(spacingDeg) && spacingDeg > 0.0)) prepared.inputProblem = InputProblem::SpacingNotPositive;
    if (prepared.inputProblem == InputProblem::None && !(std::isfinite(radiusM) && radiusM > 0.0)) prepared.inputProblem = InputProblem::RadiusNotPositive;
    if (prepared.inputProblem != InputProblem::None) return prepared;

    double spacingM = spacingDeg * EarthRadiusM * DegToRad;
    prepared.rayCount = rayCount > 0 ? rayCount : RaysForRadius(radiusM, spacingM);
    prepared.leaveOutM = spacingM / 2;

    ElevationSample observerGround = sampler.Sample(observer.latitudeDeg, observer.longitudeDeg);
    bool observerDatumOk = CanExpressInTerrainDatum(observerHeight, prepared.terrainDatum);
    prepared.observerKnown = observerGround.data == ElevationData::Present && observerDatumOk;
    if (!prepared.observerKnown)
    {
        prepared.unknownObserverAnswer = observerDatumOk ? NoAnswerFor(observerGround.data) : CellVisibility::Degraded;
        if (progress) progress(1.0);
        return prepared;
    }
    prepared.observerEyeM = *EyeHeightInTerrainDatum(observerHeight, observerGround.elevationM, prepared.terrainDatum);

    const double twoPi = 2 * 3.14159265358979323846;
    std::vector<ProfileSample> profile;
    for (int r = 0; r < prepared.rayCount; r++)
    {
        if (progress && r % 64 == 0 && !progress((double)r / prepared.rayCount))
        {
            prepared.cancelled = true;
            return prepared;
        }

        GeoPoint end = GreatCircleDestination(observer, twoPi * r / prepared.rayCount, radiusM);
        GetTerrainProfile(observer, end, spacingDeg, sampler, profile);
        if (r == 0)
        {
            prepared.samplesPerRay = (int)profile.size() - 1;
            prepared.stepM = profile.back().distanceFromStartM / prepared.samplesPerRay;
            prepared.horizon.resize((size_t)prepared.rayCount * prepared.samplesPerRay);
            prepared.firstVoidM.assign(prepared.rayCount, INFINITY);
            prepared.firstNotGivenM.assign(prepared.rayCount, INFINITY);
        }

        float* horizon = prepared.horizon.data() + (size_t)r * prepared.samplesPerRay;
        double running = -INFINITY;
        // Every ray has the first ray's sample count, give or take the rounding of its
        // length; a ray a sample short carries its last horizon on.
        for (int s = 1; s <= prepared.samplesPerRay; s++)
        {
            if (s < (int)profile.size())
            {
                if (profile[s].dataNotGiven)
                {
                    if (std::isinf(prepared.firstNotGivenM[r])) prepared.firstNotGivenM[r] = (float)profile[s].distanceFromStartM;
                }
                else if (!profile[s].elevationM.has_value())
                {
                    if (std::isinf(prepared.firstVoidM[r])) prepared.firstVoidM[r] = (float)profile[s].distanceFromStartM;
                }
                else
                {
                    running = (std::max)(running, CurvatureAdjustedSlope(*profile[s].elevationM, prepared.observerEyeM, profile[s].distanceFromStartM, k));
                }
            }
            horizon[s - 1] = (float)running;
        }
    }

    if (progress) progress(1.0);
    return prepared;
}

// === QUERY: MANY PER SECOND ===
// Whether a target at a point, at a height in any datum the terrain can be put on, is seen
// from a prepared observer. Answered from the two rays either side of the target's
// direction, blended by where it lies between them, against the horizon strictly in
// front of it, leaving out the last half sample spacing (the target's own cell, beside its
// line rather than on it). A target below the ground under it is hidden, as
// ComputeLineOfSight answers it.
//
// Complexity: O(1): a distance, a bearing, a ground read and two table reads. Allocates
// nothing. Thread-safety: as safe as the sampler's GetElevation; the prepared observer is
// only read.
inline TargetAnswer QueryTarget(const PreparedObserver& prepared, GeoPoint target, const DatumHeight& targetHeight)
{
    TargetAnswer answer;
    answer.inputProblem = CheckPoint(target);
    if (answer.inputProblem == InputProblem::None) answer.inputProblem = CheckHeight(targetHeight);
    if (answer.inputProblem == InputProblem::None) answer.inputProblem = prepared.inputProblem;
    if (answer.inputProblem != InputProblem::None || prepared.cancelled)
    {
        answer.state = CellVisibility::Degraded;
        return answer;
    }

    double dM = GreatCircleDistanceM(prepared.observer, target);
    if (dM > prepared.radiusM) return answer; // NotCovered

    // A height with no datum to put it on, Degraded, whatever is known of the observer: a line
    // of sight checks the heights first, and so does a viewshed.
    if (!CanExpressInTerrainDatum(targetHeight, prepared.terrainDatum))
    {
        answer.state = CellVisibility::Degraded;
        return answer;
    }
    if (!prepared.observerKnown)
    {
        answer.state = prepared.unknownObserverAnswer;
        return answer;
    }

    // As a line of sight answers, a void under the target or on either ray before it wins
    // over ground not given.
    const double twoPi = 2 * 3.14159265358979323846;
    double position = InitialBearingRad(prepared.observer, target) / twoPi * prepared.rayCount;
    int a = (int)std::floor(position);
    double t = position - a;
    a = ((a % prepared.rayCount) + prepared.rayCount) % prepared.rayCount;
    int b = (a + 1) % prepared.rayCount;

    ElevationSample ground = prepared.sampler->Sample(target.latitudeDeg, target.longitudeDeg);
    bool notGiven = ground.data == ElevationData::NotGiven || prepared.firstNotGivenM[a] < dM || prepared.firstNotGivenM[b] < dM;
    bool hole = ground.data == ElevationData::Void || prepared.firstVoidM[a] < dM || prepared.firstVoidM[b] < dM;
    if (notGiven || hole)
    {
        answer.state = hole ? CellVisibility::Degraded : CellVisibility::DataNotGiven;
        return answer;
    }
    double targetEyeM = *EyeHeightInTerrainDatum(targetHeight, ground.elevationM, prepared.terrainDatum);
    if (dM <= 0.0)
    {
        answer.state = CellVisibility::Visible;
        return answer;
    }

    // Samples strictly nearer than the target, less the left-out margin: horizon index count - 1.
    double inFrontM = dM - prepared.leaveOutM;
    long long count = inFrontM <= 0 ? 0 : (long long)std::ceil(inFrontM / prepared.stepM) - 1;
    if (count > prepared.samplesPerRay) count = prepared.samplesPerRay;
    double horizon = -INFINITY;
    if (count > 0)
    {
        double ha = prepared.horizon[(size_t)a * prepared.samplesPerRay + (size_t)(count - 1)];
        double hb = prepared.horizon[(size_t)b * prepared.samplesPerRay + (size_t)(count - 1)];
        horizon = std::isinf(ha) || std::isinf(hb) ? (std::max)(ha, hb) : ha + t * (hb - ha);
    }

    bool seen = targetEyeM >= ground.elevationM && CurvatureAdjustedSlope(targetEyeM, prepared.observerEyeM, dM, prepared.k) >= horizon;
    answer.state = seen ? CellVisibility::Visible : CellVisibility::NotVisible;
    return answer;
}
