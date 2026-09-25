#pragma once
#include <cstdint>
#include <vector>
#include <fstream>
#include <cmath>
#include <optional>
#include <string>
#include "IElevationSampler.h"
#include <filesystem>

// A rectangle of a tile's posts, by row (0 the northern edge) and column (0 the western),
// both ends included.
struct PostWindow
{
    int firstRow = 0;
    int lastRow = -1;
    int firstCol = 0;
    int lastCol = -1;

    bool Empty() const { return lastRow < firstRow || lastCol < firstCol; }
};

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
        window = PostWindow{ 0, size - 1, 0, size - 1 };
    }

    // The posts Sample reads for points inside a box of latitude and longitude, clipped to
    // the tile: the nearest post to each point, or with bilinear interpolation the four
    // around it. Worked out with Sample's own arithmetic, so a window of exactly these
    // posts answers every point in the box as the whole tile does. Empty when the box
    // misses the tile, or the tile didn't load.
    // Complexity: O(1). Thread-safety: never mutates state; safe to call concurrently.
    PostWindow PostsCovering(double southLatDeg, double northLatDeg, double westLonDeg, double eastLonDeg) const
    {
        PostWindow covering;
        if (!loadedSuccessfully) return covering;
        auto rowF = [&](double latitudeDeg) { return (swLat + 1.0 - latitudeDeg) * (size - 1); };
        auto colF = [&](double longitudeDeg) { return (longitudeDeg - swLon) * (size - 1); };
        auto clamp = [&](double post) { return (int)(std::max)(-1.0, (std::min)((double)size, post)); };

        if (mode == InterpolationMode::Bilinear)
        {
            covering = PostWindow{ clamp(std::floor(rowF(northLatDeg))), clamp(std::floor(rowF(southLatDeg)) + 1),
                                   clamp(std::floor(colF(westLonDeg))), clamp(std::floor(colF(eastLonDeg)) + 1) };
        }
        else
        {
            covering = PostWindow{ clamp(std::round(rowF(northLatDeg))), clamp(std::round(rowF(southLatDeg))),
                                   clamp(std::round(colF(westLonDeg))), clamp(std::round(colF(eastLonDeg))) };
        }
        covering.firstRow = (std::max)(covering.firstRow, 0);
        covering.firstCol = (std::max)(covering.firstCol, 0);
        covering.lastRow = (std::min)(covering.lastRow, size - 1);
        covering.lastCol = (std::min)(covering.lastCol, size - 1);
        return covering;
    }

    // The same tile loaded for one region only: a sampler holding just the posts in the
    // window, clipped to the tile, which answers every point it needs no other post for
    // exactly as the whole tile does, and every other point on the tile as data not given.
    // Complexity: O(posts in the window) -- they are copied. Thread-safety: never mutates
    // this sampler; safe to call concurrently.
    RealElevationSampler Window(PostWindow posts) const
    {
        posts.firstRow = (std::max)(posts.firstRow, window.firstRow);
        posts.firstCol = (std::max)(posts.firstCol, window.firstCol);
        posts.lastRow = (std::min)(posts.lastRow, window.lastRow);
        posts.lastCol = (std::min)(posts.lastCol, window.lastCol);
        RealElevationSampler part(*this, posts);
        if (!posts.Empty()) part.data.reserve((size_t)(posts.lastRow - posts.firstRow + 1) * (size_t)(posts.lastCol - posts.firstCol + 1));
        for (int row = posts.firstRow; row <= posts.lastRow; row++)
        {
            for (int col = posts.firstCol; col <= posts.lastCol; col++)
            {
                int16_t value = data[PostIndex(row, col)];
                part.data.push_back(value);
                if (value == -32768) part.voidCount++;
            }
        }
        return part;
    }

    // Which of the tile's posts this sampler holds: all of them, or a window's.
    // Complexity and thread-safety: as PostsPerSide.
    PostWindow HeldPosts() const { return window; }

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
    // the western edge, -32768 marking a void. Empty when the tile didn't load. For a
    // Window, only its posts, row-major within it (HeldPosts says which).
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
        return ElevationOf(Sample(latitudeDeg, longitudeDeg));
    }

    // A post holding -32768 is a void in the source. A point whose post (or, for bilinear,
    // any of its four posts) lies off the tile is data this sampler wasn't given -- and so is
    // every point of a tile that didn't load.
    // Complexity and thread-safety: as GetElevation.
    ElevationSample Sample(double latitudeDeg, double longitudeDeg) override
    {
        const ElevationSample notGiven{ ElevationData::NotGiven, 0.0 };
        const ElevationSample hole{ ElevationData::Void, 0.0 };
        if (!loadedSuccessfully) return notGiven;

        double topLat = swLat + 1.0;
        double rowF = (topLat - latitudeDeg) * (size - 1);
        double colF = (longitudeDeg - swLon) * (size - 1);

        // Off the tile by more than a post -- or not a number at all -- names no post.
        // Checked as doubles: casting such a value to an int would be undefined.
        if (!(rowF > -2.0 && rowF < size + 1.0 && colF > -2.0 && colF < size + 1.0)) return notGiven;

        if (mode == InterpolationMode::Bilinear)
        {
            int row0 = (int)floor(rowF);
            int col0 = (int)floor(colF);
            int row1 = row0 + 1;
            int col1 = col0 + 1;

            if (!Holds(row0, col0) || !Holds(row1, col1))
            {
                return notGiven;
            }

            int16_t v00 = data[PostIndex(row0, col0)];
            int16_t v01 = data[PostIndex(row0, col1)];
            int16_t v10 = data[PostIndex(row1, col0)];
            int16_t v11 = data[PostIndex(row1, col1)];

            if (v00 == -32768 || v01 == -32768 || v10 == -32768 || v11 == -32768)
            {
                return hole;
            }

            double fracRow = rowF - row0;
            double fracCol = colF - col0;

            double top = v00 + (v01 - v00) * fracCol;
            double bottom = v10 + (v11 - v10) * fracCol;
            return ElevationSample{ ElevationData::Present, top + (bottom - top) * fracRow };
        }

        int row = (int)round(rowF);
        int col = (int)round(colF);

        if (!Holds(row, col))
        {
            return notGiven;
        }

        int16_t value = data[PostIndex(row, col)];

        if (value == -32768)
        {
            return hole;
        }

        return ElevationSample{ ElevationData::Present, (double)value };
    }

private:
    // The same tile, holding none of its posts yet: Window fills in the window's. Copying
    // the whole tile first would hold all of it until the copy was cut down.
    RealElevationSampler(const RealElevationSampler& tile, PostWindow posts)
        : swLat(tile.swLat), swLon(tile.swLon), size(tile.size), window(posts), mode(tile.mode), loadedSuccessfully(tile.loadedSuccessfully)
    {
    }

    // Whether this sampler holds the post at a tile row and column, and where in data.
    bool Holds(int row, int col) const
    {
        return row >= window.firstRow && row <= window.lastRow && col >= window.firstCol && col <= window.lastCol;
    }
    size_t PostIndex(int row, int col) const
    {
        return (size_t)(row - window.firstRow) * (size_t)(window.lastCol - window.firstCol + 1) + (size_t)(col - window.firstCol);
    }

    double swLat;
    double swLon;
    int size = 0;
    std::vector<int16_t> data;          // the held posts, row-major within the window
    PostWindow window;                  // which of the tile's posts data holds
    InterpolationMode mode = InterpolationMode::Nearest;
    bool loadedSuccessfully = false;
    int voidCount = 0;
};