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
    std::cout << "Pass: TestFlatPlateauEverythingVisible" << std::endl;
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
    std::cout << "Pass: TestWallBlocksView" << std::endl;
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
    std::cout << "Pass: TestCurvatureBlocksFlatTerrain" << std::endl;
}