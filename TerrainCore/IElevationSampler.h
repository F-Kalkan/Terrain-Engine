#pragma once
#include <vector>
#include <optional>
#include <cmath>

enum class InterpolationMode
{
    Nearest,
    Bilinear
};

class IElevationSampler {
public:
    virtual ~IElevationSampler() = default;

    std::optional<double> virtual GetElevation(double latitude, double longitude) = 0;
};

class FakeElevationSampler : public IElevationSampler {
public:
    std::vector<std::vector<double>> grid;
    InterpolationMode mode = InterpolationMode::Nearest;

    FakeElevationSampler(std::vector<std::vector<double>> initialGrid, InterpolationMode interpolationMode = InterpolationMode::Nearest)
    {
        grid = initialGrid;
        mode = interpolationMode;
    }

    virtual ~FakeElevationSampler() = default;

    std::optional<double> virtual GetElevation(double latitude, double longitude) {
        if (mode == InterpolationMode::Bilinear)
        {
            int row0 = (int)floor(latitude);
            int col0 = (int)floor(longitude);
            int row1 = row0 + 1;
            int col1 = col0 + 1;

            if (row0 < 0 || row1 >= (int)grid.size() || col0 < 0 || col1 >= (int)grid[0].size())
            {
                return std::nullopt;
            }

            double fracRow = latitude - row0;
            double fracCol = longitude - col0;

            double v00 = grid[row0][col0];
            double v01 = grid[row0][col1];
            double v10 = grid[row1][col0];
            double v11 = grid[row1][col1];

            double top = v00 + (v01 - v00) * fracCol;
            double bottom = v10 + (v11 - v10) * fracCol;
            return top + (bottom - top) * fracRow;
        }

        int row = (int)round(latitude);
        int col = (int)round(longitude);

        if (row < 0 || row >= (int)grid.size() || col < 0 || col >= (int)grid[0].size())
        {
            return std::nullopt;
        }

        return grid[row][col];
    }
};