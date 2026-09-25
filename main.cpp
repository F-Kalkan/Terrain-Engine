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
#include "CliArguments.h"
#include <fstream>
#include <cstdlib>
#include <new>

// Every allocation this executable makes through operator new goes through here and is
// counted, so a test can show that a call allocates nothing (Tests.h, g_heapAllocations).
std::atomic<long long> g_heapAllocations{ 0 };

void* operator new(std::size_t size)
{
    g_heapAllocations++;
    if (void* memory = std::malloc(size == 0 ? 1 : size)) return memory;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

// Times a 50 km profile, a 30 km-radius fast viewshed and fast minimum visible height,
// and fast against naive on a 2 km viewshed, over one real tile. How far fast is from
// naive is a test of its own: TestFastViewshedAgreesWithNaiveAtThreeObservers. Runs once
// per tile; a tile that isn't present is skipped.
void RunWallTimeBenchmark(const std::string& hgtPath, const std::string& tileLabel, const std::string& pgmSuffix)
{
    RealElevationSampler sampler(hgtPath, 36.0, -112.0);

    if (!sampler.IsLoaded())
    {
        std::cout << "[" << tileLabel << ": " << hgtPath << " not found -- benchmark skipped]" << std::endl;
        return;
    }
    std::cout << "[" << tileLabel << ": " << hgtPath << "]" << std::endl;

    double metersPerDegreeLat = EarthRadiusM * DegToRad;
    double spacingInDegrees = 30.0 / metersPerDegreeLat;
    DatumHeight agl2m{ 2.0, VerticalDatum::HeightAboveGround };

    // Profile Test: 50 km, 30m 
    double latDeltaFor50km = 50000.0 / metersPerDegreeLat;
    GeoPoint profileA{ 36.3, -111.5 };
    GeoPoint profileB{ 36.3 + latDeltaFor50km, -111.5 };

    auto start1 = std::chrono::high_resolution_clock::now();
    std::vector<ProfileSample> profile = GetTerrainProfile(profileA, profileB, spacingInDegrees, sampler);
    LineOfSightResult los = ComputeLineOfSight(profile, agl2m, agl2m);
    auto end1 = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> profileTime = end1 - start1;
    std::cout << "50km profile (" << profile.size() << " samples), Time: " << profileTime.count() << " ms" << std::endl;

    // Viewshed Test: 30km Radius
    double radiusKm = 30.0;
    double radiusInDegrees = (radiusKm * 1000.0) / metersPerDegreeLat;
    int gridSize = (int)(2 * radiusInDegrees / spacingInDegrees);

    GeoPoint viewshedObserver{ 36.5, -111.5 };

    auto start2 = std::chrono::high_resolution_clock::now();
    ViewshedResult fastResult = ComputeViewshedFast(viewshedObserver, agl2m, gridSize, gridSize, spacingInDegrees, sampler);
    auto end2 = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> fastTime = end2 - start2;
    std::cout << radiusKm << "km fast viewshed (" << gridSize << "x" << gridSize << "), Time: " << fastTime.count() << " ms" << std::endl;

    auto startHeights = std::chrono::high_resolution_clock::now();
    MinimumVisibleHeightResult heights = ComputeMinimumVisibleHeightFast(viewshedObserver, agl2m, gridSize, gridSize, spacingInDegrees, sampler);
    auto endHeights = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> heightsTime = endHeights - startHeights;
    std::cout << radiusKm << "km fast minimum visible height (" << gridSize << "x" << gridSize << "), Time: " << heightsTime.count() << " ms" << std::endl;

    WriteProfilePGM(profile, (std::string("profile_output") + pgmSuffix + ".pgm").c_str());
    WriteViewshedPGM(fastResult, (std::string("viewshed_output") + pgmSuffix + ".pgm").c_str());



    //Temporary Test
    double smallRadiusKm = 2.0;
    double smallRadiusInDegrees = (smallRadiusKm * 1000.0) / metersPerDegreeLat;
    int smallGridSize = (int)(2 * smallRadiusInDegrees / spacingInDegrees);

    auto startSmallFast = std::chrono::high_resolution_clock::now();
    ViewshedResult smallFast = ComputeViewshedFast(viewshedObserver, agl2m, smallGridSize, smallGridSize, spacingInDegrees, sampler);
    auto endSmallFast = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> smallFastTime = endSmallFast - startSmallFast;

    auto startSmallNaive = std::chrono::high_resolution_clock::now();
    ViewshedResult smallNaive = ComputeViewshedNaive(viewshedObserver, agl2m, smallGridSize, smallGridSize, spacingInDegrees, sampler);
    auto endSmallNaive = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> smallNaiveTime = endSmallNaive - startSmallNaive;

    std::cout << smallRadiusKm << "km naive viewshed (" << smallGridSize << "x" << smallGridSize << "), Time: " << smallNaiveTime.count() << " ms" << std::endl;
    std::cout << smallRadiusKm << "km fast viewshed (" << smallGridSize << "x" << smallGridSize << "), Time: " << smallFastTime.count() << " ms" << std::endl;
    std::cout << "Speed-up (naive / fast): " << (smallNaiveTime.count() / smallFastTime.count()) << "x" << std::endl;
}

// Read argv[index] as a number, or say which argument was wrong and return false.
bool ReadNumberArg(char* argv[], int index, const char* name, double& out)
{
    std::optional<double> value = ParseFiniteDouble(argv[index]);
    if (!value.has_value())
    {
        std::cout << "Error: " << name << " must be a number, got '" << argv[index] << "'" << std::endl;
        return false;
    }
    out = *value;
    return true;
}

// As ReadNumberArg, for quantities that are meaningless at or below zero (a spacing,
// the refraction factor k, a frequency).
bool ReadPositiveNumberArg(char* argv[], int index, const char* name, double& out)
{
    if (!ReadNumberArg(argv, index, name, out)) return false;
    if (out <= 0.0)
    {
        std::cout << "Error: " << name << " must be greater than zero, got '" << argv[index] << "'" << std::endl;
        return false;
    }
    return true;
}

// The library refuses a path it can't sample -- a latitude past a pole, a spacing so fine
// the sample count overflows. Say why and stop, rather than print an answer about nothing.
bool PathCanBeSampled(GeoPoint a, GeoPoint b, double spacing)
{
    InputProblem problem = CheckProfileRequest(a, b, spacing);
    if (problem == InputProblem::None) return true;
    std::cout << "Error: " << InputProblemToString(problem) << std::endl;
    return false;
}

bool ReadPositiveIntArg(char* argv[], int index, const char* name, int& out)
{
    std::optional<int> value = ParseInt(argv[index]);
    if (!value.has_value() || *value <= 0)
    {
        std::cout << "Error: " << name << " must be a positive whole number, got '" << argv[index] << "'" << std::endl;
        return false;
    }
    out = *value;
    return true;
}

int main(int argc, char* argv[])
{
    if (argc > 1)
    {
        std::string mode = argv[1];

        if (mode == "profile" && (argc == 10 || argc == 11))
        {
            std::string hgtFile = argv[2];
            double swLat, swLon, aLat, aLon, bLat, bLon, spacing;
            if (!ReadNumberArg(argv, 3, "swLat", swLat) || !ReadNumberArg(argv, 4, "swLon", swLon)
                || !ReadNumberArg(argv, 5, "aLat", aLat) || !ReadNumberArg(argv, 6, "aLon", aLon)
                || !ReadNumberArg(argv, 7, "bLat", bLat) || !ReadNumberArg(argv, 8, "bLon", bLon)
                || !ReadPositiveNumberArg(argv, 9, "spacing", spacing))
            {
                return 1;
            }
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
            if (!PathCanBeSampled(a, b, spacing)) return 1;
            std::vector<ProfileSample> profile = GetTerrainProfile(a, b, spacing, sampler);

            for (const auto& sample : profile)
            {
                if (sample.elevationM.has_value())
                {
                    std::cout << sample.point.latitudeDeg << ", " << sample.point.longitudeDeg << ", " << *sample.elevationM << std::endl;
                }
                else
                {
                    std::cout << sample.point.latitudeDeg << ", " << sample.point.longitudeDeg << ", NODATA" << std::endl;
                }
            }
            return 0;
        }

        if (mode == "los" && (argc == 12 || argc == 13 || argc == 14))
        {
            std::string hgtFile = argv[2];
            double swLat, swLon, aLat, aLon, bLat, bLon, spacing, hA, hB;
            double k = 4.0 / 3.0;
            if (!ReadNumberArg(argv, 3, "swLat", swLat) || !ReadNumberArg(argv, 4, "swLon", swLon)
                || !ReadNumberArg(argv, 5, "aLat", aLat) || !ReadNumberArg(argv, 6, "aLon", aLon)
                || !ReadNumberArg(argv, 7, "bLat", bLat) || !ReadNumberArg(argv, 8, "bLon", bLon)
                || !ReadPositiveNumberArg(argv, 9, "spacing", spacing)
                || !ReadNumberArg(argv, 10, "hA", hA) || !ReadNumberArg(argv, 11, "hB", hB)
                || (argc >= 13 && !ReadPositiveNumberArg(argv, 12, "k", k)))
            {
                return 1;
            }
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
            if (!PathCanBeSampled(a, b, spacing)) return 1;

            std::vector<ProfileSample> profile = GetTerrainProfile(a, b, spacing, sampler);
            DatumHeight hADatum{ hA, VerticalDatum::HeightAboveGround };
            DatumHeight hBDatum{ hB, VerticalDatum::HeightAboveGround };
            LineOfSightResult los = ComputeLineOfSight(profile, hADatum, hBDatum, k);

            std::cout << "Visible: " << (los.isVisible ? "YES" : "NO") << std::endl;
            if (los.blockingPoint.has_value())
            {
                std::cout << "Blocking point: " << los.blockingPoint->latitudeDeg << ", " << los.blockingPoint->longitudeDeg << std::endl;
                std::cout << "Blocking elevation: " << *los.blockingElevationM << std::endl;
                std::cout << "Blocking feature: " << TerrainFeatureTypeToString(los.blockingFeature) << std::endl;
            }
            std::cout << "Clearance deficit: " << los.clearanceDeficitM << std::endl;
            std::cout << "Status: " << ComputationStatusToString(los.status) << std::endl;
            return 0;
        }

        if (mode == "viewshed" && argc >= 10 && argc <= 13)
        {
            std::string hgtFile = argv[2];
            double swLat, swLon, obsLat, obsLon, spacing, height;
            double targetHeight = 0.0;
            int gridSize = 0;
            double k = 4.0 / 3.0;
            if (!ReadNumberArg(argv, 3, "swLat", swLat) || !ReadNumberArg(argv, 4, "swLon", swLon)
                || !ReadNumberArg(argv, 5, "obsLat", obsLat) || !ReadNumberArg(argv, 6, "obsLon", obsLon)
                || !ReadPositiveIntArg(argv, 7, "gridSize", gridSize)
                || !ReadPositiveNumberArg(argv, 8, "spacing", spacing)
                || !ReadNumberArg(argv, 9, "height", height)
                || (argc >= 11 && !ReadPositiveNumberArg(argv, 10, "k", k))
                || (argc >= 13 && !ReadNumberArg(argv, 12, "targetHeight", targetHeight)))
            {
                return 1;
            }
            InterpolationMode interp = InterpolationMode::Nearest;
            if (argc >= 12 && std::string(argv[11]) == "bilinear") interp = InterpolationMode::Bilinear;

            RealElevationSampler sampler(hgtFile, swLat, swLon, interp);
            if (!sampler.IsLoaded())
            {
                std::cout << "Error: could not load elevation data file: " << hgtFile << std::endl;
                return 1;
            }
            
            GeoPoint observer{ obsLat, obsLon };
            DatumHeight heightDatum{ height, VerticalDatum::HeightAboveGround };

            ViewshedResult result = ComputeViewshedFast(observer, heightDatum, gridSize, gridSize, spacing, sampler, k, nullptr, DatumHeight{ targetHeight, VerticalDatum::HeightAboveGround });
            if (result.inputProblem != InputProblem::None)
            {
                std::cout << "Error: " << InputProblemToString(result.inputProblem) << std::endl;
                return 1;
            }
            WriteViewshedPGM(result, "cli_viewshed_output.pgm");

            std::cout << "Viewshed written to cli_viewshed_output.pgm" << std::endl;
            return 0;
        }
        
        if (mode == "fresnel" && (argc == 13 || argc == 14))
        {
            std::string hgtFile = argv[2];
            double swLat, swLon, aLat, aLon, bLat, bLon, spacing, hA, hB, frequencyMHz;
            double k = 4.0 / 3.0;
            if (!ReadNumberArg(argv, 3, "swLat", swLat) || !ReadNumberArg(argv, 4, "swLon", swLon)
                || !ReadNumberArg(argv, 5, "aLat", aLat) || !ReadNumberArg(argv, 6, "aLon", aLon)
                || !ReadNumberArg(argv, 7, "bLat", bLat) || !ReadNumberArg(argv, 8, "bLon", bLon)
                || !ReadPositiveNumberArg(argv, 9, "spacing", spacing)
                || !ReadNumberArg(argv, 10, "hA", hA) || !ReadNumberArg(argv, 11, "hB", hB)
                || !ReadPositiveNumberArg(argv, 12, "frequencyMHz", frequencyMHz)
                || (argc == 14 && !ReadPositiveNumberArg(argv, 13, "k", k)))
            {
                return 1;
            }

            RealElevationSampler sampler(hgtFile, swLat, swLon);
            if (!sampler.IsLoaded())
            {
                std::cout << "Error: could not load elevation data file: " << hgtFile << std::endl;
                return 1;
            }
            GeoPoint a{ aLat, aLon };
            GeoPoint b{ bLat, bLon };
            if (!PathCanBeSampled(a, b, spacing)) return 1;

            std::vector<ProfileSample> profile = GetTerrainProfile(a, b, spacing, sampler);
            DatumHeight hADatum{ hA, VerticalDatum::HeightAboveGround };
            DatumHeight hBDatum{ hB, VerticalDatum::HeightAboveGround };
            FresnelClearanceResult result = ComputeFresnelClearance(profile, hADatum, hBDatum, frequencyMHz * 1e6, k);

            if (!IsOk(result.status))
            {
                std::cout << "Fresnel status: UNKNOWN (" << ComputationStatusToString(result.status) << ")" << std::endl;
            }
            else
            {
                std::cout << "Min Fresnel clearance fraction: " << result.minClearanceFraction << std::endl;
                if (result.worstPoint.has_value())
                {
                    std::cout << "Worst point: " << result.worstPoint->latitudeDeg << ", " << result.worstPoint->longitudeDeg << std::endl;
                }
                if (result.minClearanceFraction >= 1.0)
                {
                    std::cout << "Fresnel status: CLEAR (full first Fresnel zone unobstructed)" << std::endl;
                }
                else if (result.minClearanceFraction >= 0.0)
                {
                    std::cout << "Fresnel status: PARTIALLY OBSTRUCTED" << std::endl;
                }
                else
                {
                    std::cout << "Fresnel status: BLOCKED (line of sight itself obstructed)" << std::endl;
                }
            }
            std::cout << "Status: " << ComputationStatusToString(result.status) << std::endl;
            return 0;
        }

        if (mode == "batch" && (argc == 6 || argc == 7))
        {
            std::string hgtFile = argv[2];
            double swLat, swLon;
            std::string queriesFile = argv[5];
            double k = 4.0 / 3.0;
            if (!ReadNumberArg(argv, 3, "swLat", swLat) || !ReadNumberArg(argv, 4, "swLon", swLon)
                || (argc == 7 && !ReadPositiveNumberArg(argv, 6, "k", k)))
            {
                return 1;
            }

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

            // The file is a positive spacing followed by six numbers per query
            // (aLat aLon bLat bLon hA hB). Read it as whitespace-separated tokens and
            // check every one, so a word where a number belongs, or a query cut
            // short at the end of the file, is reported instead of silently dropped.
            std::vector<std::string> tokens;
            for (std::string token; queryFile >> token; )
            {
                tokens.push_back(token);
            }

            std::optional<double> spacingValue = tokens.empty() ? std::nullopt : ParseFiniteDouble(tokens[0].c_str());
            if (!spacingValue.has_value() || *spacingValue <= 0.0)
            {
                std::cout << "Error: " << queriesFile << " must start with a spacing greater than zero" << std::endl;
                return 1;
            }
            double spacing = *spacingValue;

            if ((tokens.size() - 1) % 6 != 0)
            {
                std::cout << "Error: " << queriesFile << " has " << (tokens.size() - 1)
                    << " values after the spacing; each query is six numbers (aLat aLon bLat bLon hA hB)" << std::endl;
                return 1;
            }

            std::vector<BatchLineOfSightQuery> queries;
            for (size_t first = 1; first < tokens.size(); first += 6)
            {
                double values[6];
                for (size_t j = 0; j < 6; j++)
                {
                    std::optional<double> value = ParseFiniteDouble(tokens[first + j].c_str());
                    if (!value.has_value())
                    {
                        std::cout << "Error: " << queriesFile << ", query " << queries.size()
                            << ": '" << tokens[first + j] << "' is not a number" << std::endl;
                        return 1;
                    }
                    values[j] = *value;
                }

                BatchLineOfSightQuery q;
                q.observer = GeoPoint{ values[0], values[1] };
                q.target = GeoPoint{ values[2], values[3] };
                q.observerHeight = DatumHeight{ values[4], VerticalDatum::HeightAboveGround };
                q.targetHeight = DatumHeight{ values[5], VerticalDatum::HeightAboveGround };
                queries.push_back(q);
            }

            std::vector<LineOfSightResult> results = ComputeBatchLineOfSight(queries, spacing, sampler, k);

            for (size_t i = 0; i < results.size(); i++)
            {
                std::cout << "Query " << i << ": Visible=" << (results[i].isVisible ? "YES" : "NO")
                    << ", ClearanceDeficit=" << results[i].clearanceDeficitM
                    << ", Status=" << ComputationStatusToString(results[i].status);
                if (results[i].status == ComputationStatus::InvalidInput) std::cout << " (" << InputProblemToString(results[i].inputProblem) << ")";
                std::cout << std::endl;
            }
            return 0;
        }


        if (mode == "benchmark" && argc == 6)
        {
            std::string subMode = argv[2];
            std::string hgtFile = argv[3];
            double swLat, swLon;
            if (!ReadNumberArg(argv, 4, "swLat", swLat) || !ReadNumberArg(argv, 5, "swLon", swLon))
            {
                return 1;
            }

            RealElevationSampler sampler(hgtFile, swLat, swLon);
            if (!sampler.IsLoaded())
            {
                std::cout << "Error: could not load elevation data file: " << hgtFile << std::endl;
                return 1;
            }
            double metersPerDegreeLat = EarthRadiusM * DegToRad;
            double spacingInDegrees = 30.0 / metersPerDegreeLat;

            if (subMode == "profile")
            {
                double latDeltaFor50km = 50000.0 / metersPerDegreeLat;
                GeoPoint a{ swLat + 0.3, swLon + 0.5 };
                GeoPoint b{ a.latitudeDeg + latDeltaFor50km, a.longitudeDeg };

                auto start = std::chrono::high_resolution_clock::now();
                std::vector<ProfileSample> profile = GetTerrainProfile(a, b, spacingInDegrees, sampler);
                DatumHeight agl2m{ 2.0, VerticalDatum::HeightAboveGround };
                LineOfSightResult los = ComputeLineOfSight(profile, agl2m, agl2m);
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
                DatumHeight agl2m{ 2.0, VerticalDatum::HeightAboveGround };
                ViewshedResult result = ComputeViewshedFast(observer, agl2m, gridSize, gridSize, spacingInDegrees, sampler);
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

            if (subMode == "minheight")
            {
                double radiusKm = 30.0;
                double radiusInDegrees = (radiusKm * 1000.0) / metersPerDegreeLat;
                int gridSize = (int)(2 * radiusInDegrees / spacingInDegrees);
                GeoPoint observer{ swLat + 0.5, swLon + 0.5 };

                auto start = std::chrono::high_resolution_clock::now();
                DatumHeight agl2m{ 2.0, VerticalDatum::HeightAboveGround };
                MinimumVisibleHeightResult result = ComputeMinimumVisibleHeightFast(observer, agl2m, gridSize, gridSize, spacingInDegrees, sampler);
                auto end = std::chrono::high_resolution_clock::now();
                std::chrono::duration<double, std::milli> elapsed = end - start;

                PROCESS_MEMORY_COUNTERS pmc;
                GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc));
                double peakMB = pmc.PeakWorkingSetSize / (1024.0 * 1024.0);

                std::cout << "Minimum visible height benchmark (fast): 30km radius @ 30m data (" << gridSize << "x" << gridSize << ")" << std::endl;
                std::cout << "Wall time: " << elapsed.count() << " ms" << std::endl;
                std::cout << "Peak memory: " << peakMB << " MB" << std::endl;
                return 0;
            }

            if (subMode == "observer")
            {
                // A fixed observer 2 m up at the tile's centre, prepared out to 50 km; then a million
                // queries over targets spread evenly across the disc on the tile, alternately 2 m above
                // the ground and 5,000 m above sea level; then direct lines of sight 50 km long, due
                // north and south within 20 degrees so that they stay on the tile, for comparison.
                GeoPoint observer{ swLat + 0.5, swLon + 0.5 };
                DatumHeight agl2m{ 2.0, VerticalDatum::HeightAboveGround };
                const double pi = 3.14159265358979323846;

                auto start = std::chrono::high_resolution_clock::now();
                PreparedObserver prepared = PrepareObserver(observer, agl2m, 50000.0, spacingInDegrees, sampler);
                std::chrono::duration<double, std::milli> prepareTime = std::chrono::high_resolution_clock::now() - start;
                PROCESS_MEMORY_COUNTERS pmc;
                GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc));
                double peakMB = pmc.PeakWorkingSetSize / (1024.0 * 1024.0);

                std::vector<std::pair<GeoPoint, DatumHeight>> targets;
                for (int i = 0; targets.size() < 10000; i++)
                {
                    GeoPoint target = GreatCircleDestination(observer, i * pi * (3.0 - std::sqrt(5.0)), 50000.0 * std::sqrt((i + 0.5) / 12000.0));
                    bool onTile = target.latitudeDeg > swLat && target.latitudeDeg < swLat + 1 && target.longitudeDeg > swLon && target.longitudeDeg < swLon + 1;
                    if (onTile) targets.push_back({ target, i % 2 == 0 ? agl2m : DatumHeight{ 5000.0, VerticalDatum::OrthometricMsl } });
                }
                long long seen = 0;
                start = std::chrono::high_resolution_clock::now();
                for (int round = 0; round < 100; round++)
                {
                    for (const auto& [target, height] : targets) seen += QueryTarget(prepared, target, height).state == CellVisibility::Visible ? 1 : 0;
                }
                double queriesPerSecond = 1e6 / std::chrono::duration<double>(std::chrono::high_resolution_clock::now() - start).count();

                std::vector<ProfileSample> buffer;
                start = std::chrono::high_resolution_clock::now();
                for (int i = 0; i < 1000; i++)
                {
                    double bearing = (i % 2 == 0 ? 0.0 : pi) + (i / 2 % 41 - 20) * pi / 180.0;
                    GetTerrainProfile(observer, GreatCircleDestination(observer, bearing, 50000.0), spacingInDegrees, sampler, buffer);
                    seen += ComputeLineOfSight(buffer, agl2m, agl2m).isVisible ? 1 : 0;
                }
                double directMicroseconds = std::chrono::duration<double, std::micro>(std::chrono::high_resolution_clock::now() - start).count() / 1000;

                std::cout << "Prepared observer benchmark: 50km radius @ 30m data, " << prepared.rayCount << " rays" << std::endl;
                std::cout << "Preparation wall time: " << prepareTime.count() << " ms" << std::endl;
                std::cout << "Peak memory after preparation: " << peakMB << " MB" << std::endl;
                std::cout << "Queries per second (one core, 1,000,000 queries): " << queriesPerSecond << std::endl;
                std::cout << "Direct line of sight at 50 km: " << directMicroseconds << " us each (" << 1e6 / directMicroseconds << " a second)" << std::endl;
                std::cout << "(" << seen << " seen)" << std::endl;
                return 0;
            }

            if (subMode == "pairs")
            {
                // 24 observers against 400 targets, spread over the tile, on the ground and in the air:
                // the pairs answered a second at each thread count, each run checked against the
                // one-thread run to the bit. Then the naive viewshed over 5 km on one thread and on all.
                std::vector<SightEnd> observers, targets;
                const double golden = 3.14159265358979323846 * (3.0 - std::sqrt(5.0));
                GeoPoint centre{ swLat + 0.5, swLon + 0.5 };
                for (int i = 0; i < 24; i++)
                    observers.push_back({ GreatCircleDestination(centre, i * golden, 30000.0 * std::sqrt((i + 0.5) / 24)), DatumHeight{ 2.0, VerticalDatum::HeightAboveGround } });
                for (int i = 0; i < 400; i++)
                    targets.push_back({ GreatCircleDestination(centre, i * golden, 45000.0 * std::sqrt((i + 0.5) / 400)),
                        i % 2 == 0 ? DatumHeight{ 2.0, VerticalDatum::HeightAboveGround } : DatumHeight{ 5000.0, VerticalDatum::OrthometricMsl } });

                std::cout << "Line of sight pairs benchmark: 24 observers x 400 targets @ 30m spacing" << std::endl;
                LineOfSightPairs first;
                for (int threads : { 1, 2, 4, 8, 0 })
                {
                    auto start = std::chrono::high_resolution_clock::now();
                    LineOfSightPairs pairs = ComputeLineOfSightPairs(observers, targets, spacingInDegrees, sampler, 4.0 / 3.0, threads);
                    double seconds = std::chrono::duration<double>(std::chrono::high_resolution_clock::now() - start).count();
                    bool same = true;
                    if (threads == 1) first = pairs;
                    for (size_t i = 0; i < pairs.results.size(); i++)
                    {
                        const auto& a = pairs.results[i];
                        const auto& b = first.results[i];
                        same = same && a.isVisible == b.isVisible && a.status == b.status && a.clearanceDeficitM == b.clearanceDeficitM
                            && a.blockingElevationM == b.blockingElevationM && a.blockingFeature == b.blockingFeature;
                    }
                    std::cout << pairs.threadsUsed << " thread(s): " << seconds * 1000 << " ms, " << pairs.results.size() / seconds
                        << " pairs a second" << (same ? ", the same as on one thread" : ", DIFFERENT from one thread") << std::endl;
                }

                int grid = (int)(2 * 5000.0 / metersPerDegreeLat / spacingInDegrees);
                DatumHeight agl2m{ 2.0, VerticalDatum::HeightAboveGround };
                for (int threads : { 1, 0 })
                {
                    auto start = std::chrono::high_resolution_clock::now();
                    ViewshedResult naive = ComputeViewshedNaive(centre, agl2m, grid, grid, spacingInDegrees, sampler, 4.0 / 3.0, nullptr,
                        DatumHeight{ 0.0, VerticalDatum::HeightAboveGround }, threads);
                    double seconds = std::chrono::duration<double>(std::chrono::high_resolution_clock::now() - start).count();
                    std::cout << "5km naive viewshed (" << grid << "x" << grid << ") on " << ThreadsFor(threads) << " thread(s): " << seconds * 1000 << " ms" << std::endl;
                }
                return 0;
            }

            std::cout << "Unknown benchmark mode. Use 'profile', 'viewshed', 'minheight', 'observer' or 'pairs'." << std::endl;
            return 1;
        }

        std::cout << "Usage:" << std::endl;
        std::cout << "  TerrainEngine.exe benchmark <profile|viewshed|minheight|observer|pairs> <hgtFile> <swLat> <swLon>" << std::endl;
        std::cout << "  TerrainEngine.exe profile <hgtFile> <swLat> <swLon> <aLat> <aLon> <bLat> <bLon> <spacing> [nearest|bilinear]" << std::endl;
        std::cout << "  TerrainEngine.exe los <hgtFile> <swLat> <swLon> <aLat> <aLon> <bLat> <bLon> <spacing> <hA> <hB> [k] [nearest|bilinear]" << std::endl;
        std::cout << "  TerrainEngine.exe viewshed <hgtFile> <swLat> <swLon> <obsLat> <obsLon> <gridSize> <spacing> <height> [k] [nearest|bilinear] [targetHeight]" << std::endl;
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
    TestViewshedLongitudeSpacingCorrectsForLatitude();
    TestConvertHeightBetweenDatums();
    TestComputeLineOfSightRejectsWrongHeightDatum();
    TestComputeLineOfSightRejectsUnknownTerrainDatum();
    TestRealElevationSamplerDeclaresOrthometricDatum();
    TestRasterBlockSamplerPositiveRowStep();
    TestRasterBlockSamplerNegativeRowStepPlacesRowZeroAtNorth();
    TestRasterBlockSamplerVoidCellPassesThrough();
    TestRasterBlockSamplerWorksWithLineOfSight();
    TestScratchBufferProfileAllocatesNothingOnReuse();
    TestGreatCircleDistanceMatchesKnownValues();
    TestGreatCircleInterpolateBulgesTowardPole();
    TestViewshedRespondsToCurvatureFactor();
    TestFastViewshedSeesVoidOnEverySampleOfADiagonalRay();
    TestTerrainProfileNeverSamplesCoarserThanRequested();
    TestLineOfSightInterpolatesByDistanceNotIndex();
    TestViewshedsAgreeWhenObserverIsUnknown();
    TestLineOfSightAcceptsHeightsInAnyTerrainComparableDatum();
    TestOneArcSecondTileReadsAtNativeResolution();
    TestOneArcSecondTileAgreesWithThreeArcSecondTile();
    TestRasterBlockViewReadsHostBufferInPlace();
    TestRealElevationSamplerRejectsMalformedTile();
    TestViewshedsReturnEmptyForGridWithoutCells();
    TestCliNumberParsingRejectsWhatIsNotANumber();
    TestRealElevationSamplerReportsTileFacts();
    TestViewshedProgressIsReportedAndCanCancel();
    TestRealElevationSamplerWithInterpolationModeMatchesAFreshLoad();
    TestSharedPathGeometryMatchesHandCalculation();
    TestViewshedTargetHeightSeesOverTheWallAtTheHandWorkedHeight();
    TestViewshedTargetHeightOnlyEverRevealsAndFastStillMatchesNaive();
    TestViewshedTargetHeightInAnUnusableDatumLeavesOnlyTheObserverKnown();
    TestViewshedAgreementCountsEachDirectionOverTheVisibleCells();
    TestViewshedAgreementRejectsAViewshedWithOneAnswerEverywhere();
    TestViewshedAgreementRefusesGridsOfDifferentSizes();
    TestFastViewshedAgreesWithNaiveAtThreeObservers();
    TestTheLibraryRefusesASpacingItCannotSampleAt();
    TestTheLibraryRefusesCoordinatesThatAreNotOnTheEarth();
    TestTheLibraryRefusesHeightsThatAreNotNumbers();
    TestTheLibraryRefusesACurvatureFactorOrFrequencyItCannotUse();
    TestAViewshedGridThatWouldReachAPoleIsRefused();
    TestRealElevationSamplerBilinearMatchesAHandWorkedValue();
    TestMinimumVisibleHeightBehindTheWallIsTheHandWorkedHeight();
    TestMinimumVisibleHeightOnASmoothSphereMatchesTheClosedForm();
    TestMinimumVisibleHeightThresholdedIsTheViewshedAtThatHeight();
    TestMinimumVisibleHeightKeepsTheViewshedsNoAnswerStatesAndRefusals();
    TestMinimumVisibleHeightFastAgreesWithTheReferenceAtThreeObservers();
    TestPreparedObserverAnswersTheWallAndTheAirAsTheLineOfSightDoes();
    TestPreparedObserverAgreesWithLineOfSightOverTheListedTargets();
    TestPreparedObserverQueriesAllocateNothing();
    TestPreparedObserverKeepsTheNoAnswerStatesAndRefusals();
    TestLineOfSightPairsAreTheSameAtEveryThreadCount();
    TestLineOfSightPairsRefuseEachPairItCannotSample();
    TestReferenceGridsAreTheSameAtEveryThreadCount();
    TestDataNotGivenIsToldApartFromAVoid();
    TestQueryExtentsAreTheBoxesTheQueriesRead();
    TestAQueryGivenOnlyItsExtentAnswersAsOnTheWholeTile();

    std::cout << "-------------------------" << std::endl;

    //Naive Viewshed 
    // 330 m of flat ground at 30 m cells, with a 50 m ridge running north-south
    // 60 m east of the observer. '#' visible, '.' hidden, '?' degraded. At this
    // scale the terrain decides the picture; curvature over a few hundred metres
    // is about a millimetre.
    const int demoSize = 11;
    const double demoSpacingDeg = MetersToLatitudeDeg(30.0);
    const GeoPoint observerPos{ 36.5, -111.5 };
    ElevationCells demoCells = MakeFlatCells(demoSize, 0.0);
    for (int row = 0; row < demoSize; row++)
    {
        demoCells[row][demoSize / 2 + 2] = 50.0;
    }
    RasterBlockElevationSampler sampler = MakeViewshedAlignedRaster(demoCells, observerPos, demoSpacingDeg);
    DatumHeight demoAgl2m{ 2.0, VerticalDatum::HeightAboveGround };

    auto printViewshed = [&](const char* title, const ViewshedResult& result) {
        std::cout << title << std::endl;
        for (int row = 0; row < demoSize; row++)
        {
            for (int col = 0; col < demoSize; col++)
            {
                CellVisibility cell = result.visible[row][col];
                std::cout << (cell == CellVisibility::Visible ? "# " : cell == CellVisibility::NotVisible ? ". " : "? ");
            }
            std::cout << std::endl;
        }
    };

    printViewshed("Naive Viewshed: ", ComputeViewshedNaive(observerPos, demoAgl2m, demoSize, demoSize, demoSpacingDeg, sampler));

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
    printViewshed("Fast Viewshed: ", ComputeViewshedFast(observerPos, demoAgl2m, demoSize, demoSize, demoSpacingDeg, sampler));

    std::cout << "-------------------------" << std::endl;

    RunWallTimeBenchmark("DATA/N36W112.hgt", "3-arcsecond tile, ~90 m", "");

    std::cout << "-------------------------" << std::endl;

    RunWallTimeBenchmark("DATA/SRTM1/N36W112.hgt", "1-arcsecond tile, ~30 m", "_srtm1");

    std::cout << "-------------------------" << std::endl;

    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
    {
        double peakMB = pmc.PeakWorkingSetSize / (1024.0 * 1024.0);
        std::cout << "Peak memory usage: " << peakMB << " MB" << std::endl;
    }

    return (g_testFailureCount == 0) ? 0 : 1;
}   