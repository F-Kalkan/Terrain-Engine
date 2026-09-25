#pragma once
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <mutex>
#include <set>
#include <thread>
#include "IElevationSampler.h"
#include "TerrainProfile.h"
#include "LineOfSight.h"
#include "Viewshed.h"
#include "ViewshedAgreement.h"
#include "MinimumVisibleHeight.h"
#include "PreparedObserver.h"
#include "QueryExtent.h"
#include "LineOfSightPairs.h"
#include "RealElevationSampler.h"
#include "CliArguments.h"
#include <string>

inline int g_testFailureCount = 0;

// Every allocation the test executable makes through operator new, counted by main.cpp, so a
// test can show that a call makes none.
extern std::atomic<long long> g_heapAllocations;

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

// Shorthand for "this many metres above the local terrain". Every sampler and
// hand-built profile below also declares an explicit terrain datum, since the
// default (Unknown) is rejected rather than silently treated as usable.
inline DatumHeight Agl(double valueM)
{
    return DatumHeight{ valueM, VerticalDatum::HeightAboveGround };
}

inline void Skip(const std::string& testName, const std::string& reason)
{
    std::cout << "SKIP: " << testName << " (" << reason << ")" << std::endl;
}

// Real-scale fixtures. A small synthetic grid read as degrees is ~111 km per cell,
// a scale at which Earth curvature, not terrain, decides every answer -- so every
// test that runs GetTerrainProfile or a viewshed lays its cells on the ground at
// real spacing instead. Viewshed tests use a RasterBlockElevationSampler laid out
// so viewshed cell (row, col) reads raster cell (row, col): same centre, same row
// step, and the same latitude-corrected column step ComputeViewshedNaive/
// ComputeViewshedFast use.
using ElevationCells = std::vector<std::vector<std::optional<double>>>;

inline double MetersToLatitudeDeg(double meters)
{
    return meters / (EarthRadiusM * DegToRad);
}

inline ElevationCells MakeFlatCells(int size, double elevationM)
{
    return ElevationCells(size, std::vector<std::optional<double>>(size, elevationM));
}

inline RasterBlockElevationSampler MakeViewshedAlignedRaster(const ElevationCells& cells, GeoPoint observer, double spacingDeg)
{
    int rows = (int)cells.size();
    int cols = rows > 0 ? (int)cells[0].size() : 0;
    double lonSpacingDeg = LongitudeSpacingForLatitude(spacingDeg, observer.latitudeDeg);
    double originLatDeg = observer.latitudeDeg - (rows / 2) * spacingDeg;
    double originLonDeg = observer.longitudeDeg - (cols / 2) * lonSpacingDeg;
    return RasterBlockElevationSampler(cells, originLatDeg, originLonDeg, spacingDeg, lonSpacingDeg, VerticalDatum::OrthometricMsl);
}

inline int CountCells(const ViewshedResult& result, CellVisibility state)
{
    int count = 0;
    for (const auto& row : result.visible)
    {
        for (auto cell : row)
        {
            if (cell == state) count++;
        }
    }
    return count;
}

// Line-of-sight grids: a hand-drawn elevation grid laid on the ground in 30 m
// cells near the included tile. Row 0, column 0 is centred on TestGridOrigin; rows
// step north and columns east, the column step widened by 1/cos(latitude) so both
// axes are 30 m. GetTerrainProfile runs on it unmodified, so the distances a test
// sees are the real great-circle ones, never values written over afterwards.
inline const GeoPoint TestGridOrigin{ 36.5, -111.5 };
inline const double TestGridCellM = 30.0;

inline double TestGridCellDeg()
{
    return MetersToLatitudeDeg(TestGridCellM);
}

inline GeoPoint TestGridCellCentre(int row, int col)
{
    double cellDeg = TestGridCellDeg();
    return GeoPoint{ TestGridOrigin.latitudeDeg + row * cellDeg,
                     TestGridOrigin.longitudeDeg + col * LongitudeSpacingForLatitude(cellDeg, TestGridOrigin.latitudeDeg) };
}

inline RasterBlockElevationSampler MakeTestGrid(const std::vector<std::vector<double>>& elevationsM, VerticalDatum datum = VerticalDatum::OrthometricMsl)
{
    ElevationCells cells;
    for (const auto& row : elevationsM)
    {
        cells.emplace_back(row.begin(), row.end());
    }
    double cellDeg = TestGridCellDeg();
    return RasterBlockElevationSampler(cells, TestGridOrigin.latitudeDeg, TestGridOrigin.longitudeDeg,
        cellDeg, LongitudeSpacingForLatitude(cellDeg, TestGridOrigin.latitudeDeg), datum);
}

// Hand-built profiles, for shapes GetTerrainProfile can't produce directly. Each
// sample sits on the equator at its own distanceFromStartM -- along the equator a
// great-circle distance is exactly EarthRadiusM times the longitude difference --
// so a sample's position and its distance never disagree.
inline ProfileSample MakeSampleOnEquator(double distanceM, std::optional<double> elevationM, VerticalDatum datum = VerticalDatum::OrthometricMsl)
{
    ProfileSample sample;
    sample.point = GeoPoint{ 0.0, distanceM / (EarthRadiusM * DegToRad) };
    sample.elevationM = elevationM;
    sample.distanceFromStartM = distanceM;
    sample.elevationDatum = datum;
    return sample;
}

inline std::vector<ProfileSample> MakeEvenlySpacedProfile(const std::vector<std::optional<double>>& elevationsM, double stepM)
{
    std::vector<ProfileSample> profile;
    for (size_t i = 0; i < elevationsM.size(); i++)
    {
        profile.push_back(MakeSampleOnEquator(i * stepM, elevationsM[i]));
    }
    return profile;
}

// TEST 1 
void TestFlatPlateauEverythingVisible()
{
    RasterBlockElevationSampler sampler = MakeTestGrid({
        {10, 10, 10},
        {10, 10, 10},
        {10, 10, 10}
    });

    std::vector<ProfileSample> profile = GetTerrainProfile(TestGridCellCentre(0, 0), TestGridCellCentre(0, 2), TestGridCellDeg(), sampler);
    LineOfSightResult los = ComputeLineOfSight(profile, Agl(2.0), Agl(2.0));

    Expect(los.isVisible == true && IsOk(los.status), "TestFlatPlateauEverythingVisible");
}

// TEST 2  
void TestWallBlocksView()
{
    // A single row of 30 m cells with a 50 m peak two cells in, and 2 m eyes on
    // 10 m ground at both ends. The sight line sits at 12 m, so the peak blocks it
    // by 38 m -- plus the ~0.2 mm the Earth's curvature adds 60 m into a 120 m
    // path. The profile is GetTerrainProfile's own: 5 samples, 120 m end to end.
    RasterBlockElevationSampler sampler = MakeTestGrid({
        {10, 30, 50, 30, 10}
    });

    std::vector<ProfileSample> profile = GetTerrainProfile(TestGridCellCentre(0, 0), TestGridCellCentre(0, 4), TestGridCellDeg(), sampler);
    LineOfSightResult los = ComputeLineOfSight(profile, Agl(2.0), Agl(2.0));

    Expect(profile.size() == 5 && std::abs(profile.back().distanceFromStartM - 120.0) < 1e-6
        && IsOk(los.status) && los.isVisible == false && std::abs(los.clearanceDeficitM - 38.0) < 0.001, "TestWallBlocksView");
}

// TEST 3 
void TestCurvatureBlocksFlatTerrain()
{
    // 50 km flat terrain (elevation constanst = 0), observer and target = 2m
    // For flat terrain it should be visible on every scenario
    // But for Earths curvature;
    // Calculation: middle point (d1=d2=25000m), curvature drop = 25000*25000 / (2 * 4/3 * 6371000) ~= 36.79m
    // deficit = 36.79 - 2 = ~34.79m

    std::vector<ProfileSample> curvatureProfile = MakeEvenlySpacedProfile(std::vector<std::optional<double>>(11, 0.0), 5000.0);

    // Flat-earth comparison: an enormous k makes the curvature term negligible,
    // So the exact same geometry must report visible when curvature is effectively switched off.
    LineOfSightResult flatEarth = ComputeLineOfSight(curvatureProfile, Agl(2.0), Agl(2.0), 1e12);

    LineOfSightResult curved = ComputeLineOfSight(curvatureProfile, Agl(2.0), Agl(2.0));

    Expect(IsOk(flatEarth.status) && IsOk(curved.status)
        && flatEarth.isVisible == true && curved.isVisible == false && std::abs(curved.clearanceDeficitM - 34.79) < 0.1,
        "TestCurvatureBlocksFlatTerrain (flat-earth visible vs curved blocked)");
}

// TEST 4
void TestVoidPointIsDegraded()
{
    // P2 = Void. Function must skip that point and mark it as degraded.

    std::vector<ProfileSample> voidProfile = MakeEvenlySpacedProfile({ 10.0, std::nullopt, 10.0 }, 30.0);

    LineOfSightResult los = ComputeLineOfSight(voidProfile, Agl(2.0), Agl(2.0));

    Expect(los.status == ComputationStatus::VoidInProfile, "TestVoidPointIsDegraded");
}

// TEST 5
void TestViewshedDetectsVoid()
{
    // A 7x7 viewshed over a 5x5 raster of 30 m cells: the outer ring of viewshed
    // cells lies off the raster, where the sampler was given no data, and must come
    // back DataNotGiven -- not a void, which is a hole in data it was given; every
    // cell over real data must come back with a confident answer.
    const double spacingDeg = MetersToLatitudeDeg(30.0);
    const GeoPoint observer{ 36.5, -111.5 };
    RasterBlockElevationSampler sampler = MakeViewshedAlignedRaster(MakeFlatCells(5, 10.0), observer, spacingDeg);

    ViewshedResult viewshed = ComputeViewshedNaive(observer, Agl(2.0), 7, 7, spacingDeg, sampler);

    bool ringNotGiven = true;
    bool interiorConfident = true;
    for (int row = 0; row < 7; row++)
    {
        for (int col = 0; col < 7; col++)
        {
            bool overRaster = row >= 1 && row <= 5 && col >= 1 && col <= 5;
            if (overRaster && !IsConfident(viewshed.visible[row][col])) interiorConfident = false;
            if (!overRaster && viewshed.visible[row][col] != CellVisibility::DataNotGiven) ringNotGiven = false;
        }
    }

    Expect(ringNotGiven && interiorConfident, "TestViewshedDetectsVoid");
}

// TEST 6
void TestDeterminism()
{
    RasterBlockElevationSampler sampler = MakeTestGrid({
        {10, 10, 10, 10, 10},
        {10, 20, 30, 20, 10},
        {10, 30, 50, 30, 10},
        {10, 20, 30, 20, 10},
        {10, 10, 10, 10, 10}
    });

    GeoPoint a = TestGridCellCentre(2, 0);
    GeoPoint b = TestGridCellCentre(2, 4);

    std::vector<ProfileSample> profile1 = GetTerrainProfile(a, b, TestGridCellDeg(), sampler);
    LineOfSightResult los1 = ComputeLineOfSight(profile1, Agl(2.0), Agl(2.0));

    std::vector<ProfileSample> profile2 = GetTerrainProfile(a, b, TestGridCellDeg(), sampler);
    LineOfSightResult los2 = ComputeLineOfSight(profile2, Agl(2.0), Agl(2.0));

    Expect(IsOk(los1.status) && IsOk(los2.status)
        && los1.isVisible == los2.isVisible && los1.clearanceDeficitM == los2.clearanceDeficitM, "TestDeterminism");
}

//Test 7
void TestFastViewshedMatchesNaive()
{
    // 21x21 cells of 30 m on flat ground, with a 50 m wall running north-south
    // three cells east of the observer. Everything west of the wall must be
    // visible and everything east of it hidden -- in both algorithms, and only
    // because of the wall: the same scene without it hides nothing. The wall's own
    // column is left out, since whether a grazing ray sees a wall-top cell depends
    // on exactly where its samples fall along that cell.
    const int size = 21;
    const int center = size / 2;
    const int wallCol = center + 3;
    const double spacingDeg = MetersToLatitudeDeg(30.0);
    const GeoPoint observer{ 36.5, -111.5 };

    ElevationCells wallCells = MakeFlatCells(size, 0.0);
    for (int row = 0; row < size; row++)
    {
        wallCells[row][wallCol] = 50.0;
    }
    RasterBlockElevationSampler wallScene = MakeViewshedAlignedRaster(wallCells, observer, spacingDeg);
    RasterBlockElevationSampler flatScene = MakeViewshedAlignedRaster(MakeFlatCells(size, 0.0), observer, spacingDeg);

    ViewshedResult naive = ComputeViewshedNaive(observer, Agl(2.0), size, size, spacingDeg, wallScene);
    ViewshedResult fast = ComputeViewshedFast(observer, Agl(2.0), size, size, spacingDeg, wallScene);
    ViewshedResult naiveFlat = ComputeViewshedNaive(observer, Agl(2.0), size, size, spacingDeg, flatScene);
    ViewshedResult fastFlat = ComputeViewshedFast(observer, Agl(2.0), size, size, spacingDeg, flatScene);

    bool westVisible = true;
    bool eastHidden = true;
    int mismatchesOffWall = 0;
    for (int row = 0; row < size; row++)
    {
        for (int col = 0; col < size; col++)
        {
            if (col == wallCol) continue;
            if (naive.visible[row][col] != fast.visible[row][col]) mismatchesOffWall++;

            bool westOfWall = col < wallCol;
            CellVisibility expected = westOfWall ? CellVisibility::Visible : CellVisibility::NotVisible;
            if (naive.visible[row][col] != expected || fast.visible[row][col] != expected)
            {
                if (westOfWall) westVisible = false;
                else eastHidden = false;
            }
        }
    }

    Expect(westVisible && eastHidden && mismatchesOffWall == 0
        && CountCells(naiveFlat, CellVisibility::NotVisible) == 0
        && CountCells(fastFlat, CellVisibility::NotVisible) == 0
        && CountCells(naive, CellVisibility::Degraded) == 0
        && CountCells(fast, CellVisibility::Degraded) == 0,
        "TestFastViewshedMatchesNaive");
}

//TEST 8    
void TestSymmetricHillReciprocity()
{
    RasterBlockElevationSampler sampler = MakeTestGrid({
        {10, 10, 10, 10, 10},
        {10, 20, 30, 20, 10},
        {10, 30, 50, 30, 10},
        {10, 20, 30, 20, 10},
        {10, 10, 10, 10, 10}
    });

    GeoPoint a = TestGridCellCentre(2, 0);
    GeoPoint b = TestGridCellCentre(2, 4);

    std::vector<ProfileSample> profileAB = GetTerrainProfile(a, b, TestGridCellDeg(), sampler);
    LineOfSightResult losAB = ComputeLineOfSight(profileAB, Agl(2.0), Agl(2.0));

    std::vector<ProfileSample> profileBA = GetTerrainProfile(b, a, TestGridCellDeg(), sampler);
    LineOfSightResult losBA = ComputeLineOfSight(profileBA, Agl(2.0), Agl(2.0));

    Expect(IsOk(losAB.status) && IsOk(losBA.status)
        && losAB.isVisible == losBA.isVisible && std::abs(losAB.clearanceDeficitM - losBA.clearanceDeficitM) < 0.001, "TestSymmetricHillReciprocity");
}

//TEST 9    
void TestObserverBelowRim()
{
    RasterBlockElevationSampler sampler = MakeTestGrid({
        {5, 30, 5, 30, 5}
    });

    std::vector<ProfileSample> profile = GetTerrainProfile(TestGridCellCentre(0, 2), TestGridCellCentre(0, 4), TestGridCellDeg(), sampler);
    LineOfSightResult los = ComputeLineOfSight(profile, Agl(2.0), Agl(2.0));

    Expect(IsOk(los.status) && los.isVisible == false, "TestObserverBelowRim");
}

//TEST 10
void TestTargetOnFarSlopeVisible()
{
    std::vector<ProfileSample> slopeProfile = MakeEvenlySpacedProfile({ 0.0, 5.0, 10.0, 15.0, 20.0 }, 30.0);

    LineOfSightResult los = ComputeLineOfSight(slopeProfile, Agl(2.0), Agl(2.0));

    Expect(los.isVisible == true && IsOk(los.status), "TestTargetOnFarSlopeVisible");
}

//TEST 11
void TestFresnelClearancePartialObstruction()
{
    // Flat terrain (0m) except a 40m bump exactly at the midpoint.
    // 10km link, 50m masts both ends, 2.4 GHz.
    // Hand calc: wavelength=0.124914m, curvatureDrop(mid)=1.47151m,
    // clearance=50-(40+1.47151)=8.52849m, fresnelRadius=sqrt(0.124914*5000*5000/10000)=17.6716m
    // fraction = 8.52849/17.6716 ~= 0.4826 (partially obstructed, but not fully blocked)
    std::vector<std::optional<double>> elevations(11, 0.0);
    elevations[5] = 40.0;
    std::vector<ProfileSample> profile = MakeEvenlySpacedProfile(elevations, 1000.0);

    FresnelClearanceResult result = ComputeFresnelClearance(profile, Agl(50.0), Agl(50.0), 2.4e9);

    Expect(std::abs(result.minClearanceFraction - 0.4826) < 0.001 && result.worstPoint.has_value()
        && result.worstPoint->longitudeDeg == profile[5].point.longitudeDeg,
        "TestFresnelClearancePartialObstruction");
}

//TEST 12
void TestBatchLineOfSightMatchesIndividualCalls()
{
    // One query straight across the pyramid's summit, one diagonal to a far corner.
    // Both sides of the comparison go through GetTerrainProfile unmodified at the
    // same spacing, so a batch that built its profiles any differently would show.
    RasterBlockElevationSampler sampler = MakeTestGrid({
        {10, 10, 10, 10, 10},
        {10, 20, 30, 20, 10},
        {10, 30, 50, 30, 10},
        {10, 20, 30, 20, 10},
        {10, 10, 10, 10, 10}
    });

    GeoPoint observer = TestGridCellCentre(2, 0);
    GeoPoint targetAcrossSummit = TestGridCellCentre(2, 4);
    GeoPoint targetDiagonal = TestGridCellCentre(0, 4);

    std::vector<BatchLineOfSightQuery> queries = {
        { observer, Agl(2.0), targetAcrossSummit, Agl(2.0) },
        { observer, Agl(2.0), targetDiagonal, Agl(2.0) }
    };

    std::vector<LineOfSightResult> batchResults = ComputeBatchLineOfSight(queries, TestGridCellDeg(), sampler);

    std::vector<ProfileSample> profile1 = GetTerrainProfile(observer, targetAcrossSummit, TestGridCellDeg(), sampler);
    LineOfSightResult direct1 = ComputeLineOfSight(profile1, Agl(2.0), Agl(2.0));

    std::vector<ProfileSample> profile2 = GetTerrainProfile(observer, targetDiagonal, TestGridCellDeg(), sampler);
    LineOfSightResult direct2 = ComputeLineOfSight(profile2, Agl(2.0), Agl(2.0));

    Expect(batchResults.size() == 2
        && batchResults[0].isVisible == false
        && batchResults[0].isVisible == direct1.isVisible
        && std::abs(batchResults[0].clearanceDeficitM - direct1.clearanceDeficitM) < 0.001
        && batchResults[1].isVisible == direct2.isVisible
        && IsOk(batchResults[0].status) && IsOk(batchResults[1].status),
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
    // immediate neighbours (30, 30), which is exactly a local peak -- 20 m of rise
    // and fall over 30 m cells, far outside the classifier's 1% "flat" grade.
    RasterBlockElevationSampler sampler = MakeTestGrid({
        {10, 30, 50, 30, 10}
    });

    std::vector<ProfileSample> profile = GetTerrainProfile(TestGridCellCentre(0, 0), TestGridCellCentre(0, 4), TestGridCellDeg(), sampler);
    LineOfSightResult los = ComputeLineOfSight(profile, Agl(2.0), Agl(2.0));

    Expect(IsOk(los.status) && los.isVisible == false && los.blockingFeature == TerrainFeatureType::LocalPeak, "TestBlockingFeatureIsLocalPeak");
}

//TEST 15
void TestFastViewshedVoidDegradesDownstream()
{
    // 9x9 cells of 30 m, flat, with one void cell two cells east of the observer.
    // The ray due east crosses the void before reaching the two cells behind it.
    // Naive marks those farther cells degraded too, since their own profiles pass
    // through the same void -- ComputeViewshedFast must do the same instead of
    // flagging only the void cell and confidently classifying what lies behind it.
    const int size = 9;
    const int center = size / 2;
    const double spacingDeg = MetersToLatitudeDeg(30.0);
    const GeoPoint observer{ 36.5, -111.5 };

    ElevationCells cells = MakeFlatCells(size, 10.0);
    cells[center][center + 2] = std::nullopt;
    RasterBlockElevationSampler sampler = MakeViewshedAlignedRaster(cells, observer, spacingDeg);

    ViewshedResult viewshed = ComputeViewshedFast(observer, Agl(2.0), size, size, spacingDeg, sampler);

    Expect(IsConfident(viewshed.visible[center][center + 1])
        && viewshed.visible[center][center + 2] == CellVisibility::Degraded
        && viewshed.visible[center][center + 3] == CellVisibility::Degraded
        && viewshed.visible[center][center + 4] == CellVisibility::Degraded,
        "TestFastViewshedVoidDegradesDownstream");
}

//TEST 16
void TestRealElevationSamplerReadsVoidFromFile()
{
    // TestVoidPointIsDegraded only ever exercises the "elevation == nullopt"
    // branch by construction, on a hand-built profile -- it never touches
    // RealElevationSampler's own void sentinel (-32768) or its file-reading path
    // at all. This writes a 2x2 .hgt with a real void sentinel and reads it back
    // through RealElevationSampler itself.
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
    // Nearest and bilinear interpolation on the same ridgeline: query a point
    // straddling the pyramid's ridge (between the
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
    // The naive profile's output, frozen as the oracle everything after it is
    // measured against. DATA/oracle_profile.csv freezes
    // GetTerrainProfile's output for a real 2.9 km path across the included
    // Grand Canyon tile, sampled at the data's own ~90 m spacing: 33 samples, no
    // voids, elevations from 1287 m to 1709 m. This regenerates that profile and
    // diffs it field by field, so a change to the distance model, the sample
    // count or the tile reader that shifts a single number is caught here.
    //
    // DATA/ is read relative to the working directory, as the benchmark does, and
    // the test skips rather than fails when run from somewhere the data isn't.
    const std::string testName = "TestProfileMatchesFrozenOracle";

    RealElevationSampler sampler("DATA/N36W112.hgt", 36.0, -112.0);
    std::ifstream oracleFile("DATA/oracle_profile.csv");
    if (!sampler.IsLoaded() || !oracleFile.is_open())
    {
        Skip(testName, "DATA/N36W112.hgt or DATA/oracle_profile.csv not found from this working directory");
        return;
    }

    GeoPoint a{ 36.30, -111.90 };
    GeoPoint b{ 36.32, -111.88 };
    std::vector<ProfileSample> profile = GetTerrainProfile(a, b, MetersToLatitudeDeg(90.0), sampler);

    bool matches = profile.size() == 33;

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

        // A checkout with CRLF line endings leaves a '\r' on every line.
        if (!line.empty() && line.back() == '\r') line.pop_back();

        // Four comma-separated numbers, nothing else. A damaged row fails the test
        // and says which row, rather than throwing out of std::stod and taking the
        // whole suite down with it.
        std::stringstream ss(line);
        std::string fields[4];
        std::string extra;
        bool rowComplete = true;
        for (auto& field : fields)
        {
            if (!std::getline(ss, field, ',')) rowComplete = false;
        }
        std::optional<double> expectedLatValue = ParseFiniteDouble(fields[0].c_str());
        std::optional<double> expectedLonValue = ParseFiniteDouble(fields[1].c_str());
        std::optional<double> expectedElevValue = ParseFiniteDouble(fields[2].c_str());
        std::optional<double> expectedDistValue = ParseFiniteDouble(fields[3].c_str());
        if (!rowComplete || std::getline(ss, extra, ',')
            || !expectedLatValue || !expectedLonValue || !expectedElevValue || !expectedDistValue)
        {
            std::cout << "  " << testName << ": oracle row " << index << " is not four numbers: '" << line << "'" << std::endl;
            matches = false;
            break;
        }
        double expectedLat = *expectedLatValue;
        double expectedLon = *expectedLonValue;
        double expectedElev = *expectedElevValue;
        double expectedDist = *expectedDistValue;

        const ProfileSample& actual = profile[index];
        double actualElev = actual.elevationM.has_value() ? *actual.elevationM : -32768.0;

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

    Expect(matches, testName);
}

//TEST 19
void TestNarrowSpikeCanFallBetweenSamples()
{
    // A one-cell spike on an otherwise flat raster row of 30 m cells. Sampling at
    // the data's own spacing visits every cell, so the spike is found. Sampling
    // coarser than the data cannot promise that: at 45 m over 300 m the interval
    // count rounds up to 7 intervals of ~42.9 m, landing on columns
    // 0,1,3,4,6,7,9,10 -- column 5 is never queried and the spike falls between
    // samples. The requested spacing bounds the effective spacing from above; it
    // does not make features narrower than that spacing visible.
    const double spacingDeg = MetersToLatitudeDeg(30.0);
    const double latDeg = 36.5;
    const double lonStepDeg = LongitudeSpacingForLatitude(spacingDeg, latDeg);
    const int spikeCol = 5;

    ElevationCells rowCells(1, std::vector<std::optional<double>>(11, 0.0));
    rowCells[0][spikeCol] = 999.0;
    RasterBlockElevationSampler sampler(rowCells, latDeg, -111.5, spacingDeg, lonStepDeg, VerticalDatum::OrthometricMsl);

    GeoPoint a{ latDeg, -111.5 };
    GeoPoint b{ latDeg, -111.5 + 10 * lonStepDeg };

    auto sawSpike = [](const std::vector<ProfileSample>& profile) {
        for (const auto& sample : profile)
        {
            if (sample.elevationM.has_value() && *sample.elevationM == 999.0) return true;
        }
        return false;
    };

    std::vector<ProfileSample> atDataSpacing = GetTerrainProfile(a, b, spacingDeg, sampler);
    std::vector<ProfileSample> coarser = GetTerrainProfile(a, b, MetersToLatitudeDeg(45.0), sampler);

    Expect(atDataSpacing.size() == 11 && sawSpike(atDataSpacing)
        && coarser.size() == 8 && !sawSpike(coarser),
        "TestNarrowSpikeCanFallBetweenSamples");
}

//TEST 20
void TestMultiTileProfileCrossesSeamWithoutGap()
{
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
    // profile.front()/back() on an empty profile is
    // undefined behaviour, and a single-sample profile has zero total distance,
    // so there is no sight line to interpolate along. GetTerrainProfile never produces either,
    // but ComputeLineOfSight/ComputeFresnelClearance are public entry points
    // that accept any caller-built profile, so they must guard against both.
    std::vector<ProfileSample> emptyProfile;
    std::vector<ProfileSample> singleSampleProfile;
    ProfileSample onlySample;
    onlySample.point = GeoPoint{ 0, 0 };
    onlySample.elevationM = 10.0;
    onlySample.distanceFromStartM = 0.0;
    singleSampleProfile.push_back(onlySample);

    LineOfSightResult losEmpty = ComputeLineOfSight(emptyProfile, Agl(2.0), Agl(2.0));
    LineOfSightResult losSingle = ComputeLineOfSight(singleSampleProfile, Agl(2.0), Agl(2.0));
    FresnelClearanceResult fresnelEmpty = ComputeFresnelClearance(emptyProfile, Agl(2.0), Agl(2.0), 2.4e9);
    FresnelClearanceResult fresnelSingle = ComputeFresnelClearance(singleSampleProfile, Agl(2.0), Agl(2.0), 2.4e9);

    Expect(losEmpty.status == ComputationStatus::EmptyOrSingleSampleProfile
        && losSingle.status == ComputationStatus::EmptyOrSingleSampleProfile
        && fresnelEmpty.status == ComputationStatus::EmptyOrSingleSampleProfile
        && fresnelSingle.status == ComputationStatus::EmptyOrSingleSampleProfile,
        "TestEmptyAndSingleSampleProfilesAreDegradedNotUB");
}

//TEST 22
void TestBlockingFeatureClassificationIsSpacingInvariant()
{
    // A fixed 1 m epsilon in ClassifyBlockingFeature meant
    // the same physical grade classified differently depending on sample
    // spacing -- a 2% slope sampled every 10 m has 0.2 m steps (under the old
    // fixed 1 m tolerance, misread as "flat"/Plateau), while the same 2% slope
    // sampled every 100 m has 2 m steps (correctly read as RisingSlope). The
    // epsilon must scale with spacing so the same terrain classifies the same
    // way regardless of how finely it happens to be sampled.
    std::vector<ProfileSample> fineProfile = MakeEvenlySpacedProfile({ 0.0, 0.2, 0.4 }, 10.0);
    std::vector<ProfileSample> coarseProfile = MakeEvenlySpacedProfile({ 0.0, 2.0, 4.0 }, 100.0);

    TerrainFeatureType fineResult = ClassifyBlockingFeature(fineProfile, 1);
    TerrainFeatureType coarseResult = ClassifyBlockingFeature(coarseProfile, 1);

    Expect(fineResult == TerrainFeatureType::RisingSlope && coarseResult == TerrainFeatureType::RisingSlope,
        "TestBlockingFeatureClassificationIsSpacingInvariant");
}

//TEST 23
void TestViewshedLongitudeSpacingCorrectsForLatitude()
{
    // A viewshed grid stepped by equal degrees on both axes reaches 30 km either
    // side north-south but only 24.1 km east-west at 36.5 N -- an ellipse, not
    // the circle its radius implies. The distance model in TerrainProfile.h
    // doesn't touch this -- it's a
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
    // Convert between ellipsoidal (HAE) and
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
    // A query that mixes datums without the means to convert must be rejected as
    // a value, not an exception. An ellipsoidal height over orthometric terrain
    // with no geoid undulation supplied can't be converted, so it must be
    // rejected -- not silently compared with terrain it isn't compatible with.
    RasterBlockElevationSampler sampler = MakeTestGrid({
        {10, 10, 10},
        {10, 10, 10},
        {10, 10, 10}
    });

    std::vector<ProfileSample> profile = GetTerrainProfile(TestGridCellCentre(0, 0), TestGridCellCentre(0, 2), TestGridCellDeg(), sampler);

    DatumHeight wrongDatum{ 2.0, VerticalDatum::EllipsoidalHae };
    LineOfSightResult los = ComputeLineOfSight(profile, wrongDatum, Agl(2.0));

    Expect(los.status == ComputationStatus::DatumRejected, "TestComputeLineOfSightRejectsWrongHeightDatum");
}

//TEST 26
void TestComputeLineOfSightRejectsUnknownTerrainDatum()
{
    // A sampler that never declared its datum (left at the default Unknown)
    // must not have its elevations silently trusted -- the query is rejected
    // rather than assuming the terrain values mean anything comparable to an
    // AGL height.
    RasterBlockElevationSampler sampler = MakeTestGrid({
        {10, 10, 10},
        {10, 10, 10},
        {10, 10, 10}
    }, VerticalDatum::Unknown);

    std::vector<ProfileSample> profile = GetTerrainProfile(TestGridCellCentre(0, 0), TestGridCellCentre(0, 2), TestGridCellDeg(), sampler);

    LineOfSightResult los = ComputeLineOfSight(profile, Agl(2.0), Agl(2.0));

    Expect(los.status == ComputationStatus::DatumRejected && sampler.GetDatum() == VerticalDatum::Unknown,
        "TestComputeLineOfSightRejectsUnknownTerrainDatum");
}

//TEST 27
void TestRealElevationSamplerDeclaresOrthometricDatum()
{
    // Every IElevationSampler declares the datum of what it returns. SRTM .hgt files are
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
    // Real raster sources usually put row 0 at the northern edge, which a signed,
    // negative latitude step states outright instead of leaving it as an
    // unwritten convention. The origin is the centre of the north-west cell
    // (latitude 2); a negative row step means increasing row index moves SOUTH
    // (decreasing latitude).
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
    // A block of 30 m cells, the column step widened by 1/cos(latitude).
    std::vector<std::vector<std::optional<double>>> block = {
        { 10.0, 10.0, 10.0, 10.0, 10.0 },
        { 10.0, 10.0, 10.0, 10.0, 10.0 },
        { 10.0, 10.0, 10.0, 10.0, 10.0 }
    };
    const double rowStepDeg = MetersToLatitudeDeg(30.0);
    const double colStepDeg = LongitudeSpacingForLatitude(rowStepDeg, 36.5);
    RasterBlockElevationSampler sampler(block, 36.5, -111.5, rowStepDeg, colStepDeg, VerticalDatum::OrthometricMsl);

    GeoPoint a{ 36.5, -111.5 };
    GeoPoint b{ 36.5, -111.5 + 4 * colStepDeg };
    std::vector<ProfileSample> profile = GetTerrainProfile(a, b, rowStepDeg, sampler);
    LineOfSightResult los = ComputeLineOfSight(profile, Agl(2.0), Agl(2.0));

    Expect(los.isVisible == true && IsOk(los.status), "TestRasterBlockSamplerWorksWithLineOfSight");
}

//TEST 32
void TestScratchBufferProfileAllocatesNothingOnReuse()
{
    // A per-frame line-of-sight query should be able to
    // reuse one caller-owned buffer instead of allocating a new vector every call.
    // std::vector::clear() keeps its capacity, so a second call that needs no more
    // elements than the first must not grow capacity -- that is the observable
    // proof that the buffer was reused rather than replaced.
    RasterBlockElevationSampler sampler = MakeTestGrid({
        {10, 10, 10, 10, 10},
        {10, 10, 10, 10, 10},
        {10, 10, 10, 10, 10},
        {10, 10, 10, 10, 10},
        {10, 10, 10, 10, 10}
    });

    GeoPoint a = TestGridCellCentre(0, 0);
    GeoPoint b = TestGridCellCentre(0, 4);

    std::vector<ProfileSample> scratch;
    GetTerrainProfile(a, b, TestGridCellDeg(), sampler, scratch);
    size_t firstSize = scratch.size();
    size_t firstCapacity = scratch.capacity();

    GetTerrainProfile(a, b, TestGridCellDeg(), sampler, scratch);
    size_t secondSize = scratch.size();
    size_t secondCapacity = scratch.capacity();

    LineOfSightResult los = ComputeLineOfSight(scratch, Agl(2.0), Agl(2.0));

    Expect(firstSize == secondSize && secondCapacity == firstCapacity
        && los.isVisible == true && IsOk(los.status),
        "TestScratchBufferProfileAllocatesNothingOnReuse");
}

//TEST 33
void TestGreatCircleDistanceMatchesKnownValues()
{
    // The great-circle distance model had no known-value test anywhere: every
    // profile test used to overwrite distanceFromStartM before asserting, so
    // nothing pinned GreatCircleDistanceM to a number derivable
    // independently of the code. These cases are exact on a sphere of radius
    // EarthRadiusM, so they check the implementation against geometry rather
    // than against its own output.
    const double pi = 3.14159265358979323846;
    const double oneDegreeArcM = EarthRadiusM * DegToRad;
    const double quarterCircleM = EarthRadiusM * pi / 2.0;

    double meridianOneDeg = GreatCircleDistanceM(GeoPoint{ 0, 0 }, GeoPoint{ 1, 0 });
    double equatorOneDeg = GreatCircleDistanceM(GeoPoint{ 0, 0 }, GeoPoint{ 0, 1 });
    double equatorQuarter = GreatCircleDistanceM(GeoPoint{ 0, 0 }, GeoPoint{ 0, 90 });
    double equatorToPole = GreatCircleDistanceM(GeoPoint{ 0, 0 }, GeoPoint{ 90, 0 });
    double samePoint = GreatCircleDistanceM(GeoPoint{ 36.5, -111.5 }, GeoPoint{ 36.5, -111.5 });

    Expect(std::abs(meridianOneDeg - oneDegreeArcM) < 1e-6
        && std::abs(equatorOneDeg - oneDegreeArcM) < 1e-6
        && std::abs(equatorQuarter - quarterCircleM) < 1e-6
        && std::abs(equatorToPole - quarterCircleM) < 1e-6
        && samePoint == 0.0,
        "TestGreatCircleDistanceMatchesKnownValues");
}

//TEST 34
void TestGreatCircleInterpolateBulgesTowardPole()
{
    // The property that separates a great-circle path from straight-line
    // interpolation in degree-space: between two points sharing a non-equatorial
    // latitude, the great circle does not follow that latitude, it bends toward
    // the pole. Degree-space interpolation would hold the latitude constant, so
    // this asserts the model is genuinely spherical and not the old flat one.
    // For two points at latitude L separated by dLon, the midpoint latitude
    // satisfies tan(mid) = tan(L) / cos(dLon / 2) -- at 60 deg and 40 deg apart
    // that is about 61.5 deg, clearly poleward of both endpoints.
    GeoPoint a{ 60.0, 0.0 };
    GeoPoint b{ 60.0, 40.0 };
    double angleRad = GreatCircleDistanceM(a, b) / EarthRadiusM;
    GeoPoint mid = GreatCircleInterpolate(a, b, 0.5, angleRad);

    double expectedMidLatDeg = atan(tan(60.0 * DegToRad) / cos(20.0 * DegToRad)) / DegToRad;

    // On the equator the great circle IS the parallel, so there is no bulge --
    // the one case where both models agree exactly.
    GeoPoint e1{ 0.0, 0.0 };
    GeoPoint e2{ 0.0, 40.0 };
    double equatorAngleRad = GreatCircleDistanceM(e1, e2) / EarthRadiusM;
    GeoPoint equatorMid = GreatCircleInterpolate(e1, e2, 0.5, equatorAngleRad);

    Expect(std::abs(mid.latitudeDeg - expectedMidLatDeg) < 1e-6
        && mid.latitudeDeg > 61.0
        && std::abs(mid.longitudeDeg - 20.0) < 1e-9
        && std::abs(equatorMid.latitudeDeg) < 1e-9
        && std::abs(equatorMid.longitudeDeg - 20.0) < 1e-9,
        "TestGreatCircleInterpolateBulgesTowardPole");
}

//TEST 35
void TestViewshedRespondsToCurvatureFactor()
{
    // The viewsheds' original blocking defect was that Earth curvature never reached
    // the geometry inside either viewshed: k was accepted as a parameter and then
    // had no effect. Every other viewshed test asserts something that stays true
    // whether curvature is applied or not, so forcing curvature inert left the
    // whole suite green -- the defect could come back unnoticed.
    //
    // This test pins the one thing that cannot be true if curvature is inert: over
    // flat ground at a range where the Earth's bulge is the only thing that can
    // hide a cell, a near-flat-earth k must leave strictly more cells visible than
    // the default k does. If k stops reaching the curvature term, both counts
    // become equal and this fails.
    // 41x41 cells of 1 km (a 20 km radius) on flat ground at sea level, observer
    // 2 m up. Curvature only matters at range, so this runs at kilometre cells
    // rather than the 30 m of the terrain tests: under a 20 km sightline the
    // Earth's bulge at the midpoint is ~6 m, well above a 2 m eye.
    const int size = 41;
    const double spacingDeg = MetersToLatitudeDeg(1000.0);
    const GeoPoint observer{ 36.5, -111.5 };
    RasterBlockElevationSampler sampler = MakeViewshedAlignedRaster(MakeFlatCells(size, 0.0), observer, spacingDeg);

    const double flatEarthK = 1e12; // curvature term driven to ~0

    int naiveCurved = CountCells(ComputeViewshedNaive(observer, Agl(2.0), size, size, spacingDeg, sampler), CellVisibility::Visible);
    int naiveFlat = CountCells(ComputeViewshedNaive(observer, Agl(2.0), size, size, spacingDeg, sampler, flatEarthK), CellVisibility::Visible);

    int fastCurved = CountCells(ComputeViewshedFast(observer, Agl(2.0), size, size, spacingDeg, sampler), CellVisibility::Visible);
    int fastFlat = CountCells(ComputeViewshedFast(observer, Agl(2.0), size, size, spacingDeg, sampler, flatEarthK), CellVisibility::Visible);

    Expect(naiveFlat > naiveCurved && fastFlat > fastCurved,
        "TestViewshedRespondsToCurvatureFactor");
}

//TEST 36
void TestFastViewshedSeesVoidOnEverySampleOfADiagonalRay()
{
    // A void anywhere along a ray must degrade what lies behind it. On a diagonal
    // ray, consecutive samples frequently round into the same grid cell, and the
    // fast viewshed used to skip such samples outright -- so a void landing on one
    // was never noticed at all: zero Degraded cells, the ray reporting full
    // confidence straight through a hole, which is exactly the defect the void
    // handling was supposed to close.
    //
    // TestFastViewshedVoidDegradesDownstream only ever places its void on a
    // straight east-pointing ray, where every sample lands in a fresh cell, so it
    // could never see this. This walks a diagonal ray and puts the void on each of
    // its in-grid samples in turn; every single one must be noticed.
    class VoidAtPointSampler : public IElevationSampler
    {
    public:
        double targetLatDeg = 1e9;
        double targetLonDeg = 1e9;
        std::optional<double> GetElevation(double latitudeDeg, double longitudeDeg) override
        {
            if (std::abs(latitudeDeg - targetLatDeg) < 1e-9 && std::abs(longitudeDeg - targetLonDeg) < 1e-9)
            {
                return std::nullopt;
            }
            return 10.0;
        }
        VerticalDatum GetDatum() const override { return VerticalDatum::OrthometricMsl; }
    };

    const int gridSize = 11;
    const double spacingDeg = MetersToLatitudeDeg(30.0);
    const GeoPoint observer{ 36.5, -111.5 };
    const int centerRow = gridSize / 2;
    const int centerCol = gridSize / 2;
    const double lonSpacingDeg = LongitudeSpacingForLatitude(spacingDeg, observer.latitudeDeg);

    // Map the diagonal corner ray first, with no void anywhere.
    VoidAtPointSampler mapper;
    GeoPoint corner{ observer.latitudeDeg + (gridSize - 1 - centerRow) * spacingDeg,
                     observer.longitudeDeg + (gridSize - 1 - centerCol) * lonSpacingDeg };
    std::vector<ProfileSample> ray = GetTerrainProfile(observer, corner, spacingDeg, mapper);

    // sameCellSamples proves the ray really contains the case under test: a test
    // whose ray never had two samples in one cell would pass without ever
    // exercising it.
    int samplesTested = 0;
    int samplesMissed = 0;
    int sameCellSamples = 0;
    int lastRow = -1;
    int lastCol = -1;
    for (size_t i = 1; i < ray.size(); i++)
    {
        int row = (int)round((ray[i].point.latitudeDeg - observer.latitudeDeg) / spacingDeg) + centerRow;
        int col = (int)round((ray[i].point.longitudeDeg - observer.longitudeDeg) / lonSpacingDeg) + centerCol;
        if (row < 0 || row >= gridSize || col < 0 || col >= gridSize) continue;
        if (row == lastRow && col == lastCol) sameCellSamples++;
        lastRow = row;
        lastCol = col;

        VoidAtPointSampler sampler;
        sampler.targetLatDeg = ray[i].point.latitudeDeg;
        sampler.targetLonDeg = ray[i].point.longitudeDeg;

        ViewshedResult result = ComputeViewshedFast(observer, Agl(2.0), gridSize, gridSize, spacingDeg, sampler);

        int degradedCells = 0;
        for (const auto& resultRow : result.visible)
        {
            for (auto cell : resultRow)
            {
                if (cell == CellVisibility::Degraded) degradedCells++;
            }
        }

        samplesTested++;
        if (degradedCells == 0) samplesMissed++;
    }

    Expect(samplesTested > 2 && samplesMissed == 0 && sameCellSamples > 0,
        "TestFastViewshedSeesVoidOnEverySampleOfADiagonalRay");
}

//TEST 37
void TestTerrainProfileNeverSamplesCoarserThanRequested()
{
    // GetTerrainProfile's own sample count and distances had no known-value test:
    // every profile test overwrote distanceFromStartM before asserting. Three
    // paths at 30 m spacing: exactly 10 spacings along a meridian and along a
    // parallel -- the shape of a viewshed's axis rays, where rounding the count
    // down used to come out one sample short -- and 10.5 spacings, which must
    // round the interval count up. Each must end at the true great-circle
    // distance with no interval wider than requested.
    const double spacingM = 30.0;
    const double spacingDeg = MetersToLatitudeDeg(spacingM);
    const double latDeg = 36.5;
    const double lonStepDeg = LongitudeSpacingForLatitude(spacingDeg, latDeg);
    FakeElevationSampler anyTerrain({ { 0.0 } }, InterpolationMode::Nearest, VerticalDatum::OrthometricMsl);

    const GeoPoint start{ latDeg, -111.5 };
    const GeoPoint alongMeridian{ latDeg + 10 * spacingDeg, -111.5 };
    const GeoPoint alongParallel{ latDeg, -111.5 + 10 * lonStepDeg };
    const GeoPoint offMultiple{ latDeg + 10.5 * spacingDeg, -111.5 };

    auto holds = [&](GeoPoint end, size_t expectedSamples) {
        std::vector<ProfileSample> profile = GetTerrainProfile(start, end, spacingDeg, anyTerrain);
        if (profile.size() != expectedSamples) return false;
        if (std::abs(profile.back().distanceFromStartM - GreatCircleDistanceM(start, end)) > 1e-9) return false;
        for (size_t i = 1; i < profile.size(); i++)
        {
            if (profile[i].distanceFromStartM - profile[i - 1].distanceFromStartM > spacingM + 1e-9) return false;
        }
        return true;
    };

    Expect(holds(alongMeridian, 11) && holds(alongParallel, 11) && holds(offMultiple, 12),
        "TestTerrainProfileNeverSamplesCoarserThanRequested");
}

//TEST 38
void TestLineOfSightInterpolatesByDistanceNotIndex()
{
    // A caller-built profile with uneven spacing: samples at 0, 900 and 1000 m,
    // ground 0 -> 85 -> 100 m, both ends 0 m above ground, curvature switched
    // off. The true sight line from 0 m to 100 m is 90 m high at 900 m, so the
    // 85 m post there clears it by 5 m. Interpolating the line by sample index
    // puts it at 50 m instead -- halfway through three samples -- and reports the
    // post blocking by 35 m. GetTerrainProfile always spaces samples evenly, but
    // the frame-safe path invites callers to fill their own buffers.
    std::vector<ProfileSample> uneven = { MakeSampleOnEquator(0.0, 0.0), MakeSampleOnEquator(900.0, 85.0), MakeSampleOnEquator(1000.0, 100.0) };

    const double flatEarthK = 1e12;
    LineOfSightResult los = ComputeLineOfSight(uneven, Agl(0.0), Agl(0.0), flatEarthK);
    FresnelClearanceResult fresnel = ComputeFresnelClearance(uneven, Agl(0.0), Agl(0.0), 10e9, flatEarthK);

    Expect(IsOk(los.status) && los.isVisible
        && IsOk(fresnel.status) && fresnel.minClearanceFraction > 0.0,
        "TestLineOfSightInterpolatesByDistanceNotIndex");
}

//TEST 39
void TestViewshedsAgreeWhenObserverIsUnknown()
{
    // Two ways nothing is known about the observer: it stands on a void, or its
    // height can't be put on the terrain's datum. Either way every cell -- the
    // observer's own included -- must be Degraded, in both algorithms. Naive used
    // to mark the observer's cell Visible unconditionally, and fast returned
    // NotCovered everywhere for a void, so the two disagreed on exactly the case
    // CellVisibility exists to keep apart.
    const int size = 7;
    const int center = size / 2;
    const int allCells = size * size;
    const double spacingDeg = MetersToLatitudeDeg(30.0);
    const GeoPoint observer{ 36.5, -111.5 };

    ElevationCells voidUnderObserver = MakeFlatCells(size, 10.0);
    voidUnderObserver[center][center] = std::nullopt;
    RasterBlockElevationSampler onVoid = MakeViewshedAlignedRaster(voidUnderObserver, observer, spacingDeg);
    RasterBlockElevationSampler solidGround = MakeViewshedAlignedRaster(MakeFlatCells(size, 10.0), observer, spacingDeg);

    // Terrain is orthometric and no geoid undulation is supplied, so this can't be converted.
    DatumHeight unconvertible{ 500.0, VerticalDatum::EllipsoidalHae };

    Expect(CountCells(ComputeViewshedNaive(observer, Agl(2.0), size, size, spacingDeg, onVoid), CellVisibility::Degraded) == allCells
        && CountCells(ComputeViewshedFast(observer, Agl(2.0), size, size, spacingDeg, onVoid), CellVisibility::Degraded) == allCells
        && CountCells(ComputeViewshedNaive(observer, unconvertible, size, size, spacingDeg, solidGround), CellVisibility::Degraded) == allCells
        && CountCells(ComputeViewshedFast(observer, unconvertible, size, size, spacingDeg, solidGround), CellVisibility::Degraded) == allCells,
        "TestViewshedsAgreeWhenObserverIsUnknown");
}

//TEST 40
void TestLineOfSightAcceptsHeightsInAnyTerrainComparableDatum()
{
    // A host holds an airborne platform's altitude as a height above the
    // ellipsoid. Orthometric terrain 100 m / 130 m / 100 m over 100 m, curvature
    // off, observer and target both 115 m above mean sea level -- expressed three
    // ways: 15 m above ground, 115 m orthometric, and 95 m ellipsoidal where the
    // geoid sits 20 m below the ellipsoid (95 - (-20) = 115). All three must see
    // the 130 m ridge block the line by exactly 15 m. Each mistake gives a
    // different answer: the wrong undulation sign gives 75 m (blocked by 55),
    // using the ellipsoidal value unconverted gives 95 m (blocked by 35), and
    // reading it as above-ground gives 195 m (not blocked at all).
    auto sampleAt = [](double distanceM, double elevationM, VerticalDatum datum) {
        return MakeSampleOnEquator(distanceM, elevationM, datum);
    };
    const VerticalDatum msl = VerticalDatum::OrthometricMsl;
    std::vector<ProfileSample> ridge = { sampleAt(0.0, 100.0, msl), sampleAt(50.0, 130.0, msl), sampleAt(100.0, 100.0, msl) };

    const double flatEarthK = 1e12;
    auto blockedBy15 = [&](DatumHeight height) {
        LineOfSightResult los = ComputeLineOfSight(ridge, height, height, flatEarthK);
        return IsOk(los.status) && !los.isVisible && std::abs(los.clearanceDeficitM - 15.0) < 1e-9;
    };

    DatumHeight aboveGround{ 15.0, VerticalDatum::HeightAboveGround };
    DatumHeight orthometric{ 115.0, VerticalDatum::OrthometricMsl };
    DatumHeight ellipsoidal{ 95.0, VerticalDatum::EllipsoidalHae, -20.0 };

    // And what must still be rejected rather than guessed.
    auto rejected = [&](const std::vector<ProfileSample>& profile, DatumHeight height) {
        return ComputeLineOfSight(profile, height, height, flatEarthK).status == ComputationStatus::DatumRejected;
    };
    DatumHeight ellipsoidalWithoutUndulation{ 95.0, VerticalDatum::EllipsoidalHae };
    DatumHeight pressureAltitude{ 1000.0, VerticalDatum::PressureAltitude };
    std::vector<ProfileSample> terrainAboveGround = { sampleAt(0.0, 100.0, VerticalDatum::HeightAboveGround), sampleAt(100.0, 100.0, VerticalDatum::HeightAboveGround) };
    std::vector<ProfileSample> mixedTerrain = { sampleAt(0.0, 100.0, msl), sampleAt(100.0, 100.0, VerticalDatum::EllipsoidalHae) };

    // The fast viewshed resolves the observer itself rather than through
    // ComputeLineOfSight, so check an ellipsoidal observer is accepted there too.
    const double spacingDeg = MetersToLatitudeDeg(30.0);
    RasterBlockElevationSampler flatGround = MakeViewshedAlignedRaster(MakeFlatCells(7, 100.0), GeoPoint{ 36.5, -111.5 }, spacingDeg);
    ViewshedResult fromEllipsoidalObserver = ComputeViewshedFast(GeoPoint{ 36.5, -111.5 }, ellipsoidal, 7, 7, spacingDeg, flatGround);

    Expect(blockedBy15(aboveGround) && blockedBy15(orthometric) && blockedBy15(ellipsoidal)
        && rejected(ridge, ellipsoidalWithoutUndulation)
        && rejected(ridge, pressureAltitude)
        && rejected(terrainAboveGround, Agl(2.0))
        && rejected(mixedTerrain, Agl(2.0))
        && CountCells(fromEllipsoidalObserver, CellVisibility::Degraded) == 0,
        "TestLineOfSightAcceptsHeightsInAnyTerrainComparableDatum");
}

//TEST 41
void TestOneArcSecondTileReadsAtNativeResolution()
{
    // The 1-arcsecond (~30 m) tile holds 3601x3601 posts, which RealElevationSampler
    // infers from the file size. This walks 200 consecutive posts north along one
    // column with GetTerrainProfile at the tile's own post spacing: every sample
    // must return exactly the value stored in the file at its own row and column.
    // A reader that assumed 1201 posts, or placed the grid one post off, would
    // return a neighbour's elevation instead. Skips when the tile isn't present.
    const std::string testName = "TestOneArcSecondTileReadsAtNativeResolution";
    const std::string path = "DATA/SRTM1/N36W112.hgt";
    RealElevationSampler sampler(path, 36.0, -112.0);
    std::ifstream file(path, std::ios::binary);
    if (!sampler.IsLoaded() || !file.is_open())
    {
        Skip(testName, path + " not found from this working directory");
        return;
    }

    const int posts = 3601;
    std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (bytes.size() != 2 * (size_t)posts * posts)
    {
        Expect(false, testName);
        return;
    }
    auto storedAt = [&](int row, int col) {
        size_t i = (size_t)row * posts + col;
        return (int16_t)(((unsigned char)bytes[2 * i] << 8) | (unsigned char)bytes[2 * i + 1]);
    };

    // Row 0 is the tile's northern edge (latitude 37), column 0 its western edge.
    const double postDeg = 1.0 / (posts - 1);
    const int col = 1500;
    const int southRow = 2000;
    const int steps = 200;
    GeoPoint a{ 37.0 - southRow * postDeg, -112.0 + col * postDeg };
    GeoPoint b{ 37.0 - (southRow - steps) * postDeg, a.longitudeDeg };
    std::vector<ProfileSample> profile = GetTerrainProfile(a, b, postDeg, sampler);

    bool matches = profile.size() == steps + 1;
    int changesBetweenPosts = 0; // proves neighbouring posts differ, so a one-post slip would show
    for (size_t i = 0; matches && i < profile.size(); i++)
    {
        int row = southRow - (int)i;
        if (!profile[i].elevationM.has_value() || *profile[i].elevationM != (double)storedAt(row, col)) matches = false;
        if (i > 0 && storedAt(row, col) != storedAt(row + 1, col)) changesBetweenPosts++;
    }

    Expect(matches && changesBetweenPosts > 0, testName);
}

//TEST 42
void TestOneArcSecondTileAgreesWithThreeArcSecondTile()
{
    // Two independent products for the same square degree: the ~90 m tile (SRTM
    // v2.1) and the ~30 m tile (SRTMGL1 v3, reprocessed and void-filled). Every
    // third 1-arcsecond post sits on a 3-arcsecond post. The two are not identical
    // -- different processing, and canyon walls steep enough that a post's exact
    // footprint moves the answer by hundreds of metres -- but read through
    // RealElevationSampler at every interior 3-arcsecond post, they must agree to a
    // few metres on average, and agree best with no offset: shifting the fine
    // tile's queries one of its own posts north, south, east or west must make the
    // agreement worse. A reader that misplaced either grid, even by one post, would
    // find its best match somewhere other than zero. Skips when either tile is missing.
    const std::string testName = "TestOneArcSecondTileAgreesWithThreeArcSecondTile";
    RealElevationSampler coarse("DATA/N36W112.hgt", 36.0, -112.0);
    RealElevationSampler fine("DATA/SRTM1/N36W112.hgt", 36.0, -112.0);
    if (!coarse.IsLoaded() || !fine.IsLoaded())
    {
        Skip(testName, "DATA/N36W112.hgt or DATA/SRTM1/N36W112.hgt not found from this working directory");
        return;
    }

    const int coarsePosts = 1201;
    const double coarsePostDeg = 1.0 / (coarsePosts - 1);
    const double finePostDeg = 1.0 / 3600;

    auto meanAbsDifference = [&](int finePostsNorth, int finePostsEast) {
        double sum = 0.0;
        long count = 0;
        for (int row = 1; row < coarsePosts - 1; row++)
        {
            for (int col = 1; col < coarsePosts - 1; col++)
            {
                double lat = 37.0 - row * coarsePostDeg;
                double lon = -112.0 + col * coarsePostDeg;
                auto c = coarse.GetElevation(lat, lon);
                auto f = fine.GetElevation(lat + finePostsNorth * finePostDeg, lon + finePostsEast * finePostDeg);
                if (!c.has_value() || !f.has_value()) continue;
                sum += std::abs(*c - *f);
                count++;
            }
        }
        return count > 0 ? sum / count : 1e9;
    };

    double aligned = meanAbsDifference(0, 0);
    bool bestAtZero = aligned < meanAbsDifference(1, 0) && aligned < meanAbsDifference(-1, 0)
        && aligned < meanAbsDifference(0, 1) && aligned < meanAbsDifference(0, -1);

    std::cout << "  1-arcsecond vs 3-arcsecond tile, mean absolute difference at shared posts: " << aligned << " m" << std::endl;
    Expect(aligned < 10.0 && bestAtZero, testName);
}

//TEST 43
void TestRasterBlockViewReadsHostBufferInPlace()
{
    // A raster reply is one flat row-major elevation array plus a parallel validity
    // array. The view must read those arrays where they are. A 3x3 block of 30 m
    // cells, row 0 at the north (negative row step), with the centre cell flagged
    // void and a plausible-looking elevation stored beside the flag that must never
    // be returned. The view must agree with the owning sampler built from the same
    // cells at every cell centre and one cell beyond the block on every side -- and,
    // the property that makes it a view, a change made to the buffer after
    // construction must show up in the very next query.
    std::vector<double> elevationsM = { 10.0, 11.0, 12.0,
                                        20.0, 21.0, 22.0,
                                        30.0, 31.0, 32.0 };
    std::vector<uint8_t> validity = { 1, 1, 1,
                                      1, 0, 1,
                                      1, 1, 1 };

    RasterBlockGeometry geometry;
    geometry.originCellCentreLatitudeDeg = 36.5;
    geometry.originCellCentreLongitudeDeg = -111.5;
    geometry.rowStepDeg = -MetersToLatitudeDeg(30.0);
    geometry.colStepDeg = LongitudeSpacingForLatitude(MetersToLatitudeDeg(30.0), 36.5);
    geometry.rows = 3;
    geometry.cols = 3;
    RasterBlockViewElevationSampler view(elevationsM.data(), validity.data(), geometry, VerticalDatum::OrthometricMsl);

    ElevationCells cells = {
        { 10.0, 11.0, 12.0 },
        { 20.0, std::nullopt, 22.0 },
        { 30.0, 31.0, 32.0 }
    };
    RasterBlockElevationSampler owning(cells, geometry.originCellCentreLatitudeDeg, geometry.originCellCentreLongitudeDeg,
        geometry.rowStepDeg, geometry.colStepDeg, VerticalDatum::OrthometricMsl);

    auto cellCentre = [&](int row, int col) {
        return GeoPoint{ geometry.originCellCentreLatitudeDeg + row * geometry.rowStepDeg,
                         geometry.originCellCentreLongitudeDeg + col * geometry.colStepDeg };
    };

    bool agreesWithOwning = true;
    for (int row = -1; row <= 3; row++)
    {
        for (int col = -1; col <= 3; col++)
        {
            GeoPoint p = cellCentre(row, col);
            if (view.GetElevation(p.latitudeDeg, p.longitudeDeg) != owning.GetElevation(p.latitudeDeg, p.longitudeDeg)) agreesWithOwning = false;
        }
    }

    GeoPoint northWest = cellCentre(0, 0);
    GeoPoint centre = cellCentre(1, 1);
    auto northWestBefore = view.GetElevation(northWest.latitudeDeg, northWest.longitudeDeg);
    auto centreBefore = view.GetElevation(centre.latitudeDeg, centre.longitudeDeg);

    elevationsM[0] = 99.0;
    validity[4] = 1;
    auto northWestAfter = view.GetElevation(northWest.latitudeDeg, northWest.longitudeDeg);
    auto centreAfter = view.GetElevation(centre.latitudeDeg, centre.longitudeDeg);

    Expect(agreesWithOwning
        && northWestBefore == 10.0 && !centreBefore.has_value()
        && northWestAfter == 99.0 && centreAfter == 21.0
        && view.GetDatum() == VerticalDatum::OrthometricMsl,
        "TestRasterBlockViewReadsHostBufferInPlace");
}

//TEST 44
void TestRealElevationSamplerRejectsMalformedTile()
{
    // A .hgt tile is a square of 16-bit posts, so its size must be twice a perfect
    // square. A download cut short is not: a 2x2 tile missing its last post holds 3
    // posts, which the reader used to round up to 2x2 -- reporting the file loaded
    // and then reading the missing south-east post from past the end of its data.
    // Every malformed size below (empty, a single post, a post short, an odd byte,
    // a post too many) must report IsLoaded() == false and answer queries at every
    // corner with nullopt; a well-formed 2x2 tile written the same way must still load.
    const std::string path = "malformed_test_tile.hgt";
    auto writeTile = [&](size_t byteCount) {
        std::ofstream file(path, std::ios::binary);
        for (size_t i = 0; i < byteCount; i++)
        {
            char byte = (i % 2 == 0) ? 0 : 100; // every complete post reads as 100 m
            file.write(&byte, 1);
        }
    };
    auto answersAnyCorner = [](RealElevationSampler& sampler) {
        return sampler.GetElevation(1.0, 0.0).has_value() || sampler.GetElevation(1.0, 1.0).has_value()
            || sampler.GetElevation(0.0, 0.0).has_value() || sampler.GetElevation(0.0, 1.0).has_value();
    };

    bool malformedRejected = true;
    for (size_t byteCount : { (size_t)0, (size_t)2, (size_t)6, (size_t)7, (size_t)10 })
    {
        writeTile(byteCount);
        RealElevationSampler sampler(path, 0.0, 0.0);
        if (sampler.IsLoaded() || answersAnyCorner(sampler)) malformedRejected = false;
    }

    writeTile(8);
    RealElevationSampler wellFormed(path, 0.0, 0.0);
    bool wellFormedLoads = wellFormed.IsLoaded() && wellFormed.GetElevation(0.0, 1.0) == 100.0;

    std::remove(path.c_str());

    Expect(malformedRejected && wellFormedLoads, "TestRealElevationSamplerRejectsMalformedTile");
}

//TEST 45
void TestViewshedsReturnEmptyForGridWithoutCells()
{
    // A grid with no rows or no columns has no cells. Both viewsheds used to set the
    // observer's cell unconditionally -- writing into an empty result for a 0-cell
    // grid -- and a negative size became an enormous one on its way into
    // std::vector::resize, which throws. Each shape below must come back as an empty
    // result, in both algorithms, with nothing written and nothing thrown; the
    // smallest real grid, 1x1, must still hold the observer's own Visible cell.
    const double spacingDeg = MetersToLatitudeDeg(30.0);
    const GeoPoint observer{ 36.5, -111.5 };
    RasterBlockElevationSampler sampler = MakeViewshedAlignedRaster(MakeFlatCells(7, 10.0), observer, spacingDeg);

    const int shapes[][2] = { { 0, 0 }, { -5, -5 }, { 0, 7 }, { 7, 0 }, { -1, 7 } };
    bool allEmpty = true;
    for (const auto& shape : shapes)
    {
        ViewshedResult naive = ComputeViewshedNaive(observer, Agl(2.0), shape[0], shape[1], spacingDeg, sampler);
        ViewshedResult fast = ComputeViewshedFast(observer, Agl(2.0), shape[0], shape[1], spacingDeg, sampler);
        if (!naive.visible.empty() || !fast.visible.empty()) allEmpty = false;
    }

    ViewshedResult naiveSingle = ComputeViewshedNaive(observer, Agl(2.0), 1, 1, spacingDeg, sampler);
    ViewshedResult fastSingle = ComputeViewshedFast(observer, Agl(2.0), 1, 1, spacingDeg, sampler);
    auto isSingleVisibleCell = [](const ViewshedResult& r) {
        return r.visible.size() == 1 && r.visible[0].size() == 1 && r.visible[0][0] == CellVisibility::Visible;
    };

    Expect(allEmpty && isSingleVisibleCell(naiveSingle) && isSingleVisibleCell(fastSingle),
        "TestViewshedsReturnEmptyForGridWithoutCells");
}

//TEST 46
void TestCliNumberParsingRejectsWhatIsNotANumber()
{
    // The command line used std::stod/std::stoi, which throw on anything that isn't a
    // number; uncaught, that ended the process with no message. The replacements
    // return nullopt instead. A number must be the whole argument and finite: "12abc"
    // is not 12, and "nan", "inf" or an out-of-range value are not usable numbers.
    // Whether a parsed value is also in range (a positive spacing, a positive grid
    // size) is the command's own check, not the parser's, so "-5" parses.
    bool doublesAccepted = ParseFiniteDouble("2.0") == 2.0 && ParseFiniteDouble("-111.45") == -111.45
        && ParseFiniteDouble("1e3") == 1000.0 && ParseFiniteDouble("0.0002694") == 0.0002694;
    bool doublesRejected = !ParseFiniteDouble("abc").has_value() && !ParseFiniteDouble("").has_value()
        && !ParseFiniteDouble(nullptr).has_value() && !ParseFiniteDouble("12abc").has_value()
        && !ParseFiniteDouble("2.0 ").has_value() && !ParseFiniteDouble("nan").has_value()
        && !ParseFiniteDouble("inf").has_value() && !ParseFiniteDouble("1e999").has_value();

    bool intsAccepted = ParseInt("21") == 21 && ParseInt("-5") == -5 && ParseInt("0") == 0;
    bool intsRejected = !ParseInt("2.5").has_value() && !ParseInt("abc").has_value() && !ParseInt("").has_value()
        && !ParseInt(nullptr).has_value() && !ParseInt("21x").has_value() && !ParseInt("99999999999").has_value();

    Expect(doublesAccepted && doublesRejected && intsAccepted && intsRejected,
        "TestCliNumberParsingRejectsWhatIsNotANumber");
}

//TEST 47   
void TestRealElevationSamplerReportsTileFacts()
{
    // A 2x2 tile with one void post, written to a file whose name holds a
    // non-ASCII character, as a Turkish user's folder names often do.
    std::filesystem::path path = std::filesystem::u8path("facts_test_\xC5\x9F.hgt"); // "facts_test_�.hgt"

    // 2x2 tile, row-major north-to-south / west-to-east, big-endian int16:
    // NW=100, NE=void, SW=200, SE=300.
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

    RealElevationSampler loaded(path, 36.0, -112.0);
    bool loadedFactsRight = loaded.IsLoaded()
        && loaded.PostsPerSide() == 2
        && loaded.VoidCount() == 1
        && loaded.SouthWestLatitudeDeg() == 36.0
        && loaded.SouthWestLongitudeDeg() == -112.0;

    RealElevationSampler missing("no_such_tile.hgt", 36.0, -112.0);
    bool failedFactsRight = missing.PostsPerSide() == 0
        && missing.VoidCount() == 0;

    std::filesystem::remove(path);

    Expect(loadedFactsRight && failedFactsRight, "TestRealElevationSamplerReportsTileFacts");
}

//TEST 48
void TestViewshedProgressIsReportedAndCanCancel()
{
    // A long viewshed must be able to say how far it has got and stop when asked,
    // without the reporting changing a single cell. For both algorithms over a 21x21
    // grid with a wall in it: a run that reports must match a run that doesn't, cell
    // for cell; its reported fractions must start at 0, never go backwards and end at
    // 1; and a run whose first report says stop must come back cancelled after exactly
    // that one report.
    const int size = 21;
    const double spacingDeg = MetersToLatitudeDeg(30.0);
    const GeoPoint observer{ 36.5, -111.5 };
    ElevationCells cells = MakeFlatCells(size, 0.0);
    for (int row = 0; row < size; row++)
    {
        cells[row][size / 2 + 3] = 50.0;
    }
    RasterBlockElevationSampler sampler = MakeViewshedAlignedRaster(cells, observer, spacingDeg);

    auto holdsFor = [&](bool naive) {
        auto run = [&](const ViewshedProgress& progress) {
            return naive ? ComputeViewshedNaive(observer, Agl(2.0), size, size, spacingDeg, sampler, 4.0 / 3.0, progress)
                         : ComputeViewshedFast(observer, Agl(2.0), size, size, spacingDeg, sampler, 4.0 / 3.0, progress);
        };

        ViewshedResult silent = run(nullptr);

        std::vector<double> fractions;
        ViewshedResult reported = run([&](double fraction) { fractions.push_back(fraction); return true; });
        bool inOrder = fractions.size() > 2 && fractions.front() == 0.0 && fractions.back() == 1.0;
        for (size_t i = 1; i < fractions.size(); i++)
        {
            if (fractions[i] < fractions[i - 1]) inOrder = false;
        }

        int reportsBeforeStopping = 0;
        ViewshedResult stopped = run([&](double) { reportsBeforeStopping++; return false; });

        return !silent.cancelled && !reported.cancelled && reported.visible == silent.visible && inOrder
            && stopped.cancelled && reportsBeforeStopping == 1;
    };

    Expect(holdsFor(true) && holdsFor(false), "TestViewshedProgressIsReportedAndCanCancel");
}

//TEST 49
void TestRealElevationSamplerWithInterpolationModeMatchesAFreshLoad()
{
    // One loaded tile answering in either interpolation mode without reopening the
    // file: WithInterpolationMode must answer exactly as a sampler opened fresh in that
    // mode, leave the original's mode alone, and Posts() must hold the file's values in
    // file order (row-major, north row first). A 3x3 tile over one degree, so posts
    // sit half a degree apart and (0.75, 0.25) lies midway between four of them:
    // bilinear reads (10 + 20 + 40 + 50) / 4 = 30 there, nearest rounds to the 50 post.
    const std::string path = "interpolation_test_tile.hgt";
    const int16_t posts[9] = { 10, 20, 30, 40, 50, 60, 70, 80, 90 };
    {
        std::ofstream file(path, std::ios::binary);
        for (int16_t value : posts)
        {
            char bytes[2] = { (char)((value >> 8) & 0xFF), (char)(value & 0xFF) };
            file.write(bytes, 2);
        }
    }

    RealElevationSampler nearest(path, 0.0, 0.0, InterpolationMode::Nearest);
    RealElevationSampler freshBilinear(path, 0.0, 0.0, InterpolationMode::Bilinear);
    RealElevationSampler copiedBilinear = nearest.WithInterpolationMode(InterpolationMode::Bilinear);
    std::remove(path.c_str());

    bool postsInFileOrder = nearest.Posts().size() == 9;
    for (size_t i = 0; postsInFileOrder && i < 9; i++)
    {
        postsInFileOrder = nearest.Posts()[i] == posts[i];
    }

    bool agreesWithFreshLoad = true;
    for (double lat : { 0.1, 0.25, 0.75, 0.9 })
    {
        for (double lon : { 0.1, 0.25, 0.6, 0.95 })
        {
            if (copiedBilinear.GetElevation(lat, lon) != freshBilinear.GetElevation(lat, lon)) agreesWithFreshLoad = false;
        }
    }

    Expect(postsInFileOrder && agreesWithFreshLoad
        && copiedBilinear.GetElevation(0.75, 0.25) == 30.0
        && nearest.GetElevation(0.75, 0.25) == 50.0
        && nearest.Interpolation() == InterpolationMode::Nearest
        && copiedBilinear.Interpolation() == InterpolationMode::Bilinear
        && copiedBilinear.PostsPerSide() == 3,
        "TestRealElevationSamplerWithInterpolationModeMatchesAFreshLoad");
}

//TEST 50
void TestSharedPathGeometryMatchesHandCalculation()
{
    // The curvature drop, the sight line and the Fresnel radius are each written once,
    // shared by line of sight, Fresnel clearance, the fast viewshed and anything that
    // charts their results. Pinned here against numbers worked out by hand:
    // - curvature midway along 50 km with k = 4/3: 25000^2 / (2 * 4/3 * 6371000) = 36.7878 m;
    // - the sight line from 0 m to 100 m, 900 m along a 1000 m path: 90 m, and on a path
    //   of no length it stays at the observer's eye;
    // - 2.4 GHz: wavelength 0.124914 m, first Fresnel radius midway along 10 km 17.6716 m.
    Expect(std::abs(CurvatureDropM(25000.0, 25000.0, 4.0 / 3.0) - 36.7878) < 1e-3
        && std::abs(SightLineHeightM(0.0, 100.0, 900.0, 1000.0) - 90.0) < 1e-9
        && SightLineHeightM(12.0, 50.0, 0.0, 0.0) == 12.0
        && std::abs(WavelengthM(2.4e9) - 0.124914) < 1e-6
        && std::abs(FirstFresnelRadiusM(WavelengthM(2.4e9), 5000.0, 5000.0, 10000.0) - 17.6716) < 1e-3,
        "TestSharedPathGeometryMatchesHandCalculation");
}
// The wall scene of test 7: 21x21 cells of 30 m on flat ground at 0 m, with a 50 m wall
// running north-south three cells east of the observer.
inline RasterBlockElevationSampler MakeWallScene(GeoPoint observer, double spacingDeg, int size, int wallCol)
{
    ElevationCells cells = MakeFlatCells(size, 0.0);
    for (int row = 0; row < size; row++)
    {
        cells[row][wallCol] = 50.0;
    }
    return MakeViewshedAlignedRaster(cells, observer, spacingDeg);
}

//TEST 51
void TestViewshedTargetHeightSeesOverTheWallAtTheHandWorkedHeight()
{
    // Straight east of a 2 m observer, the wall stands 90 m out and the cell being asked
    // about 180 m out, all on 0 m ground. The sight line to a target of height h there
    // passes the wall at (2 + h) / 2, so it clears the 50 m wall once h reaches 98 m --
    // plus the fraction of a millimetre the Earth's curvature adds to the wall at 90 m.
    // Both algorithms must hide the cell at 97.9 m and show it at 98.1 m, and at the
    // default target height of 0 m (the ground itself) hide it as they always have.
    const int size = 21;
    const int center = size / 2;
    const double spacingDeg = MetersToLatitudeDeg(30.0);
    const GeoPoint observer{ 36.5, -111.5 };
    RasterBlockElevationSampler scene = MakeWallScene(observer, spacingDeg, size, center + 3);
    const int cellCol = center + 6;

    auto answer = [&](bool fast, double targetHeightM)
    {
        ViewshedResult result = fast
            ? ComputeViewshedFast(observer, Agl(2.0), size, size, spacingDeg, scene, 4.0 / 3.0, nullptr, Agl(targetHeightM))
            : ComputeViewshedNaive(observer, Agl(2.0), size, size, spacingDeg, scene, 4.0 / 3.0, nullptr, Agl(targetHeightM));
        return result.visible[center][cellCol];
    };

    bool naiveRight = answer(false, 0.0) == CellVisibility::NotVisible
        && answer(false, 97.9) == CellVisibility::NotVisible
        && answer(false, 98.1) == CellVisibility::Visible;
    bool fastRight = answer(true, 0.0) == CellVisibility::NotVisible
        && answer(true, 97.9) == CellVisibility::NotVisible
        && answer(true, 98.1) == CellVisibility::Visible;

    Expect(naiveRight && fastRight, "TestViewshedTargetHeightSeesOverTheWallAtTheHandWorkedHeight");
}

//TEST 52
void TestViewshedTargetHeightOnlyEverRevealsAndFastStillMatchesNaive()
{
    // A taller target can see over more terrain, never less, and it never raises the
    // horizon for the cells behind it: every cell visible at a lower target height stays
    // visible at a higher one, and more cells become visible, in both algorithms. An
    // explicit 0 m above ground is the default, cell for cell.
    //
    // Fast against naive, off the wall's column (for the reason test 7 leaves it out):
    // at 0 m and 30 m nothing east of the wall can be seen, so there is no boundary in
    // the open field and the two must agree on every cell. At 120 m a target clears the
    // wall out to about 225 m east, so the visible region ends in the open field -- and
    // there, as docs/ENGINE.md's fast/naive section measures on real terrain, the fast
    // algorithm's horizon comes from rays beside the cell's own line and may answer the
    // other way. Every
    // disagreement must lie on that boundary: a cell with a neighbour naive answers
    // differently. One anywhere else fails the test.
    const int size = 21;
    const int center = size / 2;
    const int wallCol = center + 3;
    const double spacingDeg = MetersToLatitudeDeg(30.0);
    const GeoPoint observer{ 36.5, -111.5 };
    RasterBlockElevationSampler scene = MakeWallScene(observer, spacingDeg, size, wallCol);

    const double heightsM[] = { 0.0, 30.0, 120.0 };
    ViewshedResult naive[3], fast[3];
    for (int i = 0; i < 3; i++)
    {
        naive[i] = ComputeViewshedNaive(observer, Agl(2.0), size, size, spacingDeg, scene, 4.0 / 3.0, nullptr, Agl(heightsM[i]));
        fast[i] = ComputeViewshedFast(observer, Agl(2.0), size, size, spacingDeg, scene, 4.0 / 3.0, nullptr, Agl(heightsM[i]));
    }

    auto onNaiveBoundary = [&](const ViewshedResult& result, int row, int col)
    {
        const int steps[4][2] = { { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 } };
        for (const auto& step : steps)
        {
            int r = row + step[0], c = col + step[1];
            if (r >= 0 && r < size && c >= 0 && c < size && result.visible[r][c] != result.visible[row][col]) return true;
        }
        return false;
    };

    bool onlyReveals = true;
    int mismatchesAtLowTargets = 0;
    int mismatchesOffTheBoundary = 0;
    for (int i = 0; i < 3; i++)
    {
        for (int row = 0; row < size; row++)
        {
            for (int col = 0; col < size; col++)
            {
                if (col != wallCol && naive[i].visible[row][col] != fast[i].visible[row][col])
                {
                    if (i < 2) mismatchesAtLowTargets++;
                    else if (!onNaiveBoundary(naive[i], row, col)) mismatchesOffTheBoundary++;
                }
                if (i == 0) continue;
                for (const ViewshedResult* results : { naive, fast })
                {
                    if (results[i - 1].visible[row][col] == CellVisibility::Visible && results[i].visible[row][col] != CellVisibility::Visible) onlyReveals = false;
                }
            }
        }
    }

    bool moreEachTime = CountCells(naive[1], CellVisibility::Visible) > CountCells(naive[0], CellVisibility::Visible)
        && CountCells(naive[2], CellVisibility::Visible) > CountCells(naive[1], CellVisibility::Visible)
        && CountCells(fast[1], CellVisibility::Visible) > CountCells(fast[0], CellVisibility::Visible)
        && CountCells(fast[2], CellVisibility::Visible) > CountCells(fast[1], CellVisibility::Visible);

    bool zeroIsTheDefault = ComputeViewshedNaive(observer, Agl(2.0), size, size, spacingDeg, scene).visible == naive[0].visible
        && ComputeViewshedFast(observer, Agl(2.0), size, size, spacingDeg, scene).visible == fast[0].visible;

    Expect(onlyReveals && moreEachTime && mismatchesAtLowTargets == 0 && mismatchesOffTheBoundary == 0 && zeroIsTheDefault,
        "TestViewshedTargetHeightOnlyEverRevealsAndFastStillMatchesNaive");
}

//TEST 53
void TestViewshedTargetHeightInAnUnusableDatumLeavesOnlyTheObserverKnown()
{
    // A target height above the ellipsoid, with no geoid undulation to put it on the
    // terrain's mean-sea-level datum, can't be compared with the terrain: every cell but
    // the observer's own is Degraded, the same in both algorithms -- never guessed.
    const int size = 7;
    const int center = size / 2;
    const double spacingDeg = MetersToLatitudeDeg(30.0);
    const GeoPoint observer{ 36.5, -111.5 };
    RasterBlockElevationSampler ground = MakeViewshedAlignedRaster(MakeFlatCells(size, 100.0), observer, spacingDeg);
    DatumHeight unusable{ 10.0, VerticalDatum::EllipsoidalHae };

    ViewshedResult naive = ComputeViewshedNaive(observer, Agl(2.0), size, size, spacingDeg, ground, 4.0 / 3.0, nullptr, unusable);
    ViewshedResult fast = ComputeViewshedFast(observer, Agl(2.0), size, size, spacingDeg, ground, 4.0 / 3.0, nullptr, unusable);

    Expect(CountCells(naive, CellVisibility::Degraded) == size * size - 1
        && naive.visible[center][center] == CellVisibility::Visible
        && fast.visible == naive.visible,
        "TestViewshedTargetHeightInAnUnusableDatumLeavesOnlyTheObserverKnown");
}

// A viewshed drawn as text, one string per row: '#' Visible, '.' NotVisible, '?' Degraded.
inline ViewshedResult ViewshedFromText(const std::vector<std::string>& rows)
{
    ViewshedResult result;
    for (const auto& text : rows)
    {
        std::vector<CellVisibility> row;
        for (char c : text)
        {
            row.push_back(c == '#' ? CellVisibility::Visible : c == '.' ? CellVisibility::NotVisible : CellVisibility::Degraded);
        }
        result.visible.push_back(row);
    }
    return result;
}

// A viewshed that gives every cell `reference` answers with confidence the one answer `state`.
inline ViewshedResult OneStateLike(const ViewshedResult& reference, CellVisibility state)
{
    ViewshedResult result = reference;
    for (auto& row : result.visible)
    {
        for (auto& cell : row)
        {
            if (IsConfident(cell)) cell = state;
        }
    }
    return result;
}

// The tolerances the fast viewshed is held to against the naive one, and why they
// are these numbers: NOTES.md, "A measure that doesn't shrink". Off the edge, the
// 1-arcsecond tile is held to 2% and the 3-arcsecond tile to 3%: read at 30 m, the
// coarser tile's 90 m posts stand as terraces three cells wide, and their cliffs give
// the visible region a more ragged edge.
constexpr double FastViewshedDisagreementTolerance = 0.30;
constexpr double FastViewshedOffEdgeTolerance = 0.02;
constexpr double FastViewshedOffEdgeToleranceCoarseTile = 0.03;
constexpr double FastViewshedMinimumOnEdge = 0.80;

inline bool WithinFastViewshedTolerance(const ViewshedAgreement& agreement, double offEdgeTolerance = FastViewshedOffEdgeTolerance)
{
    return agreement.sameGrid
        && agreement.DisagreementRate() < FastViewshedDisagreementTolerance
        && agreement.OffEdgeRate() < offEdgeTolerance;
}

//TEST 54
void TestViewshedAgreementCountsEachDirectionOverTheVisibleCells()
{
    // Worked by hand. The reference sees a 3x3 block. The approximation draws the
    // block one column east, leaves a hole in its middle, and adds a stray cell in
    // the far corner. Six of the eight differences sit on the reference's edge --
    // the block's west column, and the column just east of it. The hole and the
    // stray have no neighbour that agrees with the approximation: off the edge.
    //   visible in both: 5, reference only: 4, approximation only: 4, in either: 13.
    // The two '?' cells have no confident answer and are left out: 49 - 2 = 47.
    ViewshedResult reference = ViewshedFromText({
        ".......",
        ".......",
        "..###..",
        "..###..",
        "..###..",
        ".......",
        "?......",
    });
    ViewshedResult approximation = ViewshedFromText({
        "#......",
        ".......",
        "...###.",
        "....##.",
        "...###.",
        ".......",
        "......?",
    });

    ViewshedAgreement a = CompareViewsheds(approximation, reference);

    // A neighbour with no confident answer says nothing about where the edge is: the
    // hole in the middle of this visible square stays off the edge, '?' beside it or not.
    ViewshedAgreement besideUnknown = CompareViewsheds(
        ViewshedFromText({ "###", "#.#", "###" }),
        ViewshedFromText({ "###", "###", "##?" }));

    Expect(a.sameGrid && a.comparedCells == 47
        && a.referenceVisible == 9 && a.approximateVisible == 9
        && a.approximateOnlyVisible == 4 && a.referenceOnlyVisible == 4
        && a.differingOnEdge == 6 && a.DifferingOffEdge() == 2
        && a.VisibleInEither() == 13
        && a.DisagreementRate() == 8.0 / 13 && a.OffEdgeRate() == 2.0 / 13 && a.OnEdgeFraction() == 6.0 / 8
        && besideUnknown.comparedCells == 8 && besideUnknown.Differing() == 1 && besideUnknown.DifferingOffEdge() == 1,
        "TestViewshedAgreementCountsEachDirectionOverTheVisibleCells");
}

//TEST 55
void TestViewshedAgreementRejectsAViewshedWithOneAnswerEverywhere()
{
    // Against the same 3x3 block in a 7x7 grid, where 40 of 49 cells are hidden:
    // counted over every cell, "hidden everywhere" would differ on 9 / 49 = 18%.
    // Over the cells either one sees it differs on all of them, and "visible
    // everywhere" differs on 40 / 49, 24 of those more than a cell from any visible
    // cell. Both fall outside the tolerances; the block itself, and the block moved
    // one cell, do not.
    ViewshedResult reference = ViewshedFromText({
        ".......",
        ".......",
        "..###..",
        "..###..",
        "..###..",
        ".......",
        ".......",
    });
    ViewshedAgreement allHidden = CompareViewsheds(OneStateLike(reference, CellVisibility::NotVisible), reference);
    ViewshedAgreement allVisible = CompareViewsheds(OneStateLike(reference, CellVisibility::Visible), reference);
    ViewshedAgreement itself = CompareViewsheds(reference, reference);
    ViewshedAgreement movedOneCell = CompareViewsheds(ViewshedFromText({
        ".......",
        ".......",
        ".......",
        "..###..",
        "..###..",
        "..###..",
        ".......",
    }), reference);

    Expect(allHidden.DisagreementRate() == 1.0 && allHidden.referenceOnlyVisible == 9
        && allVisible.DisagreementRate() == 40.0 / 49 && allVisible.OffEdgeRate() == 24.0 / 49
        && !WithinFastViewshedTolerance(allHidden) && !WithinFastViewshedTolerance(allVisible)
        && itself.Differing() == 0 && itself.OnEdgeFraction() == 1.0 && WithinFastViewshedTolerance(itself)
        && movedOneCell.DifferingOffEdge() == 0 && movedOneCell.OffEdgeRate() == 0.0,
        "TestViewshedAgreementRejectsAViewshedWithOneAnswerEverywhere");
}

//TEST 56
void TestViewshedAgreementRefusesGridsOfDifferentSizes()
{
    // Nothing to compare cell for cell: the answer says so rather than counting a
    // partial overlap. And two viewsheds that see nothing agree completely.
    ViewshedResult threeByThree = ViewshedFromText({ "...", ".#.", "..." });
    ViewshedResult threeByFour = ViewshedFromText({ "....", ".#..", "...." });
    ViewshedResult ragged = ViewshedFromText({ "...", ".#", "..." });
    ViewshedResult dark = ViewshedFromText({ "...", "...", "..." });

    ViewshedAgreement sizes = CompareViewsheds(threeByThree, threeByFour);
    ViewshedAgreement shape = CompareViewsheds(ragged, threeByThree);
    ViewshedAgreement nothingVisible = CompareViewsheds(dark, dark);

    Expect(!sizes.sameGrid && sizes.comparedCells == 0
        && !shape.sameGrid && shape.comparedCells == 0
        && nothingVisible.sameGrid && nothingVisible.comparedCells == 9
        && nothingVisible.DisagreementRate() == 0.0 && nothingVisible.OffEdgeRate() == 0.0,
        "TestViewshedAgreementRefusesGridsOfDifferentSizes");
}

// Fast against naive on a real tile: at each observer and radius, the fast viewshed
// is within tolerance, its differences are measured to lie on naive's visibility
// edge, and a viewshed with one answer everywhere fails the same tolerance. Prints
// the counts behind every rate. Skips when the tile isn't present.
struct RealTerrainViewshedRun
{
    GeoPoint observer;
    double radiusKm;
};

inline void CheckFastViewshedAgainstNaiveOnRealTerrain(const std::string& path, const std::string& tileLabel, double offEdgeTolerance, const std::vector<RealTerrainViewshedRun>& runs)
{
    const std::string testName = "FastViewshedAgreesWithNaive (" + tileLabel + ")";
    RealElevationSampler sampler(path, 36.0, -112.0);
    if (!sampler.IsLoaded())
    {
        Skip(testName, path + " not found from this working directory");
        return;
    }

    // 30 m cells, 2 m above ground, k = 4/3, nearest: the app's defaults.
    const double spacingDeg = MetersToLatitudeDeg(30.0);
    for (const auto& run : runs)
    {
        int grid = (int)(2 * MetersToLatitudeDeg(run.radiusKm * 1000.0) / spacingDeg);
        ViewshedResult naive = ComputeViewshedNaive(run.observer, Agl(2.0), grid, grid, spacingDeg, sampler);
        ViewshedResult fast = ComputeViewshedFast(run.observer, Agl(2.0), grid, grid, spacingDeg, sampler);
        ViewshedAgreement a = CompareViewsheds(fast, naive);

        std::ostringstream where;
        where << "(" << run.observer.latitudeDeg << ", " << run.observer.longitudeDeg << "), " << run.radiusKm << " km, " << tileLabel;
        std::cout << "  " << where.str() << ": " << grid << "x" << grid << ", " << a.comparedCells << " cells compared; naive sees "
            << a.referenceVisible << ", fast " << a.approximateVisible << "; fast only " << a.approximateOnlyVisible
            << ", naive only " << a.referenceOnlyVisible << "; differ on " << (100.0 * a.DisagreementRate()) << "% of "
            << a.VisibleInEither() << " cells either sees, " << (100.0 * a.OffEdgeRate()) << "% off naive's edge ("
            << a.DifferingOffEdge() << " cells); " << (100.0 * a.OnEdgeFraction()) << "% of differences on the edge" << std::endl;

        Expect(WithinFastViewshedTolerance(a, offEdgeTolerance), "FastViewshedWithinTolerance at " + where.str());
        Expect(a.OnEdgeFraction() >= FastViewshedMinimumOnEdge, "FastViewshedDifferencesLieOnNaivesEdge at " + where.str());
        Expect(!WithinFastViewshedTolerance(CompareViewsheds(OneStateLike(naive, CellVisibility::NotVisible), naive), offEdgeTolerance)
            && !WithinFastViewshedTolerance(CompareViewsheds(OneStateLike(naive, CellVisibility::Visible), naive), offEdgeTolerance),
            "OneAnswerEverywhereFailsTheTolerance at " + where.str());
    }
}

//TEST 57
void TestFastViewshedAgreesWithNaiveAtThreeObservers()
{
    // The three observers on the 1-arcsecond tile: little visible, half visible, and
    // in between -- each at 2 km and 5 km.
    CheckFastViewshedAgainstNaiveOnRealTerrain("DATA/SRTM1/N36W112.hgt", "1-arcsecond tile, ~30 m", FastViewshedOffEdgeTolerance, {
        { { 36.5, -111.5 }, 2.0 }, { { 36.5, -111.5 }, 5.0 },
        { { 36.86361, -111.30861 }, 2.0 }, { { 36.86361, -111.30861 }, 5.0 },
        { { 36.55861, -111.81361 }, 2.0 }, { { 36.55861, -111.81361 }, 5.0 },
    });

    // The bundled 3-arcsecond tile, at the observer and radius the suite has always used.
    CheckFastViewshedAgainstNaiveOnRealTerrain("DATA/N36W112.hgt", "3-arcsecond tile, ~90 m", FastViewshedOffEdgeToleranceCoarseTile, {
        { { 36.5, -111.5 }, 2.0 },
    });
}

// Ground at a constant height everywhere on the Earth, and none for a coordinate that
// isn't one -- so a test can place a point anywhere, poles included.
class LevelGroundSampler : public IElevationSampler
{
public:
    explicit LevelGroundSampler(double groundM, double ridgeFromLatDeg = 1e9, double ridgeToLatDeg = 1e9, double ridgeM = 0.0)
        : groundM(groundM), ridgeFromLatDeg(ridgeFromLatDeg), ridgeToLatDeg(ridgeToLatDeg), ridgeM(ridgeM) {}

    std::optional<double> GetElevation(double latitudeDeg, double longitudeDeg) override
    {
        return ElevationOf(Sample(latitudeDeg, longitudeDeg));
    }
    // Everywhere on the Earth is given; a point that isn't on it is data never given.
    ElevationSample Sample(double latitudeDeg, double longitudeDeg) override
    {
        if (CheckPoint({ latitudeDeg, longitudeDeg }) != InputProblem::None) return ElevationSample{ ElevationData::NotGiven, 0.0 };
        return ElevationSample{ ElevationData::Present, latitudeDeg >= ridgeFromLatDeg && latitudeDeg <= ridgeToLatDeg ? ridgeM : groundM };
    }
    VerticalDatum GetDatum() const override { return VerticalDatum::OrthometricMsl; }

private:
    double groundM, ridgeFromLatDeg, ridgeToLatDeg, ridgeM;
};

//TEST 58
void TestTheLibraryRefusesASpacingItCannotSampleAt()
{
    // A 20 km path due north over level ground at 100 m, with a 400 m ridge 100 m deep
    // across it halfway: at 30 m spacing the ridge blocks the view. Every other spacing
    // below once gave a confident "visible" -- the two endpoints alone, nothing between.
    // Now the path isn't sampled, the profile comes back empty with the reason, and no
    // line of sight, Fresnel clearance or batch answer built on it is Ok.
    const GeoPoint start{ 36.5, -111.5 };
    const GeoPoint end{ 36.5 + MetersToLatitudeDeg(20000.0), -111.5 };
    const double ridgeFrom = 36.5 + MetersToLatitudeDeg(9950.0);
    const double ridgeTo = 36.5 + MetersToLatitudeDeg(10050.0);
    LevelGroundSampler ground(100.0, ridgeFrom, ridgeTo, 400.0);

    std::vector<ProfileSample> buffer;
    bool sampledAt30m = GetTerrainProfile(start, end, MetersToLatitudeDeg(30.0), ground, buffer) == InputProblem::None;
    LineOfSightResult at30m = ComputeLineOfSight(buffer, Agl(2.0), Agl(2.0));
    bool blockedAt30m = sampledAt30m && buffer.size() == 668 && IsOk(at30m.status) && !at30m.isVisible;

    struct Case { double spacingDeg; InputProblem expected; };
    const Case cases[] = {
        { 0.0, InputProblem::SpacingNotPositive },
        { MetersToLatitudeDeg(-30.0), InputProblem::SpacingNotPositive },
        { std::nan(""), InputProblem::SpacingNotPositive },
        { INFINITY, InputProblem::SpacingNotPositive },
        { MetersToLatitudeDeg(1e-6), InputProblem::SpacingTooFine }, // 2 x 10^10 intervals, past INT_MAX
    };
    bool allRefused = true;
    for (const Case& c : cases)
    {
        GetTerrainProfile(start, end, MetersToLatitudeDeg(30.0), ground, buffer); // a full buffer, to see it emptied
        InputProblem inPlace = GetTerrainProfile(start, end, c.spacingDeg, ground, buffer);
        std::vector<ProfileSample> byValue = GetTerrainProfile(start, end, c.spacingDeg, ground);
        LineOfSightResult los = ComputeLineOfSight(byValue, Agl(2.0), Agl(2.0));
        FresnelClearanceResult fresnel = ComputeFresnelClearance(byValue, Agl(2.0), Agl(2.0), 2.4e9);
        std::vector<LineOfSightResult> batch = ComputeBatchLineOfSight({ { start, Agl(2.0), end, Agl(2.0) } }, c.spacingDeg, ground);

        allRefused = allRefused
            && CheckProfileRequest(start, end, c.spacingDeg) == c.expected && inPlace == c.expected
            && buffer.empty() && byValue.empty()
            && los.status == ComputationStatus::EmptyOrSingleSampleProfile
            && fresnel.status == ComputationStatus::EmptyOrSingleSampleProfile
            && batch.size() == 1 && batch[0].status == ComputationStatus::InvalidInput && batch[0].inputProblem == c.expected;
    }

    Expect(blockedAt30m && allRefused, "TestTheLibraryRefusesASpacingItCannotSampleAt");
}

//TEST 59
void TestTheLibraryRefusesCoordinatesThatAreNotOnTheEarth()
{
    // A latitude or longitude that isn't a finite number, or a latitude past a pole, is
    // refused with the reason by every entry point that takes one. The samplers answer
    // such a coordinate -- or a finite one wildly off their data -- with no elevation,
    // never by casting it to an int, which would be undefined.
    const double nan = std::nan("");
    const double spacingDeg = MetersToLatitudeDeg(30.0);
    const GeoPoint good{ 36.5, -111.5 };
    const GeoPoint nearby{ 36.51, -111.5 };
    LevelGroundSampler ground(100.0);

    bool pointsChecked = CheckPoint({ nan, -111.5 }) == InputProblem::CoordinateNotFinite
        && CheckPoint({ 36.5, INFINITY }) == InputProblem::CoordinateNotFinite
        && CheckPoint({ 91.0, -111.5 }) == InputProblem::LatitudeOutOfRange
        && CheckPoint({ -90.0, 0.0 }) == InputProblem::None
        && CheckPoint({ 36.5, 540.0 }) == InputProblem::None; // any finite longitude names a meridian

    std::vector<ProfileSample> buffer;
    bool profilesRefused = GetTerrainProfile(good, { nan, -111.5 }, spacingDeg, ground, buffer) == InputProblem::CoordinateNotFinite
        && buffer.empty()
        && GetTerrainProfile({ 91.0, -111.5 }, good, spacingDeg, ground, buffer) == InputProblem::LatitudeOutOfRange;

    // One bad query in a batch is refused on its own; the others are still answered.
    std::vector<LineOfSightResult> batch = ComputeBatchLineOfSight({
        { good, Agl(2.0), nearby, Agl(2.0) },
        { { nan, nan }, Agl(2.0), nearby, Agl(2.0) },
    }, spacingDeg, ground);
    bool batchAnswersTheRest = batch.size() == 2 && IsOk(batch[0].status)
        && batch[1].status == ComputationStatus::InvalidInput && batch[1].inputProblem == InputProblem::CoordinateNotFinite;

    ViewshedResult naive = ComputeViewshedNaive({ nan, -111.5 }, Agl(2.0), 5, 5, spacingDeg, ground);
    ViewshedResult fast = ComputeViewshedFast({ 36.5, -INFINITY }, Agl(2.0), 5, 5, spacingDeg, ground);
    bool viewshedsRefused = naive.visible.empty() && naive.inputProblem == InputProblem::CoordinateNotFinite
        && fast.visible.empty() && fast.inputProblem == InputProblem::CoordinateNotFinite;

    // Every sampler, at a coordinate that is no number, an infinite one, and a finite one far off its data.
    const std::string path = "coordinate_test_tile.hgt";
    {
        std::ofstream file(path, std::ios::binary);
        const unsigned char posts[8] = { 0, 100, 0, 100, 0, 100, 0, 100 }; // 2x2, 100 m each
        file.write((const char*)posts, sizeof posts);
    }
    RealElevationSampler realNearest(path, 36.0, -112.0);
    RealElevationSampler realBilinear(path, 36.0, -112.0, InterpolationMode::Bilinear);
    std::remove(path.c_str());
    FakeElevationSampler fake({ { 1, 2 }, { 3, 4 } }, InterpolationMode::Nearest, VerticalDatum::OrthometricMsl);
    FakeElevationSampler fakeBilinear({ { 1, 2 }, { 3, 4 } }, InterpolationMode::Bilinear, VerticalDatum::OrthometricMsl);
    FakeElevationSampler empty({}, InterpolationMode::Nearest, VerticalDatum::OrthometricMsl);
    RasterBlockElevationSampler raster = MakeViewshedAlignedRaster(MakeFlatCells(3, 10.0), good, spacingDeg);
    MultiTileElevationSampler tiles;
    tiles.AddTile(36.0, -112.0, realNearest);

    IElevationSampler* samplers[] = { &realNearest, &realBilinear, &fake, &fakeBilinear, &empty, &raster, &tiles };
    const double badCoordinates[] = { nan, INFINITY, -INFINITY, 1e300, -1e300 };
    bool samplersSafe = realNearest.IsLoaded() && realNearest.GetElevation(36.5, -111.5) == 100.0 && empty.GetElevation(0, 0) == std::nullopt;
    for (IElevationSampler* sampler : samplers)
    {
        for (double bad : badCoordinates)
        {
            samplersSafe = samplersSafe
                && !sampler->GetElevation(bad, -111.5).has_value()
                && !sampler->GetElevation(36.5, bad).has_value();
        }
    }

    Expect(pointsChecked && profilesRefused && batchAnswersTheRest && viewshedsRefused && samplersSafe,
        "TestTheLibraryRefusesCoordinatesThatAreNotOnTheEarth");
}

//TEST 60
void TestTheLibraryRefusesHeightsThatAreNotNumbers()
{
    // A height, or its geoid undulation, that isn't a finite number is refused as that --
    // not taken for a datum problem, and never compared: NaN makes every comparison
    // false, which once read as "nothing blocks the view".
    const double nan = std::nan("");
    const double spacingDeg = MetersToLatitudeDeg(30.0);
    const GeoPoint a{ 36.5, -111.5 };
    const GeoPoint b{ 36.51, -111.5 };
    LevelGroundSampler ground(100.0);
    std::vector<ProfileSample> profile = GetTerrainProfile(a, b, spacingDeg, ground);

    const DatumHeight badHeights[] = {
        Agl(nan),
        Agl(INFINITY),
        DatumHeight{ 150.0, VerticalDatum::EllipsoidalHae, nan },  // an undulation that is no number
        DatumHeight{ -INFINITY, VerticalDatum::OrthometricMsl },
    };
    bool refused = true;
    for (const DatumHeight& bad : badHeights)
    {
        LineOfSightResult asObserver = ComputeLineOfSight(profile, bad, Agl(2.0));
        LineOfSightResult asTarget = ComputeLineOfSight(profile, Agl(2.0), bad);
        FresnelClearanceResult fresnel = ComputeFresnelClearance(profile, bad, Agl(2.0), 2.4e9);
        ViewshedResult naive = ComputeViewshedNaive(a, bad, 5, 5, spacingDeg, ground);
        ViewshedResult fast = ComputeViewshedFast(a, Agl(2.0), 5, 5, spacingDeg, ground, 4.0 / 3.0, nullptr, bad);
        refused = refused
            && asObserver.status == ComputationStatus::InvalidInput && asObserver.inputProblem == InputProblem::HeightNotFinite
            && asTarget.status == ComputationStatus::InvalidInput && asTarget.inputProblem == InputProblem::HeightNotFinite
            && fresnel.status == ComputationStatus::InvalidInput && fresnel.inputProblem == InputProblem::HeightNotFinite
            && naive.visible.empty() && naive.inputProblem == InputProblem::HeightNotFinite
            && fast.visible.empty() && fast.inputProblem == InputProblem::HeightNotFinite;
    }

    // The datum helpers underneath convert such a value to nothing, not to NaN.
    bool helpersRefuse = !ConvertHeightBetweenDatums(nan, VerticalDatum::EllipsoidalHae, VerticalDatum::OrthometricMsl, 30.0).has_value()
        && !ConvertHeightBetweenDatums(150.0, VerticalDatum::EllipsoidalHae, VerticalDatum::OrthometricMsl, nan).has_value()
        && !EyeHeightInTerrainDatum(Agl(nan), 100.0, VerticalDatum::OrthometricMsl).has_value()
        && EyeHeightInTerrainDatum(Agl(2.0), 100.0, VerticalDatum::OrthometricMsl) == 102.0;

    Expect(refused && helpersRefuse && IsOk(ComputeLineOfSight(profile, Agl(2.0), Agl(2.0)).status),
        "TestTheLibraryRefusesHeightsThatAreNotNumbers");
}

//TEST 61
void TestTheLibraryRefusesACurvatureFactorOrFrequencyItCannotUse()
{
    // k divides the Earth's radius: zero divides by zero, a negative k bends the Earth
    // the wrong way, and NaN turns every comparison false. A frequency of zero or less
    // has no wavelength. Each is refused by name; a huge k -- curvature switched off, as
    // the suite's own curvature tests do -- is still a k.
    const double nan = std::nan("");
    const double spacingDeg = MetersToLatitudeDeg(30.0);
    const GeoPoint a{ 36.5, -111.5 };
    const GeoPoint b{ 36.51, -111.5 };
    LevelGroundSampler ground(100.0);
    std::vector<ProfileSample> profile = GetTerrainProfile(a, b, spacingDeg, ground);

    bool kRefused = true;
    const double unusableK[] = { 0.0, -4.0 / 3.0, nan, (double)INFINITY };
    for (double k : unusableK)
    {
        LineOfSightResult los = ComputeLineOfSight(profile, Agl(2.0), Agl(2.0), k);
        FresnelClearanceResult fresnel = ComputeFresnelClearance(profile, Agl(2.0), Agl(2.0), 2.4e9, k);
        std::vector<LineOfSightResult> batch = ComputeBatchLineOfSight({ { a, Agl(2.0), b, Agl(2.0) } }, spacingDeg, ground, k);
        ViewshedResult naive = ComputeViewshedNaive(a, Agl(2.0), 5, 5, spacingDeg, ground, k);
        ViewshedResult fast = ComputeViewshedFast(a, Agl(2.0), 5, 5, spacingDeg, ground, k);
        kRefused = kRefused
            && los.status == ComputationStatus::InvalidInput && los.inputProblem == InputProblem::CurvatureFactorNotPositive
            && fresnel.inputProblem == InputProblem::CurvatureFactorNotPositive
            && batch[0].inputProblem == InputProblem::CurvatureFactorNotPositive
            && naive.inputProblem == InputProblem::CurvatureFactorNotPositive && naive.visible.empty()
            && fast.inputProblem == InputProblem::CurvatureFactorNotPositive && fast.visible.empty();
    }

    bool frequencyRefused = true;
    const double unusableFrequency[] = { 0.0, -2.4e9, nan, (double)INFINITY };
    for (double hz : unusableFrequency)
    {
        FresnelClearanceResult fresnel = ComputeFresnelClearance(profile, Agl(2.0), Agl(2.0), hz);
        frequencyRefused = frequencyRefused
            && fresnel.status == ComputationStatus::InvalidInput && fresnel.inputProblem == InputProblem::FrequencyNotPositive;
    }

    Expect(kRefused && frequencyRefused
        && IsOk(ComputeLineOfSight(profile, Agl(2.0), Agl(2.0), 1e12).status)
        && ComputeViewshedFast(a, Agl(2.0), 5, 5, spacingDeg, ground, 1e12).inputProblem == InputProblem::None,
        "TestTheLibraryRefusesACurvatureFactorOrFrequencyItCannotUse");
}

//TEST 62
void TestAViewshedGridThatWouldReachAPoleIsRefused()
{
    // A viewshed lays out its columns along each row's latitude, dividing by its cosine:
    // at a pole there is no longitude to lay them along. A grid is refused when any row
    // would reach a pole, and laid out when every row stays short of one -- however close.
    const double spacingDeg = MetersToLatitudeDeg(30.0);
    const int thirtyKm = (int)(2 * MetersToLatitudeDeg(30000.0) / spacingDeg);   // 2000 cells, 0.27 degrees each way
    LevelGroundSampler ground(0.0);

    auto refusedAt = [&](GeoPoint observer, int grid) {
        ViewshedResult naive = ComputeViewshedNaive(observer, Agl(2.0), grid, grid, spacingDeg, ground);
        ViewshedResult fast = ComputeViewshedFast(observer, Agl(2.0), grid, grid, spacingDeg, ground);
        return naive.inputProblem == InputProblem::GridBeyondPole && naive.visible.empty()
            && fast.inputProblem == InputProblem::GridBeyondPole && fast.visible.empty();
    };

    bool refused = refusedAt({ 90.0, 0.0 }, 1)          // at the pole itself, even a single cell
        && refusedAt({ 89.8, 10.0 }, thirtyKm)            // 0.2 degrees short, rows reaching 0.27 degrees north
        && refusedAt({ -89.8, 10.0 }, thirtyKm);          // and the same in the south

    ViewshedResult near = ComputeViewshedFast({ 89.5, 10.0 }, Agl(2.0), 67, 67, spacingDeg, ground); // 1 km: rows stay short of it
    bool laidOutNearIt = near.inputProblem == InputProblem::None && near.visible.size() == 67 && CountCells(near, CellVisibility::Visible) == 67 * 67;

    Expect(refused && laidOutNearIt
        && ComputeViewshedNaive({ 91.0, 0.0 }, Agl(2.0), 5, 5, spacingDeg, ground).inputProblem == InputProblem::LatitudeOutOfRange
        && ComputeViewshedFast({ 36.5, -111.5 }, Agl(2.0), 5, 5, 0.0, ground).inputProblem == InputProblem::SpacingNotPositive
        && ComputeViewshedFast({ 36.5, -111.5 }, Agl(2.0), 0, 5, spacingDeg, ground).inputProblem == InputProblem::None,
        "TestAViewshedGridThatWouldReachAPoleIsRefused");
}

//TEST 63
void TestRealElevationSamplerBilinearMatchesAHandWorkedValue()
{
    // The .hgt reader's own bilinear interpolation, against a value worked by hand. Test
    // 49 reads a point in the middle of four posts, where the row and column fractions
    // are both 1/2 and swapping them changes nothing -- so a reader that swapped them
    // passed. This point is off the middle.
    //
    // A 3x3 tile over one degree: posts half a degree apart, row 0 the northern edge.
    // The north-west square's corners are 100 (north-west), 140 (north-east), 180
    // (south-west) and 260 (south-east). At (36.625, -111.875) the point is 3/4 of the
    // way south across the square and 1/4 of the way east:
    //   north edge   100 + 1/4 * (140 - 100) = 110
    //   south edge   180 + 1/4 * (260 - 180) = 200
    //   between them 110 + 3/4 * (200 - 110) = 177.5
    // With the fractions swapped it would read 157.5; nearest rounds to the 180 post.
    const std::string path = "bilinear_known_value_tile.hgt";
    const int16_t posts[9] = { 100, 140, 0, 180, 260, 0, 0, 0, 0 };
    {
        std::ofstream file(path, std::ios::binary);
        for (int16_t value : posts)
        {
            unsigned char bytes[2] = { (unsigned char)((value >> 8) & 0xFF), (unsigned char)(value & 0xFF) };
            file.write((const char*)bytes, 2);
        }
    }
    RealElevationSampler bilinear(path, 36.0, -112.0, InterpolationMode::Bilinear);
    RealElevationSampler nearest(path, 36.0, -112.0, InterpolationMode::Nearest);
    std::remove(path.c_str());

    Expect(bilinear.IsLoaded() && bilinear.PostsPerSide() == 3
        && bilinear.GetElevation(36.625, -111.875) == 177.5
        && nearest.GetElevation(36.625, -111.875) == 180.0
        && bilinear.GetElevation(36.5, -112.0) == 180.0,  // on a post, bilinear reads the post itself
        "TestRealElevationSamplerBilinearMatchesAHandWorkedValue");
}

// Two minimum-visible-height grids with the same answer in every cell: the same state,
// and the same height and ground to the bit, NaN matching NaN where a cell has none.
inline bool SameMinimumVisibleHeights(const MinimumVisibleHeightResult& a, const MinimumVisibleHeightResult& b)
{
    auto same = [](const std::vector<std::vector<double>>& x, const std::vector<std::vector<double>>& y) {
        if (x.size() != y.size()) return false;
        for (size_t row = 0; row < x.size(); row++)
        {
            if (x[row].size() != y[row].size()) return false;
            for (size_t col = 0; col < x[row].size(); col++)
            {
                double p = x[row][col], q = y[row][col];
                if (!(p == q || (std::isnan(p) && std::isnan(q)))) return false;
            }
        }
        return true;
    };
    return a.state == b.state && same(a.heightAboveGroundM, b.heightAboveGroundM) && same(a.groundM, b.groundM)
        && a.terrainDatum == b.terrainDatum && a.inputProblem == b.inputProblem && a.cancelled == b.cancelled;
}

// Every finite height in the grid other than 0, once each, in order.
inline std::vector<double> DistinctMinimumVisibleHeights(const MinimumVisibleHeightResult& result)
{
    std::vector<double> heights;
    for (const auto& row : result.heightAboveGroundM)
    {
        for (double heightM : row)
        {
            if (std::isfinite(heightM) && heightM > 0.0) heights.push_back(heightM);
        }
    }
    std::sort(heights.begin(), heights.end());
    heights.erase(std::unique(heights.begin(), heights.end()), heights.end());
    return heights;
}

//TEST 64
void TestMinimumVisibleHeightBehindTheWallIsTheHandWorkedHeight()
{
    // The wall scene of test 51: a 2 m observer, 0 m ground, a 50 m wall 90 m east. A
    // target D metres east is seen once the sight line to it clears the wall's top, raised
    // by the Earth's curvature d (D - d) / 2kR at d = 90 m. The line to a target of eye
    // height T passes the wall at 2 + (T - 2) * 90 / D, so the lowest T is
    //     T = 2 + (50 + 90 (D - 90) / 2kR - 2) * D / 90
    // 120 m east, just behind the wall: 2 + 48 * 4/3 = 66 m, plus 3600 / 2kR -- 66.000212 m.
    // 180 m east, the cell test 51 asks about: 2 + 48 * 2 = 98 m, plus 16200 / 2kR -- 98.000954 m.
    // In front of the wall (60 m) and on it (90 m) the ground itself is seen: 0.
    // Both versions give these; the fast one keeps its horizon as floats, so to 10^-5 m.
    // Each cell carries its own ground, the wall's top on the wall -- not the observer's.
    const int size = 21;
    const int center = size / 2;
    const double spacingDeg = MetersToLatitudeDeg(30.0);
    const GeoPoint observer{ 36.5, -111.5 };
    RasterBlockElevationSampler scene = MakeWallScene(observer, spacingDeg, size, center + 3);

    const double twoKR = 2 * (4.0 / 3.0) * EarthRadiusM;
    const double behindWallM = 66.0 + 3600.0 / twoKR;
    const double twiceAsFarM = 98.0 + 16200.0 / twoKR;

    bool right = true;
    for (bool fast : { false, true })
    {
        MinimumVisibleHeightResult result = fast
            ? ComputeMinimumVisibleHeightFast(observer, Agl(2.0), size, size, spacingDeg, scene)
            : ComputeMinimumVisibleHeightReference(observer, Agl(2.0), size, size, spacingDeg, scene);
        const auto& east = result.heightAboveGroundM[center];
        double tolerance = fast ? 1e-5 : 1e-9;
        right = right && east[center + 2] == 0.0 && east[center + 3] == 0.0
            && std::abs(east[center + 4] - behindWallM) < tolerance
            && std::abs(east[center + 6] - twiceAsFarM) < tolerance
            && result.state[center][center + 2] == CellVisibility::Visible
            && result.state[center][center + 6] == CellVisibility::NotVisible
            && result.groundM[center][center + 3] == 50.0 && result.groundM[center][center + 6] == 0.0
            && MinimumVisibleAbsoluteHeightM(result, center, center + 3) == 50.0;
    }

    Expect(right, "TestMinimumVisibleHeightBehindTheWallIsTheHandWorkedHeight");
}

//TEST 65
void TestMinimumVisibleHeightOnASmoothSphereMatchesTheClosedForm()
{
    // Level ground at 0 m everywhere: only the Earth's curvature hides anything. An eye h
    // above the ground sees it out to the horizon d_h = sqrt(2kR h); a target D beyond that
    // must stand (D - d_h)^2 / 2kR above the ground to be seen, and inside it, 0. Derived in
    // docs/ENGINE.md, "Minimum visible height".
    //
    // The engine finds the horizon among its samples, not at d_h itself: the sample
    // nearest d_h is at most half a spacing s from it, and there s(d) is lower than its
    // peak by at most 1/2 |s''| (s/2)^2, where s''(d) = -2h / d^3. The answer is D times
    // that. Along a single path at 30 m, from a 10 m eye (d_h = 13.0 km) out to 50 km:
    // 50000 * 1/2 * 9.0e-12 * 15^2 = 5.1e-5 m -- held to 10^-4 m.
    //
    // Over a 101 x 101 grid of 300 m cells, both versions, every cell: a target beyond
    // the horizon can have its nearest counted sample up to one spacing away (the target's
    // own isn't counted), and the fast version, which leaves out the last half cell, one
    // and a half: 21.2 km * 1/2 * 9.0e-12 * 450^2 = 0.019 m for the farthest cell -- held
    // to 0.03 m. Inside the horizon every answer must be exactly 0.
    const double twoKR = 2 * (4.0 / 3.0) * EarthRadiusM;
    const GeoPoint observer{ 36.5, -111.5 };
    LevelGroundSampler level(0.0);
    auto closedForm = [&](double eyeM, double dM) {
        double horizonM = std::sqrt(twoKR * eyeM);
        return dM <= horizonM ? 0.0 : (dM - horizonM) * (dM - horizonM) / twoKR;
    };

    bool pathsRight = true;
    struct Range { double eyeM, dM; };
    for (Range range : { Range{ 10, 5000 }, Range{ 10, 12000 }, Range{ 10, 13500 }, Range{ 10, 15000 }, Range{ 10, 20000 },
                         Range{ 10, 30000 }, Range{ 10, 50000 }, Range{ 100, 30000 }, Range{ 100, 45000 }, Range{ 100, 50000 } })
    {
        GeoPoint target{ observer.latitudeDeg + MetersToLatitudeDeg(range.dM), observer.longitudeDeg };
        std::vector<ProfileSample> profile = GetTerrainProfile(observer, target, MetersToLatitudeDeg(30.0), level);
        MinimumVisibleHeightAnswer answer = MinimumVisibleHeightAlongProfile(profile, Agl(range.eyeM));
        double expectedM = closedForm(range.eyeM, profile.back().distanceFromStartM);
        pathsRight = pathsRight && IsOk(answer.status) && answer.groundM == 0.0
            && (expectedM == 0.0 ? answer.heightAboveGroundM == 0.0 : std::abs(answer.heightAboveGroundM - expectedM) < 1e-4);
    }

    const int size = 101;
    const int center = size / 2;
    const double spacingDeg = MetersToLatitudeDeg(300.0);
    const double lonSpacingDeg = LongitudeSpacingForLatitude(spacingDeg, observer.latitudeDeg);
    bool gridsRight = true;
    int beyondTheHorizon = 0;
    for (bool fast : { false, true })
    {
        MinimumVisibleHeightResult result = fast
            ? ComputeMinimumVisibleHeightFast(observer, Agl(10.0), size, size, spacingDeg, level)
            : ComputeMinimumVisibleHeightReference(observer, Agl(10.0), size, size, spacingDeg, level);
        for (int row = 0; row < size; row++)
        {
            for (int col = 0; col < size; col++)
            {
                GeoPoint centre{ observer.latitudeDeg + (row - center) * spacingDeg, observer.longitudeDeg + (col - center) * lonSpacingDeg };
                double expectedM = closedForm(10.0, GreatCircleDistanceM(observer, centre));
                double gotM = result.heightAboveGroundM[row][col];
                if (expectedM == 0.0) gridsRight = gridsRight && gotM == 0.0;
                else
                {
                    gridsRight = gridsRight && std::abs(gotM - expectedM) < 0.03;
                    beyondTheHorizon++;
                }
            }
        }
    }

    Expect(pathsRight && gridsRight && beyondTheHorizon > 1000, "TestMinimumVisibleHeightOnASmoothSphereMatchesTheClosedForm");
}

//TEST 66
void TestMinimumVisibleHeightThresholdedIsTheViewshedAtThatHeight()
{
    // Asking the reference "which cells does a target H above the ground see?" must give
    // exactly the cells ComputeViewshedNaive gives for a target of height H -- and the fast
    // version exactly ComputeViewshedFast's. Asked at every height the grid holds and at
    // the double just below each, where a rounding in either direction would show, as
    // well as at round heights.
    //
    // The wall scene with a void beyond the wall, on a 25 x 25 grid over 21 x 21 cells of
    // data: none of the 184-cell outer ring has an answer -- data not given, or the void where
    // a line crosses it -- and the cells behind the void have none either. Then the
    // 1-arcsecond tile at the observer with the most uneven view, over a 1 km radius.
    const double spacingDeg = MetersToLatitudeDeg(30.0);
    const GeoPoint observer{ 36.5, -111.5 };
    ElevationCells cells = MakeFlatCells(21, 0.0);
    for (auto& row : cells) row[13] = 50.0;
    cells[4][17] = std::nullopt;
    RasterBlockElevationSampler scene = MakeViewshedAlignedRaster(cells, observer, spacingDeg);

    auto agreesAtEveryHeight = [&](GeoPoint from, int size, IElevationSampler& sampler, size_t everyNth, bool fast) {
        MinimumVisibleHeightResult result = fast
            ? ComputeMinimumVisibleHeightFast(from, Agl(2.0), size, size, spacingDeg, sampler)
            : ComputeMinimumVisibleHeightReference(from, Agl(2.0), size, size, spacingDeg, sampler);
        std::vector<double> heights = { 0.0, 2.0, 10.0, 30.0, 97.9, 98.1, 120.0, 1000.0 };
        std::vector<double> held = DistinctMinimumVisibleHeights(result);
        for (size_t i = 0; i < held.size(); i += everyNth)
        {
            heights.push_back(held[i]);
            heights.push_back(std::nextafter(held[i], 0.0));
        }
        for (double heightM : heights)
        {
            ViewshedResult direct = fast
                ? ComputeViewshedFast(from, Agl(2.0), size, size, spacingDeg, sampler, 4.0 / 3.0, nullptr, Agl(heightM))
                : ComputeViewshedNaive(from, Agl(2.0), size, size, spacingDeg, sampler, 4.0 / 3.0, nullptr, Agl(heightM));
            if (ViewshedAtTargetHeight(result, heightM).visible != direct.visible) return false;
        }
        return held.size() > 20;
    };

    MinimumVisibleHeightResult sceneResult = ComputeMinimumVisibleHeightReference(observer, Agl(2.0), 25, 25, spacingDeg, scene);
    ViewshedResult sceneAtGround = ViewshedAtTargetHeight(sceneResult, 0.0);
    int notGiven = CountCells(sceneAtGround, CellVisibility::DataNotGiven), voids = CountCells(sceneAtGround, CellVisibility::Degraded);
    bool sceneHasNoAnswerCells = notGiven > 0 && voids > 0 && notGiven + voids > 25 * 25 - 21 * 21;
    bool sceneAgrees = agreesAtEveryHeight(observer, 25, scene, 1, false) && agreesAtEveryHeight(observer, 25, scene, 1, true);

    RealElevationSampler tile("DATA/SRTM1/N36W112.hgt", 36.0, -112.0);
    if (!tile.IsLoaded())
    {
        Skip("TestMinimumVisibleHeightThresholdedIsTheViewshedAtThatHeight (1-arcsecond part)", "DATA/SRTM1/N36W112.hgt not found from this working directory");
        Expect(sceneHasNoAnswerCells && sceneAgrees, "TestMinimumVisibleHeightThresholdedIsTheViewshedAtThatHeight");
        return;
    }
    const GeoPoint uneven{ 36.55861, -111.81361 };
    int size = (int)(2 * MetersToLatitudeDeg(1000.0) / spacingDeg);
    bool tileAgrees = agreesAtEveryHeight(uneven, size, tile, 200, false) && agreesAtEveryHeight(uneven, size, tile, 200, true);

    Expect(sceneHasNoAnswerCells && sceneAgrees && tileAgrees, "TestMinimumVisibleHeightThresholdedIsTheViewshedAtThatHeight");
}

//TEST 67
void TestMinimumVisibleHeightKeepsTheViewshedsNoAnswerStatesAndRefusals()
{
    // Where the viewshed has no confident answer, neither has this, for the same reason:
    // an observer standing on a void leaves every cell Degraded, as in both viewsheds, with
    // no height and no ground. An observer whose eye is below the ground under it sees
    // nothing at any height: every other cell needs an infinite height -- and a target
    // 1 km up is still hidden, as naive says. The absolute answer is the ground the height
    // was computed on plus the height. The inputs the viewsheds refuse are refused, with the
    // same reason; a target height below the ground or not a number is refused by
    // ViewshedAtTargetHeight. And progress is reported from 0 to 1 without changing a cell,
    // and a stop request honoured.
    const int size = 7;
    const int center = size / 2;
    const double spacingDeg = MetersToLatitudeDeg(30.0);
    const GeoPoint observer{ 36.5, -111.5 };

    ElevationCells holed = MakeFlatCells(size, 100.0);
    holed[center][center] = std::nullopt;
    RasterBlockElevationSampler onVoid = MakeViewshedAlignedRaster(holed, observer, spacingDeg);
    RasterBlockElevationSampler flat = MakeViewshedAlignedRaster(MakeFlatCells(size, 100.0), observer, spacingDeg);
    LevelGroundSampler level(0.0);
    const DatumHeight belowGround{ 50.0, VerticalDatum::OrthometricMsl };

    bool right = true;
    for (bool fast : { false, true })
    {
        auto compute = [&](GeoPoint from, DatumHeight height, int rows, double spacing, IElevationSampler& sampler, double k, const ViewshedProgress& progress) {
            return fast ? ComputeMinimumVisibleHeightFast(from, height, rows, rows, spacing, sampler, k, progress)
                        : ComputeMinimumVisibleHeightReference(from, height, rows, rows, spacing, sampler, k, progress);
        };
        auto viewshed = [&](GeoPoint from, DatumHeight height, IElevationSampler& sampler) {
            return fast ? ComputeViewshedFast(from, height, size, size, spacingDeg, sampler)
                        : ComputeViewshedNaive(from, height, size, size, spacingDeg, sampler);
        };

        MinimumVisibleHeightResult voidResult = compute(observer, Agl(2.0), size, spacingDeg, onVoid, 4.0 / 3.0, nullptr);
        bool noHeights = true;
        for (int row = 0; row < size; row++)
        {
            for (int col = 0; col < size; col++)
            {
                noHeights = noHeights && std::isnan(voidResult.heightAboveGroundM[row][col]) && std::isnan(voidResult.groundM[row][col]);
            }
        }
        right = right && voidResult.state == viewshed(observer, Agl(2.0), onVoid).visible && noHeights
            && CountCells(ViewshedAtTargetHeight(voidResult, 0.0), CellVisibility::Degraded) == size * size;

        MinimumVisibleHeightResult onFlat = compute(observer, Agl(2.0), size, spacingDeg, flat, 4.0 / 3.0, nullptr);
        right = right && onFlat.terrainDatum == VerticalDatum::OrthometricMsl
            && MinimumVisibleAbsoluteHeightM(onFlat, 0, 0) == 100.0 + onFlat.heightAboveGroundM[0][0]
            && onFlat.groundM[0][0] == 100.0 && onFlat.groundM[center][center] == 100.0
            && onFlat.heightAboveGroundM[center][center] == 0.0;

        auto refused = [&](MinimumVisibleHeightResult r, InputProblem expected) { return r.inputProblem == expected && r.state.empty(); };
        right = right
            && refused(compute(observer, Agl(2.0), size, 0.0, flat, 4.0 / 3.0, nullptr), InputProblem::SpacingNotPositive)
            && refused(compute(observer, Agl(std::nan("")), size, spacingDeg, flat, 4.0 / 3.0, nullptr), InputProblem::HeightNotFinite)
            && refused(compute(observer, Agl(2.0), size, spacingDeg, flat, 0.0, nullptr), InputProblem::CurvatureFactorNotPositive)
            && refused(compute({ 89.8, -111.5 }, Agl(2.0), 41, 0.01, level, 4.0 / 3.0, nullptr), InputProblem::GridBeyondPole)
            && refused(compute({ std::nan(""), -111.5 }, Agl(2.0), size, spacingDeg, flat, 4.0 / 3.0, nullptr), InputProblem::CoordinateNotFinite)
            && refused(compute(observer, Agl(2.0), 0, spacingDeg, flat, 4.0 / 3.0, nullptr), InputProblem::None);

        std::vector<double> fractions;
        MinimumVisibleHeightResult reported = compute(observer, Agl(2.0), size, spacingDeg, flat, 4.0 / 3.0,
            [&](double fraction) { fractions.push_back(fraction); return true; });
        bool inOrder = fractions.size() > 2 && fractions.front() == 0.0 && fractions.back() == 1.0;
        for (size_t i = 1; i < fractions.size(); i++) inOrder = inOrder && fractions[i] >= fractions[i - 1];
        int reportsBeforeStopping = 0;
        MinimumVisibleHeightResult stopped = compute(observer, Agl(2.0), size, spacingDeg, flat, 4.0 / 3.0,
            [&](double) { reportsBeforeStopping++; return false; });
        right = right && SameMinimumVisibleHeights(reported, onFlat) && inOrder && stopped.cancelled && reportsBeforeStopping == 1;
    }

    MinimumVisibleHeightResult buried = ComputeMinimumVisibleHeightReference(observer, belowGround, size, size, spacingDeg, flat);
    int infinite = 0;
    for (const auto& row : buried.heightAboveGroundM)
    {
        for (double heightM : row) infinite += std::isinf(heightM) ? 1 : 0;
    }
    bool buriedRight = infinite == size * size - 1 && buried.heightAboveGroundM[center][center] == 0.0
        && ViewshedAtTargetHeight(buried, 1000.0).visible
            == ComputeViewshedNaive(observer, belowGround, size, size, spacingDeg, flat, 4.0 / 3.0, nullptr, Agl(1000.0)).visible;

    MinimumVisibleHeightResult onFlat = ComputeMinimumVisibleHeightReference(observer, Agl(2.0), size, size, spacingDeg, flat);
    bool thresholdsRefused = ViewshedAtTargetHeight(onFlat, std::nan("")).inputProblem == InputProblem::HeightNotFinite
        && ViewshedAtTargetHeight(onFlat, INFINITY).inputProblem == InputProblem::HeightNotFinite
        && ViewshedAtTargetHeight(onFlat, -1.0).inputProblem == InputProblem::HeightBelowGround
        && ViewshedAtTargetHeight(onFlat, -1.0).visible.empty();

    Expect(right && buriedRight && thresholdsRefused, "TestMinimumVisibleHeightKeepsTheViewshedsNoAnswerStatesAndRefusals");
}

// The fast minimum visible height is held to the fast viewshed's tolerances against the
// reference, applied to the viewsheds both give for a target of each of these heights.
// At 0 m that is test 57's comparison exactly; a taller target sees over the terrain that
// decides the edge at 0 m.
inline const double MinimumVisibleHeightComparedAtM[] = { 0.0, 2.0, 10.0, 30.0, 100.0 };

//TEST 68
void TestMinimumVisibleHeightFastAgreesWithTheReferenceAtThreeObservers()
{
    // The three observers of test 57 on the 1-arcsecond tile, each at 2 km and 5 km, with
    // its defaults: at every height above, the fast version is within the fast viewshed's
    // tolerance of the reference, at least 80% of their differences lie on the
    // reference's visibility edge, and an answer that is the same in every cell -- 0
    // everywhere, or out of sight everywhere -- fails the same tolerance. Prints the counts,
    // and how far the heights themselves are apart.
    const std::string path = "DATA/SRTM1/N36W112.hgt";
    RealElevationSampler sampler(path, 36.0, -112.0);
    if (!sampler.IsLoaded())
    {
        Skip("TestMinimumVisibleHeightFastAgreesWithTheReferenceAtThreeObservers", path + " not found from this working directory");
        return;
    }

    const double spacingDeg = MetersToLatitudeDeg(30.0);
    const RealTerrainViewshedRun runs[] = {
        { { 36.5, -111.5 }, 2.0 }, { { 36.5, -111.5 }, 5.0 },
        { { 36.86361, -111.30861 }, 2.0 }, { { 36.86361, -111.30861 }, 5.0 },
        { { 36.55861, -111.81361 }, 2.0 }, { { 36.55861, -111.81361 }, 5.0 },
    };
    for (const auto& run : runs)
    {
        int grid = (int)(2 * MetersToLatitudeDeg(run.radiusKm * 1000.0) / spacingDeg);
        MinimumVisibleHeightResult reference = ComputeMinimumVisibleHeightReference(run.observer, Agl(2.0), grid, grid, spacingDeg, sampler);
        MinimumVisibleHeightResult fast = ComputeMinimumVisibleHeightFast(run.observer, Agl(2.0), grid, grid, spacingDeg, sampler);

        std::ostringstream where;
        where << "(" << run.observer.latitudeDeg << ", " << run.observer.longitudeDeg << "), " << run.radiusKm << " km";
        bool withinTolerance = true, onEdge = true, oneAnswerFails = true;
        for (double heightM : MinimumVisibleHeightComparedAtM)
        {
            ViewshedResult referenceView = ViewshedAtTargetHeight(reference, heightM);
            ViewshedAgreement a = CompareViewsheds(ViewshedAtTargetHeight(fast, heightM), referenceView);
            std::cout << "  " << where.str() << ", target " << heightM << " m: reference sees " << a.referenceVisible << ", fast "
                << a.approximateVisible << "; fast only " << a.approximateOnlyVisible << ", reference only " << a.referenceOnlyVisible
                << "; differ on " << (100.0 * a.DisagreementRate()) << "% of " << a.VisibleInEither() << ", "
                << (100.0 * a.OffEdgeRate()) << "% off the edge; " << (100.0 * a.OnEdgeFraction()) << "% of differences on it" << std::endl;
            withinTolerance = withinTolerance && WithinFastViewshedTolerance(a);
            onEdge = onEdge && a.OnEdgeFraction() >= FastViewshedMinimumOnEdge;
            oneAnswerFails = oneAnswerFails
                && !WithinFastViewshedTolerance(CompareViewsheds(OneStateLike(referenceView, CellVisibility::NotVisible), referenceView))
                && !WithinFastViewshedTolerance(CompareViewsheds(OneStateLike(referenceView, CellVisibility::Visible), referenceView));
        }

        std::vector<double> apartM;
        for (int row = 0; row < grid; row++)
        {
            for (int col = 0; col < grid; col++)
            {
                double r = reference.heightAboveGroundM[row][col], f = fast.heightAboveGroundM[row][col];
                if (std::isfinite(r) && std::isfinite(f)) apartM.push_back(std::abs(r - f));
            }
        }
        std::sort(apartM.begin(), apartM.end());
        if (!apartM.empty())
        {
            std::cout << "  " << where.str() << ": heights apart by a median of " << apartM[apartM.size() / 2] << " m, 90th percentile "
                << apartM[apartM.size() * 9 / 10] << " m, 99th " << apartM[apartM.size() * 99 / 100] << " m, most " << apartM.back() << " m" << std::endl;
        }

        Expect(withinTolerance, "MinimumVisibleHeightFastWithinTolerance at " + where.str());
        Expect(onEdge, "MinimumVisibleHeightFastDifferencesLieOnTheReferencesEdge at " + where.str());
        Expect(oneAnswerFails, "MinimumVisibleHeightOneAnswerEverywhereFailsTheTolerance at " + where.str());
    }
}

//TEST 69
void TestPreparedObserverAnswersTheWallAndTheAirAsTheLineOfSightDoes()
{
    // The wall scene of test 51, prepared once out to 300 m. Straight east of the 2 m
    // observer, a target 180 m out clears the 50 m wall once it stands 98.00095 m tall
    // (test 64 works it by hand): hidden at 97.9 m, seen at 98.1 m. In front of the wall the
    // ground is seen; an aircraft 10 km up is seen from anywhere; a target below the ground
    // under it is hidden, as ComputeLineOfSight answers it -- 1 m under the wall's top as well,
    // where its slope alone would clear everything in front of it; a target past the prepared radius
    // has no answer from these rays at all (NotCovered). Heights are taken in any datum the
    // terrain can be put on.
    const int size = 21;
    const int center = size / 2;
    const double spacingDeg = MetersToLatitudeDeg(30.0);
    const GeoPoint observer{ 36.5, -111.5 };
    RasterBlockElevationSampler scene = MakeWallScene(observer, spacingDeg, size, center + 3);
    PreparedObserver prepared = PrepareObserver(observer, Agl(2.0), 300.0, spacingDeg, scene);

    auto east = [&](double metres) { return GreatCircleDestination(observer, 3.14159265358979323846 / 2, metres); };
    auto state = [&](GeoPoint target, DatumHeight height) { return QueryTarget(prepared, target, height).state; };
    const DatumHeight tenKilometresUp{ 10000.0, VerticalDatum::OrthometricMsl };
    const DatumHeight belowTheGround{ -10.0, VerticalDatum::OrthometricMsl };

    Expect(prepared.inputProblem == InputProblem::None && prepared.observerKnown
        && prepared.rayCount == RaysForRadius(300.0, 30.0) && prepared.samplesPerRay == 10
        && state(east(180.0), Agl(97.9)) == CellVisibility::NotVisible
        && state(east(180.0), Agl(98.1)) == CellVisibility::Visible
        && state(east(180.0), DatumHeight{ 98.1, VerticalDatum::OrthometricMsl }) == CellVisibility::Visible
        && state(east(60.0), Agl(0.0)) == CellVisibility::Visible
        && state(east(150.0), Agl(0.0)) == CellVisibility::NotVisible
        && state(east(250.0), tenKilometresUp) == CellVisibility::Visible
        && state(GreatCircleDestination(observer, 4.0, 200.0), tenKilometresUp) == CellVisibility::Visible
        && state(east(60.0), belowTheGround) == CellVisibility::NotVisible
        && state(east(90.0), DatumHeight{ 51.0, VerticalDatum::OrthometricMsl }) == CellVisibility::Visible
        && state(east(90.0), DatumHeight{ 49.0, VerticalDatum::OrthometricMsl }) == CellVisibility::NotVisible
        && state(east(400.0), tenKilometresUp) == CellVisibility::NotCovered,
        "TestPreparedObserverAnswersTheWallAndTheAirAsTheLineOfSightDoes");
}

// Direct line of sight from an observer 2 m above the ground to one target, as a cell state.
inline CellVisibility DirectLineOfSightState(GeoPoint observer, GeoPoint target, const DatumHeight& height, double spacingDeg, IElevationSampler& sampler, std::vector<ProfileSample>& buffer)
{
    GetTerrainProfile(observer, target, spacingDeg, sampler, buffer);
    LineOfSightResult los = ComputeLineOfSight(buffer, Agl(2.0), height);
    if (!IsOk(los.status)) return CellVisibility::Degraded;
    return los.isVisible ? CellVisibility::Visible : CellVisibility::NotVisible;
}

// The listed targets' agreement with the reference, counted as CompareViewsheds counts a
// grid: over the targets both answer with confidence, each direction apart, and each
// difference placed on the reference's edge when the reference answers a target moved 30 m
// north, east, south or west the other way. onEdge(i) is asked only for targets that differ.
template <class OnEdge>
ViewshedAgreement CompareTargetAnswers(const std::vector<CellVisibility>& approximate, const std::vector<CellVisibility>& reference, OnEdge&& onEdge)
{
    ViewshedAgreement a;
    a.sameGrid = approximate.size() == reference.size();
    if (!a.sameGrid) return a;
    for (size_t i = 0; i < reference.size(); i++)
    {
        if (!IsConfident(approximate[i]) || !IsConfident(reference[i])) continue;
        a.comparedCells++;
        bool seenByReference = reference[i] == CellVisibility::Visible;
        bool seenByApproximate = approximate[i] == CellVisibility::Visible;
        if (seenByReference) a.referenceVisible++;
        if (seenByApproximate) a.approximateVisible++;
        if (seenByReference == seenByApproximate) continue;
        if (seenByApproximate) a.approximateOnlyVisible++;
        else a.referenceOnlyVisible++;
        if (onEdge(i)) a.differingOnEdge++;
    }
    return a;
}

//TEST 70
void TestPreparedObserverAgreesWithLineOfSightOverTheListedTargets()
{
    // DATA/prepared_observer_targets.csv lists 15,000 seeded targets for each of the three
    // observers of the fast/naive comparison, over a 50 km disc on the 1-arcsecond tile: a
    // third 0-10 m above the ground, a third 10-500 m above it, a third up to 15,000 m above
    // mean sea level. Each is answered by QueryTarget and by ComputeLineOfSight along its own
    // line at 30 m, and the two are compared by height class to the fast viewshed's
    // tolerances: under 30% of the targets either sees, under 2% off the reference's edge, at
    // least 80% of the differences on it; a prepared observer that answers every target the
    // same way fails them. A target may be confident in one answer only -- a ray beside its
    // line leaving the tile, 5 m inside its edge, just before the target does -- but for
    // fewer than 1 in 1,000. Queries must run at 100,000 a second or more. Prints the counts,
    // the preparation's time and memory, and the query rate.
    const std::string path = "DATA/prepared_observer_targets.csv";
    RealElevationSampler sampler("DATA/SRTM1/N36W112.hgt", 36.0, -112.0);
    std::ifstream file(path);
    if (!sampler.IsLoaded() || !file.is_open())
    {
        Skip("TestPreparedObserverAgreesWithLineOfSightOverTheListedTargets", "DATA/SRTM1/N36W112.hgt or " + path + " not found from this working directory");
        return;
    }

    struct ListedTarget { GeoPoint point; DatumHeight height; int heightClass; };
    const GeoPoint observers[] = { { 36.5, -111.5 }, { 36.86361, -111.30861 }, { 36.55861, -111.81361 } };
    std::vector<ListedTarget> targets[3];
    std::string line;
    int perObserver[3] = {};
    bool parsed = true;
    while (std::getline(file, line))
    {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream fields(line);
        std::string observerText, latText, lonText, heightText, datumText;
        std::getline(fields, observerText, ',');
        std::getline(fields, latText, ',');
        std::getline(fields, lonText, ',');
        std::getline(fields, heightText, ',');
        std::getline(fields, datumText, ',');
        if (!datumText.empty() && datumText.back() == '\r') datumText.pop_back();
        auto index = ParseInt(observerText.c_str());
        auto lat = ParseFiniteDouble(latText.c_str()), lon = ParseFiniteDouble(lonText.c_str()), height = ParseFiniteDouble(heightText.c_str());
        if (!index || *index < 0 || *index > 2 || !lat || !lon || !height || (datumText != "agl" && datumText != "msl"))
        {
            parsed = false;
            break;
        }
        DatumHeight datumHeight{ *height, datumText == "agl" ? VerticalDatum::HeightAboveGround : VerticalDatum::OrthometricMsl };
        targets[*index].push_back({ GeoPoint{ *lat, *lon }, datumHeight, perObserver[*index]++ % 3 });
    }
    Expect(parsed && targets[0].size() == 15000 && targets[1].size() == 15000 && targets[2].size() == 15000, "PreparedObserverTargetListReads");
    if (!parsed) return;

    const char* classNames[] = { "0-10 m above ground", "10-500 m above ground", "up to 15,000 m above sea level" };
    const double spacingDeg = MetersToLatitudeDeg(30.0);
    std::vector<ProfileSample> buffer;
    for (int o = 0; o < 3; o++)
    {
        auto start = std::chrono::steady_clock::now();
        PreparedObserver prepared = PrepareObserver(observers[o], Agl(2.0), 50000.0, spacingDeg, sampler);
        double prepareS = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

        start = std::chrono::steady_clock::now();
        long long queries = 0, seen = 0;
        for (int rep = 0; rep < 20; rep++)
        {
            for (const auto& target : targets[o]) seen += QueryTarget(prepared, target.point, target.height).state == CellVisibility::Visible ? 1 : 0;
            queries += targets[o].size();
        }
        double perSecond = queries / std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

        std::ostringstream where;
        where << "(" << observers[o].latitudeDeg << ", " << observers[o].longitudeDeg << "), 50 km";
        std::cout << "  " << where.str() << ": prepared " << prepared.rayCount << " rays in " << prepareS << " s, "
            << (prepared.horizon.size() * sizeof(float) + prepared.firstVoidM.size() * sizeof(float)) / 1048576.0 << " MB; "
            << perSecond << " queries a second (" << seen << " seen)" << std::endl;
        Expect(perSecond >= 100000.0, "PreparedObserverAnswers100000TargetsASecond at " + where.str());

        for (int heightClass = 0; heightClass < 3; heightClass++)
        {
            std::vector<const ListedTarget*> chosen;
            for (const auto& target : targets[o]) if (target.heightClass == heightClass) chosen.push_back(&target);
            std::vector<CellVisibility> reference(chosen.size()), fast(chosen.size());
            for (size_t i = 0; i < chosen.size(); i++)
            {
                reference[i] = DirectLineOfSightState(observers[o], chosen[i]->point, chosen[i]->height, spacingDeg, sampler, buffer);
                fast[i] = QueryTarget(prepared, chosen[i]->point, chosen[i]->height).state;
            }

            // Whether the reference answers a target 30 m away the other way; worked out once, when first asked.
            std::vector<signed char> edge(chosen.size(), -1);
            auto onEdge = [&](size_t i) {
                if (edge[i] < 0)
                {
                    edge[i] = 0;
                    for (int n = 0; n < 4 && edge[i] == 0; n++)
                    {
                        GeoPoint moved = GreatCircleDestination(chosen[i]->point, n * 3.14159265358979323846 / 2, 30.0);
                        CellVisibility there = DirectLineOfSightState(observers[o], moved, chosen[i]->height, spacingDeg, sampler, buffer);
                        if (IsConfident(there) && there != reference[i]) edge[i] = 1;
                    }
                }
                return edge[i] == 1;
            };

            ViewshedAgreement a = CompareTargetAnswers(fast, reference, onEdge);
            int confidentMismatch = 0;
            for (size_t i = 0; i < chosen.size(); i++) confidentMismatch += IsConfident(fast[i]) != IsConfident(reference[i]) ? 1 : 0;

            std::string label = where.str() + ", " + classNames[heightClass];
            std::cout << "    " << classNames[heightClass] << ": " << a.comparedCells << " compared; line of sight sees " << a.referenceVisible
                << ", prepared " << a.approximateVisible << "; prepared only " << a.approximateOnlyVisible << ", line of sight only "
                << a.referenceOnlyVisible << "; differ on " << (100.0 * a.DisagreementRate()) << "% of " << a.VisibleInEither() << ", "
                << (100.0 * a.OffEdgeRate()) << "% off the edge; " << confidentMismatch << " confident in one only" << std::endl;

            auto oneAnswer = [&](CellVisibility answer) {
                std::vector<CellVisibility> same = reference;
                for (auto& s : same) if (IsConfident(s)) s = answer;
                return CompareTargetAnswers(same, reference, onEdge);
            };
            Expect(WithinFastViewshedTolerance(a) && a.OnEdgeFraction() >= FastViewshedMinimumOnEdge && confidentMismatch * 1000 < (int)chosen.size(),
                "PreparedObserverWithinTolerance at " + label);
            Expect(!WithinFastViewshedTolerance(oneAnswer(CellVisibility::NotVisible)) && !WithinFastViewshedTolerance(oneAnswer(CellVisibility::Visible)),
                "PreparedObserverOneAnswerEverywhereFailsTheTolerance at " + label);
        }
    }
}

//TEST 71
void TestPreparedObserverQueriesAllocateNothing()
{
    // After the preparation, answering a target allocates nothing, whatever the answer: seen,
    // hidden, past the radius, with no confident answer, or refused. Counted over 10,000
    // queries by the executable's own operator new.
    const int size = 21;
    const double spacingDeg = MetersToLatitudeDeg(30.0);
    const GeoPoint observer{ 36.5, -111.5 };
    ElevationCells cells = MakeFlatCells(size, 0.0);
    for (auto& row : cells) row[size / 2 + 3] = 50.0;
    cells[3][17] = std::nullopt;
    RasterBlockElevationSampler scene = MakeViewshedAlignedRaster(cells, observer, spacingDeg);
    PreparedObserver prepared = PrepareObserver(observer, Agl(2.0), 300.0, spacingDeg, scene);

    int states[4] = {};
    long long before = g_heapAllocations.load();
    for (int i = 0; i < 10000; i++)
    {
        GeoPoint target = GreatCircleDestination(observer, i * 0.61803, 5.0 + (i % 80) * 5.0);
        DatumHeight height = i % 7 == 0 ? DatumHeight{ std::nan(""), VerticalDatum::HeightAboveGround } : Agl((i % 13) * 10.0);
        states[(int)QueryTarget(prepared, target, height).state]++;
    }
    long long allocations = g_heapAllocations.load() - before;
    std::cout << "  10,000 queries: " << allocations << " allocations; visible " << states[(int)CellVisibility::Visible] << ", hidden "
        << states[(int)CellVisibility::NotVisible] << ", no confident answer " << states[(int)CellVisibility::Degraded] << ", past the radius "
        << states[(int)CellVisibility::NotCovered] << std::endl;

    Expect(allocations == 0 && states[0] > 0 && states[1] > 0 && states[2] > 0 && states[3] > 0,
        "TestPreparedObserverQueriesAllocateNothing");
}

//TEST 72
void TestPreparedObserverKeepsTheNoAnswerStatesAndRefusals()
{
    // As the viewsheds: a target whose rays cross a void, or which stands on one, has no
    // confident answer; an observer standing on a void, or a target height that can't be put
    // on the terrain's datum, leaves nothing known. A target that isn't on the Earth -- not a
    // number, or past a pole, a finite distance away and still no place -- or whose height
    // isn't a number, is refused with the reason; so is every target of a preparation that
    // was refused, a radius or a spacing of zero among them. And the preparation reports its
    // progress from 0 to 1 without changing a ray, and stops when asked.
    const int size = 21;
    const int center = size / 2;
    const double spacingDeg = MetersToLatitudeDeg(30.0);
    const GeoPoint observer{ 36.5, -111.5 };
    ElevationCells cells = MakeFlatCells(size, 0.0);
    cells[center][center + 4] = std::nullopt;   // 120 m east
    RasterBlockElevationSampler scene = MakeViewshedAlignedRaster(cells, observer, spacingDeg);
    PreparedObserver prepared = PrepareObserver(observer, Agl(2.0), 300.0, spacingDeg, scene);
    auto east = [&](double metres) { return GreatCircleDestination(observer, 3.14159265358979323846 / 2, metres); };

    ElevationCells holed = MakeFlatCells(size, 0.0);
    holed[center][center] = std::nullopt;
    RasterBlockElevationSampler onVoid = MakeViewshedAlignedRaster(holed, observer, spacingDeg);
    PreparedObserver blind = PrepareObserver(observer, Agl(2.0), 300.0, spacingDeg, onVoid);

    TargetAnswer notOnEarth = QueryTarget(prepared, GeoPoint{ std::nan(""), -111.5 }, Agl(2.0));
    TargetAnswer pastThePole = QueryTarget(prepared, GeoPoint{ 91.0, -111.5 }, Agl(2.0)); // a finite distance, but no place
    TargetAnswer noHeight = QueryTarget(prepared, east(60.0), Agl(INFINITY));
    PreparedObserver refused = PrepareObserver(observer, Agl(2.0), 300.0, 0.0, scene);
    TargetAnswer fromRefused = QueryTarget(refused, east(60.0), Agl(2.0));
    PreparedObserver noRadius = PrepareObserver(observer, Agl(2.0), 0.0, spacingDeg, scene);

    std::vector<double> fractions;
    PreparedObserver reported = PrepareObserver(observer, Agl(2.0), 300.0, spacingDeg, scene, 4.0 / 3.0, 0,
        [&](double fraction) { fractions.push_back(fraction); return true; });
    bool inOrder = fractions.size() >= 2 && fractions.front() == 0.0 && fractions.back() == 1.0;
    for (size_t i = 1; i < fractions.size(); i++) inOrder = inOrder && fractions[i] >= fractions[i - 1];
    int reportsBeforeStopping = 0;
    PreparedObserver stopped = PrepareObserver(observer, Agl(2.0), 300.0, spacingDeg, scene, 4.0 / 3.0, 0,
        [&](double) { reportsBeforeStopping++; return false; });

    Expect(QueryTarget(prepared, east(90.0), Agl(2.0)).state == CellVisibility::Visible
        && QueryTarget(prepared, east(180.0), Agl(2.0)).state == CellVisibility::Degraded
        && QueryTarget(prepared, east(120.0), Agl(2.0)).state == CellVisibility::Degraded
        && QueryTarget(prepared, east(90.0), DatumHeight{ 10.0, VerticalDatum::EllipsoidalHae }).state == CellVisibility::Degraded
        && !blind.observerKnown && QueryTarget(blind, east(90.0), Agl(2.0)).state == CellVisibility::Degraded
        && notOnEarth.state == CellVisibility::Degraded && notOnEarth.inputProblem == InputProblem::CoordinateNotFinite
        && pastThePole.state == CellVisibility::Degraded && pastThePole.inputProblem == InputProblem::LatitudeOutOfRange
        && noHeight.state == CellVisibility::Degraded && noHeight.inputProblem == InputProblem::HeightNotFinite
        && refused.inputProblem == InputProblem::SpacingNotPositive && refused.horizon.empty()
        && noRadius.inputProblem == InputProblem::RadiusNotPositive && noRadius.horizon.empty()
        && fromRefused.state == CellVisibility::Degraded && fromRefused.inputProblem == InputProblem::SpacingNotPositive
        && inOrder && reported.horizon == prepared.horizon && !reported.cancelled
        && stopped.cancelled && reportsBeforeStopping == 1
        && QueryTarget(stopped, east(60.0), Agl(2.0)).state == CellVisibility::Degraded,
        "TestPreparedObserverKeepsTheNoAnswerStatesAndRefusals");
}

// Two doubles with the same bits: equal numbers, and NaN matching NaN only if it is the same NaN.
inline bool SameBits(double a, double b)
{
    return std::memcmp(&a, &b, sizeof a) == 0;
}

// Two line-of-sight answers the same to the bit, field by field.
inline bool SameLineOfSight(const LineOfSightResult& a, const LineOfSightResult& b)
{
    bool samePoint = a.blockingPoint.has_value() == b.blockingPoint.has_value()
        && (!a.blockingPoint || (SameBits(a.blockingPoint->latitudeDeg, b.blockingPoint->latitudeDeg) && SameBits(a.blockingPoint->longitudeDeg, b.blockingPoint->longitudeDeg)));
    bool sameElevation = a.blockingElevationM.has_value() == b.blockingElevationM.has_value()
        && (!a.blockingElevationM || SameBits(*a.blockingElevationM, *b.blockingElevationM));
    return a.isVisible == b.isVisible && a.status == b.status && samePoint && sameElevation
        && SameBits(a.clearanceDeficitM, b.clearanceDeficitM) && a.blockingFeature == b.blockingFeature && a.inputProblem == b.inputProblem;
}

// Observers and targets spread over the 1-arcsecond tile and a little past it, on the ground
// and in the air, with one target that isn't on the Earth.
inline void SpreadSightEnds(int observerCount, int targetCount, std::vector<SightEnd>& observers, std::vector<SightEnd>& targets)
{
    const double golden = 3.14159265358979323846 * (3.0 - std::sqrt(5.0));
    const GeoPoint centre{ 36.5, -111.5 };
    for (int i = 0; i < observerCount; i++)
    {
        observers.push_back({ GreatCircleDestination(centre, i * golden, 30000.0 * std::sqrt((i + 0.5) / observerCount)), Agl(2.0 + i % 3 * 10.0) });
    }
    for (int i = 0; i < targetCount; i++)
    {
        GeoPoint point = GreatCircleDestination(centre, i * golden, 62000.0 * std::sqrt((i + 0.5) / targetCount));
        DatumHeight height = i % 3 == 0 ? DatumHeight{ 3000.0 + 20.0 * i, VerticalDatum::OrthometricMsl } : Agl(i % 7 * 5.0);
        targets.push_back({ point, height });
    }
    targets.push_back({ GeoPoint{ std::nan(""), -111.5 }, Agl(2.0) });
}

// Reads through another sampler and notes which threads read it, so a test can see how many
// threads really did the work.
class ThreadRecordingSampler : public IElevationSampler
{
public:
    explicit ThreadRecordingSampler(IElevationSampler& inner) : inner(inner) {}

    std::optional<double> GetElevation(double latitudeDeg, double longitudeDeg) override
    {
        return ElevationOf(Sample(latitudeDeg, longitudeDeg));
    }
    ElevationSample Sample(double latitudeDeg, double longitudeDeg) override
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            threads.insert(std::this_thread::get_id());
        }
        return inner.Sample(latitudeDeg, longitudeDeg);
    }
    VerticalDatum GetDatum() const override { return inner.GetDatum(); }

    size_t ThreadsSeen() const { return threads.size(); }

private:
    IElevationSampler& inner;
    std::mutex mutex;
    std::set<std::thread::id> threads;
};

//TEST 73
void TestLineOfSightPairsAreTheSameAtEveryThreadCount()
{
    // Eight observers against 241 targets on the 1-arcsecond tile -- on the ground, in the
    // air, some past the tile's edge where there is no data, and one not on the Earth at all --
    // answered on 1, 2, 3, 7 threads and one per hardware thread. Every answer at every thread
    // count must be, to the bit, what ComputeBatchLineOfSight gives for the same pair on one
    // thread: the verdict, the status, the blocking point, its elevation, the clearance
    // deficit, the kind of feature and any refusal. It must say it ran on as many threads as
    // asked -- worked out here, not by ThreadsFor -- and three threads asked for must be three
    // threads reading the terrain. Prints the pairs answered a second.
    RealElevationSampler sampler("DATA/SRTM1/N36W112.hgt", 36.0, -112.0);
    if (!sampler.IsLoaded())
    {
        Skip("TestLineOfSightPairsAreTheSameAtEveryThreadCount", "DATA/SRTM1/N36W112.hgt not found from this working directory");
        return;
    }
    std::vector<SightEnd> observers, targets;
    SpreadSightEnds(8, 240, observers, targets);
    const double spacingDeg = MetersToLatitudeDeg(30.0);

    std::vector<BatchLineOfSightQuery> queries;
    for (const auto& o : observers) for (const auto& t : targets) queries.push_back({ o.point, o.height, t.point, t.height });
    std::vector<LineOfSightResult> oneByOne = ComputeBatchLineOfSight(queries, spacingDeg, sampler);

    int kinds[4] = {}; // visible, blocked, no confident answer, refused
    for (const auto& r : oneByOne) kinds[r.status == ComputationStatus::InvalidInput ? 3 : !IsOk(r.status) ? 2 : r.isVisible ? 0 : 1]++;

    bool same = true, threadCountsRight = true;
    const int allThreads = std::thread::hardware_concurrency() == 0 ? 1 : (int)std::thread::hardware_concurrency();
    for (int threads : { 1, 2, 3, 7, 0 })
    {
        auto start = std::chrono::steady_clock::now();
        LineOfSightPairs pairs = ComputeLineOfSightPairs(observers, targets, spacingDeg, sampler, 4.0 / 3.0, threads);
        double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        std::cout << "  " << pairs.threadsUsed << " thread(s): " << pairs.results.size() / seconds << " pairs a second" << std::endl;
        threadCountsRight = threadCountsRight && pairs.threadsUsed == (threads > 0 ? threads : allThreads)
            && pairs.observerCount == 8 && pairs.targetCount == 241;
        for (size_t i = 0; i < oneByOne.size(); i++) same = same && SameLineOfSight(pairs.results[i], oneByOne[i]);
    }

    // And the threads asked for really share the work: three of them read the terrain.
    ThreadRecordingSampler recording(sampler);
    LineOfSightPairs recorded = ComputeLineOfSightPairs(observers, targets, spacingDeg, recording, 4.0 / 3.0, 3);
    for (size_t i = 0; i < oneByOne.size(); i++) same = same && SameLineOfSight(recorded.results[i], oneByOne[i]);
    std::cout << "  asked for 3 threads, " << recording.ThreadsSeen() << " read the terrain" << std::endl;
    threadCountsRight = threadCountsRight && recording.ThreadsSeen() == 3;
    std::cout << "  " << oneByOne.size() << " pairs: " << kinds[0] << " visible, " << kinds[1] << " blocked, " << kinds[2]
        << " without a confident answer, " << kinds[3] << " refused" << std::endl;

    Expect(same && threadCountsRight && kinds[0] > 0 && kinds[1] > 0 && kinds[2] > 0 && kinds[3] == 8,
        "TestLineOfSightPairsAreTheSameAtEveryThreadCount");
}

//TEST 74
void TestLineOfSightPairsRefuseEachPairItCannotSample()
{
    // As the batch: a spacing it can't sample at refuses every pair, each with its reason; no
    // observers, or no targets, is an empty answer; and an observer's i-th answer is its i-th
    // target's, whatever the thread count.
    LevelGroundSampler level(100.0);
    std::vector<SightEnd> observers = { { { 36.5, -111.5 }, Agl(2.0) }, { { 36.6, -111.5 }, Agl(2.0) } };
    std::vector<SightEnd> targets = { { { 36.52, -111.5 }, Agl(2.0) }, { { 91.0, -111.5 }, Agl(2.0) }, { { 36.5, -111.48 }, Agl(2.0) } };

    // Every pair refused: for the spacing, or -- checked first, as CheckProfileRequest does -- a
    // target past the pole.
    LineOfSightPairs refused = ComputeLineOfSightPairs(observers, targets, 0.0, level, 4.0 / 3.0, 3);
    bool allRefused = refused.results.size() == 6;
    for (int o = 0; o < 2; o++)
    {
        for (int t = 0; t < 3; t++)
        {
            InputProblem expected = t == 1 ? InputProblem::LatitudeOutOfRange : InputProblem::SpacingNotPositive;
            allRefused = allRefused && refused.At(o, t).status == ComputationStatus::InvalidInput && refused.At(o, t).inputProblem == expected;
        }
    }

    LineOfSightPairs mixed = ComputeLineOfSightPairs(observers, targets, MetersToLatitudeDeg(30.0), level, 4.0 / 3.0, 4);
    bool placed = IsOk(mixed.At(0, 0).status) && mixed.At(0, 0).isVisible
        && mixed.At(1, 1).inputProblem == InputProblem::LatitudeOutOfRange && mixed.At(0, 1).status == ComputationStatus::InvalidInput
        && IsOk(mixed.At(1, 2).status);

    Expect(allRefused && placed
        && ComputeLineOfSightPairs({}, targets, MetersToLatitudeDeg(30.0), level).results.empty()
        && ComputeLineOfSightPairs(observers, {}, MetersToLatitudeDeg(30.0), level).results.empty(),
        "TestLineOfSightPairsRefuseEachPairItCannotSample");
}

//TEST 75
void TestReferenceGridsAreTheSameAtEveryThreadCount()
{
    // The naive viewshed and the exact minimum visible height, over 1 km on the 1-arcsecond
    // tile, on 1, 2, 3 threads and one per hardware thread: the same grid to the bit every
    // time. Progress is reported only on the calling thread, and a stop asked for at the first
    // report stops every thread.
    RealElevationSampler sampler("DATA/SRTM1/N36W112.hgt", 36.0, -112.0);
    if (!sampler.IsLoaded())
    {
        Skip("TestReferenceGridsAreTheSameAtEveryThreadCount", "DATA/SRTM1/N36W112.hgt not found from this working directory");
        return;
    }
    const double spacingDeg = MetersToLatitudeDeg(30.0);
    const GeoPoint observer{ 36.55861, -111.81361 };
    const int size = (int)(2 * MetersToLatitudeDeg(1000.0) / spacingDeg);
    const DatumHeight ground = Agl(0.0);

    ViewshedResult naiveOne = ComputeViewshedNaive(observer, Agl(2.0), size, size, spacingDeg, sampler, 4.0 / 3.0, nullptr, ground, 1);
    MinimumVisibleHeightResult heightsOne = ComputeMinimumVisibleHeightReference(observer, Agl(2.0), size, size, spacingDeg, sampler, 4.0 / 3.0, nullptr, 1);

    bool same = true, reportedHere = true;
    const auto caller = std::this_thread::get_id();
    for (int threads : { 2, 3, 0 })
    {
        auto progress = [&](double) { reportedHere = reportedHere && std::this_thread::get_id() == caller; return true; };
        same = same && ComputeViewshedNaive(observer, Agl(2.0), size, size, spacingDeg, sampler, 4.0 / 3.0, progress, ground, threads).visible == naiveOne.visible
            && SameMinimumVisibleHeights(ComputeMinimumVisibleHeightReference(observer, Agl(2.0), size, size, spacingDeg, sampler, 4.0 / 3.0, progress, threads), heightsOne);
    }

    int reports = 0;
    ViewshedResult stopped = ComputeViewshedNaive(observer, Agl(2.0), size, size, spacingDeg, sampler, 4.0 / 3.0,
        [&](double) { reports++; return false; }, ground, 0);

    Expect(same && reportedHere && stopped.cancelled && reports == 1, "TestReferenceGridsAreTheSameAtEveryThreadCount");
}

// A sampler that answers only GetElevation, as one written before Sample existed would.
class GetElevationOnlySampler : public IElevationSampler
{
public:
    std::optional<double> GetElevation(double latitudeDeg, double) override
    {
        if (latitudeDeg > 36.6) return std::nullopt;
        return 10.0;
    }
    VerticalDatum GetDatum() const override { return VerticalDatum::OrthometricMsl; }
};

//TEST 76
void TestDataNotGivenIsToldApartFromAVoid()
{
    // A void is a hole in data the sampler was given; data not given is ground it never had.
    // Every sampler says which: the raster block and a view over one (a cell with no value,
    // and outside the block), a window of it, the .hgt reader (a -32768 post, off the tile, a window of it, a
    // tile that didn't load), the multi-tile sampler (no tile registered) and the fake grid.
    // A sampler that only answers GetElevation can't tell, and calls every gap a void. Then
    // the distinction reaches every answer -- a void, once known, winning over data not given,
    // since nothing loaded will fill it: the line of sight's status, both viewsheds' cells,
    // both minimum visible heights' cells, and a prepared observer's targets.
    const double spacingDeg = MetersToLatitudeDeg(30.0);
    const GeoPoint observer{ 36.5, -111.5 };
    const double lonSpacingDeg = LongitudeSpacingForLatitude(spacingDeg, observer.latitudeDeg);
    auto centre = [&](int row, int col) { return GeoPoint{ observer.latitudeDeg + (row - 2) * spacingDeg, observer.longitudeDeg + (col - 2) * lonSpacingDeg }; };
    auto data = [](IElevationSampler& s, GeoPoint p) { return s.Sample(p.latitudeDeg, p.longitudeDeg).data; };

    // 5 x 5 cells of flat ground at 100 m around the observer, with a void north-east of it.
    ElevationCells cells = MakeFlatCells(5, 100.0);
    cells[3][3] = std::nullopt;
    RasterBlockElevationSampler block = MakeViewshedAlignedRaster(cells, observer, spacingDeg);
    RasterBlockElevationSampler window({ { 100.0 } }, observer.latitudeDeg - 2 * spacingDeg, observer.longitudeDeg - 2 * lonSpacingDeg,
        spacingDeg, lonSpacingDeg, VerticalDatum::OrthometricMsl, 1, 2);

    // The same 2 x 1 corner of it as a view over someone else's arrays, the second cell flagged invalid.
    const double viewedM[2] = { 100.0, 0.0 };
    const uint8_t viewedValid[2] = { 1, 0 };
    RasterBlockGeometry viewGeometry;
    viewGeometry.originCellCentreLatitudeDeg = observer.latitudeDeg - 2 * spacingDeg;
    viewGeometry.originCellCentreLongitudeDeg = observer.longitudeDeg - 2 * lonSpacingDeg;
    viewGeometry.rowStepDeg = spacingDeg;
    viewGeometry.colStepDeg = lonSpacingDeg;
    viewGeometry.rows = 1;
    viewGeometry.cols = 2;
    RasterBlockViewElevationSampler view(viewedM, viewedValid, viewGeometry, VerticalDatum::OrthometricMsl);

    bool samplers = data(block, centre(0, 0)) == ElevationData::Present && data(block, centre(3, 3)) == ElevationData::Void
        && data(block, centre(6, 6)) == ElevationData::NotGiven && data(block, GeoPoint{ std::nan(""), -111.5 }) == ElevationData::NotGiven
        && data(view, centre(0, 0)) == ElevationData::Present && data(view, centre(0, 1)) == ElevationData::Void
        && data(view, centre(1, 0)) == ElevationData::NotGiven
        && data(window, centre(1, 2)) == ElevationData::Present && window.Sample(centre(1, 2).latitudeDeg, centre(1, 2).longitudeDeg).elevationM == 100.0
        && data(window, centre(2, 2)) == ElevationData::NotGiven;

    // A 2 x 2 .hgt tile over one degree, its north-east post a void.
    const std::string path = "not_given_tile.hgt";
    {
        std::ofstream file(path, std::ios::binary);
        const int16_t posts[4] = { 500, -32768, 520, 530 };
        for (int16_t value : posts)
        {
            unsigned char bytes[2] = { (unsigned char)((value >> 8) & 0xFF), (unsigned char)(value & 0xFF) };
            file.write((const char*)bytes, 2);
        }
    }
    RealElevationSampler tile(path, 36.0, -112.0);
    RealElevationSampler tileWindow = tile.Window(PostWindow{ 1, 1, 0, 1 });
    std::remove(path.c_str());
    RealElevationSampler missing("no_such_tile.hgt", 36.0, -112.0);
    samplers = samplers && data(tile, { 37.0, -112.0 }) == ElevationData::Present && data(tile, { 37.0, -111.0 }) == ElevationData::Void
        && data(tile, { 38.5, -111.5 }) == ElevationData::NotGiven && data(missing, { 36.5, -111.5 }) == ElevationData::NotGiven
        && data(tileWindow, { 36.0, -111.0 }) == ElevationData::Present && data(tileWindow, { 37.0, -112.0 }) == ElevationData::NotGiven
        && tileWindow.Posts().size() == 2 && tileWindow.Posts().capacity() < tile.Posts().size(); // holds its own posts, not the tile's

    MultiTileElevationSampler tiles;
    tiles.AddTile(36.0, -112.0, block);
    FakeElevationSampler fake({ { 1.0, 2.0 }, { 3.0, 4.0 } });
    GetElevationOnlySampler older;
    samplers = samplers && data(tiles, centre(3, 3)) == ElevationData::Void && data(tiles, { 36.5, -110.5 }) == ElevationData::NotGiven
        && data(tiles, centre(6, 6)) == ElevationData::NotGiven && data(fake, { 0.0, 1.0 }) == ElevationData::Present
        && data(fake, { 5.0, 5.0 }) == ElevationData::NotGiven && data(older, { 37.0, -111.5 }) == ElevationData::Void;

    // Lines of sight from the observer: across the void, off the block, onto the void, and
    // across the void and off the block at once.
    auto status = [&](GeoPoint target) {
        return ComputeLineOfSight(GetTerrainProfile(observer, target, spacingDeg, block), Agl(2.0), Agl(2.0)).status;
    };
    bool lines = status(centre(0, 0)) == ComputationStatus::Ok && status(centre(4, 4)) == ComputationStatus::VoidInProfile
        && status(centre(2, 6)) == ComputationStatus::DataNotGiven && status(centre(3, 3)) == ComputationStatus::EndpointMissing
        && status(centre(5, 5)) == ComputationStatus::VoidInProfile;

    // Viewsheds and minimum visible heights over 7 x 7 cells: the outer ring of 24 has no
    // data under it. A ring cell whose line crosses the void has no answer for that reason,
    // the others for the data not given -- naive exactly so, line by line; fast, whose rays
    // pass beside the lines, with both kinds in the ring and none of it confident. Observed
    // from off the block every cell is data not given, and from the void every cell Degraded.
    bool grids = true;
    const int ringSize = 24;
    auto inRing = [](int row, int col) { return row == 0 || row == 6 || col == 0 || col == 6; };
    for (bool fast : { false, true })
    {
        auto viewshed = [&](GeoPoint from, IElevationSampler& s) {
            return fast ? ComputeViewshedFast(from, Agl(2.0), 7, 7, spacingDeg, s) : ComputeViewshedNaive(from, Agl(2.0), 7, 7, spacingDeg, s);
        };
        auto heights = [&](GeoPoint from, IElevationSampler& s) {
            return fast ? ComputeMinimumVisibleHeightFast(from, Agl(2.0), 7, 7, spacingDeg, s) : ComputeMinimumVisibleHeightReference(from, Agl(2.0), 7, 7, spacingDeg, s);
        };
        ViewshedResult seen = viewshed(observer, block);
        int ringNotGiven = 0, ringVoid = 0;
        for (int row = 0; row < 7; row++)
        {
            for (int col = 0; col < 7; col++)
            {
                if (!inRing(row, col)) continue;
                CellVisibility cell = seen.visible[row][col];
                ringNotGiven += cell == CellVisibility::DataNotGiven ? 1 : 0;
                ringVoid += cell == CellVisibility::Degraded ? 1 : 0;
                if (!fast)
                {
                    // Naive: the reason is the one its own line gives.
                    ComputationStatus reason = NoAnswerStatus(GetTerrainProfile(observer, centre(row - 1, col - 1), spacingDeg, block));
                    grids = grids && cell == (reason == ComputationStatus::DataNotGiven ? CellVisibility::DataNotGiven : CellVisibility::Degraded);
                }
            }
        }
        GeoPoint offBlock = centre(6, 2), onVoid = centre(3, 3);
        grids = grids && ringNotGiven + ringVoid == ringSize && ringNotGiven > 0 && ringVoid > 0
            && heights(observer, block).state == seen.visible
            && CountCells(viewshed(offBlock, block), CellVisibility::DataNotGiven) == 49 && heights(offBlock, block).state == viewshed(offBlock, block).visible
            && CountCells(viewshed(onVoid, block), CellVisibility::Degraded) == 49 && heights(onVoid, block).state == viewshed(onVoid, block).visible;
    }

    // A prepared observer: a target off the block, and one observer standing off it.
    PreparedObserver prepared = PrepareObserver(observer, Agl(2.0), 150.0, spacingDeg, block);
    PreparedObserver offTheBlock = PrepareObserver(centre(6, 2), Agl(2.0), 150.0, spacingDeg, block);
    bool targets = QueryTarget(prepared, centre(2, 4), Agl(2.0)).state == CellVisibility::Visible
        && QueryTarget(prepared, centre(2, 6), Agl(2.0)).state == CellVisibility::DataNotGiven
        && QueryTarget(prepared, centre(3, 3), Agl(2.0)).state == CellVisibility::Degraded
        && QueryTarget(offTheBlock, centre(2, 2), Agl(2.0)).state == CellVisibility::DataNotGiven
        // A target height with no datum is Degraded whatever is known of the observer, as in a viewshed.
        && QueryTarget(offTheBlock, centre(2, 2), DatumHeight{ 2.0, VerticalDatum::EllipsoidalHae }).state == CellVisibility::Degraded
        && ComputeViewshedFast(centre(6, 2), Agl(2.0), 7, 7, spacingDeg, block, 4.0 / 3.0, nullptr, DatumHeight{ 2.0, VerticalDatum::EllipsoidalHae }).visible[2][2] == CellVisibility::Degraded;

    Expect(samplers && lines && grids && targets, "TestDataNotGivenIsToldApartFromAVoid");
}

// Reads through another sampler and keeps the box around every point it was asked for.
class ExtentRecordingSampler : public IElevationSampler
{
public:
    explicit ExtentRecordingSampler(IElevationSampler& inner) : inner(inner) {}

    std::optional<double> GetElevation(double latitudeDeg, double longitudeDeg) override
    {
        return ElevationOf(Sample(latitudeDeg, longitudeDeg));
    }
    ElevationSample Sample(double latitudeDeg, double longitudeDeg) override
    {
        read.Include(GeoPoint{ latitudeDeg, longitudeDeg });
        return inner.Sample(latitudeDeg, longitudeDeg);
    }
    VerticalDatum GetDatum() const override { return inner.GetDatum(); }

    GeoExtent read;

private:
    IElevationSampler& inner;
};

//TEST 77
void TestQueryExtentsAreTheBoxesTheQueriesRead()
{
    // Asked before it runs, a query says which box of latitude and longitude it will read;
    // run through a sampler that keeps the box around every point it is asked for, it must
    // read exactly that box, to the bit. For profiles: north-south, diagonal, reversed, a
    // few metres, and 60 km east-west at 60 degrees north, where the great circle bulges
    // toward the pole -- a box around its ends alone would miss its northern edge. For the
    // naive and fast viewsheds and both minimum visible heights, at 60 degrees north. A
    // request the query would refuse reads nothing: an empty box, carrying the reason.
    LevelGroundSampler level(100.0);
    const double spacingDeg = MetersToLatitudeDeg(30.0);
    auto profileRight = [&](GeoPoint a, GeoPoint b) {
        ExtentRecordingSampler recording(level);
        GetTerrainProfile(a, b, spacingDeg, recording);
        return ProfileExtent(a, b, spacingDeg) == recording.read;
    };
    const GeoPoint north60{ 60.0, 10.0 };
    GeoPoint east60 = GeoPoint{ 60.0, 10.0 + 60000.0 / (EarthRadiusM * DegToRad * std::cos(60.0 * DegToRad)) };
    GeoExtent bulge = ProfileExtent(north60, east60, spacingDeg);

    bool profiles = profileRight({ 36.5, -111.5 }, { 36.7, -111.5 }) && profileRight({ 36.5, -111.5 }, { 36.8, -111.2 })
        && profileRight({ 36.8, -111.2 }, { 36.5, -111.5 }) && profileRight({ 36.5, -111.5 }, { 36.50001, -111.5 })
        && profileRight(north60, east60) && profileRight(east60, north60)
        && bulge.northLatDeg > 60.0 + MetersToLatitudeDeg(50.0)
        && ProfileExtent({ 36.5, -111.5 }, { 36.7, -111.5 }, 0.0).empty
        && ProfileExtent({ 36.5, -111.5 }, { 36.7, -111.5 }, 0.0).inputProblem == InputProblem::SpacingNotPositive
        && ProfileExtent({ 36.5, -111.5 }, { 36.7, -111.5 }, spacingDeg).inputProblem == InputProblem::None;

    bool grids = true;
    const GeoPoint observer{ 60.0, 10.0 };
    const double cellDeg = MetersToLatitudeDeg(300.0);
    for (bool fast : { false, true })
    {
        GeoExtent expected = fast ? FastViewshedExtent(observer, 41, 41, cellDeg) : NaiveViewshedExtent(observer, 41, 41, cellDeg);
        ExtentRecordingSampler viewshedRead(level), heightsRead(level);
        if (fast)
        {
            ComputeViewshedFast(observer, Agl(2.0), 41, 41, cellDeg, viewshedRead);
            ComputeMinimumVisibleHeightFast(observer, Agl(2.0), 41, 41, cellDeg, heightsRead);
        }
        else
        {
            ComputeViewshedNaive(observer, Agl(2.0), 41, 41, cellDeg, viewshedRead);
            ComputeMinimumVisibleHeightReference(observer, Agl(2.0), 41, 41, cellDeg, heightsRead);
        }
        grids = grids && expected == viewshedRead.read && expected == heightsRead.read;
    }
    // No cells reads nothing, and is no refusal; a grid reaching a pole is refused, and says so.
    GeoExtent noCells = NaiveViewshedExtent(observer, 0, 41, cellDeg), pastThePole = FastViewshedExtent({ 89.99, 10.0 }, 41, 41, cellDeg);
    grids = grids && noCells.empty && noCells.inputProblem == InputProblem::None
        && pastThePole.empty && pastThePole.inputProblem == InputProblem::GridBeyondPole
        && NaiveViewshedExtent({ 89.99, 10.0 }, 41, 41, cellDeg).inputProblem == InputProblem::GridBeyondPole;

    Expect(profiles && grids, "TestQueryExtentsAreTheBoxesTheQueriesRead");
}

//TEST 78
void TestAQueryGivenOnlyItsExtentAnswersAsOnTheWholeTile()
{
    // Each query is run twice on the 1-arcsecond tile: once on the whole tile, once on a
    // window of it holding only the posts its reported box needs (PostsCovering). The two
    // answers must be the same to the bit: lines of sight to five targets, the naive viewshed
    // and the exact minimum visible height over 1 km, the fast viewshed and fast minimum
    // visible height over 3 km -- nearest, and the fast viewshed bilinear too. Then with the
    // window a post short on each side in turn, the query must report the data it wasn't
    // given: something changes, and everything that changes is DataNotGiven, never a void.
    RealElevationSampler nearest("DATA/SRTM1/N36W112.hgt", 36.0, -112.0);
    if (!nearest.IsLoaded())
    {
        Skip("TestAQueryGivenOnlyItsExtentAnswersAsOnTheWholeTile", "DATA/SRTM1/N36W112.hgt not found from this working directory");
        return;
    }
    RealElevationSampler bilinear = nearest.WithInterpolationMode(InterpolationMode::Bilinear);
    const double spacingDeg = MetersToLatitudeDeg(30.0);
    const GeoPoint observer{ 36.55861, -111.81361 };

    auto windowFor = [](const RealElevationSampler& tile, const GeoExtent& box) {
        return tile.PostsCovering(box.southLatDeg, box.northLatDeg, box.westLonDeg, box.eastLonDeg);
    };
    auto shortOn = [](PostWindow w, int side) {
        if (side == 0) w.firstRow++;       // north
        else if (side == 1) w.lastRow--;   // south
        else if (side == 2) w.firstCol++;  // west
        else w.lastCol--;                  // east
        return w;
    };

    bool same = true, reported = true;

    // Lines of sight.
    std::vector<ProfileSample> buffer;
    for (int i = 0; i < 5; i++)
    {
        GeoPoint target = GreatCircleDestination(observer, 1.3 * i + 0.4, 2000.0 + 2000.0 * i);
        GeoExtent box = LineOfSightExtent(observer, target, spacingDeg);
        auto losOn = [&](IElevationSampler& s) {
            GetTerrainProfile(observer, target, spacingDeg, s, buffer);
            return ComputeLineOfSight(buffer, Agl(2.0), Agl(2.0));
        };
        RealElevationSampler part = nearest.Window(windowFor(nearest, box));
        same = same && SameLineOfSight(losOn(part), losOn(nearest));
        for (int side = 0; side < 4; side++)
        {
            RealElevationSampler smaller = nearest.Window(shortOn(windowFor(nearest, box), side));
            reported = reported && losOn(smaller).status == ComputationStatus::DataNotGiven;
        }
    }

    // Grids: whatever changes with a post short must be data not given.
    auto onlyNotGiven = [](const std::vector<std::vector<CellVisibility>>& shortOne, const std::vector<std::vector<CellVisibility>>& whole) {
        int changed = 0;
        for (size_t row = 0; row < whole.size(); row++)
        {
            for (size_t col = 0; col < whole[row].size(); col++)
            {
                if (shortOne[row][col] == whole[row][col]) continue;
                if (shortOne[row][col] != CellVisibility::DataNotGiven) return false;
                changed++;
            }
        }
        return changed > 0;
    };
    const int oneKm = (int)(2 * MetersToLatitudeDeg(1000.0) / spacingDeg);
    const int threeKm = (int)(2 * MetersToLatitudeDeg(3000.0) / spacingDeg);
    struct GridQuery { bool fast; bool heights; int size; RealElevationSampler* tile; };
    const GridQuery queries[] = {
        { false, false, oneKm, &nearest }, { false, true, oneKm, &nearest },
        { true, false, threeKm, &nearest }, { true, true, threeKm, &nearest }, { true, false, threeKm, &bilinear },
    };
    for (const GridQuery& q : queries)
    {
        GeoExtent box = q.fast ? FastViewshedExtent(observer, q.size, q.size, spacingDeg) : NaiveViewshedExtent(observer, q.size, q.size, spacingDeg);
        auto cellsOn = [&](IElevationSampler& s) {
            if (q.heights)
            {
                return q.fast ? ComputeMinimumVisibleHeightFast(observer, Agl(2.0), q.size, q.size, spacingDeg, s)
                              : ComputeMinimumVisibleHeightReference(observer, Agl(2.0), q.size, q.size, spacingDeg, s);
            }
            MinimumVisibleHeightResult asStates;
            asStates.state = (q.fast ? ComputeViewshedFast(observer, Agl(2.0), q.size, q.size, spacingDeg, s)
                                     : ComputeViewshedNaive(observer, Agl(2.0), q.size, q.size, spacingDeg, s)).visible;
            return asStates;
        };
        MinimumVisibleHeightResult whole = cellsOn(*q.tile);
        RealElevationSampler part = q.tile->Window(windowFor(*q.tile, box));
        same = same && SameMinimumVisibleHeights(cellsOn(part), whole);
        for (int side = 0; side < 4; side++)
        {
            RealElevationSampler smaller = q.tile->Window(shortOn(windowFor(*q.tile, box), side));
            reported = reported && onlyNotGiven(cellsOn(smaller).state, whole.state);
        }
    }

    Expect(same && reported, "TestAQueryGivenOnlyItsExtentAnswersAsOnTheWholeTile");
}
