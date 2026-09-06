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
#include <fstream>

void RunWallTimeBenchmark()
{
    RealElevationSampler sampler("DATA/N36W112.hgt", 36.0, -112.0);

    if (!sampler.IsLoaded())
    {
        std::cout << "RunWallTimeBenchmark: could not load DATA/N36W112.hgt -- skipping (missing data, not a test failure)." << std::endl;
        return;
    }

    double metersPerDegreeLat = 111320.0;
    double spacingInDegrees = 30.0 / metersPerDegreeLat;

    // Profile Test: 50 km, 30m 
    double latDeltaFor50km = 50000.0 / metersPerDegreeLat;
    GeoPoint profileA{ 36.3, -111.5 };
    GeoPoint profileB{ 36.3 + latDeltaFor50km, -111.5 };

    auto start1 = std::chrono::high_resolution_clock::now();
    std::vector<ProfileSample> profile = GetTerrainProfile(profileA, profileB, spacingInDegrees, sampler);
    LineOfSightResult los = ComputeLineOfSight(profile, 2.0, 2.0);
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

    auto startSmallFast = std::chrono::high_resolution_clock::now();
    ViewshedResult smallFast = ComputeViewshedFast(viewshedObserver, 2.0, smallGridSize, smallGridSize, spacingInDegrees, sampler);
    auto endSmallFast = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> smallFastTime = endSmallFast - startSmallFast;

    auto startSmallNaive = std::chrono::high_resolution_clock::now();
    ViewshedResult smallNaive = ComputeViewshedNaive(viewshedObserver, 2.0, smallGridSize, smallGridSize, spacingInDegrees, sampler);
    auto endSmallNaive = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> smallNaiveTime = endSmallNaive - startSmallNaive;

    std::cout << smallRadiusKm << "km naive viewshed (" << smallGridSize << "x" << smallGridSize << "), Time: " << smallNaiveTime.count() << " ms" << std::endl;
    std::cout << smallRadiusKm << "km fast viewshed (" << smallGridSize << "x" << smallGridSize << "), Time: " << smallFastTime.count() << " ms" << std::endl;
    std::cout << "Speed-up (naive / fast): " << (smallNaiveTime.count() / smallFastTime.count()) << "x" << std::endl;

    int mismatches = 0;
    int totalValid = 0;
    int excludedCells = 0;
    for (int row = 0; row < smallGridSize; row++)
    {
        for (int col = 0; col < smallGridSize; col++)
        {
            if (IsConfident(smallFast.visible[row][col]) && IsConfident(smallNaive.visible[row][col]))
            {
                totalValid++;
                if (smallFast.visible[row][col] != smallNaive.visible[row][col])
                {
                    mismatches++;
                }
            }
            else
            {
                excludedCells++;
            }
        }
    }

    double mismatchRatio = (double)mismatches / totalValid;
    double tolerance = 0.05; // Ridgeline disagreement between fast's off-axis ray sampling and naive's exact per-target LOS; see notes.

    std::cout << smallRadiusKm << "km comparison (" << smallGridSize << "x" << smallGridSize << "): "
        << mismatches << " / " << totalValid << " cells differ (" << (mismatchRatio * 100.0)
        << "%, tolerance: " << (tolerance * 100.0) << "%), " << excludedCells
        << " cell(s) excluded (not confident in both algorithms)" << std::endl;

    Expect(mismatchRatio < tolerance, "FastViewshedMatchesNaive on real SRTM data within stated tolerance");

}

int main(int argc, char* argv[])
{
    if (argc > 1)
    {
        std::string mode = argv[1];

        if (mode == "profile" && (argc == 10 || argc == 11))
        {
            std::string hgtFile = argv[2];
            double swLat = std::stod(argv[3]);
            double swLon = std::stod(argv[4]);
            double aLat = std::stod(argv[5]);
            double aLon = std::stod(argv[6]);
            double bLat = std::stod(argv[7]);
            double bLon = std::stod(argv[8]);
            double spacing = std::stod(argv[9]);
            InterpolationMode interp = InterpolationMode::Nearest;
            if (argc == 11 && std::string(argv[10]) == "bilinear") interp = InterpolationMode::Bilinear;

            RealElevationSampler sampler(hgtFile, swLat, swLon, interp);
            if (!sampler.IsLoaded())
            {
                std::cout << "Error: could not load elevation data file: " << hgtFile << std::endl;
                return 1;
            }
            GeoPoint a{ aLat, aLon };
            GeoPoint b{ bLat, bLon };
            std::vector<ProfileSample> profile = GetTerrainProfile(a, b, spacing, sampler);

            for (const auto& sample : profile)
            {
                if (sample.elevation.has_value())
                {
                    std::cout << sample.point.latitude << ", " << sample.point.longitude << ", " << *sample.elevation << std::endl;
                }
                else
                {
                    std::cout << sample.point.latitude << ", " << sample.point.longitude << ", NODATA" << std::endl;
                }
            }
            return 0;
        }

        if (mode == "los" && (argc == 12 || argc == 13 || argc == 14))
        {
            std::string hgtFile = argv[2];
            double swLat = std::stod(argv[3]);
            double swLon = std::stod(argv[4]);
            double aLat = std::stod(argv[5]);
            double aLon = std::stod(argv[6]);
            double bLat = std::stod(argv[7]);
            double bLon = std::stod(argv[8]);
            double spacing = std::stod(argv[9]);
            double hA = std::stod(argv[10]);
            double hB = std::stod(argv[11]);
            double k = (argc >= 13) ? std::stod(argv[12]) : (4.0 / 3.0);
            InterpolationMode interp = InterpolationMode::Nearest;
            if (argc == 14 && std::string(argv[13]) == "bilinear") interp = InterpolationMode::Bilinear;

            RealElevationSampler sampler(hgtFile, swLat, swLon, interp);
            GeoPoint a{ aLat, aLon };
            if (!sampler.IsLoaded())
            {
                std::cout << "Error: could not load elevation data file: " << hgtFile << std::endl;
                return 1;
            }
            GeoPoint b{ bLat, bLon };

            std::vector<ProfileSample> profile = GetTerrainProfile(a, b, spacing, sampler);
            LineOfSightResult los = ComputeLineOfSight(profile, hA, hB, k);

            std::cout << "Visible: " << (los.isVisible ? "YES" : "NO") << std::endl;
            if (los.blockingPoint.has_value())
            {
                std::cout << "Blocking point: " << los.blockingPoint->latitude << ", " << los.blockingPoint->longitude << std::endl;
                std::cout << "Blocking elevation: " << *los.blockingElevation << std::endl;
                std::cout << "Blocking feature: " << TerrainFeatureTypeToString(los.blockingFeature) << std::endl;
            }
            std::cout << "Clearance deficit: " << los.clearanceDeficit << std::endl;
            std::cout << "Degraded: " << (los.isDegraded ? "YES" : "NO") << std::endl;
            return 0;
        }

        if (mode == "viewshed" && (argc == 10 || argc == 11 || argc == 12))
        {
            std::string hgtFile = argv[2];
            double swLat = std::stod(argv[3]);
            double swLon = std::stod(argv[4]);
            double obsLat = std::stod(argv[5]);
            double obsLon = std::stod(argv[6]);
            int gridSize = std::stoi(argv[7]);
            double spacing = std::stod(argv[8]);
            double height = std::stod(argv[9]);
            double k = (argc >= 11) ? std::stod(argv[10]) : (4.0 / 3.0);
            InterpolationMode interp = InterpolationMode::Nearest;
            if (argc == 12 && std::string(argv[11]) == "bilinear") interp = InterpolationMode::Bilinear;

            RealElevationSampler sampler(hgtFile, swLat, swLon, interp);
            if (!sampler.IsLoaded())
            {
                std::cout << "Error: could not load elevation data file: " << hgtFile << std::endl;
                return 1;
            }
            GeoPoint observer{ obsLat, obsLon };

            ViewshedResult result = ComputeViewshedFast(observer, height, gridSize, gridSize, spacing, sampler, k);
            WriteViewshedPGM(result, "cli_viewshed_output.pgm");

            std::cout << "Viewshed written to cli_viewshed_output.pgm" << std::endl;
            return 0;
        }
        
        if (mode == "fresnel" && (argc == 13 || argc == 14))
        {
            std::string hgtFile = argv[2];
            double swLat = std::stod(argv[3]);
            double swLon = std::stod(argv[4]);
            double aLat = std::stod(argv[5]);
            double aLon = std::stod(argv[6]);
            double bLat = std::stod(argv[7]);
            double bLon = std::stod(argv[8]);
            double spacing = std::stod(argv[9]);
            double hA = std::stod(argv[10]);
            double hB = std::stod(argv[11]);
            double frequencyMHz = std::stod(argv[12]);
            double k = (argc == 14) ? std::stod(argv[13]) : (4.0 / 3.0);

            RealElevationSampler sampler(hgtFile, swLat, swLon);
            if (!sampler.IsLoaded())
            {
                std::cout << "Error: could not load elevation data file: " << hgtFile << std::endl;
                return 1;
            }
            GeoPoint a{ aLat, aLon };
            GeoPoint b{ bLat, bLon };

            std::vector<ProfileSample> profile = GetTerrainProfile(a, b, spacing, sampler);
            FresnelClearanceResult result = ComputeFresnelClearance(profile, hA, hB, frequencyMHz * 1e6, k);

            if (result.isDegraded)
            {
                std::cout << "Status: UNKNOWN (degraded -- no interior sample between the two points, or a void was hit)" << std::endl;
            }
            else
            {
                std::cout << "Min Fresnel clearance fraction: " << result.minClearanceFraction << std::endl;
                if (result.worstPoint.has_value())
                {
                    std::cout << "Worst point: " << result.worstPoint->latitude << ", " << result.worstPoint->longitude << std::endl;
                }
                if (result.minClearanceFraction >= 1.0)
                {
                    std::cout << "Status: CLEAR (full first Fresnel zone unobstructed)" << std::endl;
                }
                else if (result.minClearanceFraction >= 0.0)
                {
                    std::cout << "Status: PARTIALLY OBSTRUCTED" << std::endl;
                }
                else
                {
                    std::cout << "Status: BLOCKED (line of sight itself obstructed)" << std::endl;
                }
            }
            std::cout << "Degraded: " << (result.isDegraded ? "YES" : "NO") << std::endl;
            return 0;
        }

        if (mode == "batch" && (argc == 6 || argc == 7))
        {
            std::string hgtFile = argv[2];
            double swLat = std::stod(argv[3]);
            double swLon = std::stod(argv[4]);
            std::string queriesFile = argv[5];
            double k = (argc == 7) ? std::stod(argv[6]) : (4.0 / 3.0);

            RealElevationSampler sampler(hgtFile, swLat, swLon);
            if (!sampler.IsLoaded())
            {
                std::cout << "Error: could not load elevation data file: " << hgtFile << std::endl;
                return 1;
            }

            std::ifstream queryFile(queriesFile);
            if (!queryFile.is_open())
            {
                std::cout << "Error: could not open queries file: " << queriesFile << std::endl;
                return 1;
            }

            std::vector<BatchLineOfSightQuery> queries;
            double aLat, aLon, bLat, bLon, hA, hB, spacing;
            queryFile >> spacing;
            while (queryFile >> aLat >> aLon >> bLat >> bLon >> hA >> hB)
            {
                BatchLineOfSightQuery q;
                q.observer = GeoPoint{ aLat, aLon };
                q.observerHeight = hA;
                q.target = GeoPoint{ bLat, bLon };
                q.targetHeight = hB;
                queries.push_back(q);
            }

            std::vector<LineOfSightResult> results = ComputeBatchLineOfSight(queries, spacing, sampler, k);

            for (size_t i = 0; i < results.size(); i++)
            {
                std::cout << "Query " << i << ": Visible=" << (results[i].isVisible ? "YES" : "NO")
                    << ", ClearanceDeficit=" << results[i].clearanceDeficit
                    << ", Degraded=" << (results[i].isDegraded ? "YES" : "NO") << std::endl;
            }
            return 0;
        }


        if (mode == "benchmark" && argc == 6)
        {
            std::string subMode = argv[2];
            std::string hgtFile = argv[3];
            double swLat = std::stod(argv[4]);
            double swLon = std::stod(argv[5]);

            RealElevationSampler sampler(hgtFile, swLat, swLon);
            if (!sampler.IsLoaded())
            {
                std::cout << "Error: could not load elevation data file: " << hgtFile << std::endl;
                return 1;
            }
            double metersPerDegreeLat = 111320.0;
            double spacingInDegrees = 30.0 / metersPerDegreeLat;

            if (subMode == "profile")
            {
                double latDeltaFor50km = 50000.0 / metersPerDegreeLat;
                GeoPoint a{ swLat + 0.3, swLon + 0.5 };
                GeoPoint b{ a.latitude + latDeltaFor50km, a.longitude };

                auto start = std::chrono::high_resolution_clock::now();
                std::vector<ProfileSample> profile = GetTerrainProfile(a, b, spacingInDegrees, sampler);
                LineOfSightResult los = ComputeLineOfSight(profile, 2.0, 2.0);
                auto end = std::chrono::high_resolution_clock::now();
                std::chrono::duration<double, std::milli> elapsed = end - start;

                PROCESS_MEMORY_COUNTERS pmc;
                GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc));
                double peakMB = pmc.PeakWorkingSetSize / (1024.0 * 1024.0);

                std::cout << "Profile benchmark: 50km @ 30m spacing (" << profile.size() << " samples)" << std::endl;
                std::cout << "Wall time: " << elapsed.count() << " ms" << std::endl;
                std::cout << "Peak memory: " << peakMB << " MB" << std::endl;
                return 0;
            }

            if (subMode == "viewshed")
            {
                double radiusKm = 30.0;
                double radiusInDegrees = (radiusKm * 1000.0) / metersPerDegreeLat;
                int gridSize = (int)(2 * radiusInDegrees / spacingInDegrees);
                GeoPoint observer{ swLat + 0.5, swLon + 0.5 };

                auto start = std::chrono::high_resolution_clock::now();
                ViewshedResult result = ComputeViewshedFast(observer, 2.0, gridSize, gridSize, spacingInDegrees, sampler);
                auto end = std::chrono::high_resolution_clock::now();
                std::chrono::duration<double, std::milli> elapsed = end - start;

                PROCESS_MEMORY_COUNTERS pmc;
                GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc));
                double peakMB = pmc.PeakWorkingSetSize / (1024.0 * 1024.0);

                std::cout << "Viewshed benchmark: 30km radius @ 30m data (" << gridSize << "x" << gridSize << ")" << std::endl;
                std::cout << "Wall time: " << elapsed.count() << " ms" << std::endl;
                std::cout << "Peak memory: " << peakMB << " MB" << std::endl;
                return 0;
            }

            std::cout << "Unknown benchmark mode. Use 'profile' or 'viewshed'." << std::endl;
            return 1;
        }

        std::cout << "Usage:" << std::endl;
        std::cout << "  TerrainEngine.exe benchmark <profile|viewshed> <hgtFile> <swLat> <swLon>" << std::endl;
        std::cout << "  TerrainEngine.exe profile <hgtFile> <swLat> <swLon> <aLat> <aLon> <bLat> <bLon> <spacing> [nearest|bilinear]" << std::endl;
        std::cout << "  TerrainEngine.exe los <hgtFile> <swLat> <swLon> <aLat> <aLon> <bLat> <bLon> <spacing> <hA> <hB> [k] [nearest|bilinear]" << std::endl;
        std::cout << "  TerrainEngine.exe viewshed <hgtFile> <swLat> <swLon> <obsLat> <obsLon> <gridSize> <spacing> <height> [k] [nearest|bilinear]" << std::endl;
        std::cout << "  TerrainEngine.exe fresnel <hgtFile> <swLat> <swLon> <aLat> <aLon> <bLat> <bLon> <spacing> <hA> <hB> <frequencyMHz> [k]" << std::endl;
        std::cout << "  TerrainEngine.exe batch <hgtFile> <swLat> <swLon> <queriesFile> [k]" << std::endl;
        return 1;
    }

    // Tests
    std::cout << "Tests\n" << std::endl;
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
    TestFresnelClearancePartialObstruction();
    TestBatchLineOfSightMatchesIndividualCalls();
    TestMultiTileSeamIsInvisible();
    TestBlockingFeatureIsLocalPeak();
    TestFastViewshedVoidDegradesDownstream();
    TestRealElevationSamplerReadsVoidFromFile();    
    TestInterpolationModesDifferOnRidgeline();
    TestProfileMatchesFrozenOracle();
    TestNarrowSpikeCanFallBetweenSamples();
    TestMultiTileProfileCrossesSeamWithoutGap();
    TestEmptyAndSingleSampleProfilesAreDegradedNotUB();
    TestBlockingFeatureClassificationIsSpacingInvariant();

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
            if (viewshed.visible[row][col] == CellVisibility::Visible)
            {
                std::cout << "# ";
            }
            else if (viewshed.visible[row][col] == CellVisibility::NotVisible)
            {
                std::cout << ". ";
            }
            else
            {
                std::cout << "? ";
            }
        }
        std::cout << std::endl;
    }

    std::cout << "-------------------------" << std::endl;

    RealElevationSampler realSampler("DATA/N36W112.hgt", 36.0, -112.0);
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
            if (fastViewshed.visible[row][col] == CellVisibility::Visible) std::cout << "# ";
            else if (fastViewshed.visible[row][col] == CellVisibility::NotVisible) std::cout << ". ";
            else std::cout << "? ";
        }
        std::cout << std::endl;
    }

    std::cout << "-------------------------" << std::endl;

    RunWallTimeBenchmark();

    std::cout << "-------------------------" << std::endl;

    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
    {
        double peakMB = pmc.PeakWorkingSetSize / (1024.0 * 1024.0);
        std::cout << "Peak memory usage: " << peakMB << " MB" << std::endl;
    }

    return (g_testFailureCount == 0) ? 0 : 1;
}   