#pragma once
#include <vector>
#include <fstream>
#include <cmath>
#include <optional>
#include <string>
#include "IElevationSampler.h"

class RealElevationSampler : public IElevationSampler
{
public:
    RealElevationSampler(std::string filePath, double swLatitude, double swLongitude, InterpolationMode interpolationMode = InterpolationMode::Nearest)
    {
        swLat = swLatitude;
        swLon = swLongitude;
        mode = interpolationMode;

        std::ifstream file(filePath, std::ios::binary);
        loadedSuccessfully = file.is_open();
        std::vector<char> buffer((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

        long totalSamples = buffer.size() / 2;
        if (totalSamples == 0)
        {
            loadedSuccessfully = false;
        }
        size = (int)round(sqrt((double)totalSamples));

        data.resize(totalSamples);
        for (long i = 0; i < totalSamples; i++)
        {
            unsigned char highByte = (unsigned char)buffer[i * 2];
            unsigned char lowByte = (unsigned char)buffer[i * 2 + 1];
            int16_t value = (int16_t)((highByte << 8) | lowByte);
            data[i] = value;
        }
    }

    virtual ~RealElevationSampler() = default;
    
    bool IsLoaded() const
    {
        return loadedSuccessfully;
    }

    std::optional<double> virtual GetElevation(double latitude, double longitude)
    {
        double topLat = swLat + 1.0;
        double rowF = (topLat - latitude) * (size - 1);
        double colF = (longitude - swLon) * (size - 1);

        if (mode == InterpolationMode::Bilinear)
        {
            int row0 = (int)floor(rowF);
            int col0 = (int)floor(colF);
            int row1 = row0 + 1;
            int col1 = col0 + 1;

            if (row0 < 0 || row1 >= size || col0 < 0 || col1 >= size)
            {
                return std::nullopt;
            }

            int16_t v00 = data[row0 * size + col0];
            int16_t v01 = data[row0 * size + col1];
            int16_t v10 = data[row1 * size + col0];
            int16_t v11 = data[row1 * size + col1];

            if (v00 == -32768 || v01 == -32768 || v10 == -32768 || v11 == -32768)
            {
                return std::nullopt;
            }

            double fracRow = rowF - row0;
            double fracCol = colF - col0;

            double top = v00 + (v01 - v00) * fracCol;
            double bottom = v10 + (v11 - v10) * fracCol;
            return top + (bottom - top) * fracRow;
        }

        int row = (int)round(rowF);
        int col = (int)round(colF);

        if (row < 0 || row >= size || col < 0 || col >= size)
        {
            return std::nullopt;
        }

        int16_t value = data[row * size + col];

        if (value == -32768)
        {
            return std::nullopt;
        }

        return (double)value;
    }

private:
    double swLat;
    double swLon;
    int size;
    std::vector<int16_t> data;
    InterpolationMode mode = InterpolationMode::Nearest;
    bool loadedSuccessfully = false;
};