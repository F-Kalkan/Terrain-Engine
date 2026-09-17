#pragma once
#include <cstdint>
#include <vector>
#include <fstream>
#include <cmath>
#include <optional>
#include <string>
#include "IElevationSampler.h"
#include <filesystem>

class RealElevationSampler : public IElevationSampler
{
public:
    RealElevationSampler(const std::filesystem::path& filePath, double swLatitudeDeg, double swLongitudeDeg, InterpolationMode interpolationMode = InterpolationMode::Nearest)
    {
        swLat = swLatitudeDeg;
        swLon = swLongitudeDeg;
        mode = interpolationMode;

        std::ifstream file(filePath, std::ios::binary);
        std::vector<char> buffer((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

        // A tile is a square of big-endian 16-bit posts, so its byte count must be
        // twice a perfect square, at least 2x2. Anything else -- most often a
        // download cut short -- is rejected rather than rounded to the nearest
        // square: rounding up would place the last posts past the end of the data.
        size_t totalSamples = buffer.size() / 2;
        size_t side = (size_t)std::llround(std::sqrt((double)totalSamples));
        loadedSuccessfully = file.is_open() && buffer.size() % 2 == 0 && side >= 2 && side * side == totalSamples;
        if (!loadedSuccessfully)
        {
            size = 0;
            return;
        }
        size = (int)side;

        data.resize(totalSamples);
        for (size_t i = 0; i < totalSamples; i++)
        {
            unsigned char highByte = (unsigned char)buffer[i * 2];
            unsigned char lowByte = (unsigned char)buffer[i * 2 + 1];
            int16_t value = (int16_t)((highByte << 8) | lowByte);
            data[i] = value;
            if (value == -32768) voidCount++;
        }
    }

    virtual ~RealElevationSampler() = default;
    
    bool IsLoaded() const
    {
        return loadedSuccessfully;
    }

    // Posts along one side of the tile: 1201 for a 3-arcsecond tile, 3601 for a
    // 1-arcsecond one, 0 when the tile didn't load.
    // Complexity: O(1). Thread-safety: never mutates state; safe to call concurrently.
    int PostsPerSide() const { return size; }

    // Posts holding the -32768 void sentinel, counted once while loading; 0 when the
    // tile didn't load. Complexity and thread-safety: as PostsPerSide.
    int VoidCount() const { return voidCount; }

    // The tile's south-west corner, as given to the constructor.
    // Complexity and thread-safety: as PostsPerSide.
    double SouthWestLatitudeDeg() const { return swLat; }
    double SouthWestLongitudeDeg() const { return swLon; }

    // The interpolation mode GetElevation answers with.
    // Complexity and thread-safety: as PostsPerSide.
    InterpolationMode Interpolation() const { return mode; }

    // The same loaded tile answering in another interpolation mode, without reading the
    // file again. The copy owns its own posts, so either sampler can outlive the other.
    // Complexity: O(posts) -- one copy of the tile in memory.
    // Thread-safety: never mutates this sampler; safe to call concurrently.
    RealElevationSampler WithInterpolationMode(InterpolationMode interpolationMode) const
    {
        RealElevationSampler copy = *this;
        copy.mode = interpolationMode;
        return copy;
    }

    // Every post as read from the file: row-major, row 0 the northern edge, column 0
    // the western edge, -32768 marking a void. Empty when the tile didn't load.
    // Complexity: O(1). Thread-safety: as PostsPerSide; the reference stays valid for
    // the sampler's lifetime.
    const std::vector<int16_t>& Posts() const { return data; }
    
    // SRTM .hgt files are EGM96-referenced -- orthometric (mean sea level) heights.
    VerticalDatum GetDatum() const override
    {
        return VerticalDatum::OrthometricMsl;
    }

    // Complexity: O(1) -- a fixed number of array lookups (1 nearest, 4 bilinear).
    // Thread-safety: never mutates state after construction, so safe for
    // concurrent calls from multiple threads once the file has finished loading.
    std::optional<double> virtual GetElevation(double latitudeDeg, double longitudeDeg)
    {
        if (!loadedSuccessfully) return std::nullopt;

        double topLat = swLat + 1.0;
        double rowF = (topLat - latitudeDeg) * (size - 1);
        double colF = (longitudeDeg - swLon) * (size - 1);

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
    int size = 0;
    std::vector<int16_t> data;
    InterpolationMode mode = InterpolationMode::Nearest;
    bool loadedSuccessfully = false;
    int voidCount = 0;
};