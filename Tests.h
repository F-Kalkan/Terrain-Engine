#pragma once
#include <cassert>
#include <cmath>
#include <iostream>
#include "IElevationSampler.h"
#include "TerrainProfile.h"
#include "LineOfSight.h"

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

    assert(los.isVisible == true);
    std::cout << "PASS: TestFlatPlateauEverythingVisible" << std::endl;
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

    assert(los.isVisible == false);
    assert(std::abs(los.clearanceDeficit - 38.0) < 0.001);
    std::cout << "PASS: TestWallBlocksView" << std::endl;
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

    LineOfSightResult los = ComputeLineOfSight(curvatureProfile, 2.0, 2.0, 50000.0);

    assert(los.isVisible == false);
    assert(std::abs(los.clearanceDeficit - 34.79) < 0.1);
    std::cout << "PASS: TestCurvatureBlocksFlatTerrain" << std::endl;
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

    assert(los.isDegraded == true);
    std::cout << "PASS: TestVoidPointIsDegraded" << std::endl;
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

    GeoPoint observerPos{ 0, 0 };
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

    assert(foundUnknown == true);
    std::cout << "PASS: TestViewshedDetectsVoid" << std::endl;
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

    assert(los1.isVisible == los2.isVisible);
    assert(los1.clearanceDeficit == los2.clearanceDeficit);
    std::cout << "PASS: TestDeterminism" << std::endl;
}