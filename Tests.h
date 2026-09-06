#pragma once
#include <cassert>
#include <cmath>
#include <iostream>
#include "IElevationSampler.h"
#include "TerrainProfile.h"
#include "LineOfSight.h"
#include "Viewshed.h"
#include <string>

inline void Expect(bool condition, const std::string& testName)
{
    if (condition)
    {
        std::cout << "PASS: " << testName << std::endl;
    }
    else
    {
        std::cout << "FAIL: " << testName << std::endl;
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
    LineOfSightResult los = ComputeLineOfSight(profile, 2.0, 2.0, 2.0);

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
    LineOfSightResult los = ComputeLineOfSight(profile, 2.0, 2.0, 4.0);

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

    // Flat-earth comparison: an enormous k makes the curvature term negligible,
    // So the exact same geometry must report visible when curvature is effectively switched off.
    LineOfSightResult flatEarth = ComputeLineOfSight(curvatureProfile, 2.0, 2.0, 50000.0, 1e12);

    LineOfSightResult curved = ComputeLineOfSight(curvatureProfile, 2.0, 2.0, 50000.0);

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

    LineOfSightResult los = ComputeLineOfSight(voidProfile, 2.0, 2.0, 2.0);

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
            if (!viewshed.visible[row][col].has_value())
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
    LineOfSightResult los1 = ComputeLineOfSight(profile1, 2.0, 2.0, 4.0);

    std::vector<ProfileSample> profile2 = GetTerrainProfile(a, b, 1.0, sampler);
    LineOfSightResult los2 = ComputeLineOfSight(profile2, 2.0, 2.0, 4.0);

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
    GeoPoint observerPos{ 2, 2 };

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
    LineOfSightResult losAB = ComputeLineOfSight(profileAB, 2.0, 2.0, 4.0);

    std::vector<ProfileSample> profileBA = GetTerrainProfile(b, a, 1.0, sampler);
    LineOfSightResult losBA = ComputeLineOfSight(profileBA, 2.0, 2.0, 4.0);

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
    LineOfSightResult los = ComputeLineOfSight(profile, 2.0, 2.0, 2.0);

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

    LineOfSightResult los = ComputeLineOfSight(slopeProfile, 2.0, 2.0, 4.0);

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

    FresnelClearanceResult result = ComputeFresnelClearance(profile, 50.0, 50.0, 10000.0, 2.4e9);

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
        { observer, 2.0, targetBlocked, 2.0, 4.0 },
        { observer, 2.0, targetClear, 2.0, sqrt(4.0 * 4.0 + 2.0 * 2.0) }
    };

    std::vector<LineOfSightResult> batchResults = ComputeBatchLineOfSight(queries, 1.0, sampler);

    std::vector<ProfileSample> profile1 = GetTerrainProfile(observer, targetBlocked, 1.0, sampler);
    LineOfSightResult direct1 = ComputeLineOfSight(profile1, 2.0, 2.0, 4.0);

    std::vector<ProfileSample> profile2 = GetTerrainProfile(observer, targetClear, 1.0, sampler);
    LineOfSightResult direct2 = ComputeLineOfSight(profile2, 2.0, 2.0, sqrt(4.0 * 4.0 + 2.0 * 2.0));

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