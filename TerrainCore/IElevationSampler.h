#pragma once
#include <cstdint>
#include <vector>
#include <optional>
#include <cmath>
#include "VerticalDatum.h"

enum class InterpolationMode
{
    Nearest,
    Bilinear
};

// What a sampler holds at a point. A void is a hole in the source itself -- the data was
// given, and says nothing is known there. Data not given is a place the sampler was never
// handed data for: off its tile, outside its block, or with no tile registered. The two
// call for different answers from a caller: nothing will fill a void, while data not given
// can be loaded and the question asked again.
enum class ElevationData
{
    Present,
    Void,
    NotGiven
};

struct ElevationSample
{
    ElevationData data = ElevationData::NotGiven;
    double elevationM = 0.0; // meaningful only when data is Present
};

class IElevationSampler {
public:
    virtual ~IElevationSampler() = default;

    // Complexity and thread-safety are per-implementation contracts (see each
    // concrete class below); every implementation here is O(1) and safe for
    // concurrent calls from multiple threads once construction has completed,
    // since none of them mutate state in GetElevation.
    std::optional<double> virtual GetElevation(double latitudeDeg, double longitudeDeg) = 0;

    // The elevation at a point, or why there is none: a void in the source, or data this
    // sampler was never given. Every sampler in this library says which; the default, for
    // one that only answers GetElevation, can't tell and calls every gap a void.
    // Complexity and thread-safety: as GetElevation.
    virtual ElevationSample Sample(double latitudeDeg, double longitudeDeg)
    {
        std::optional<double> elevationM = GetElevation(latitudeDeg, longitudeDeg);
        return elevationM.has_value() ? ElevationSample{ ElevationData::Present, *elevationM } : ElevationSample{ ElevationData::Void, 0.0 };
    }

    // Which vertical datum GetElevation's returned values are expressed in.
    // Never assumed by a caller -- declared once per sampler instance.
    virtual VerticalDatum GetDatum() const = 0;
};

// An elevation as GetElevation returns it: the value, or nothing for a void or data not given.
// Complexity: O(1). Thread-safety: pure function.
inline std::optional<double> ElevationOf(const ElevationSample& sample)
{
    if (sample.data != ElevationData::Present) return std::nullopt;
    return sample.elevationM;
}

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
        return ElevationOf(Sample(latitudeDeg, longitudeDeg));
    }

    // The grid holds no voids: anywhere off it is data not given.
    ElevationSample Sample(double latitudeDeg, double longitudeDeg) override {
        const ElevationSample notGiven{ ElevationData::NotGiven, 0.0 };

        // No grid, or a coordinate off it by more than a cell -- or not a number at all --
        // names no cell. Checked as doubles: casting such a value to an int would be undefined.
        if (grid.empty() || grid[0].empty()) return notGiven;
        if (!(latitudeDeg > -2.0 && latitudeDeg < grid.size() + 1.0 && longitudeDeg > -2.0 && longitudeDeg < grid[0].size() + 1.0)) return notGiven;

        if (mode == InterpolationMode::Bilinear)
        {
            int row0 = (int)floor(latitudeDeg);
            int col0 = (int)floor(longitudeDeg);
            int row1 = row0 + 1;
            int col1 = col0 + 1;

            if (row0 < 0 || row1 >= (int)grid.size() || col0 < 0 || col1 >= (int)grid[0].size())
            {
                return notGiven;
            }

            double fracRow = latitudeDeg - row0;
            double fracCol = longitudeDeg - col0;

            double v00 = grid[row0][col0];
            double v01 = grid[row0][col1];
            double v10 = grid[row1][col0];
            double v11 = grid[row1][col1];

            double top = v00 + (v01 - v00) * fracCol;
            double bottom = v10 + (v11 - v10) * fracCol;
            return ElevationSample{ ElevationData::Present, top + (bottom - top) * fracRow };
        }

        int row = (int)round(latitudeDeg);
        int col = (int)round(longitudeDeg);

        if (row < 0 || row >= (int)grid.size() || col < 0 || col >= (int)grid[0].size())
        {
            return notGiven;
        }

        return ElevationSample{ ElevationData::Present, grid[row][col] };
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
        return ElevationOf(Sample(latitudeDeg, longitudeDeg));
    }

    // The covering tile's own answer; where no tile is registered, data not given.
    ElevationSample Sample(double latitudeDeg, double longitudeDeg) override
    {
        for (auto& tile : tiles)
        {
            if (latitudeDeg >= tile.swLatDeg && latitudeDeg < tile.swLatDeg + 1.0 &&
                longitudeDeg >= tile.swLonDeg && longitudeDeg < tile.swLonDeg + 1.0)
            {
                return tile.sampler->Sample(latitudeDeg, longitudeDeg);
            }
        }
        return ElevationSample{ ElevationData::NotGiven, 0.0 };
    }

    // Assumes every registered tile shares one datum (true for tiles drawn from
    // the same dataset); reports the first tile's, or Unknown if none registered.
    VerticalDatum GetDatum() const override
    {
        if (tiles.empty()) return VerticalDatum::Unknown;
        return tiles.front().sampler->GetDatum();
    }
};

// Affine geometry of a raster block, as a raster-window reply describes it: the
// CENTRE of cell (0, 0) as origin, plus SIGNED per-row/per-column degree steps --
// never an assumed resolution, never a hardcoded 1x1 degree tile size. Cells are
// row-major. The row step's sign states row order explicitly: negative means row 0
// is the northern edge (the usual convention for real raster sources), positive
// means row 0 is the southern edge -- stated by the step itself, not left as an
// unwritten assumption the way MultiTileElevationSampler's 1 degree tile or
// RealElevationSampler's file-size-inferred square grid are.
struct RasterBlockGeometry
{
    double originCellCentreLatitudeDeg = 0.0;
    double originCellCentreLongitudeDeg = 0.0;
    double rowStepDeg = 0.0;
    double colStepDeg = 0.0;
    int rows = 0;
    int cols = 0;

    // A window into a larger raster: the block's first cell is cell (rowOffset, colOffset)
    // of the grid whose cell (0, 0) is centred on the origin. Every window of one raster
    // then finds a coordinate's cell with the very same arithmetic as the whole raster
    // does, so a query answered from a window reads exactly the values it would read from
    // the whole. 0 and 0 for a block that is its own grid.
    int rowOffset = 0;
    int colOffset = 0;
};

// Maps (lat, lon) to the row-major index, within the block, of the nearest cell centre,
// or nullopt outside the block. Rounding to the nearest centre is what a raster reply
// means by its origin; reading the origin as a cell's corner instead would shift every
// lookup by half a cell. Both raster-block samplers below use this one implementation,
// so they cannot disagree about which cell a coordinate falls in.
// Complexity: O(1). Thread-safety: pure function, safe to call concurrently.
inline std::optional<size_t> RasterBlockCellIndex(const RasterBlockGeometry& geometry, double latitudeDeg, double longitudeDeg)
{
    if (geometry.rowStepDeg == 0.0 || geometry.colStepDeg == 0.0) return std::nullopt;

    // Rounded as doubles and range-checked before any cast: a coordinate that is NaN,
    // infinite or far outside the block would otherwise be cast to an int it doesn't
    // fit, which is undefined.
    double row = round((latitudeDeg - geometry.originCellCentreLatitudeDeg) / geometry.rowStepDeg) - geometry.rowOffset;
    double col = round((longitudeDeg - geometry.originCellCentreLongitudeDeg) / geometry.colStepDeg) - geometry.colOffset;

    if (!(row >= 0 && row < geometry.rows && col >= 0 && col < geometry.cols)) return std::nullopt;

    return (size_t)row * (size_t)geometry.cols + (size_t)col;
}

// A view over a raster block someone else already holds -- one flat row-major
// elevation array plus a parallel per-cell validity array, the shape a raster reply
// arrives in -- read in place, with nothing copied. A validity flag of 0 is a void:
// the elevation stored beside it is never read, so no sentinel value is invented or
// trusted, and the per-cell validity is carried through rather than flattened away.
//
// Non-owning: both arrays must hold at least rows * cols elements and stay alive for
// as long as the sampler is used, and must not be modified while a query (or a
// viewshed built on this sampler) is running.
//
// Elevations are read as double and validity flags as bytes. A buffer of another
// element type (float elevations, say) would need this class templated on it, or one
// conversion at this boundary; it isn't templated because nothing here supplies one.
class RasterBlockViewElevationSampler : public IElevationSampler
{
public:
    RasterBlockViewElevationSampler(const double* elevationsM, const uint8_t* validity, RasterBlockGeometry geometryIn, VerticalDatum datumIn)
        : elevations(elevationsM), valid(validity), geometry(geometryIn), datum(datumIn)
    {
    }

    virtual ~RasterBlockViewElevationSampler() = default;

    // Complexity: O(1) -- affine coordinate-to-index math plus two array reads.
    // Thread-safety: never mutates state, so safe for concurrent calls from multiple
    // threads, provided nothing modifies the viewed arrays meanwhile.
    std::optional<double> virtual GetElevation(double latitudeDeg, double longitudeDeg)
    {
        return ElevationOf(Sample(latitudeDeg, longitudeDeg));
    }

    // Outside the block, data not given; a cell flagged invalid, a void.
    ElevationSample Sample(double latitudeDeg, double longitudeDeg) override
    {
        if (elevations == nullptr || valid == nullptr) return ElevationSample{ ElevationData::NotGiven, 0.0 };

        std::optional<size_t> index = RasterBlockCellIndex(geometry, latitudeDeg, longitudeDeg);
        if (!index.has_value()) return ElevationSample{ ElevationData::NotGiven, 0.0 };
        if (valid[*index] == 0) return ElevationSample{ ElevationData::Void, 0.0 };

        return ElevationSample{ ElevationData::Present, elevations[*index] };
    }

    VerticalDatum GetDatum() const override { return datum; }

    const RasterBlockGeometry& Geometry() const { return geometry; }

private:
    const double* elevations = nullptr;
    const uint8_t* valid = nullptr;
    RasterBlockGeometry geometry;
    VerticalDatum datum = VerticalDatum::Unknown;
};

// The same raster block, owned -- for a caller that doesn't already hold one (tests,
// or data assembled cell by cell). It keeps the block in the view's flat row-major
// layout and answers through the same RasterBlockCellIndex. Built from per-row
// std::optional cells for convenience, copied once at construction; a row shorter
// than the first is treated as void past its end.
class RasterBlockElevationSampler : public IElevationSampler
{
public:
    // rowOffset and colOffset: see RasterBlockGeometry -- the block as a window into a larger raster.
    RasterBlockElevationSampler(const std::vector<std::vector<std::optional<double>>>& block, double originCellCentreLatitudeDeg, double originCellCentreLongitudeDeg, double rowStepDeg, double colStepDeg, VerticalDatum datumIn, int rowOffset = 0, int colOffset = 0)
    {
        geometry.originCellCentreLatitudeDeg = originCellCentreLatitudeDeg;
        geometry.originCellCentreLongitudeDeg = originCellCentreLongitudeDeg;
        geometry.rowStepDeg = rowStepDeg;
        geometry.colStepDeg = colStepDeg;
        geometry.rowOffset = rowOffset;
        geometry.colOffset = colOffset;
        geometry.rows = (int)block.size();
        geometry.cols = geometry.rows > 0 ? (int)block[0].size() : 0;
        datum = datumIn;

        size_t cellCount = (size_t)geometry.rows * (size_t)geometry.cols;
        elevations.assign(cellCount, 0.0);
        valid.assign(cellCount, 0);
        for (int row = 0; row < geometry.rows; row++)
        {
            for (int col = 0; col < geometry.cols && col < (int)block[row].size(); col++)
            {
                if (!block[row][col].has_value()) continue;
                size_t index = (size_t)row * (size_t)geometry.cols + (size_t)col;
                elevations[index] = *block[row][col];
                valid[index] = 1;
            }
        }
    }

    virtual ~RasterBlockElevationSampler() = default;

    // Complexity: O(1) -- affine coordinate-to-index math plus two vector reads.
    // Thread-safety: never mutates state, so safe for concurrent calls from
    // multiple threads once construction has completed.
    std::optional<double> virtual GetElevation(double latitudeDeg, double longitudeDeg)
    {
        return ElevationOf(Sample(latitudeDeg, longitudeDeg));
    }

    // Outside the block, data not given; a cell with no value, a void.
    ElevationSample Sample(double latitudeDeg, double longitudeDeg) override
    {
        std::optional<size_t> index = RasterBlockCellIndex(geometry, latitudeDeg, longitudeDeg);
        if (!index.has_value()) return ElevationSample{ ElevationData::NotGiven, 0.0 };
        if (valid[*index] == 0) return ElevationSample{ ElevationData::Void, 0.0 };

        return ElevationSample{ ElevationData::Present, elevations[*index] };
    }

    VerticalDatum GetDatum() const override { return datum; }

    int Rows() const { return geometry.rows; }
    int Cols() const { return geometry.cols; }

private:
    std::vector<double> elevations;
    std::vector<uint8_t> valid;
    RasterBlockGeometry geometry;
    VerticalDatum datum = VerticalDatum::Unknown;
};
