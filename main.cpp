#include <iostream>
#include "IElevationSampler.h"
#include "TerrainProfile.h"
#include "LineOfSight.h"
#include "Viewshed.h"

void PrintLosResult(LineOfSightResult los)
{
    std::cout << "Visible: " << (los.isVisible ? "YES" : "NO") << std::endl;

    if (los.blockingPoint.has_value())
    {
        std::cout << "Blocking point - Lat: " << los.blockingPoint->latitude
            << ", Lon: " << los.blockingPoint->longitude << std::endl;
        std::cout << "Blocking elevation: " << *los.blockingElevation << std::endl;
    }
    else
    {
        std::cout << "Blocking point: None" << std::endl;
    }

    std::cout << "Clearance deficit: " << los.clearanceDeficit << std::endl;
    std::cout << "-----" << std::endl;
}

int main()
{
    std::vector<std::vector<double>> testGrid = {
        {10, 10, 10, 10, 10},
        {10, 20, 30, 20, 10},
        {10, 30, 50, 30, 10},
        {10, 20, 30, 20, 10},
        {10, 10, 10, 10, 10}
    };

    FakeElevationSampler sampler(testGrid);

    // LineOfSight Test 1 - No Clear Sight
    GeoPoint a{ 2, 0 };
    GeoPoint b{ 2, 4 };
    std::vector<ProfileSample> profile = GetTerrainProfile(a, b, 1.0, sampler);
    LineOfSightResult los = ComputeLineOfSight(profile, 2.0, 2.0, 4);
    PrintLosResult(los);

    // LineOfSight Test 2 - Clear Sight
    GeoPoint a2{ 0, 0 };
    GeoPoint b2{ 0, 4 };
    std::vector<ProfileSample> flatProfile = GetTerrainProfile(a2, b2, 1.0, sampler);
    LineOfSightResult flatLos = ComputeLineOfSight(flatProfile, 2.0, 2.0, 4);
    PrintLosResult(flatLos);
    
    
    
    // LineOfSight Test 3 - Curved
    std::vector<ProfileSample> curvatureProfile;
    for (int i = 0; i <= 10; i++)
    {
        double t = (double)i / 10;
        ProfileSample s;
        s.point = GeoPoint{ 0, t * 50000.0 };  // longitude'u mesafe olarak kullan
        s.elevation = 0;
        curvatureProfile.push_back(s);
    }

    LineOfSightResult curvatureLos = ComputeLineOfSight(curvatureProfile, 2.0, 2.0, 50000.0);
    PrintLosResult(curvatureLos);
    
    
    
    // Viewshed Test From (0,0)
    GeoPoint observerPos{ 0, 0 };
    ViewshedResult viewshed = ComputeViewshedNaive(observerPos, 2.0, 5, 5, 1.0, sampler);

    for (int row = 0; row < 5; row++)
    {
        for (int col = 0; col < 5; col++)
        {
            std::cout << (viewshed.visible[row][col] ? "#" : ".") << " ";
        }
        std::cout << std::endl;
    }

    return 0;
}