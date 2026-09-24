#pragma once
#include <stdint.h>
#include <wchar.h>

// TerrainEngineApi -- the terrain engine as a Windows x64 DLL, callable from .NET through
// P/Invoke or from C. Nothing C++-specific crosses this boundary.
//
// Conventions that hold for every function below unless its own comment says otherwise:
//
// - Results. Functions return an int32_t result code: TE_OK (0) or one of the
//   TE_ERROR_* codes. On failure, te_last_error_message() says what went wrong in plain
//   English, ready to show to a user, and every out parameter is left zeroed or NULL.
// - Nothing throws. Every function catches internally; running out of memory comes back
//   as TE_ERROR_OUT_OF_MEMORY, anything unexpected as TE_ERROR_INTERNAL.
// - Handles. A tile is identified by a te_tile number, never a pointer. 0 is never a
//   valid handle, and numbers are never reused, so a made-up, closed or zero handle is
//   reported as TE_ERROR_INVALID_HANDLE instead of reading freed memory.
// - Memory. Strings passed in are borrowed for the duration of the call. Strings handed
//   out (te_engine_version, te_engine_commit, te_last_error_message) are owned by the
//   DLL: the caller must not free them. Arrays handed out through an out pointer are
//   allocated by the DLL and belong to the caller, who must release each exactly once
//   with te_free -- never with the caller's own allocator.
// - Structs are plain C, laid out with fixed-size fields and explicit padding so they
//   map field for field onto .NET's sequential layout. Booleans are int32_t, 0 or 1.
// - Thread-safety. Every function is safe to call concurrently from any number of
//   threads, including on the same tile. A tile closed while another call is still
//   using it stays alive until that call returns.
// - Determinism. For the same inputs, answers are bit-identical to the static engine the
//   command-line tool links: the same source, compiled with /fp:precise.

#ifdef __cplusplus
extern "C" {
#endif

#ifdef TERRAINENGINEAPI_EXPORTS
#define TE_API __declspec(dllexport)
#else
#define TE_API __declspec(dllimport)
#endif

// ---------------------------------------------------------------------------------------
// Result codes

#define TE_OK                       0
#define TE_ERROR_INVALID_ARGUMENT   1  // an input outside what the function allows
#define TE_ERROR_INVALID_HANDLE     2  // a tile handle that isn't open
#define TE_ERROR_LOAD_FAILED        3  // the .hgt file is missing, empty, unreadable or not a whole tile
#define TE_ERROR_OUTSIDE_TILE       4  // a point outside the open tile's square degree
#define TE_ERROR_CANCELLED          5  // a progress callback asked a viewshed to stop
#define TE_ERROR_OUT_OF_MEMORY      6
#define TE_ERROR_INTERNAL           7

// Interpolation modes
#define TE_INTERPOLATION_NEAREST    0
#define TE_INTERPOLATION_BILINEAR   1

// Why a line of sight or Fresnel clearance has, or hasn't, a confident answer
#define TE_STATUS_OK                             0
#define TE_STATUS_EMPTY_OR_SINGLE_SAMPLE_PROFILE 1
#define TE_STATUS_ENDPOINT_MISSING               2  // the ground under the observer or the target is void
#define TE_STATUS_VOID_IN_PROFILE                3  // the path crosses missing data
#define TE_STATUS_DATUM_REJECTED                 4
#define TE_STATUS_NOTHING_EVALUATED              5  // Fresnel only: no point between the ends to evaluate
#define TE_STATUS_INVALID_INPUT                  6  // an input outside the engine's domain; the functions here refuse
                                                    // those with TE_ERROR_INVALID_ARGUMENT first, so it isn't returned

// Kind of terrain feature at a blocking point
#define TE_FEATURE_UNKNOWN          0
#define TE_FEATURE_LOCAL_PEAK       1
#define TE_FEATURE_RISING_SLOPE     2
#define TE_FEATURE_FALLING_SLOPE    3
#define TE_FEATURE_PLATEAU          4

// Viewshed cell states
#define TE_CELL_NOT_COVERED         0  // no ray reached the cell
#define TE_CELL_DEGRADED            1  // no confident answer: the line to it crosses missing data
#define TE_CELL_VISIBLE             2
#define TE_CELL_NOT_VISIBLE         3

// Viewshed algorithms
#define TE_ALGORITHM_FAST           0
#define TE_ALGORITHM_NAIVE          1

// Input limits, so a request can't exhaust memory
#define TE_MAX_PATH_SAMPLES             1000000
#define TE_MAX_VIEWSHED_CELLS_PER_SIDE  4001

// ---------------------------------------------------------------------------------------
// Version and errors

// The engine version this DLL was built as, e.g. "1.0.0".
// Ownership: returns a static, NUL-terminated string owned by the DLL. It stays valid
// for the life of the process; the caller must not free or modify it.
// Complexity: O(1). Thread-safety: safe to call concurrently from any thread.
TE_API const char* te_engine_version(void);

// The git commit the DLL was built from, or "unknown" for a build that wasn't given one.
// Ownership, complexity and thread-safety: as te_engine_version.
TE_API const char* te_engine_commit(void);

// A plain-English description of the most recent failure on the calling thread, or ""
// when the most recent call on this thread succeeded. UTF-8.
// Ownership: owned by the DLL; valid until the next call into this DLL on the same
// thread. Copy it if it must live longer.
// Complexity: O(1). Thread-safety: each thread sees only its own message.
TE_API const char* te_last_error_message(void);

// Releases an array handed out by this DLL. te_free(NULL) does nothing.
// Complexity: O(1). Thread-safety: safe to call concurrently, once per array.
TE_API void te_free(void* pointer);

// ---------------------------------------------------------------------------------------
// Tiles

typedef uint64_t te_tile;

typedef struct te_tile_info
{
    double south_west_latitude_deg;
    double south_west_longitude_deg;
    double north_east_latitude_deg;
    double north_east_longitude_deg;
    int32_t posts_per_side;              // 1201 for a 3-arcsecond tile, 3601 for a 1-arcsecond one
    int32_t void_count;                  // posts holding no data
    double post_spacing_arcsec;
    double post_spacing_north_south_m;   // great-circle distance between neighbouring posts in a column
    double post_spacing_east_west_m;     // the same along the tile's middle row
} te_tile_info;

// Opens an SRTM .hgt tile and returns a handle to it. path is UTF-16. The south-west
// corner is the one the file name states (N36W112 -> 36, -112) and must lie within
// latitude [-90, 89] and longitude [-180, 179]. A missing, empty, unreadable or
// incomplete file -- a download cut short -- is TE_ERROR_LOAD_FAILED, never a tile of voids.
// Complexity: O(posts) -- reads the file once and keeps it in memory, twice (one copy
// per interpolation mode): about 5.5 MB for a 3-arcsecond tile, 52 MB for 1-arcsecond.
// Thread-safety: safe to call concurrently.
TE_API int32_t te_tile_open(const wchar_t* path, double south_west_latitude_deg, double south_west_longitude_deg, te_tile* out_tile);

// Closes a tile. Calls still running on it finish first; afterwards the handle is
// invalid. Complexity: O(1), plus freeing the tile. Thread-safety: safe to call concurrently.
TE_API int32_t te_tile_close(te_tile tile);

// The tile's extent, grid size, resolution and void count.
// Complexity: O(1). Thread-safety: safe to call concurrently.
TE_API int32_t te_tile_get_info(te_tile tile, te_tile_info* out_info);

// Elevation, in metres above mean sea level, at a point inside the tile. A void answers
// TE_OK with *out_has_value = 0; a point outside the tile is TE_ERROR_OUTSIDE_TILE.
// Complexity: O(1). Thread-safety: safe to call concurrently.
TE_API int32_t te_tile_get_elevation(te_tile tile, double latitude_deg, double longitude_deg, int32_t interpolation, double* out_elevation_m, int32_t* out_has_value);

// Every post of the tile, for drawing it: *out_elevations_m and *out_valid each hold
// posts_per_side * posts_per_side entries, row-major, row 0 the northern edge and
// column 0 the western edge. A post with out_valid 0 holds no data; its elevation reads 0.
// Ownership: both arrays belong to the caller; release each with te_free.
// Complexity: O(posts). Thread-safety: safe to call concurrently.
TE_API int32_t te_tile_copy_posts(te_tile tile, float** out_elevations_m, uint8_t** out_valid, int32_t* out_posts_per_side);

// ---------------------------------------------------------------------------------------
// Distances

// Great-circle distance between two points, in metres.
// Complexity: O(1). Thread-safety: safe to call concurrently.
TE_API int32_t te_distance_m(double latitude1_deg, double longitude1_deg, double latitude2_deg, double longitude2_deg, double* out_distance_m);

// The spacing in degrees the engine uses for a spacing in metres -- the value to give
// the command-line tool to reproduce a query made in metres.
// Complexity: O(1). Thread-safety: safe to call concurrently.
TE_API int32_t te_spacing_m_to_deg(double spacing_m, double* out_spacing_deg);

// ---------------------------------------------------------------------------------------
// Profile, line of sight and Fresnel clearance along one path

typedef struct te_path_query
{
    double observer_latitude_deg;
    double observer_longitude_deg;
    double observer_height_above_ground_m;
    double target_latitude_deg;
    double target_longitude_deg;
    double target_height_above_ground_m;
    double spacing_m;          // sample spacing along the path, > 0
    double refraction_k;       // effective Earth radius factor, > 0; 4/3 is standard atmosphere
    double frequency_mhz;      // > 0 also computes Fresnel clearance; 0 skips it
    int32_t interpolation;     // TE_INTERPOLATION_*
    int32_t reserved;          // set to 0
} te_path_query;

typedef struct te_path_result
{
    double spacing_deg;               // the spacing passed to the engine (see te_spacing_m_to_deg)
    double total_distance_m;          // great-circle length of the path
    double observer_eye_height_m;     // metres above mean sea level; valid when eye_heights_known
    double target_eye_height_m;
    int32_t eye_heights_known;        // 0 when the ground under either end is void
    int32_t sample_count;

    // Line of sight
    int32_t los_status;               // TE_STATUS_*; is_visible and the blocking fields mean something only when TE_STATUS_OK
    int32_t is_visible;
    int32_t has_blocking_point;
    int32_t blocking_feature;         // TE_FEATURE_*
    int32_t blocking_sample_index;    // index into the samples, -1 without a blocking point
    int32_t reserved0;
    double blocking_latitude_deg;
    double blocking_longitude_deg;
    double blocking_elevation_m;      // metres above mean sea level
    double blocking_distance_m;       // from the observer
    double clearance_deficit_m;       // how far the sight line falls short of clearing the blocking point

    // Fresnel clearance, when frequency_mhz > 0
    int32_t fresnel_computed;
    int32_t fresnel_status;           // TE_STATUS_*
    int32_t has_worst_point;
    int32_t worst_sample_index;       // -1 without a worst point
    double min_clearance_fraction;    // >= 1 clear, 0..1 partly obstructed, < 0 the sight line itself is blocked
    double wavelength_m;
} te_path_result;

typedef struct te_path_sample
{
    double latitude_deg;
    double longitude_deg;
    double distance_m;                // from the observer, along the great circle
    double elevation_m;               // metres above mean sea level; valid when has_elevation
    int32_t has_elevation;            // 0 where the data is void -- draw a gap, never a zero
    int32_t reserved;
    double curvature_corrected_elevation_m; // elevation raised by the Earth's curvature under refraction_k; valid when has_elevation
    double sight_line_height_m;       // valid when the result's eye_heights_known
    double first_fresnel_radius_m;    // 0 at the ends or when Fresnel wasn't computed
} te_path_sample;

// Samples the terrain between observer and target, decides line of sight, and -- when
// asked -- Fresnel clearance, all over the one profile, with heights above ground. The
// samples carry everything needed to chart the result: terrain, curvature-corrected
// terrain, sight line and Fresnel radius. Both points must lie inside the tile.
// Ownership: *out_samples (result.sample_count entries) belongs to the caller; release
// it with te_free.
// Complexity: O(samples) = O(path length / spacing), at most TE_MAX_PATH_SAMPLES.
// Thread-safety: safe to call concurrently.
TE_API int32_t te_analyze_path(te_tile tile, const te_path_query* query, te_path_result* out_result, te_path_sample** out_samples);

// ---------------------------------------------------------------------------------------
// Viewshed

typedef struct te_viewshed_query
{
    double observer_latitude_deg;
    double observer_longitude_deg;
    double observer_height_above_ground_m;
    double radius_km;          // > 0
    double spacing_m;          // grid cell size, > 0
    double refraction_k;       // > 0
    int32_t interpolation;     // TE_INTERPOLATION_*
    int32_t algorithm;         // TE_ALGORITHM_*
    double target_height_above_ground_m;  // each cell asks about a target this high above its ground; 0 = the ground itself
} te_viewshed_query;

typedef struct te_viewshed_grid
{
    int32_t rows;
    int32_t cols;
    int32_t observer_row;
    int32_t observer_col;
    double spacing_deg;                 // row step, passed to the engine
    double col_step_deg;                // column step, widened for latitude so cells are square on the ground
    double south_west_cell_latitude_deg;  // centre of cell (row 0, col 0); rows go north, columns east
    double south_west_cell_longitude_deg;
} te_viewshed_grid;

// Called while a viewshed runs, on the thread that called te_viewshed, with the
// fraction done (0 to 1). Return 0 to continue, anything else to stop. It must not
// call back into a viewshed on the same thread, and must return promptly.
typedef int32_t (*te_progress_callback)(double fraction_done, void* user_data);

// Which cells within radius_km of the observer are visible. The grid is square, with
// the observer at its centre cell; *out_cells holds rows * cols TE_CELL_* values,
// row-major, row 0 the southernmost row (so a north-up picture draws the last row
// first). progress may be NULL. A stopped run is TE_ERROR_CANCELLED with no cells.
// Rejected: an observer outside the tile or within 0.1 degree of a pole, and grids over
// TE_MAX_VIEWSHED_CELLS_PER_SIDE cells across.
// Ownership: *out_cells belongs to the caller; release it with te_free.
// Complexity: fast O(rows + cols) rays of O(radius / spacing) samples each; naive
// O(rows * cols) paths of the same length -- minutes for 30 km at 30 m.
// Thread-safety: safe to call concurrently; the callback runs on the calling thread.
TE_API int32_t te_viewshed(te_tile tile, const te_viewshed_query* query, te_progress_callback progress, void* user_data, te_viewshed_grid* out_grid, uint8_t** out_cells);

#ifdef __cplusplus
}
#endif
