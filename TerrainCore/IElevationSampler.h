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
};

// Maps (lat, lon) to the row-major index of the nearest cell centre, or nullopt
// outside the block. Rounding to the nearest centre is what a raster reply means by
// its origin; reading the origin as a cell's corner instead would shift every lookup
// by half a cell. Both raster-block samplers below use this one implementation, so
// they cannot disagree about which cell a coordinate falls in.
// Complexity: O(1). Thread-safety: pure function, safe to call concurrently.
inline std::optional<size_t> RasterBlockCellIndex(const RasterBlockGeometry& geometry, double latitudeDeg, double longitudeDeg)
{
    if (geometry.rowStepDeg == 0.0 || geometry.colStepDeg == 0.0) return std::nullopt;

    int row = (int)round((latitudeDeg - geometry.originCellCentreLatitudeDeg) / geometry.rowStepDeg);
    int col = (int)round((longitudeDeg - geometry.originCellCentreLongitudeDeg) / geometry.colStepDeg);

    if (row < 0 || row >= geometry.rows || col < 0 || col >= geometry.cols) return std::nullopt;

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
        if (elevations == nullptr || valid == nullptr) return std::nullopt;

        std::optional<size_t> index = RasterBlockCellIndex(geometry, latitudeDeg, longitudeDeg);
        if (!index.has_value() || valid[*index] == 0) return std::nullopt;

        return elevations[*index];
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
    RasterBlockElevationSampler(const std::vector<std::vector<std::optional<double>>>& block, double originCellCentreLatitudeDeg, double originCellCentreLongitudeDeg, double rowStepDeg, double colStepDeg, VerticalDatum datumIn)
    {
        geometry.originCellCentreLatitudeDeg = originCellCentreLatitudeDeg;
        geometry.originCellCentreLongitudeDeg = originCellCentreLongitudeDeg;
        geometry.rowStepDeg = rowStepDeg;
        geometry.colStepDeg = colStepDeg;
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
        std::optional<size_t> index = RasterBlockCellIndex(geometry, latitudeDeg, longitudeDeg);
        if (!index.has_value() || valid[*index] == 0) return std::nullopt;

        return elevations[*index];
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
