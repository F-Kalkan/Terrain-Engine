#include <iostream>
#include "IElevationSampler.h"
#include "TerrainProfile.h"
#include "LineOfSight.h"
#include "Viewshed.h"
#include "Tests.h"
#include "RealElevationSampler.h"
#include <chrono>
#include <windows.h>
#include <psapi.h>
#pragma comment(lib, "psapi.lib")
#include "ImageWriter.h"

void RunPerformanceBenchmark()
{
    RealElevationSampler sampler("Data/N36W112.hgt", 36.0, -112.0);

    double metersPerDegreeLat = 111320.0;
    double spacingInDegrees = 30.0 / metersPerDegreeLat;

    // Profile Test: 50 km, 30m 
    double latDeltaFor50km = 50000.0 / metersPerDegreeLat;
    GeoPoint profileA{ 36.3, -111.5 };
    GeoPoint profileB{ 36.3 + latDeltaFor50km, -111.5 };

    auto start1 = std::chrono::high_resolution_clock::now();
    std::vector<ProfileSample> profile = GetTerrainProfile(profileA, profileB, spacingInDegrees, sampler);
    LineOfSightResult los = ComputeLineOfSight(profile, 2.0, 2.0, 50000.0);
    auto end1 = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> profileTime = end1 - start1;
    std::cout << "50km profile (" << profile.size() << " samples), Time: " << profileTime.count() << " ms" << std::endl;

    // Viewshed Test: 30km Radius
    double radiusKm = 30.0;
    double radiusInDegrees = (radiusKm * 1000.0) / metersPerDegreeLat;
    int gridSize = (int)(2 * radiusInDegrees / spacingInDegrees);

    GeoPoint viewshedObserver{ 36.5, -111.5 };

    auto start2 = std::chrono::high_resolution_clock::now();
    ViewshedResult fastResult = ComputeViewshedFast(viewshedObserver, 2.0, gridSize, gridSize, spacingInDegrees, sampler);
    auto end2 = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> fastTime = end2 - start2;
    std::cout << radiusKm << "km fast viewshed (" << gridSize << "x" << gridSize << "), Time: " << fastTime.count() << " ms" << std::endl;


    WriteProfilePGM(profile, "profile_output.pgm");
    WriteViewshedPGM(fastResult, "viewshed_output.pgm");



    //Temporary Test
    double smallRadiusKm = 2.0;
    double smallRadiusInDegrees = (smallRadiusKm * 1000.0) / metersPerDegreeLat;
    int smallGridSize = (int)(2 * smallRadiusInDegrees / spacingInDegrees);

    ViewshedResult smallFast = ComputeViewshedFast(viewshedObserver, 2.0, smallGridSize, smallGridSize, spacingInDegrees, sampler);
    ViewshedResult smallNaive = ComputeViewshedNaive(viewshedObserver, 2.0, smallGridSize, smallGridSize, spacingInDegrees, sampler);

    int mismatches = 0;
    int totalValid = 0;
    for (int row = 0; row < smallGridSize; row++)
    {
        for (int col = 0; col < smallGridSize; col++)
        {
            if (smallFast.visible[row][col].has_value() && smallNaive.visible[row][col].has_value())
            {
                totalValid++;
                if (*smallFast.visible[row][col] != *smallNaive.visible[row][col])
                {
                    mismatches++;
                }
            }
        }
    }

    std::cout << smallRadiusKm << "km karsilastirma (" << smallGridSize << "x" << smallGridSize << "): "
        << mismatches << " / " << totalValid << " hucre farkli" << std::endl;

}

int main()
{
    // Tests
    TestFlatPlateauEverythingVisible();
    TestWallBlocksView();
    TestCurvatureBlocksFlatTerrain();
    TestVoidPointIsDegraded();
    TestViewshedDetectsVoid();
    TestDeterminism();
    TestFastViewshedMatchesNaive();
    TestSymmetricHillReciprocity();
    TestObserverBelowRim();
    TestTargetOnFarSlopeVisible();


    std::cout << "-------------------------" << std::endl;

    //Naive Viewshed 
    std::vector<std::vector<double>> testGrid = {
        {10, 10, 10, 10, 10},
        {10, 20, 30, 20, 10},
        {10, 30, 50, 30, 10},
        {10, 20, 30, 20, 10},
        {10, 10, 10, 10, 10}
    };
    FakeElevationSampler sampler(testGrid);

    GeoPoint observerPos{ 2, 2 };
    ViewshedResult viewshed = ComputeViewshedNaive(observerPos, 2.0, 5, 5, 1.0, sampler);

    std::cout << "Naive Viewshed: " << std::endl;
    for (int row = 0; row < 5; row++)
    {
        for (int col = 0; col < 5; col++)
        {
            if (!viewshed.visible[row][col].has_value())
            {
                std::cout << "? ";
            }
            else if (*viewshed.visible[row][col])
            {
                std::cout << "# ";
            }
            else
            {
                std::cout << ". ";
            }
        }
        std::cout << std::endl;
    }

    std::cout << "-------------------------" << std::endl;

    RealElevationSampler realSampler("Data/N36W112.hgt", 36.0, -112.0);
    auto elevation = realSampler.GetElevation(36.5, -111.5);
    if (elevation.has_value())
    {
        std::cout << "Elevation Near Grand Canyon: " << *elevation << "m" << std::endl;
    }
    else
    {
        std::cout << "No Data" << std::endl;
    }

    std::cout << "-------------------------" << std::endl;
    
    //Fast Viewshed
    ViewshedResult fastViewshed = ComputeViewshedFast(observerPos, 2.0, 5, 5, 1.0, sampler);
    std::cout << "Fast Viewshed: " << std::endl;
    for (int row = 0; row < 5; row++)
    {
        for (int col = 0; col < 5; col++)
        {
            if (!fastViewshed.visible[row][col].has_value()) std::cout << "? ";
            else if (*fastViewshed.visible[row][col]) std::cout << "# ";
            else std::cout << ". ";
        }
        std::cout << std::endl;
    }
    
    std::cout << "-------------------------" << std::endl;
 
    RunPerformanceBenchmark();
    
    

    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    std::cout << "-------------------------" << std::endl;

    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
    {
        double peakMB = pmc.PeakWorkingSetSize / (1024.0 * 1024.0);
        std::cout << "Peak memory usage: " << peakMB << " MB" << std::endl;
    }



    return 0;
}

