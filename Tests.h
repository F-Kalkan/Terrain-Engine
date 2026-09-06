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

// TEST 1 
void TestFlatPlateauEverythingVisible()
{
    std::vector<std::vector<double>> flatGrid = {
        {10, 10, 10},
        {10, 10, 10},
        {10, 10, 10}
    };
    FakeElevationSampler sampler(flatGrid);

    GeoPoint a{ 0, 0 };
    GeoPoint b{ 0, 2 };
    std::vector<ProfileSample> profile = GetTerrainProfile(a, b, 1.0, sampler);
    SetLinearDistances(profile, 1.0);
    LineOfSightResult los = ComputeLineOfSight(profile, 2.0, 2.0);

    Expect(los.isVisible == true, "TestFlatPlateauEverythingVisible");
}

// TEST 2  
void TestWallBlocksView()
{
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
    SetLinearDistances(profile, 1.0);
    LineOfSightResult los = ComputeLineOfSight(profile, 2.0, 2.0);

    Expect(los.isVisible == false && std::abs(los.clearanceDeficit - 38.0) < 0.001, "TestWallBlocksView");
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
        s.elevation = 0;
        curvatureProfile.push_back(s);
    }
    SetLinearDistances(curvatureProfile, 5000.0);

    // Flat-earth comparison: an enormous k makes the curvature term negligible,
    // So the exact same geometry must report visible when curvature is effectively switched off.
    LineOfSightResult flatEarth = ComputeLineOfSight(curvatureProfile, 2.0, 2.0, 1e12);

    LineOfSightResult curved = ComputeLineOfSight(curvatureProfile, 2.0, 2.0);

    Expect(flatEarth.isVisible == true && curved.isVisible == false && std::abs(curved.clearanceDeficit - 34.79) < 0.1,
        "TestCurvatureBlocksFlatTerrain (flat-earth visible vs curved blocked)");
}

// TEST 4
void TestVoidPointIsDegraded()
{
    // P2 = Void. Function must skip that point and mark it as degraded.

    std::vector<ProfileSample> voidProfile;

    ProfileSample p0;
    p0.point = GeoPoint{ 0, 0 };
    p0.elevation = 10;
    voidProfile.push_back(p0);

    ProfileSample p1;
    p1.point = GeoPoint{ 0, 1 };
    p1.elevation = std::nullopt;
    voidProfile.push_back(p1);

    ProfileSample p2;
    p2.point = GeoPoint{ 0, 2 };
    p2.elevation = 10;
    voidProfile.push_back(p2);

    LineOfSightResult los = ComputeLineOfSight(voidProfile, 2.0, 2.0);

    Expect(los.isDegraded == true, "TestVoidPointIsDegraded");
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
    FakeElevationSampler sampler(smallGrid);

    GeoPoint observerPos{ 2, 2 };
    ViewshedResult viewshed = ComputeViewshedNaive(observerPos, 2.0, 7, 7, 1.0, sampler);

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
    FakeElevationSampler sampler(testGrid);

    GeoPoint a{ 2, 0 };
    GeoPoint b{ 2, 4 };

    std::vector<ProfileSample> profile1 = GetTerrainProfile(a, b, 1.0, sampler);
    SetLinearDistances(profile1, 1.0);
    LineOfSightResult los1 = ComputeLineOfSight(profile1, 2.0, 2.0);

    std::vector<ProfileSample> profile2 = GetTerrainProfile(a, b, 1.0, sampler);
    SetLinearDistances(profile2, 1.0);
    LineOfSightResult los2 = ComputeLineOfSight(profile2, 2.0, 2.0);

    Expect(los1.isVisible == los2.isVisible && los1.clearanceDeficit == los2.clearanceDeficit, "TestDeterminism");
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
    FakeElevationSampler sampler(testGrid);
    GeoPoint observerPos{ 2, 0 };

    ViewshedResult naive = ComputeViewshedNaive(observerPos, 2.0, 5, 5, 1.0, sampler);
    ViewshedResult fast = ComputeViewshedFast(observerPos, 2.0, 5, 5, 1.0, sampler);

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
    FakeElevationSampler sampler(testGrid);

    GeoPoint a{ 2, 0 };
    GeoPoint b{ 2, 4 };

    std::vector<ProfileSample> profileAB = GetTerrainProfile(a, b, 1.0, sampler);
    SetLinearDistances(profileAB, 1.0);
    LineOfSightResult losAB = ComputeLineOfSight(profileAB, 2.0, 2.0);

    std::vector<ProfileSample> profileBA = GetTerrainProfile(b, a, 1.0, sampler);
    SetLinearDistances(profileBA, 1.0);
    LineOfSightResult losBA = ComputeLineOfSight(profileBA, 2.0, 2.0);

    Expect(losAB.isVisible == losBA.isVisible && std::abs(losAB.clearanceDeficit - losBA.clearanceDeficit) < 0.001, "TestSymmetricHillReciprocity");
}

//TEST 9    
void TestObserverBelowRim()
{
    std::vector<std::vector<double>> testGrid = {
        {5, 5, 5, 5, 5},
        {5, 30, 30, 30, 5},
        {5, 30, 5, 30, 5},
        {5, 30, 30, 30, 5},
        {5, 5, 5, 5, 5}
    };
    FakeElevationSampler sampler(testGrid);

    GeoPoint a{ 2, 2 };
    GeoPoint b{ 2, 4 };

    std::vector<ProfileSample> profile = GetTerrainProfile(a, b, 1.0, sampler);
    SetLinearDistances(profile, 1.0);
    LineOfSightResult los = ComputeLineOfSight(profile, 2.0, 2.0);

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
        s.elevation = elevations[i];
        slopeProfile.push_back(s);
    }
    SetLinearDistances(slopeProfile, 1.0);

    LineOfSightResult los = ComputeLineOfSight(slopeProfile, 2.0, 2.0);

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
        s.elevation = (i == 5) ? 40.0 : 0.0;
        profile.push_back(s);
    }
    SetLinearDistances(profile, 1000.0);

    FresnelClearanceResult result = ComputeFresnelClearance(profile, 50.0, 50.0, 2.4e9);

    Expect(std::abs(result.minClearanceFraction - 0.4826) < 0.001 && result.worstPoint.has_value() && result.worstPoint->longitude == 5.0,
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
    FakeElevationSampler sampler(testGrid);

    GeoPoint observer{ 2, 0 };
    GeoPoint targetBlocked{ 2, 4 };
    GeoPoint targetClear{ 0, 4 };

    std::vector<BatchLineOfSightQuery> queries = {
        { observer, 2.0, targetBlocked, 2.0 },
        { observer, 2.0, targetClear, 2.0 }
    };

    std::vector<LineOfSightResult> batchResults = ComputeBatchLineOfSight(queries, 1.0, sampler);

    // Deliberately NOT overriding distances here: this test checks that batch
    // matches an equivalent direct call, so both sides must go through the
    // exact same (real-degree) GetTerrainProfile distance model as production
    // code does -- patching one side would make them diverge instead of match.
    std::vector<ProfileSample> profile1 = GetTerrainProfile(observer, targetBlocked, 1.0, sampler);
    LineOfSightResult direct1 = ComputeLineOfSight(profile1, 2.0, 2.0);

    std::vector<ProfileSample> profile2 = GetTerrainProfile(observer, targetClear, 1.0, sampler);
    LineOfSightResult direct2 = ComputeLineOfSight(profile2, 2.0, 2.0);

    Expect(batchResults.size() == 2
        && batchResults[0].isVisible == direct1.isVisible
        && std::abs(batchResults[0].clearanceDeficit - direct1.clearanceDeficit) < 0.001
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
    // immediate neighbours (30, 30), which is exactly a local peak.
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
    SetLinearDistances(profile, 1.0);
    LineOfSightResult los = ComputeLineOfSight(profile, 2.0, 2.0);

    Expect(los.isVisible == false && los.blockingFeature == TerrainFeatureType::LocalPeak,
        "TestBlockingFeatureIsLocalPeak");
}

//TEST 15
void TestFastViewshedVoidDegradesDownstream()
{
    // A custom sampler with a single hole at (lat=2, lon=2). A straight ray
    // due east from the observer at (2,0) crosses that hole before reaching
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
            if (row == 2 && col == 2) return std::nullopt;
            return 10.0;
        }
    };

    SamplerWithHole sampler;
    GeoPoint observer{ 2, 0 };

    ViewshedResult viewshed = ComputeViewshedFast(observer, 2.0, 9, 9, 1.0, sampler);

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
        double actualElev = actual.elevation.has_value() ? *actual.elevation : -1.0;

        if (std::abs(actual.point.latitude - expectedLat) > 1e-6
            || std::abs(actual.point.longitude - expectedLon) > 1e-6
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
        if (sample.elevation.has_value() && *sample.elevation == 999.0)
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
        if (!sample.elevation.has_value())
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
    onlySample.elevation = 10.0;
    onlySample.distanceFromStartM = 0.0;
    singleSampleProfile.push_back(onlySample);

    LineOfSightResult losEmpty = ComputeLineOfSight(emptyProfile, 2.0, 2.0);
    LineOfSightResult losSingle = ComputeLineOfSight(singleSampleProfile, 2.0, 2.0);
    FresnelClearanceResult fresnelEmpty = ComputeFresnelClearance(emptyProfile, 2.0, 2.0, 2.4e9);
    FresnelClearanceResult fresnelSingle = ComputeFresnelClearance(singleSampleProfile, 2.0, 2.0, 2.4e9);

    Expect(losEmpty.isDegraded && losSingle.isDegraded
        && fresnelEmpty.isDegraded && fresnelSingle.isDegraded,
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
        s.elevation = fineElevations[i];
        s.distanceFromStartM = i * 10.0;
        fineProfile.push_back(s);
    }

    std::vector<ProfileSample> coarseProfile;
    double coarseElevations[] = { 0.0, 2.0, 4.0 };
    for (int i = 0; i < 3; i++)
    {
        ProfileSample s;
        s.point = GeoPoint{ 0, (double)i };
        s.elevation = coarseElevations[i];
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
    double spacingDeg = 30.0 / 111320.0;
    double degToRad = 3.14159265358979323846 / 180.0;

    double atEquator = LongitudeSpacingForLatitude(spacingDeg, 0.0);
    double at36_5 = LongitudeSpacingForLatitude(spacingDeg, 36.5);
    double expectedRatioAt36_5 = 1.0 / cos(36.5 * degToRad);

    Expect(std::abs(atEquator - spacingDeg) < 1e-12
        && std::abs(at36_5 / spacingDeg - expectedRatioAt36_5) < 1e-9,
        "TestViewshedLongitudeSpacingCorrectsForLatitude");
}