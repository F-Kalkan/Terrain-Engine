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
    RealElevationSampler(std::string filePath, double swLatitude, double swLongitude)
    {
        swLat = swLatitude;
        swLon = swLongitude;

        std::ifstream file(filePath, std::ios::binary);
        std::vector<char> buffer((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

        long totalSamples = buffer.size() / 2;
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

    std::optional<double> virtual GetElevation(double latitude, double longtitude)
    {
        double topLat = swLat + 1.0;
        double rowF = (topLat - latitude) * (size - 1);
        double colF = (longtitude - swLon) * (size - 1);

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
};