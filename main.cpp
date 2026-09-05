#include <iostream>
#include "IElevationSampler.h"
#include "TerrainProfile.h"
#include "LineOfSight.h"
#include "Viewshed.h"
#include "Tests.h"
#include "RealElevationSampler.h"


int main()
{
    // Tests
    TestFlatPlateauEverythingVisible();
    TestWallBlocksView();
    TestCurvatureBlocksFlatTerrain();
    TestVoidPointIsDegraded();
    TestViewshedDetectsVoid();

    std::cout << "-------------------------" << std::endl;

    // Viewshed 
    std::vector<std::vector<double>> testGrid = {
        {10, 10, 10, 10, 10},
        {10, 20, 30, 20, 10},
        {10, 30, 50, 30, 10},
        {10, 20, 30, 20, 10},
        {10, 10, 10, 10, 10}
    };
    FakeElevationSampler sampler(testGrid);

    GeoPoint observerPos{ 0, 0 };
    ViewshedResult viewshed = ComputeViewshedNaive(observerPos, 2.0, 5, 5, 1.0, sampler);

    std::cout << "Viewshed From Point (0,0)" << std::endl;
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

    return 0;
}

