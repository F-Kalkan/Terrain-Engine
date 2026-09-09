#pragma once
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include "IElevationSampler.h"
#include "TerrainProfile.h"
#include "LineOfSight.h"
#include "Viewshed.h"
#include "RealElevationSampler.h"
#include <string>

inline int g_testFailureCount = 0;

inline void Expect(bool condition, const std::string& testName)
{
    if (condition)
    {
        std::cout << "PASS: " << testName << std::endl;
    }
    else
    {
        std::cout << "FAIL: " << testName << std::endl;
        g_testFailureCount++;
    }
}

// FakeElevationSampler tests use small integer lat/lon values as pretend grid
// indices, not real Earth degrees -- GetTerrainProfile's real degree-to-meter
// conversion would turn a "4 unit" hop into ~445km and let curvature swamp
// the terrain being tested. Reset distances to the synthetic scale the test
// was designed around (1 grid step = stepMeters).
inline void SetLinearDistances(std::vector<ProfileSample>& profile, double stepMeters)
{
    for (size_t i = 0; i < profile.size(); i++)
    {
        profile[i].distanceFromStartM = i * stepMeters;
    }
}

// Shorthand for "this many metres above the local terrain" -- HeightAboveGround
// is the datum ComputeLineOfSight/ComputeFresnelClearance/the viewsheds accept
// for observerHeightAgl/targetHeightAgl. Every FakeElevationSampler below also picks an
// explicit terrain datum, since the default (Unknown) is now correctly
// rejected rather than silently treated as usable.
inline DatumHeight Agl(double valueM)
{
    return DatumHeight{ valueM, VerticalDatum::HeightAboveGround };
}

// TEST 1 
void TestFlatPlateauEverythingVisible()
{
    std::vector<std::vector<double>> flatGrid = {
        {10, 10, 10},
        {10, 10, 10},
        {10, 10, 10}
    };
    FakeElevationSampler sampler(flatGrid, InterpolationMode::Nearest, VerticalDatum::OrthometricMsl);

    GeoPoint a{ 0, 0 };
    GeoPoint b{ 0, 2 };
    std::vector<ProfileSample> profile = GetTerrainProfile(a, b, 1.0, sampler);
    SetLinearDistances(profile, 1.0);
    LineOfSightResult los = ComputeLineOfSight(profile, Agl(2.0), Agl(2.0), sampler.GetDatum());

    Expect(los.isVisible == true, "TestFlatPlateauEverythingVisible");
}

// TEST 2  
void TestWallBlocksView()
{
    // A single row at the equator: away from the equator, a great-circle path
    // between two same-latitude points is very slightly shorter than the
    // naive degree-Euclidean distance (it bulges toward the pole, taking a
    // shortcut), which can shift GetTerrainProfile's floored sample count by
    // one and skip a hand-placed peak entirely. At the equator the two
    // distance models coincide exactly, so this test can rely on landing on
    // every integer column the way it always has.
    std::vector<std::vector<double>> testGrid = {
        {10, 30, 50, 30, 10}
    };
    FakeElevationSampler sampler(testGrid, InterpolationMode::Nearest, VerticalDatum::OrthometricMsl);

    GeoPoint a{ 0, 0 };
    GeoPoint b{ 0, 4 };
    std::vector<ProfileSample> profile = GetTerrainProfile(a, b, 1.0, sampler);
    SetLinearDistances(profile, 1.0);
    LineOfSightResult los = ComputeLineOfSight(profile, Agl(2.0), Agl(2.0), sampler.GetDatum());

    Expect(los.isVisible == false && std::abs(los.clearanceDeficitM - 38.0) < 0.001, "TestWallBlocksView");
}

// TEST 3 
void TestCurvatureBlocksFlatTerrain()
{
    // 50 km flat terrain (elevation constanst = 0), observer and target = 2m
    // For flat terrain it should be visible on every scenario
    // But for Earths curvature;
    // Calculation: middle point (d1=d2=25000m), curvature drop = 25000*25000 / (2 * 4/3 * 6371000) ~= 36.79m
    // deficit = 36.79 - 2 = ~34.79m

    std::vector<ProfileSample> curvatureProfile;
    for (int i = 0; i <= 10; i++)
    {
        double t = (double)i / 10;
        ProfileSample s;
        s.point = GeoPoint{ 0, t * 50000.0 };
        s.elevationM = 0;
        curvatureProfile.push_back(s);
    }
    SetLinearDistances(curvatureProfile, 5000.0);

    // Flat-earth comparison: an enormous k makes the curvature term negligible,
    // So the exact same geometry must report visible when curvature is effectively switched off.
    LineOfSightResult flatEarth = ComputeLineOfSight(curvatureProfile, Agl(2.0), Agl(2.0), VerticalDatum::OrthometricMsl, 1e12);

    LineOfSightResult curved = ComputeLineOfSight(curvatureProfile, Agl(2.0), Agl(2.0), VerticalDatum::OrthometricMsl);

    Expect(flatEarth.isVisible == true && curved.isVisible == false && std::abs(curved.clearanceDeficitM - 34.79) < 0.1,
        "TestCurvatureBlocksFlatTerrain (flat-earth visible vs curved blocked)");
}

// TEST 4
void TestVoidPointIsDegraded()
{
    // P2 = Void. Function must skip that point and mark it as degraded.

    std::vector<ProfileSample> voidProfile;

    ProfileSample p0;
    p0.point = GeoPoint{ 0, 0 };
    p0.elevationM = 10;
    voidProfile.push_back(p0);

    ProfileSample p1;
    p1.point = GeoPoint{ 0, 1 };
    p1.elevationM = std::nullopt;
    voidProfile.push_back(p1);

    ProfileSample p2;
    p2.point = GeoPoint{ 0, 2 };
    p2.elevationM = 10;
    voidProfile.push_back(p2);

    LineOfSightResult los = ComputeLineOfSight(voidProfile, Agl(2.0), Agl(2.0), VerticalDatum::OrthometricMsl);

    Expect(los.status == ComputationStatus::VoidInProfile, "TestVoidPointIsDegraded");
}

// TEST 5
void TestViewshedDetectsVoid()
{
    // Testing viewshed outside the grid
    // It should return as ?

    std::vector<std::vector<double>> smallGrid = {
        {10, 10, 10, 10, 10},
        {10, 10, 10, 10, 10},
        {10, 10, 10, 10, 10},
        {10, 10, 10, 10, 10},
        {10, 10, 10, 10, 10}
    };
    FakeElevationSampler sampler(smallGrid, InterpolationMode::Nearest, VerticalDatum::OrthometricMsl);

    GeoPoint observerPos{ 2, 2 };
    ViewshedResult viewshed = ComputeViewshedNaive(observerPos, Agl(2.0), 7, 7, 1.0, sampler);

    bool foundUnknown = false;
    for (int row = 0; row < 7; row++)
    {
        for (int col = 0; col < 7; col++)
        {
            if (viewshed.visible[row][col] == CellVisibility::Degraded)
            {
                foundUnknown = true;
            }
        }
    }

    Expect(foundUnknown == true, "TestViewshedDetectsVoid");
}

// TEST 6
void TestDeterminism()
{
    std::vector<std::vector<double>> testGrid = {
        {10, 10, 10, 10, 10},
        {10, 20, 30, 20, 10},
        {10, 30, 50, 30, 10},
        {10, 20, 30, 20, 10},
        {10, 10, 10, 10, 10}
    };
    FakeElevationSampler sampler(testGrid, InterpolationMode::Nearest, VerticalDatum::OrthometricMsl);

    GeoPoint a{ 2, 0 };
    GeoPoint b{ 2, 4 };

    std::vector<ProfileSample> profile1 = GetTerrainProfile(a, b, 1.0, sampler);
    SetLinearDistances(profile1, 1.0);
    LineOfSightResult los1 = ComputeLineOfSight(profile1, Agl(2.0), Agl(2.0), sampler.GetDatum());

    std::vector<ProfileSample> profile2 = GetTerrainProfile(a, b, 1.0, sampler);
    SetLinearDistances(profile2, 1.0);
    LineOfSightResult los2 = ComputeLineOfSight(profile2, Agl(2.0), Agl(2.0), sampler.GetDatum());

    Expect(los1.isVisible == los2.isVisible && los1.clearanceDeficitM == los2.clearanceDeficitM, "TestDeterminism");
}

//Test 7
void TestFastViewshedMatchesNaive()
{
    std::vector<std::vector<double>> testGrid = {
        {10, 10, 10, 10, 10},
        {10, 20, 30, 20, 10},
        {10, 30, 50, 30, 10},
        {10, 20, 30, 20, 10},
        {10, 10, 10, 10, 10}
    };
    FakeElevationSampler sampler(testGrid, InterpolationMode::Nearest, VerticalDatum::OrthometricMsl);
    GeoPoint observerPos{ 2, 0 };

    ViewshedResult naive = ComputeViewshedNaive(observerPos, Agl(2.0), 5, 5, 1.0, sampler);
    ViewshedResult fast = ComputeViewshedFast(observerPos, Agl(2.0), 5, 5, 1.0, sampler);

    int mismatches = 0;
    for (int row = 0; row < 5; row++)
    {
        for (int col = 0; col < 5; col++)
        {
            if (naive.visible[row][col] != fast.visible[row][col])
            {
                mismatches++;
            }
        }
    }

    Expect(mismatches == 0, "TestFastViewshedMatchesNaive");
}

//TEST 8    
void TestSymmetricHillReciprocity()
{
    std::vector<std::vector<double>> testGrid = {
        {10, 10, 10, 10, 10},
        {10, 20, 30, 20, 10},
        {10, 30, 50, 30, 10},
        {10, 20, 30, 20, 10},
        {10, 10, 10, 10, 10}
    };
    FakeElevationSampler sampler(testGrid, InterpolationMode::Nearest, VerticalDatum::OrthometricMsl);

    GeoPoint a{ 2, 0 };
    GeoPoint b{ 2, 4 };

    std::vector<ProfileSample> profileAB = GetTerrainProfile(a, b, 1.0, sampler);
    SetLinearDistances(profileAB, 1.0);
    LineOfSightResult losAB = ComputeLineOfSight(profileAB, Agl(2.0), Agl(2.0), sampler.GetDatum());

    std::vector<ProfileSample> profileBA = GetTerrainProfile(b, a, 1.0, sampler);
    SetLinearDistances(profileBA, 1.0);
    LineOfSightResult losBA = ComputeLineOfSight(profileBA, Agl(2.0), Agl(2.0), sampler.GetDatum());

    Expect(losAB.isVisible == losBA.isVisible && std::abs(losAB.clearanceDeficitM - losBA.clearanceDeficitM) < 0.001, "TestSymmetricHillReciprocity");
}

//TEST 9    
void TestObserverBelowRim()
{
    // At the equator (see TestWallBlocksView) so GetTerrainProfile's floored
    // sample count can't drift off the integer columns this test's fixed
    // crater-rim layout depends on landing on exactly.
    std::vector<std::vector<double>> testGrid = {
        {5, 30, 5, 30, 5}
    };
    FakeElevationSampler sampler(testGrid, InterpolationMode::Nearest, VerticalDatum::OrthometricMsl);

    GeoPoint a{ 0, 2 };
    GeoPoint b{ 0, 4 };

    std::vector<ProfileSample> profile = GetTerrainProfile(a, b, 1.0, sampler);
    SetLinearDistances(profile, 1.0);
    LineOfSightResult los = ComputeLineOfSight(profile, Agl(2.0), Agl(2.0), sampler.GetDatum());

    Expect(los.isVisible == false, "TestObserverBelowRim");
}

//TEST 10
void TestTargetOnFarSlopeVisible()
{
    std::vector<ProfileSample> slopeProfile;
    double elevations[] = { 0, 5, 10, 15, 20 };
    for (int i = 0; i < 5; i++)
    {
        ProfileSample s;
        s.point = GeoPoint{ 0, (double)i };
        s.elevationM = elevations[i];
        slopeProfile.push_back(s);
    }
    SetLinearDistances(slopeProfile, 1.0);

    LineOfSightResult los = ComputeLineOfSight(slopeProfile, Agl(2.0), Agl(2.0), VerticalDatum::OrthometricMsl);

    Expect(los.isVisible == true, "TestTargetOnFarSlopeVisible");
}

//TEST 11
void TestFresnelClearancePartialObstruction()
{
    // Flat terrain (0m) except a 40m bump exactly at the midpoint.
    // 10km link, 50m masts both ends, 2.4 GHz.
    // Hand calc: wavelength=0.124914m, curvatureDrop(mid)=1.47151m,
    // clearance=50-(40+1.47151)=8.52849m, fresnelRadius=sqrt(0.124914*5000*5000/10000)=17.6716m
    // fraction = 8.52849/17.6716 ~= 0.4826 (partially obstructed, but not fully blocked)
    std::vector<ProfileSample> profile;
    for (int i = 0; i <= 10; i++)
    {
        ProfileSample s;
        s.point = GeoPoint{ 0, (double)i };
        s.elevationM = (i == 5) ? 40.0 : 0.0;
        profile.push_back(s);
    }
    SetLinearDistances(profile, 1000.0);

    FresnelClearanceResult result = ComputeFresnelClearance(profile, Agl(50.0), Agl(50.0), VerticalDatum::OrthometricMsl, 2.4e9);

    Expect(std::abs(result.minClearanceFraction - 0.4826) < 0.001 && result.worstPoint.has_value() && result.worstPoint->longitudeDeg == 5.0,
        "TestFresnelClearancePartialObstruction");
}

//TEST 12
void TestBatchLineOfSightMatchesIndividualCalls()
{
    std::vector<std::vector<double>> testGrid = {
        {10, 10, 10, 10, 10},
        {10, 20, 30, 20, 10},
        {10, 30, 50, 30, 10},
        {10, 20, 30, 20, 10},
        {10, 10, 10, 10, 10}
    };
    FakeElevationSampler sampler(testGrid, InterpolationMode::Nearest, VerticalDatum::OrthometricMsl);

    GeoPoint observer{ 2, 0 };
    GeoPoint targetBlocked{ 2, 4 };
    GeoPoint targetClear{ 0, 4 };

    std::vector<BatchLineOfSightQuery> queries = {
        { observer, Agl(2.0), targetBlocked, Agl(2.0) },
        { observer, Agl(2.0), targetClear, Agl(2.0) }
    };

    std::vector<LineOfSightResult> batchResults = ComputeBatchLineOfSight(queries, 1.0, sampler);

    // Deliberately NOT overriding distances here: this test checks that batch
    // matches an equivalent direct call, so both sides must go through the
    // exact same (real-degree) GetTerrainProfile distance model as production
    // code does -- patching one side would make them diverge instead of match.
    std::vector<ProfileSample> profile1 = GetTerrainProfile(observer, targetBlocked, 1.0, sampler);
    LineOfSightResult direct1 = ComputeLineOfSight(profile1, Agl(2.0), Agl(2.0), sampler.GetDatum());

    std::vector<ProfileSample> profile2 = GetTerrainProfile(observer, targetClear, 1.0, sampler);
    LineOfSightResult direct2 = ComputeLineOfSight(profile2, Agl(2.0), Agl(2.0), sampler.GetDatum());

    Expect(batchResults.size() == 2
        && batchResults[0].isVisible == direct1.isVisible
        && std::abs(batchResults[0].clearanceDeficitM - direct1.clearanceDeficitM) < 0.001
        && batchResults[1].isVisible == direct2.isVisible,
        "TestBatchLineOfSightMatchesIndividualCalls");
}

//TEST 13
void TestMultiTileSeamIsInvisible()
{
    // Tile A covers world lat [0,1), lon [0,1). Tile B covers world lat [1,2), lon [0,1).
    // Query (0.3, 0.3) falls in Tile A; query (1.3, 0.3) falls in Tile B.
    // Both must resolve to the correct tile's data through the SAME sampler,
    // with no gap/void at the seam, and a point outside both tiles must be void.
    std::vector<std::vector<double>> gridA = { {111.0} };
    FakeElevationSampler tileA(gridA);

    std::vector<std::vector<double>> gridB = { {0.0}, {222.0} }; // row 0 unused padding, row 1 is the real data
    FakeElevationSampler tileB(gridB);

    MultiTileElevationSampler multi;
    multi.AddTile(0.0, 0.0, tileA);
    multi.AddTile(1.0, 0.0, tileB);

    auto valueInTileA = multi.GetElevation(0.3, 0.3);
    auto valueInTileB = multi.GetElevation(1.3, 0.3);
    auto valueOutsideBoth = multi.GetElevation(5.0, 5.0);

    Expect(valueInTileA.has_value() && *valueInTileA == 111.0
        && valueInTileB.has_value() && *valueInTileB == 222.0
        && !valueOutsideBoth.has_value(),
        "TestMultiTileSeamIsInvisible");
}

//TEST 14
void TestBlockingFeatureIsLocalPeak()
{
    // Same pyramid shape as the wall test: elevations along the path are
    // 10,30,50,30,10 -- the blocking point (50) is higher than both its
    // immediate neighbours (30, 30), which is exactly a local peak. At the
    // equator (see TestWallBlocksView) the great-circle and degree-Euclidean
    // distance models coincide exactly, so the profile lands on every integer
    // column the way this test's fixed elevations assume.
    std::vector<std::vector<double>> testGrid = {
        {10, 30, 50, 30, 10}
    };
    FakeElevationSampler sampler(testGrid, InterpolationMode::Nearest, VerticalDatum::OrthometricMsl);

    GeoPoint a{ 0, 0 };
    GeoPoint b{ 0, 4 };
    std::vector<ProfileSample> profile = GetTerrainProfile(a, b, 1.0, sampler);
    SetLinearDistances(profile, 1.0);
    LineOfSightResult los = ComputeLineOfSight(profile, Agl(2.0), Agl(2.0), sampler.GetDatum());

    Expect(los.isVisible == false && los.blockingFeature == TerrainFeatureType::LocalPeak, "TestBlockingFeatureIsLocalPeak");
}

//TEST 15
void TestFastViewshedVoidDegradesDownstream()
{
    // A custom sampler with a single hole at (lat=0, lon=2) -- the equator, so
    // GetTerrainProfile's floored sample count lands on every integer column
    // along this east-pointing ray exactly (see TestWallBlocksView for why
    // that stops being guaranteed away from the equator). A straight ray due
    // east from the observer at (0,0) crosses that hole before reaching
    // farther cells at lon=3 and lon=4. Naive would mark every one of those
    // farther cells as degraded too, since their own profile also passes
    // through the same hole -- ComputeViewshedFast must do the same instead
    // of only flagging the hole cell and confidently classifying what's behind it.
    class SamplerWithHole : public IElevationSampler
    {
    public:
        std::optional<double> GetElevation(double latitude, double longitude) override
        {
            int row = (int)round(latitude);
            int col = (int)round(longitude);
            if (row == 0 && col == 2) return std::nullopt;
            return 10.0;
        }

        VerticalDatum GetDatum() const override { return VerticalDatum::OrthometricMsl; }
    };

    SamplerWithHole sampler;
    GeoPoint observer{ 0, 0 };

    ViewshedResult viewshed = ComputeViewshedFast(observer, Agl(2.0), 9, 9, 1.0, sampler);

    int centerRow = 4;
    int beforeHoleCol = 5; // lon=1, before the hole -- should still resolve normally
    int holeCol = 6;       // lon=2, the hole itself
    int afterHoleCol1 = 7; // lon=3, behind the hole
    int afterHoleCol2 = 8; // lon=4, grid boundary, also behind the hole

    Expect(IsConfident(viewshed.visible[centerRow][beforeHoleCol])
        && viewshed.visible[centerRow][holeCol] == CellVisibility::Degraded
        && viewshed.visible[centerRow][afterHoleCol1] == CellVisibility::Degraded
        && viewshed.visible[centerRow][afterHoleCol2] == CellVisibility::Degraded,
        "TestFastViewshedVoidDegradesDownstream");
}

//TEST 16
void TestRealElevationSamplerReadsVoidFromFile()
{
    // REVIEW.md: "The void test does not test a void... A 2x2 synthetic .hgt
    // written by the test and read back closes this." TestVoidPointIsDegraded
    // only ever exercises the "elevation == nullopt" branch by construction, on
    // a FakeElevationSampler -- it never touches RealElevationSampler's own
    // void sentinel (-32768) or its file-reading path at all.
    std::string path = "void_test_tile.hgt";

    // 2x2 tile, row-major north-to-south / west-to-east, big-endian int16:
    // NW=100 (valid), NE=-32768 (void), SW=200 (valid), SE=300 (valid).
    int16_t samples[4] = { 100, -32768, 200, 300 };
    {
        std::ofstream file(path, std::ios::binary);
        for (int i = 0; i < 4; i++)
        {
            unsigned char highByte = (unsigned char)((samples[i] >> 8) & 0xFF);
            unsigned char lowByte = (unsigned char)(samples[i] & 0xFF);
            file.write((char*)&highByte, 1);
            file.write((char*)&lowByte, 1);
        }
    }

    RealElevationSampler sampler(path, 0.0, 0.0);

    bool loaded = sampler.IsLoaded();
    auto nw = sampler.GetElevation(1.0, 0.0);
    auto ne = sampler.GetElevation(1.0, 1.0);
    auto sw = sampler.GetElevation(0.0, 0.0);
    auto se = sampler.GetElevation(0.0, 1.0);

    std::remove(path.c_str());

    Expect(loaded
        && nw.has_value() && *nw == 100.0
        && !ne.has_value()
        && sw.has_value() && *sw == 200.0
        && se.has_value() && *se == 300.0,
        "TestRealElevationSamplerReadsVoidFromFile");
}

//TEST 17
void TestInterpolationModesDifferOnRidgeline()
{
    // REVIEW.md: "no test puts the two modes on the same ridgeline to show the
    // difference." Query a point straddling the pyramid's ridge (between the
    // peak, 50, and its shoulder, 30) with both modes and show they disagree,
    // by a hand-calculable amount.
    std::vector<std::vector<double>> testGrid = {
        {10, 10, 10, 10, 10},
        {10, 20, 30, 20, 10},
        {10, 30, 50, 30, 10},
        {10, 20, 30, 20, 10},
        {10, 10, 10, 10, 10}
    };

    FakeElevationSampler nearestSampler(testGrid, InterpolationMode::Nearest);
    FakeElevationSampler bilinearSampler(testGrid, InterpolationMode::Bilinear);

    // Halfway between (row 2, col 2)=50 (the peak) and (row 2, col 3)=30 (its
    // shoulder). Nearest rounds to the shoulder post itself (30); bilinear
    // averages the peak into it and reports a value no real post has (40).
    auto nearestValue = nearestSampler.GetElevation(2.0, 2.5);
    auto bilinearValue = bilinearSampler.GetElevation(2.0, 2.5);

    Expect(nearestValue.has_value() && *nearestValue == 30.0
        && bilinearValue.has_value() && std::abs(*bilinearValue - 40.0) < 0.001
        && std::abs(*nearestValue - *bilinearValue) >= 5.0,
        "TestInterpolationModesDifferOnRidgeline");
}

//TEST 18
void TestProfileMatchesFrozenOracle()
{
    // Task spec step 3: "Freeze [the naive profile's] output; it is the oracle
    // everything after it is measured against... Do not delete it later -- it
    // is a deliverable." REVIEW.md: no golden-output file or comparison test
    // existed. DATA/oracle_profile.csv freezes GetTerrainProfile's own output
    // for this fixed input; this test regenerates the same profile and diffs
    // against it, so any future change to the distance/sampling model that
    // silently shifts these numbers gets caught here.
    //
    // The frozen values were regenerated once, deliberately, when
    // GetTerrainProfile switched from a flat-plane distance approximation to
    // a true great-circle (haversine) one: at this profile's non-equatorial
    // latitude, a great-circle path bulges slightly toward the pole and is
    // marginally shorter than the old flat estimate, which also shifted the
    // floored sample count from 5 samples to 4. This is exactly the kind of
    // change this test exists to catch -- caught here, verified, and the
    // oracle updated on purpose, not silently.
    std::vector<std::vector<double>> testGrid = {
        {10, 10, 10, 10, 10},
        {10, 20, 30, 20, 10},
        {10, 30, 50, 30, 10},
        {10, 20, 30, 20, 10},
        {10, 10, 10, 10, 10}
    };
    FakeElevationSampler sampler(testGrid);

    GeoPoint a{ 2, 0 };
    GeoPoint b{ 2, 4 };
    std::vector<ProfileSample> profile = GetTerrainProfile(a, b, 1.0, sampler);

    std::ifstream oracleFile("DATA/oracle_profile.csv");
    bool matches = oracleFile.is_open();

    std::string header;
    std::getline(oracleFile, header); // skip header line

    size_t index = 0;
    std::string line;
    while (matches && std::getline(oracleFile, line))
    {
        if (index >= profile.size())
        {
            matches = false;
            break;
        }

        std::stringstream ss(line);
        std::string latStr, lonStr, elevStr, distStr;
        std::getline(ss, latStr, ',');
        std::getline(ss, lonStr, ',');
        std::getline(ss, elevStr, ',');
        std::getline(ss, distStr, ',');

        double expectedLat = std::stod(latStr);
        double expectedLon = std::stod(lonStr);
        double expectedElev = std::stod(elevStr);
        double expectedDist = std::stod(distStr);

        const ProfileSample& actual = profile[index];
        double actualElev = actual.elevationM.has_value() ? *actual.elevationM : -1.0;

        if (std::abs(actual.point.latitudeDeg - expectedLat) > 1e-6
            || std::abs(actual.point.longitudeDeg - expectedLon) > 1e-6
            || std::abs(actualElev - expectedElev) > 1e-6
            || std::abs(actual.distanceFromStartM - expectedDist) > 1e-6)
        {
            matches = false;
        }

        index++;
    }

    if (index != profile.size()) matches = false;

    Expect(matches, "TestProfileMatchesFrozenOracle");
}

//TEST 19
void TestNarrowSpikeCanFallBetweenSamples()
{
    // README.md: "Because sampleCount is floored, the effective spacing along
    // the path... can be slightly larger than the requested spacing... a peak
    // narrower than this effective spacing can fall between two samples and be
    // missed." REVIEW.md: this cost is stated but never shown. A one-cell
    // spike (999) sits at column 1; requesting spacing=1.5 over a distance of
    // 5 floors sampleCount to 3, so the four samples land on columns
    // 0, 2 (rounded from 1.667), 3, 5 -- column 1 is never queried at all.
    std::vector<std::vector<double>> spikeGrid = {
        { 0, 999, 0, 0, 0, 0 }
    };
    FakeElevationSampler sampler(spikeGrid);

    GeoPoint a{ 0, 0 };
    GeoPoint b{ 0, 5 };
    std::vector<ProfileSample> profile = GetTerrainProfile(a, b, 1.5, sampler);

    bool spikeSeen = false;
    for (const auto& sample : profile)
    {
        if (sample.elevationM.has_value() && *sample.elevationM == 999.0)
        {
            spikeSeen = true;
        }
    }

    Expect(profile.size() == 4 && !spikeSeen,
        "TestNarrowSpikeCanFallBetweenSamples (floored sampleCount misses a one-cell spike)");
}

//TEST 20
void TestMultiTileProfileCrossesSeamWithoutGap()
{
    // README.md explicitly claims multi-tile stitching was "verified with a
    // profile whose path crosses the seam directly" -- REVIEW.md points out
    // TestMultiTileSeamIsInvisible only ever does point queries, never a
    // profile. This builds an actual GetTerrainProfile path that starts in
    // one tile and ends in the next, crossing the lat=1.0 seam, and checks
    // every sample resolved to real data -- no gap or void exactly at the
    // boundary.
    std::vector<std::vector<double>> gridA = {
        { 10, 10 },
        { 10, 10 }
    };
    FakeElevationSampler tileA(gridA);

    std::vector<std::vector<double>> gridB = {
        { 20, 20 },
        { 20, 20 }
    };
    FakeElevationSampler tileB(gridB);

    MultiTileElevationSampler multi;
    multi.AddTile(0.0, 0.0, tileA);
    multi.AddTile(1.0, 0.0, tileB);

    GeoPoint a{ 0.7, 0.3 };
    GeoPoint b{ 1.3, 0.3 };
    std::vector<ProfileSample> profile = GetTerrainProfile(a, b, 0.2, multi);

    bool allResolved = profile.size() > 1;
    for (const auto& sample : profile)
    {
        if (!sample.elevationM.has_value())
        {
            allResolved = false;
        }
    }

    Expect(allResolved, "TestMultiTileProfileCrossesSeamWithoutGap");
}

//TEST 21
void TestEmptyAndSingleSampleProfilesAreDegradedNotUB()
{
    // REVIEW.md smaller item: profile.front()/back() on an empty profile is
    // undefined behaviour, and a single-sample profile divides by zero in
    // t = i / (profile.size() - 1). GetTerrainProfile never produces either,
    // but ComputeLineOfSight/ComputeFresnelClearance are public entry points
    // that accept any caller-built profile, so they must guard against both.
    std::vector<ProfileSample> emptyProfile;
    std::vector<ProfileSample> singleSampleProfile;
    ProfileSample onlySample;
    onlySample.point = GeoPoint{ 0, 0 };
    onlySample.elevationM = 10.0;
    onlySample.distanceFromStartM = 0.0;
    singleSampleProfile.push_back(onlySample);

    LineOfSightResult losEmpty = ComputeLineOfSight(emptyProfile, Agl(2.0), Agl(2.0), VerticalDatum::OrthometricMsl);
    LineOfSightResult losSingle = ComputeLineOfSight(singleSampleProfile, Agl(2.0), Agl(2.0), VerticalDatum::OrthometricMsl);
    FresnelClearanceResult fresnelEmpty = ComputeFresnelClearance(emptyProfile, Agl(2.0), Agl(2.0), VerticalDatum::OrthometricMsl, 2.4e9);
    FresnelClearanceResult fresnelSingle = ComputeFresnelClearance(singleSampleProfile, Agl(2.0), Agl(2.0), VerticalDatum::OrthometricMsl, 2.4e9);

    Expect(losEmpty.status == ComputationStatus::EmptyOrSingleSampleProfile
        && losSingle.status == ComputationStatus::EmptyOrSingleSampleProfile
        && fresnelEmpty.status == ComputationStatus::EmptyOrSingleSampleProfile
        && fresnelSingle.status == ComputationStatus::EmptyOrSingleSampleProfile,
        "TestEmptyAndSingleSampleProfilesAreDegradedNotUB");
}

//TEST 22
void TestBlockingFeatureClassificationIsSpacingInvariant()
{
    // REVIEW.md smaller item: ClassifyBlockingFeature's fixed 1 m epsilon meant
    // the same physical grade classified differently depending on sample
    // spacing -- a 2% slope sampled every 10 m has 0.2 m steps (under the old
    // fixed 1 m tolerance, misread as "flat"/Plateau), while the same 2% slope
    // sampled every 100 m has 2 m steps (correctly read as RisingSlope). The
    // epsilon must scale with spacing so the same terrain classifies the same
    // way regardless of how finely it happens to be sampled.
    std::vector<ProfileSample> fineProfile;
    double fineElevations[] = { 0.0, 0.2, 0.4 };
    for (int i = 0; i < 3; i++)
    {
        ProfileSample s;
        s.point = GeoPoint{ 0, (double)i };
        s.elevationM = fineElevations[i];
        s.distanceFromStartM = i * 10.0;
        fineProfile.push_back(s);
    }

    std::vector<ProfileSample> coarseProfile;
    double coarseElevations[] = { 0.0, 2.0, 4.0 };
    for (int i = 0; i < 3; i++)
    {
        ProfileSample s;
        s.point = GeoPoint{ 0, (double)i };
        s.elevationM = coarseElevations[i];
        s.distanceFromStartM = i * 100.0;
        coarseProfile.push_back(s);
    }

    TerrainFeatureType fineResult = ClassifyBlockingFeature(fineProfile, 1);
    TerrainFeatureType coarseResult = ClassifyBlockingFeature(coarseProfile, 1);

    Expect(fineResult == TerrainFeatureType::RisingSlope && coarseResult == TerrainFeatureType::RisingSlope,
        "TestBlockingFeatureClassificationIsSpacingInvariant");
}

//TEST 23
void TestViewshedLongitudeSpacingCorrectsForLatitude()
{
    // REVIEW.md: "the viewshed's geometry... actually covers ±30 km north-south
    // and ±24.1 km east-west... It is an ellipse described as a circle." The
    // distance/curvature fix in TerrainProfile.h doesn't touch this -- it's a
    // separate bug in how the viewshed functions choose which longitude to
    // query per grid column. LongitudeSpacingForLatitude widens the longitude
    // step by 1/cos(latitude) so a column step covers the same real ground
    // distance as a row step, at any latitude.
    double spacingDeg = 30.0 / (EarthRadiusM * DegToRad);
    double degToRad = 3.14159265358979323846 / 180.0;

    double atEquator = LongitudeSpacingForLatitude(spacingDeg, 0.0);
    double at36_5 = LongitudeSpacingForLatitude(spacingDeg, 36.5);
    double expectedRatioAt36_5 = 1.0 / cos(36.5 * degToRad);

    Expect(std::abs(atEquator - spacingDeg) < 1e-12
        && std::abs(at36_5 / spacingDeg - expectedRatioAt36_5) < 1e-9,
        "TestViewshedLongitudeSpacingCorrectsForLatitude");
}

//TEST 24
void TestConvertHeightBetweenDatums()
{
    // INTEGRATION-READINESS.md section 2: convert between ellipsoidal (HAE) and
    // orthometric (MSL) using a REQUIRED undulation argument -- ellipsoidal =
    // orthometric + undulation. Undulation of +30m here (a plausible real value;
    // globally it ranges roughly -107m to +85m).
    auto toEllipsoidal = ConvertHeightBetweenDatums(100.0, VerticalDatum::OrthometricMsl, VerticalDatum::EllipsoidalHae, 30.0);
    auto backToOrthometric = ConvertHeightBetweenDatums(*toEllipsoidal, VerticalDatum::EllipsoidalHae, VerticalDatum::OrthometricMsl, 30.0);

    // No formula exists for a pair involving PressureAltitude -- must return
    // nullopt as a value, not silently pick a number.
    auto unsupported = ConvertHeightBetweenDatums(100.0, VerticalDatum::PressureAltitude, VerticalDatum::OrthometricMsl, 30.0);

    Expect(toEllipsoidal.has_value() && std::abs(*toEllipsoidal - 130.0) < 1e-9
        && backToOrthometric.has_value() && std::abs(*backToOrthometric - 100.0) < 1e-9
        && !unsupported.has_value(),
        "TestConvertHeightBetweenDatums");
}

//TEST 25
void TestComputeLineOfSightRejectsWrongHeightDatum()
{
    // INTEGRATION-READINESS.md section 2: "reject -- as a value, not an
    // exception -- a query that mixes datums without a conversion."
    // observerHeightAgl/targetHeightAgl must be HeightAboveGround; passing an ellipsoidal height by mistake must be
    // rejected, not silently added to a terrain elevation it isn't compatible with.
    std::vector<std::vector<double>> flatGrid = {
        {10, 10, 10},
        {10, 10, 10},
        {10, 10, 10}
    };
    FakeElevationSampler sampler(flatGrid, InterpolationMode::Nearest, VerticalDatum::OrthometricMsl);

    GeoPoint a{ 0, 0 };
    GeoPoint b{ 0, 2 };
    std::vector<ProfileSample> profile = GetTerrainProfile(a, b, 1.0, sampler);
    SetLinearDistances(profile, 1.0);

    DatumHeight wrongDatum{ 2.0, VerticalDatum::EllipsoidalHae };
    LineOfSightResult los = ComputeLineOfSight(profile, wrongDatum, Agl(2.0), sampler.GetDatum());

    Expect(los.status == ComputationStatus::DatumRejected, "TestComputeLineOfSightRejectsWrongHeightDatum");
}

//TEST 26
void TestComputeLineOfSightRejectsUnknownTerrainDatum()
{
    // A sampler that never declared its datum (left at the default Unknown)
    // must not have its elevations silently trusted -- the query is rejected
    // rather than assuming the terrain values mean anything comparable to an
    // AGL height.
    std::vector<std::vector<double>> flatGrid = {
        {10, 10, 10},
        {10, 10, 10},
        {10, 10, 10}
    };
    FakeElevationSampler sampler(flatGrid); // datum left at the default: Unknown

    GeoPoint a{ 0, 0 };
    GeoPoint b{ 0, 2 };
    std::vector<ProfileSample> profile = GetTerrainProfile(a, b, 1.0, sampler);
    SetLinearDistances(profile, 1.0);

    LineOfSightResult los = ComputeLineOfSight(profile, Agl(2.0), Agl(2.0), sampler.GetDatum());

    Expect(los.status == ComputationStatus::DatumRejected && sampler.GetDatum() == VerticalDatum::Unknown,
        "TestComputeLineOfSightRejectsUnknownTerrainDatum");
}

//TEST 27
void TestRealElevationSamplerDeclaresOrthometricDatum()
{
    // INTEGRATION-READINESS.md section 2: "have IElevationSampler declare the
    // datum of what it returns, per implementation." SRTM .hgt files are
    // EGM96-referenced -- orthometric (MSL) heights -- regardless of whether
    // the specific file loads successfully.
    RealElevationSampler sampler("DATA/does_not_need_to_exist_for_this_check.hgt", 0.0, 0.0);
    Expect(sampler.GetDatum() == VerticalDatum::OrthometricMsl, "TestRealElevationSamplerDeclaresOrthometricDatum");
}

//TEST 28
void TestRasterBlockSamplerPositiveRowStep()
{
    // Positive row step: row 0 sits at the origin's own latitude, and
    // increasing row increases latitude (row 0 = south edge, row 2 = north edge).
    std::vector<std::vector<std::optional<double>>> block = {
        { 10.0, 11.0, 12.0 },
        { 20.0, 21.0, 22.0 },
        { 30.0, 31.0, 32.0 }
    };
    RasterBlockElevationSampler sampler(block, 0.0, 0.0, 1.0, 1.0, VerticalDatum::OrthometricMsl);

    auto origin = sampler.GetElevation(0.0, 0.0);
    auto farCorner = sampler.GetElevation(2.0, 2.0);
    auto outOfBounds = sampler.GetElevation(5.0, 5.0);

    Expect(sampler.Rows() == 3 && sampler.Cols() == 3
        && origin.has_value() && *origin == 10.0
        && farCorner.has_value() && *farCorner == 32.0
        && !outOfBounds.has_value()
        && sampler.GetDatum() == VerticalDatum::OrthometricMsl,
        "TestRasterBlockSamplerPositiveRowStep");
}

//TEST 29
void TestRasterBlockSamplerNegativeRowStepPlacesRowZeroAtNorth()
{
    // INTEGRATION-READINESS.md: "The host's blocks carry a signed latitude
    // step, typically negative -- row 0 is the northern edge... make the step
    // signed and the ambiguity disappears rather than needing a paragraph."
    // Origin is the north-west corner (latitude 2); a negative row step means
    // increasing row index moves SOUTH (decreasing latitude).
    std::vector<std::vector<std::optional<double>>> block = {
        { 10.0, 11.0, 12.0 }, // row 0: the northernmost row (latitude 2)
        { 20.0, 21.0, 22.0 },
        { 30.0, 31.0, 32.0 }  // row 2: the southernmost row (latitude 0)
    };
    RasterBlockElevationSampler sampler(block, 2.0, 0.0, -1.0, 1.0, VerticalDatum::OrthometricMsl);

    auto north = sampler.GetElevation(2.0, 0.0); // origin itself, the north edge
    auto south = sampler.GetElevation(0.0, 0.0); // 2 degrees south of origin

    Expect(north.has_value() && *north == 10.0
        && south.has_value() && *south == 30.0,
        "TestRasterBlockSamplerNegativeRowStepPlacesRowZeroAtNorth");
}

//TEST 30
void TestRasterBlockSamplerVoidCellPassesThrough()
{
    // A cell's own std::optional carries validity directly -- no invented
    // sentinel value, unlike RealElevationSampler's -32768.
    std::vector<std::vector<std::optional<double>>> block = {
        { 10.0, std::nullopt },
        { 20.0, 21.0 }
    };
    RasterBlockElevationSampler sampler(block, 0.0, 0.0, 1.0, 1.0, VerticalDatum::OrthometricMsl);

    auto validCell = sampler.GetElevation(0.0, 0.0);
    auto voidCell = sampler.GetElevation(0.0, 1.0);

    Expect(validCell.has_value() && *validCell == 10.0 && !voidCell.has_value(),
        "TestRasterBlockSamplerVoidCellPassesThrough");
}

//TEST 31
void TestRasterBlockSamplerWorksWithLineOfSight()
{
    // "IElevationSampler itself survives" -- GetTerrainProfile/ComputeLineOfSight
    // need no changes at all to use this sampler instead of a file-backed one.
    std::vector<std::vector<std::optional<double>>> block = {
        { 10.0, 10.0, 10.0, 10.0, 10.0 },
        { 10.0, 10.0, 10.0, 10.0, 10.0 },
        { 10.0, 10.0, 10.0, 10.0, 10.0 }
    };
    RasterBlockElevationSampler sampler(block, 0.0, 0.0, 1.0, 1.0, VerticalDatum::OrthometricMsl);

    GeoPoint a{ 0, 0 };
    GeoPoint b{ 0, 4 };
    std::vector<ProfileSample> profile = GetTerrainProfile(a, b, 1.0, sampler);
    SetLinearDistances(profile, 1.0);
    LineOfSightResult los = ComputeLineOfSight(profile, Agl(2.0), Agl(2.0), sampler.GetDatum());

    Expect(los.isVisible == true && IsOk(los.status), "TestRasterBlockSamplerWorksWithLineOfSight");
}

//TEST 32
void TestScratchBufferProfileAllocatesNothingOnReuse()
{
    // INTEGRATION-READINESS.md: a per-frame line-of-sight query should be able to
    // reuse one caller-owned buffer instead of allocating a new vector every call.
    // std::vector::clear() keeps its capacity, so a second call that needs no more
    // elements than the first must not grow capacity -- that is the observable
    // proof that the buffer was reused rather than replaced.
    std::vector<std::vector<double>> flatGrid = {
        {10, 10, 10, 10, 10},
        {10, 10, 10, 10, 10},
        {10, 10, 10, 10, 10},
        {10, 10, 10, 10, 10},
        {10, 10, 10, 10, 10}
    };
    FakeElevationSampler sampler(flatGrid, InterpolationMode::Nearest, VerticalDatum::OrthometricMsl);

    GeoPoint a{ 0, 0 };
    GeoPoint b{ 0, 4 };

    std::vector<ProfileSample> scratch;
    GetTerrainProfile(a, b, 1.0, sampler, scratch);
    size_t firstSize = scratch.size();
    size_t firstCapacity = scratch.capacity();

    GetTerrainProfile(a, b, 1.0, sampler, scratch);
    size_t secondSize = scratch.size();
    size_t secondCapacity = scratch.capacity();

    SetLinearDistances(scratch, 1.0);
    LineOfSightResult los = ComputeLineOfSight(scratch, Agl(2.0), Agl(2.0), sampler.GetDatum());

    Expect(firstSize == secondSize && secondCapacity == firstCapacity
        && los.isVisible == true && IsOk(los.status),
        "TestScratchBufferProfileAllocatesNothingOnReuse");
}