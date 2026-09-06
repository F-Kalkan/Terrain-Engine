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
3. Run `TerrainEngine.exe` with no arguments — this runs the 22-case test suite and a
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

`TerrainReader` contains `RealElevationSampler`, a reader for SRTM `.hgt` 3-arcsecond
tiles (1201×1201, ~90 m resolution at the equator, big-endian 16-bit signed elevations,
void = `-32768`).

The Windows dependency (`psapi.h`) lives only in `TerrainEngine` (the CLI/benchmark
driver), never in `TerrainCore` or `TerrainReader` — it is not a library dependency,
only a demo/tooling one.

## Geometric model and assumptions

- **Horizontal coordinates**: geodetic latitude/longitude in degrees. Distances are
  computed as a flat-plane (Euclidean) approximation in degree-space, converted to
  metres with a fixed constant (111,320 m per degree of latitude) and a
  `cos(latitude)` correction on the longitude component (`GetTerrainProfile`, in
  `TerrainProfile.h`) — a degree of longitude covers less real ground than a degree
  of latitude away from the equator, and skipping that correction is a 24% distance
  error (54% on the curvature term, since it scales with distance squared) at this
  project's demo latitude. This is accurate enough at the scales tested here (up to
  50 km) but is **not** a true geodesic calculation and would drift at larger
  distances or near the poles.
- **Viewshed grid shape**: `ComputeViewshedNaive`/`ComputeViewshedFast` take one
  `spacing` value (degrees of latitude per row) and derive the column spacing from
  it via `LongitudeSpacingForLatitude` (`Viewshed.h`), widening it by
  `1/cos(observer latitude)` so a column step covers the same real ground distance
  as a row step. Without this, a grid built from equal degree-steps on both axes is
  an ellipse in real-world terms — narrower east-west — even though every individual
  cell's line-of-sight answer (which depends on `GetTerrainProfile`'s distance, not
  on which cells get queried in the first place) is already correct; verified for
  the included tile's latitude (36.5°N) at a "30 km radius": north/south reach
  30,000 m, east/west reach 29,970 m, both real distances measured through
  `GetTerrainProfile`.
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
  `Bilinear` alternative), on both `FakeElevationSampler` and `RealElevationSampler` —
  settable from the CLI (`profile`/`los`/`viewshed` commands take an optional trailing
  `nearest`/`bilinear` argument). Bilinear returns `nullopt` if any of the 4 surrounding cells is out of bounds or a
  void, rather than interpolating across a hole.
  **Why `Nearest` is the default:** it reports the actual measured elevation at the
  closest real post, never a value nobody measured. `Bilinear` smooths across posts,
  which can shave a narrow ridge's peak down toward its shoulders or fill a narrow
  notch up toward its rim — softening exactly the sharp features a line-of-sight
  query cares most about, in a direction that isn't predictable in advance (it can
  make a blocked path look clear, or a clear path look blocked, depending on which
  side of the feature is queried). `Nearest` is kept as the default for that reason;
  `Bilinear` stays reachable from the CLI for callers who want a smoother profile and
  accept that tradeoff. `TestInterpolationModesDifferOnRidgeline` (`Tests.h`) measures
  the size of the disagreement on a hand-built ridge.
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

- Single SRTM 3-arcsecond tile (1°×1°, ~90 m resolution), addressed by its south-west corner.
  No multi-tile stitching.
- Distances up to the tested 50 km (profile) / 30 km radius (viewshed); the flat-plane
  distance approximation is not validated beyond that.
- No threading — determinism across thread counts is satisfied vacuously.

## What it deliberately does not model

- Vertical datum conversion (geoid ↔ ellipsoid).

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
| 50 km profile @ 30 m spacing (1,667 samples)     | ~0.09–0.2 ms | ~11.9 MB |
| 30 km-radius viewshed @ 30 m spacing (2000×2000) | ~232–252 ms  | ~16.6 MB |

Each row was measured as its own process (`TerrainEngine.exe benchmark profile ...` /
`TerrainEngine.exe benchmark viewshed ...`), so the two peak-memory figures are
independent — Windows' `PeakWorkingSetSize` is a running high-water mark for the whole
process and can't be reset mid-run, so measuring both operations in one process would
have let the first operation's peak leak into the second's number.

The `30 m` figures above are the sampling spacing passed to the CLI, not the source
data's native resolution — the included tile is 3-arcsecond (~90 m); sampling at 30 m
oversamples between real data points via the chosen interpolation mode rather than
adding new information the source data doesn't have.

## Fast vs. naive viewshed: accuracy and performance

On a 2 km-radius viewshed over the real SRTM tile (133×133 = 17,689 grid cells total,
17,687 of them valid in both algorithms and **2 excluded** because at least one
algorithm wasn't confident there), the fast (boundary-ray-sweep) algorithm disagrees
with the naive (one-LOS-per-cell) algorithm on **822 cells (4.65%)**, asserted to stay
under a 5% tolerance (`TestFastViewshedMatchesNaive on real SRTM data within stated
tolerance` in the CLI's test/benchmark output, which now also prints the
excluded-cell count directly alongside the ratio).

The disagreement is concentrated on ridgelines. The fast algorithm only casts rays to
the grid's boundary cells and derives every interior cell's visibility from whichever
ray happens to pass nearest it, rather than that cell's own exact-direction line — an
interior cell just off a boundary ray's path is judged by a slightly different sightline
than the one a naive per-target LOS would use for it. That difference in the assumed
sightline only changes the answer where the terrain's slope is changing fast underfoot,
which is exactly a ridgeline; over flat or smoothly-sloped ground the two sightlines
agree. Two earlier attempts to shrink this gap (casting more, angularly-denser rays;
using `ceil` instead of `floor` for the profile's sample count) both made the mismatch
worse (4.85%, then 4.29%) — see `NOTES.md` for why. The ratio moved again, from 3.58%
to the current 4.65%, when the "Viewshed grid shape" fix above changed the real-world
spacing of the fast algorithm's boundary rays; it is still comfortably under the 5%
tolerance, but closer to it than before, which is worth knowing rather than discovering
under a slightly different tile or radius.

**Performance side of the same comparison** (Release build, same real SRTM tile):

| Radius / grid                        | Naive         | Fast          | Speed-up   |
|----------------------------------------|---------------|---------------|------------|
| 2 km / 133×133 (17,687 valid cells)    | ~31–33 ms     | ~1.5–1.8 ms   | ~18–20x    |
| 30 km / 2000×2000 (the table above)    | ~65–67 s      | ~278–293 ms   | ~220–240x  |

Naive's cost grows faster than fast's as the radius grows: it runs one full profile +
line-of-sight per cell, so its total work scales with roughly (cell count) ×
(average profile length), both of which grow with radius, while fast only casts rays
to the boundary, so its work scales closer to the boundary's perimeter × profile
length. That is why the speed-up is larger at 30 km than at 2 km — and it is the
number this project was missing: the performance table above used to report the fast
viewshed's time in isolation, with no naive baseline next to it, so the actual
speed-up this design buys was never stated.

## Void handling and degraded results

Asserted by tests (`TestVoidPointIsDegraded`, `TestViewshedDetectsVoid`). A missing or
unreadable elevation file is also distinguished from a genuine void:
`RealElevationSampler::IsLoaded()` reports load failure explicitly, and the CLI refuses
to run rather than silently treating every query as void.

## API contract

Every public result type (`ProfileSample`, `LineOfSightResult`, `ViewshedResult`) is a
plain struct of value types. `ProfileSample::elevation` and `LineOfSightResult`'s
optional fields use `std::optional` for absence, not a sentinel value.
`ViewshedResult::visible` instead uses an explicit `CellVisibility` enum
(`NotCovered` / `Degraded` / `Visible` / `NotVisible`) — a single `optional<bool>`
cannot distinguish "no ray ever reached this cell" from "a ray reached it but crossed
a void partway", and a caller needs to tell those two apart. Nothing in `TerrainCore`
or `TerrainReader` throws across its own boundary.

## Building

Open `TerrainEngine.sln` in Visual Studio 2022, build the `x64` platform (`Debug` or
`Release`). `TerrainCore` and `TerrainReader` build as static libraries; `TerrainEngine`
links both and produces `TerrainEngine.exe`.

## CLI usage
```
TerrainEngine.exe # run the 22-case test suite + demo
TerrainEngine.exe benchmark <profile|viewshed> <hgtFile> <swLat> <swLon>
TerrainEngine.exe profile <hgtFile> <swLat> <swLon> <aLat> <aLon> <bLat> <bLon> <spacing> [nearest|bilinear]
TerrainEngine.exe los <hgtFile> <swLat> <swLon> <aLat> <aLon> <bLat> <bLon> <spacing> <hA> <hB> [k] [nearest|bilinear]
TerrainEngine.exe viewshed <hgtFile> <swLat> <swLon> <obsLat> <obsLon> <gridSize> <spacing> <height> [k] [nearest|bilinear]
TerrainEngine.exe fresnel <hgtFile> <swLat> <swLon> <aLat> <aLon> <bLat> <bLon> <spacing> <hA> <hB> <frequencyMHz> [k]
TerrainEngine.exe batch <hgtFile> <swLat> <swLon> <queriesFile> [k]
```
Example, using the included Grand Canyon tile: TerrainEngine.exe los DATA/N36W112.hgt 36.0 -112.0 36.3 -111.5 36.35 -111.45 0.0002694 2.0 2.0

## Test suite

22 hand-checkable test functions, plus one real-data tolerance assertion:

- **Synthetic, in-memory data** (`FakeElevationSampler`, no file on disk): flat
  plateau, wall, curvature, void, viewshed void, determinism, fast-vs-naive on a
  small grid, symmetric-hill reciprocity, observer-below-rim, target-on-far-slope,
  Fresnel partial obstruction, batch line-of-sight vs individual calls, multi-tile
  seam (point queries), blocking-feature classification, fast-viewshed void
  propagation, interpolation modes on a ridgeline, a narrow spike falling between
  floored samples, a profile crossing a multi-tile seam, empty/single-sample
  profile guards, and blocking-feature classification being invariant to sample
  spacing.
- **Synthetic, file-based**: `TestRealElevationSamplerReadsVoidFromFile` writes a
  2×2 `.hgt`-format tile with a real void sentinel and reads it back through
  `RealElevationSampler` itself.
- **Frozen golden output**: `TestProfileMatchesFrozenOracle` regenerates a fixed
  profile and diffs it against the checked-in `DATA/oracle_profile.csv`.
- **Real data**: one tolerance assertion comparing naive and fast viewshed output
  over the actual Grand Canyon SRTM tile (`FastViewshedMatchesNaive on real SRTM
  data within stated tolerance`, printed by `RunWallTimeBenchmark`).

All of the above pass identically in Debug and Release.

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

## Stretch goals implemented

- **Fresnel-zone clearance** — `ComputeFresnelClearance` (in `LineOfSight.h`) reports
  the worst-case fraction of the first Fresnel zone that is clear along a path, given
  a frequency, instead of a boolean visible/blocked answer. `>= 1.0` means fully clear,
  `0` to `1` means the terrain intrudes into the Fresnel zone without touching the
  direct line, `< 0` means the direct line itself is blocked. Exposed via
  `TerrainEngine.exe fresnel <hgtFile> <swLat> <swLon> <aLat> <aLon> <bLat> <bLon>
  <spacing> <hA> <hB> <frequencyMHz> [k]`. If the two points are closer together than
  one sampling step, there is no interior sample to evaluate a Fresnel radius at, and
  the result is reported as degraded rather than a false "fully clear".

- **Batch line of sight** — `ComputeBatchLineOfSight` (in `LineOfSight.h`) takes a list
  of `BatchLineOfSightQuery` entries (observer, target, heights, distance) and a single
  shared `IElevationSampler`, returning one `LineOfSightResult` per query. The shared
  work is the sampler itself: one file load serves the whole batch, instead of N*M
  separate CLI invocations each reconstructing a `RealElevationSampler` (and re-reading
  the whole `.hgt` file) from scratch. Exposed via `TerrainEngine.exe batch <hgtFile>
  <swLat> <swLon> <queriesFile> [k]`, where `queriesFile` is a plain text file: a
  spacing value on the first line, then one `aLat aLon bLat bLon hA hB` per line.

- **Tile boundaries** — `MultiTileElevationSampler` (in `IElevationSampler.h`) composes
  multiple `IElevationSampler` instances, each registered with its own south-west
  corner, and routes each query to whichever tile's 1°×1° box contains it. Because it
  implements `IElevationSampler` itself, `GetTerrainProfile`/`ComputeLineOfSight`/
  `ComputeViewshedFast` need no changes to use it — a path crossing from one tile into
  another gets seamless elevation data, with zero gap or void exactly at the boundary
  (verified in `TestMultiTileProfileCrossesSeamWithoutGap`, a profile whose path
  crosses the seam directly, alongside the point-query check in
  `TestMultiTileSeamIsInvisible`).
  **Contract:** `MultiTileElevationSampler` selects the right tile by its registered
  corner, but then passes the query's **world** latitude/longitude straight through to
  that tile's sampler, untranslated. This only produces correct answers if the
  sub-sampler itself knows how to place a world coordinate on its own grid — true of
  `RealElevationSampler` (it stores its own south-west corner and computes `.hgt`
  row/col from it) but not of a sampler that treats its coordinates as raw local
  indices; such a sampler cannot be registered as a tile without adapting it first.

- **A "why" answer** — `ClassifyBlockingFeature` (in `LineOfSight.h`) looks at the
  blocking point's immediate profile neighbours and classifies it as a local
  peak/ridge, a rising slope, a falling slope, or a plateau, rather than only
  reporting a position and elevation. Attached to `LineOfSightResult::blockingFeature`
  and printed by the `los` CLI command as `Blocking feature: ...`. This is a
  profile-local shape classification, not a named-landmark lookup (that would need an
  external gazetteer, out of scope) — but it turns "blocked" into "blocked by what kind
  of terrain," which is what the geometry itself can honestly answer.