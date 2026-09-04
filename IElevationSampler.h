#pragma once
#include <vector>

  
class IElevationSampler {
public:
	virtual ~IElevationSampler() = default;
	
	double virtual GetElevation(double latitude, double longtitude) = 0;
};

class FakeElevationSampler : public IElevationSampler {
public:
    std::vector<std::vector<double>> grid;

    FakeElevationSampler(std::vector<std::vector<double>> initialGrid){ grid = initialGrid; }

    virtual ~FakeElevationSampler() = default;

    double virtual GetElevation(double latitude, double longtitude) {
        int row = (int)round(latitude);
        int col = (int)round(longtitude);

        if (row < 0 || row >= grid.size() || col < 0 || col >= grid[0].size())
        {
            return -9999;
        }

        return grid[row][col];
    }
};