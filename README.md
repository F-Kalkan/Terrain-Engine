# Terrain Line-of-Sight and Viewshed Engine

## Why this exists

"Can point A see point B, given the terrain in between?" comes up in a lot of
practical contexts: planning where a radio or telecom tower needs line of sight to
another tower, choosing an observation/camera position that covers the most ground,
or checking whether a ridge blocks a sightline before you hike somewhere to find out.
This library answers that question against real elevation data, accounting for how
far the ground actually drops away due to Earth's curvature — a flat map alone gets
this wrong past a few kilometres.

A C++17 library and command-line tool that answers three questions about the ground
between two points:

1. **Terrain profile** — ground elevation along a path between two geodetic points,
   sampled at a stated spacing.
2. **Line of sight** — can an observer at point A, `hA` metres above the ground, see a
   target at point B, `hB` metres above the ground? If blocked, the position, elevation,
   and clearance deficit of the blocking point.
3. **Viewshed** — from one observer, which cells within a radius are visible, as a
   raster mask.

## Quick start

1. Clone this repository and open `TerrainEngine.sln` in Visual Studio 2022.
2. Build the solution (`x64`, `Release` recommended for real use).
3. Run `TerrainEngine.exe` with no arguments — this runs the 10-case test suite and a
   small demo against the included sample tile (`DATA/N36W112.hgt`, a stretch of the
   Grand Canyon), and writes `profile_output.pgm` / `viewshed_output.pgm` you can open
   in any image viewer that supports PGM (e.g. IrfanView, GIMP).
4. To ask your own question, use the CLI directly, e.g.: 
  ```
  TerrainEngine.exe los DATA/N36W112.hgt 36.0 -112.0 36.3 -111.5 36.35 -111.45 0.0002694 2.0 2.0
  ```
   This asks: standing 2 m above the ground at (36.3, -111.5), can you see a point
   2 m above the ground at (36.35, -111.45)? The answer, and if blocked, exactly where
   and by how much.
    

## Architecture / dependency list

The solution is split into three build targets so the core computation is provably
independent of any file format:

| Target          | Type            | Depends on                          |
|------------------|-----------------|--------------------------------------|
| `TerrainCore`    | Static library  | **C++17 and the standard library only** (`<vector>`, `<optional>`, `<cmath>`) |
| `TerrainReader`  | Static library  | `TerrainCore` (for the sampler interface) + stdlib (`<fstream>`, `<cstdint>`, `<string>`) |
| `TerrainEngine`  | Console exe/CLI | `TerrainCore`, `TerrainReader`, plus Windows-only `<windows.h>`/`<psapi.h>` for peak-memory reporting |

`TerrainCore` contains the elevation-sampler interface (`IElevationSampler`), the
in-memory `FakeElevationSampler` used by every test, and the three algorithms
(`GetTerrainProfile`, `ComputeLineOfSight`, `ComputeViewshedNaive`/`ComputeViewshedFast`).
It has no include path to `TerrainReader` and cannot see `RealElevationSampler.h`.

`TerrainReader` contains `RealElevationSampler`, a reader for SRTM `.hgt` 1-arcsecond
tiles (1201×1201, big-endian 16-bit signed elevations, void = `-32768`).

The Windows dependency (`psapi.h`) lives only in `TerrainEngine` (the CLI/benchmark
driver), never in `TerrainCore` or `TerrainReader` — it is not a library dependency,
only a demo/tooling one.

## Geometric model and assumptions

- **Horizontal coordinates**: geodetic latitude/longitude in degrees. Distances are
  computed as a flat-plane (Euclidean) approximation in degree-space, converted to
  metres with a fixed constant (111,320 m per degree of latitude). This is accurate
  enough at the scales tested here (up to 50 km) but is **not** a true geodesic
  calculation and would drift at larger distances or near the poles.
- **Vertical datum**: **not modeled.** Elevation values are taken directly from the
  SRTM file (heights above the EGM96 geoid) and added straight to `hA`/`hB` without
  any conversion to/from the WGS84 ellipsoid. This is a known, deliberate gap — the
  two differ by tens of metres and would need its own conversion function with its
  own tests to close properly.
- **Earth curvature**: `drop = d1*d2 / (2*k*R)`, `R = 6,371,000 m`, `k` defaults to
  `4/3` (standard atmospheric refraction) and is a real parameter — on
  `ComputeLineOfSight`, `ComputeViewshedNaive`, and `ComputeViewshedFast` — settable
  from the CLI (`los`/`viewshed` commands take an optional trailing `k` argument).
- **Interpolation**: two modes, chosen via `InterpolationMode` (`Nearest` default,
  `Bilinear` alternative), on both `FakeElevationSampler` and `RealElevationSampler`.
  Bilinear returns `nullopt` if any of the 4 surrounding cells is out of bounds or a
  void, rather than interpolating across a hole.
- **Sampling**: `sampleCount = max(1, floor(totalDistance / spacing))`; the profile
  always has at least 2 samples (the two endpoints), even if the two points are closer
  together than one `spacing` unit. Because `sampleCount` is floored, the effective
  spacing along the path (`totalDistance / sampleCount`) can be slightly larger than
  the requested `spacing` when the distance isn't an exact multiple of it — a peak
  narrower than this effective spacing can fall between two samples and be missed.
- **Voids**: a void (source `-32768`, or a query outside the tile) is never treated as
  zero or sea level. It surfaces as `std::optional` at every layer: `GetElevation`
  returns `nullopt`, `LineOfSightResult::isDegraded` is set to `true`, and viewshed
  cells become `std::optional<bool>` with no value — never a silent "visible".

## Validity envelope

- Single SRTM 1-arcsecond tile (1°×1°), addressed by its south-west corner. No
  multi-tile stitching.
- Distances up to the tested 50 km (profile) / 30 km radius (viewshed); the flat-plane
  distance approximation is not validated beyond that.
- No threading — determinism across thread counts is satisfied vacuously.

## What it deliberately does not model

- Vertical datum conversion (geoid ↔ ellipsoid).
- Fresnel-zone / radio-path clearance.
- Atmospheric refraction beyond the constant `k`.
- Multiple elevation tiles / tile-boundary stitching.
- Batch line-of-sight (N observers × M targets).

## Determinism and floating-point settings

- Verified bit-identical output between Debug and Release builds for `profile`, `los`,
  and `viewshed` CLI commands on real data (text output diffed, binary PGM output
  byte-compared).
- No fast-math: the actual compiler invocation uses `/fp:precise` in every
  configuration (confirmed directly from the `cl.exe` command line); `/fp:fast` is
  never enabled.
- Not threaded, so no cross-thread-count claim is needed.

## Performance (measured on this machine)

AMD Ryzen 7 3800X (8 cores / 16 threads), 16 GB RAM, Windows 11 Pro x64, Release build,
`/fp:precise`, real Grand Canyon SRTM tile (`DATA/N36W112.hgt`):

| Operation                                   | Wall time | Peak memory |
|-----------------------------------------------|-----------|--------------|
| 50 km profile @ 30 m spacing (1,667 samples)  | ~0.09–0.2 ms | ~11.9 MB |
| 30 km-radius viewshed @ 30 m data (2000×2000) | ~232–252 ms  | ~16.6 MB |

Each row was measured as its own process (`TerrainEngine.exe benchmark profile ...` /
`TerrainEngine.exe benchmark viewshed ...`), so the two peak-memory figures are
independent — Windows' `PeakWorkingSetSize` is a running high-water mark for the whole
process and can't be reset mid-run, so measuring both operations in one process would
have let the first operation's peak leak into the second's number.

## Fast vs. naive viewshed accuracy

On a 2 km-radius viewshed over the real SRTM tile (133×133 grid, 17,687 valid cells),
the fast (boundary-ray-sweep) algorithm disagrees with the naive (one-LOS-per-cell)
algorithm on **647 cells (3.66%)**, asserted to stay under a 5% tolerance
(`TestFastViewshedMatchesNaive on real SRTM data within stated tolerance` in the CLI's
test/benchmark output).

The disagreement is concentrated on ridgelines. The fast algorithm only casts rays to
the grid's boundary cells and derives every interior cell's visibility from whichever
ray happens to pass nearest it, rather than that cell's own exact-direction line — an
interior cell just off a boundary ray's path is judged by a slightly different sightline
than the one a naive per-target LOS would use for it. That difference in the assumed
sightline only changes the answer where the terrain's slope is changing fast underfoot,
which is exactly a ridgeline; over flat or smoothly-sloped ground the two sightlines
agree. Two attempts to shrink this gap (casting more, angularly-denser rays; using
`ceil` instead of `floor` for the profile's sample count) both made the mismatch worse
(4.85%, then 4.29%) — see `NOTES.md` for why.

## Void handling and degraded results

Asserted by tests (`TestVoidPointIsDegraded`, `TestViewshedDetectsVoid`). A missing or
unreadable elevation file is also distinguished from a genuine void:
`RealElevationSampler::IsLoaded()` reports load failure explicitly, and the CLI refuses
to run rather than silently treating every query as void.

## API contract

Every public result type (`ProfileSample`, `LineOfSightResult`, `ViewshedResult`) is a
plain struct of value types. Absence is `std::optional`, not a sentinel value. Nothing
in `TerrainCore` or `TerrainReader` throws across its own boundary.

## Building

Open `TerrainEngine.sln` in Visual Studio 2022, build the `x64` platform (`Debug` or
`Release`). `TerrainCore` and `TerrainReader` build as static libraries; `TerrainEngine`
links both and produces `TerrainEngine.exe`.

## CLI usage
```
TerrainEngine.exe # run the 10-case test suite + demo
TerrainEngine.exe profile <hgtFile> <swLat> <swLon> <aLat> <aLon> <bLat> <bLon> <spacing>
TerrainEngine.exe los <hgtFile> <swLat> <swLon> <aLat> <aLon> <bLat> <bLon> <spacing> <hA> <hB> [k]
TerrainEngine.exe viewshed <hgtFile> <swLat> <swLon> <obsLat> <obsLon> <gridSize> <spacing> <height> [k]
TerrainEngine.exe benchmark <profile|viewshed> <hgtFile> <swLat> <swLon>
```
Example, using the included Grand Canyon tile: TerrainEngine.exe los DATA/N36W112.hgt 36.0 -112.0 36.3 -111.5 36.35 -111.45 0.0002694 2.0 2.0

## Test suite

10 synthetic, hand-checkable cases with no data file (flat plateau, wall, curvature,
void, viewshed void, determinism, fast-vs-naive on a small grid, symmetric-hill
reciprocity, observer-below-rim, target-on-far-slope), plus one real-data tolerance
assertion. All 10 + the tolerance check pass identically in Debug and Release.

## Rendered output

`profile_output.pgm` and `viewshed_output.pgm` (produced by the built-in benchmark) and
`cli_viewshed_output.pgm` (produced by the `viewshed` CLI command) — plain P5 binary
PGM, written by a single self-contained writer (`ImageWriter.h`), no graphics
dependency.

**Orientation note:** in the viewshed PGM/PNG, west is left and east is right (as
expected), but **north is at the bottom of the image, not the top** — row 0 (written
first, so the top of the image) corresponds to the observer's south, because
`ComputeViewshedFast`/`ComputeViewshedNaive` compute `target.latitude = observer.latitude
+ (row - centerRow) * spacing`, and increasing `row` means increasing latitude (further
north). This is not a standard north-up map; it's the raw grid orientation exactly as
computed. Flip the image vertically if you want a conventional north-up view.