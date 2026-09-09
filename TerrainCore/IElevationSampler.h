#pragma once
#include <vector>
#include <optional>
#include <cmath>
#include "VerticalDatum.h"

enum class InterpolationMode
{
    Nearest,
    Bilinear
};

class IElevationSampler {
public:
    virtual ~IElevationSampler() = default;

    // Complexity and thread-safety are per-implementation contracts (see each
    // concrete class below); every implementation here is O(1) and safe for
    // concurrent calls from multiple threads once construction has completed,
    // since none of them mutate state in GetElevation.
    std::optional<double> virtual GetElevation(double latitudeDeg, double longitudeDeg) = 0;

    // Which vertical datum GetElevation's returned values are expressed in.
    // Never assumed by a caller -- declared once per sampler instance.
    virtual VerticalDatum GetDatum() const = 0;
};

class FakeElevationSampler : public IElevationSampler {
public:
    std::vector<std::vector<double>> grid;
    InterpolationMode mode = InterpolationMode::Nearest;
    VerticalDatum datum = VerticalDatum::Unknown;

    FakeElevationSampler(std::vector<std::vector<double>> initialGrid, InterpolationMode interpolationMode = InterpolationMode::Nearest, VerticalDatum datumIn = VerticalDatum::Unknown)
    {
        grid = initialGrid;
        mode = interpolationMode;
        datum = datumIn;
    }

    virtual ~FakeElevationSampler() = default;

    // Complexity: O(1) -- a fixed number of grid lookups (1 nearest, 4 bilinear).
    // Thread-safety: internally synchronised in the sense that matters here --
    // GetElevation never mutates grid/mode/datum, so concurrent calls from
    // multiple threads are safe as long as no thread is concurrently mutating
    // the public grid field itself.
    std::optional<double> virtual GetElevation(double latitudeDeg, double longitudeDeg) {
        if (mode == InterpolationMode::Bilinear)
        {
            int row0 = (int)floor(latitudeDeg);
            int col0 = (int)floor(longitudeDeg);
            int row1 = row0 + 1;
            int col1 = col0 + 1;

            if (row0 < 0 || row1 >= (int)grid.size() || col0 < 0 || col1 >= (int)grid[0].size())
            {
                return std::nullopt;
            }

            double fracRow = latitudeDeg - row0;
            double fracCol = longitudeDeg - col0;

            double v00 = grid[row0][col0];
            double v01 = grid[row0][col1];
            double v10 = grid[row1][col0];
            double v11 = grid[row1][col1];

            double top = v00 + (v01 - v00) * fracCol;
            double bottom = v10 + (v11 - v10) * fracCol;
            return top + (bottom - top) * fracRow;
        }

        int row = (int)round(latitudeDeg);
        int col = (int)round(longitudeDeg);

        if (row < 0 || row >= (int)grid.size() || col < 0 || col >= (int)grid[0].size())
        {
            return std::nullopt;
        }

        return grid[row][col];
    }

    VerticalDatum GetDatum() const override { return datum; }
};

struct TileEntry
{
    double swLatDeg;
    double swLonDeg;
    IElevationSampler* sampler; // non-owning -- caller must keep the referenced sampler alive for at least as long as this entry is registered
};

// Selects a tile by its registered south-west corner, then passes the query's
// WORLD latitude/longitude straight through to that tile's sampler, untranslated.
// Only correct if the sub-sampler itself can place a world coordinate on its own
// grid (RealElevationSampler does, via its own stored corner); a sampler that
// treats coordinates as raw local indices is not usable here without adapting it.
class MultiTileElevationSampler : public IElevationSampler
{
public:
    std::vector<TileEntry> tiles;

    // Complexity: O(1) amortised (std::vector::push_back).
    // Thread-safety: caller-synchronised. Mutates tiles, so a caller registering
    // tiles from multiple threads (or concurrently with a GetElevation call) must
    // provide its own synchronisation.
    void AddTile(double swLatDeg, double swLonDeg, IElevationSampler& sampler)
    {
        tiles.push_back(TileEntry{ swLatDeg, swLonDeg, &sampler });
    }

    virtual ~MultiTileElevationSampler() = default;

    // Complexity: O(tiles registered) -- a linear scan to find the covering tile,
    // plus whatever the selected tile's own GetElevation costs.
    // Thread-safety: safe for concurrent GetElevation calls from multiple threads
    // as long as no thread is concurrently calling AddTile; never internally
    // synchronised against that.
    std::optional<double> virtual GetElevation(double latitudeDeg, double longitudeDeg)
    {
        for (auto& tile : tiles)
        {
            if (latitudeDeg >= tile.swLatDeg && latitudeDeg < tile.swLatDeg + 1.0 &&
                longitudeDeg >= tile.swLonDeg && longitudeDeg < tile.swLonDeg + 1.0)
            {
                return tile.sampler->GetElevation(latitudeDeg, longitudeDeg);
            }
        }
        return std::nullopt;
    }

    // Assumes every registered tile shares one datum (true for tiles drawn from
    // the same dataset); reports the first tile's, or Unknown if none registered.
    VerticalDatum GetDatum() const override
    {
        if (tiles.empty()) return VerticalDatum::Unknown;
        return tiles.front().sampler->GetDatum();
    }
};

// A view over an already-resident raster block, described by its own affine
// geometry (an origin corner plus SIGNED per-row/per-column degree steps) --
// never an assumed resolution, never a hardcoded 1x1 degree tile size. This
// is the shape a real host hands back from a raster-window request: the
// block is already in memory, and mapping (lat, lon) to a cell is index
// arithmetic, not a file read.
//
// The row step's sign states row order explicitly -- negative means row 0 is
// the northern edge (the usual convention for real raster sources), positive
// means row 0 is the southern edge. Either way it is stated by the step
// itself, not left as an unwritten assumption the way MultiTileElevationSampler's
// 1 degree tile or RealElevationSampler's file-size-inferred square grid were.
//
// A cell's std::optional<double> carries validity directly -- no invented
// sentinel value -- because a real host reply already distinguishes valid
// from void per sample rather than encoding it into the number itself.
class RasterBlockElevationSampler : public IElevationSampler
{
public:
    RasterBlockElevationSampler(std::vector<std::vector<std::optional<double>>> blockIn, double originLatitudeDeg, double originLongitudeDeg, double rowStepDeg, double colStepDeg, VerticalDatum datumIn)
    {
        block = std::move(blockIn);
        originLat = originLatitudeDeg;
        originLon = originLongitudeDeg;
        rowStep = rowStepDeg;
        colStep = colStepDeg;
        datum = datumIn;
        rows = (int)block.size();
        cols = rows > 0 ? (int)block[0].size() : 0;
    }

    virtual ~RasterBlockElevationSampler() = default;

    // Complexity: O(1) -- affine coordinate-to-index math plus one vector lookup.
    // Thread-safety: never mutates state, so safe for concurrent calls from
    // multiple threads once construction has completed.
    std::optional<double> virtual GetElevation(double latitudeDeg, double longitudeDeg)
    {
        if (rowStep == 0.0 || colStep == 0.0) return std::nullopt;

        double rowF = (latitudeDeg - originLat) / rowStep;
        double colF = (longitudeDeg - originLon) / colStep;

        int row = (int)round(rowF);
        int col = (int)round(colF);

        if (row < 0 || row >= rows || col < 0 || col >= cols) return std::nullopt;

        return block[row][col];
    }

    VerticalDatum GetDatum() const override { return datum; }

    int Rows() const { return rows; }
    int Cols() const { return cols; }

private:
    std::vector<std::vector<std::optional<double>>> block;
    double originLat = 0.0;
    double originLon = 0.0;
    double rowStep = 0.0;
    double colStep = 0.0;
    VerticalDatum datum = VerticalDatum::Unknown;
    int rows = 0;
    int cols = 0;
};
