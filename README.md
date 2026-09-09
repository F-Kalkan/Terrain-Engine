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
2. **Line of sight** — can an observer at point A, `observerHeightAgl` metres above the
   ground, see a target at point B, `targetHeightAgl` metres above the ground? If
   blocked, the position, elevation, and clearance deficit of the blocking point.
3. **Viewshed** — from one observer, which cells within a radius are visible, as a
   raster mask.

## Quick start

1. Clone this repository and open `TerrainEngine.sln` in Visual Studio 2022.
2. Build the solution (`x64`, `Release` recommended for real use).
3. Run `TerrainEngine.exe` with no arguments — this runs the 31-case test suite and a
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

`TerrainCore` contains the elevation-sampler interface (`IElevationSampler`), three
in-memory implementations of it (`FakeElevationSampler` used by every test,
`MultiTileElevationSampler`, and `RasterBlockElevationSampler` — see "Integration
readiness work" below), and the three algorithms (`GetTerrainProfile`,
`ComputeLineOfSight`, `ComputeViewshedNaive`/`ComputeViewshedFast`). It has no
include path to `TerrainReader` and cannot see `RealElevationSampler.h`.

`TerrainReader` contains `RealElevationSampler`, a reader for SRTM `.hgt` 3-arcsecond
tiles (1201×1201, ~90 m resolution at the equator, big-endian 16-bit signed elevations,
void = `-32768`).

The Windows dependency (`psapi.h`) lives only in `TerrainEngine` (the CLI/benchmark
driver), never in `TerrainCore` or `TerrainReader` — it is not a library dependency,
only a demo/tooling one.

## Geometric model and assumptions

- **Horizontal coordinates**: geodetic latitude/longitude in degrees. Distance and
  sample position along a path are computed as a true great-circle (haversine)
  calculation over a sphere of radius `EarthRadiusM` (`GreatCircleDistanceM`,
  `GreatCircleInterpolate`, in `TerrainProfile.h`) — the same Earth radius already
  used for line-of-sight curvature, so both are derived from one shared constant
  instead of two disagreeing ones. Each sample's position is the true point at its
  fractional distance along the great-circle arc between the path's endpoints, not
  a straight line in degree-space, so `distanceFromStartM` is exact by construction
  rather than re-derived per sample from an approximation.
- **Viewshed grid shape**: `ComputeViewshedNaive`/`ComputeViewshedFast` take one
  `spacingDeg` value (degrees of latitude per row) and derive the column spacing from
  it via `LongitudeSpacingForLatitude` (`Viewshed.h`), widening it by
  `1/cos(observer latitude)` so a column step covers the same real ground distance
  as a row step. Without this, a grid built from equal degree-steps on both axes is
  an ellipse in real-world terms — narrower east-west — even though every individual
  cell's line-of-sight answer (which depends on `GetTerrainProfile`'s distance, not
  on which cells get queried in the first place) is already correct; verified for
  the included tile's latitude (36.5°N) at a "30 km radius": north/south reach
  30,000 m, east/west reach 29,970 m, both real distances measured through
  `GetTerrainProfile`.
- **Vertical datum**: modeled as an explicit, closed set (`VerticalDatum`, in
  `VerticalDatum.h`) — `EllipsoidalHae`, `OrthometricMsl`, `PressureAltitude`,
  `HeightAboveGround`, and `Unknown`. `ComputeLineOfSight`/`ComputeFresnelClearance`/
  the viewsheds take `observerHeightAgl`/`targetHeightAgl` as `DatumHeight` (a value
  plus its datum) and require `HeightAboveGround` — the only datum this library's
  terrain-elevation-plus-relative-offset arithmetic is valid for. Every
  `IElevationSampler` declares the datum its elevations are in via `GetDatum()`
  (`RealElevationSampler` → `OrthometricMsl`, since SRTM is EGM96-referenced). A
  query is **rejected as a value** (`status` set to `ComputationStatus::DatumRejected`,
  never an exception) if `observerHeightAgl`/`targetHeightAgl` isn't `HeightAboveGround`
  or the sampler's datum is `Unknown` — never silently combined, never guessed.
  `ConvertHeightBetweenDatums` converts between `EllipsoidalHae` and
  `OrthometricMsl` with a **required** undulation argument (no default of zero,
  because "zero undulation" silently asserts the point sits exactly on the geoid);
  no other datum pair has a defined conversion, and it returns `nullopt` for those,
  as a value rather than a guess.
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
- **Sampling**: `sampleCount = max(1, floor(totalDistanceDeg / spacingDeg))`; the
  profile always has at least 2 samples (the two endpoints), even if the two points
  are closer together than one `spacingDeg` unit. Because `sampleCount` is floored,
  the effective spacing along the path (`totalDistanceDeg / sampleCount`) can be
  slightly larger than the requested `spacingDeg` when the distance isn't an exact
  multiple of it — a peak narrower than this effective spacing can fall between two
  samples and be missed.
- **Voids**: a void (source `-32768`, or a query outside the tile) is never treated as
  zero or sea level. It surfaces as `std::optional` at every layer: `GetElevation`
  returns `nullopt`, `LineOfSightResult::status` becomes `ComputationStatus::VoidInProfile`,
  and viewshed cells become `std::optional<bool>` with no value — never a silent "visible".

## Validity envelope

- Single SRTM 3-arcsecond tile (1°×1°, ~90 m resolution), addressed by its south-west corner.
  No multi-tile stitching.
- Distances up to the tested 50 km (profile) / 30 km radius (viewshed). The
  great-circle distance calculation itself has no inherent range limit, but the
  curvature model used for visibility (an effective-Earth-radius approximation,
  standard for line-of-sight and radio-propagation calculations) is not validated
  beyond the ranges tested here, and neither model handles a path that crosses the
  antimeridian (±180° longitude).
- No threading — determinism across thread counts is satisfied vacuously.
- **Unvalidated inputs, not exercised by any test or CLI path today:**
  - `spacingDeg <= 0` passed to `GetTerrainProfile` divides by zero
    (`sampleCount = totalDistanceM / (spacingDeg * metersPerDegree)`). No CLI
    argument or test ever passes a non-positive spacing, so this has never been
    hit in practice.
  - `FakeElevationSampler` constructed with an empty grid (`{}`) would index `grid[0]` out of
    bounds in its bilinear path. Every test and demo grid is non-empty; this is a latent
    fragility in test-only code, not a path real data goes through.
  - `LongitudeSpacingForLatitude` divides by `cos(latitude)`, which is zero exactly at the poles
    (±90°) — a viewshed centred exactly on a pole would divide by zero when laying
    out its grid columns. No test or CLI path ever places an observer there.

## What it deliberately does not model

- A live geoid-undulation data source. `ConvertHeightBetweenDatums` implements the
  ellipsoidal↔orthometric formula, but nothing in this project supplies a real
  undulation value for a given coordinate — a caller needs one from elsewhere.

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
| 50 km profile @ 30 m spacing (1,667 samples)     | ~0.18–0.43 ms | ~11.2–11.7 MB |
| 30 km-radius viewshed @ 30 m spacing (2000×2000) | ~657–672 ms   | ~24.0 MB |

The distance calculation switched from a flat-plane approximation to a true
great-circle (haversine) one (see "Horizontal coordinates" above), which is
roughly 2.4x more expensive per sample — trigonometric functions cost more than
the handful of multiplications the old approximation needed. The viewshed
figure above reflects that; the profile figure barely moves because it does so
little work in absolute terms either way.

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
all 17,689 of them valid and confident in both algorithms, **0 excluded**), the fast
(boundary-ray-sweep) algorithm disagrees with the naive (one-LOS-per-cell) algorithm
on **693 cells (3.92%)**, asserted to stay under a 5% tolerance
(`TestFastViewshedMatchesNaive on real SRTM data within stated tolerance` in the
CLI's test/benchmark output, which also prints the excluded-cell count directly
alongside the ratio).

The disagreement is concentrated on ridgelines. The fast algorithm only casts rays to
the grid's boundary cells and derives every interior cell's visibility from whichever
ray happens to pass nearest it, rather than that cell's own exact-direction line — an
interior cell just off a boundary ray's path is judged by a slightly different sightline
than the one a naive per-target LOS would use for it. That difference in the assumed
sightline only changes the answer where the terrain's slope is changing fast underfoot,
which is exactly a ridgeline; over flat or smoothly-sloped ground the two sightlines
agree. This ratio has moved several times as the underlying geometry was refined —
worse after two early attempts to shrink it (casting more, angularly-denser rays;
using `ceil` instead of `floor` for the profile's sample count both made it worse,
4.85% then 4.29% — see `NOTES.md` for why); up to 4.65% when the viewshed grid's
column spacing was corrected for latitude; and down to the current 3.92% when the
distance calculation itself switched from a flat-plane approximation to a true
great-circle calculation — both algorithms now place their samples and measure
their distances more accurately, which brings their independently-derived
sightlines into closer agreement, particularly on the ridgelines where the old
approximation's error was concentrated. It is comfortably under the 5% tolerance,
which is worth knowing rather than discovering under a slightly different tile or
radius.

**Performance side of the same comparison** (Release build, same real SRTM tile):

| Radius / grid                        | Naive         | Fast          | Speed-up   |
|----------------------------------------|---------------|---------------|------------|
| 2 km / 133×133 (17,689 valid cells)    | ~77–86 ms     | ~3.3–6.4 ms   | ~12–26x    |
| 30 km / 2000×2000 (the table above)    | ~199.3 s      | ~657–672 ms   | ~297–303x  |

Both columns grew substantially from the flat-plane-approximation numbers this table
used to report — the great-circle distance calculation is real trigonometry, not a
handful of multiplications, and naive pays for it once per cell while fast pays for
it once per boundary ray, which is also why naive's cost grew by roughly the same
~2.4–3x factor at both radii while the speed-up ratio itself stayed in the same
range.

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
plain struct of value types. `ProfileSample::elevationM` and `LineOfSightResult`'s
optional fields use `std::optional` for absence, not a sentinel value.
`ViewshedResult::visible` instead uses an explicit `CellVisibility` enum
(`NotCovered` / `Degraded` / `Visible` / `NotVisible`) — a single `optional<bool>`
cannot distinguish "no ray ever reached this cell" from "a ray reached it but crossed
a void partway", and a caller needs to tell those two apart. Similarly, a height
crossing the API boundary (`observerHeightAgl`/`targetHeightAgl`) is never a bare
`double` — it is a `DatumHeight`, and a query built from the wrong datum is rejected as a value
(`LineOfSightResult::status == ComputationStatus::DatumRejected`), not silently
computed or thrown. Nothing in `TerrainCore` or `TerrainReader` throws across its
own boundary.

## Building

Open `TerrainEngine.sln` in Visual Studio 2022, build the `x64` platform (`Debug` or
`Release`). `TerrainCore` and `TerrainReader` build as static libraries; `TerrainEngine`
links both and produces `TerrainEngine.exe`.

## CLI usage
```
TerrainEngine.exe # run the 31-case test suite + demo
TerrainEngine.exe benchmark <profile|viewshed> <hgtFile> <swLat> <swLon>
TerrainEngine.exe profile <hgtFile> <swLat> <swLon> <aLat> <aLon> <bLat> <bLon> <spacing> [nearest|bilinear]
TerrainEngine.exe los <hgtFile> <swLat> <swLon> <aLat> <aLon> <bLat> <bLon> <spacing> <hA> <hB> [k] [nearest|bilinear]
TerrainEngine.exe viewshed <hgtFile> <swLat> <swLon> <obsLat> <obsLon> <gridSize> <spacing> <height> [k] [nearest|bilinear]
TerrainEngine.exe fresnel <hgtFile> <swLat> <swLon> <aLat> <aLon> <bLat> <bLon> <spacing> <hA> <hB> <frequencyMHz> [k]
TerrainEngine.exe batch <hgtFile> <swLat> <swLon> <queriesFile> [k]
```
Example, using the included Grand Canyon tile: TerrainEngine.exe los DATA/N36W112.hgt 36.0 -112.0 36.3 -111.5 36.35 -111.45 0.0002694 2.0 2.0

## Test suite

31 hand-checkable test functions, plus one real-data tolerance assertion:

- **Synthetic, in-memory data** (`FakeElevationSampler`, no file on disk): flat
  plateau, wall, curvature, void, viewshed void, determinism, fast-vs-naive on a
  small grid, symmetric-hill reciprocity, observer-below-rim, target-on-far-slope,
  Fresnel partial obstruction, batch line-of-sight vs individual calls, multi-tile
  seam (point queries), blocking-feature classification, fast-viewshed void
  propagation, interpolation modes on a ridgeline, a narrow spike falling between
  floored samples, a profile crossing a multi-tile seam, empty/single-sample
  profile guards, blocking-feature classification being invariant to sample
  spacing, the viewshed's longitude spacing correcting for latitude, and a
  line-of-sight query rejected when the observer/target height or the terrain
  sampler's datum isn't what the computation requires.
- **Datum arithmetic, no sampler**: `TestConvertHeightBetweenDatums` round-trips
  ellipsoidal↔orthometric with a known undulation and confirms an unsupported
  datum pair returns `nullopt` rather than a guess.
- **Resident raster block** (`RasterBlockElevationSampler`, no file on disk): a
  positive row step (origin at the south edge), a negative row step (origin at
  the north edge, proving the signed step — not an unwritten convention — decides
  row order), a void cell passed through untouched, and a full
  `GetTerrainProfile`/`ComputeLineOfSight` run against it with no changes to
  either function.
- **Synthetic, file-based**: `TestRealElevationSamplerReadsVoidFromFile` writes a
  2×2 `.hgt`-format tile with a real void sentinel and reads it back through
  `RealElevationSampler` itself.
- **Datum declaration**: `TestRealElevationSamplerDeclaresOrthometricDatum`
  confirms `RealElevationSampler::GetDatum()` always reports `OrthometricMsl`,
  independent of whether the specific file loads.
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
`ComputeViewshedFast`/`ComputeViewshedNaive` compute `target.latitudeDeg = observer.latitudeDeg
+ (row - centerRow) * spacingDeg`, and increasing `row` means increasing latitude (further
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

## Adapting toward a real-time host application

Everything above describes the library as a standalone tool. This section covers
further changes aimed at a different consumer: a real-time simulation or
visualization runtime that would embed this library rather than call it from the
command line. That kind of host typically supplies terrain asynchronously in
batches instead of one point at a time, tracks altitude in a specific vertical
reference frame, and runs a line-of-sight query many times per second on a budget
that cannot tolerate a heap allocation per call. The changes below address those
constraints directly.

- **Vertical datum, as a type** — see "Vertical datum" under "Geometric model and
  assumptions" above. Heights and terrain elevation now carry an explicit
  `VerticalDatum`; a query built from the wrong one is rejected as a value, not
  silently computed.
- **A raster-block `IElevationSampler`** — `RasterBlockElevationSampler` (in
  `IElevationSampler.h`) is a view over an already-resident block of elevation data,
  described by an origin plus **signed** per-row/per-column degree steps — no assumed
  resolution (unlike `RealElevationSampler`, which infers its size from a `.hgt`
  file's byte count) and no hardcoded 1°×1° box (unlike `MultiTileElevationSampler`).
  This is the shape a real host hands back from a raster-window request: the block is
  already in memory, and answering a query is index arithmetic, not I/O. The row
  step's **sign** states row order explicitly — negative means row 0 is the northern
  edge, positive means row 0 is the southern edge — closing the exact ambiguity the
  "Orientation note" above describes for the file-backed viewshed PGM output.
  `GetTerrainProfile`/`ComputeLineOfSight` need no changes to use it, proving
  `IElevationSampler` itself was the right abstraction all along; only its
  implementations were file-shaped.
- **A structured `ComputationStatus`, replacing two separate bools** — `isDegraded`
  and `datumRejected` collapsed several unrelated reasons for "no confident answer"
  into one flag that could not say which one applied. `LineOfSightResult` and
  `FresnelClearanceResult` now carry a single `ComputationStatus status`
  (`Ok`, `EmptyOrSingleSampleProfile`, `EndpointMissing`, `VoidInProfile`,
  `DatumRejected`, `NothingEvaluated`), with `IsOk(status)` for the common
  "is this a real answer" check and `ComputationStatusToString(status)` for
  diagnostics — the CLI's `los`/`fresnel`/`batch` output now names the specific
  cause instead of a bare `true`/`false`.
- **A caller-owned scratch buffer for the profile** — `ComputeLineOfSight` and
  `ComputeFresnelClearance` already took their profile by `const&` (no copy on the
  hot path); the remaining allocation was `GetTerrainProfile` itself, which built a
  fresh `std::vector<ProfileSample>` on every call. It now has an in-place overload,
  `GetTerrainProfile(startPoint, endPoint, spacingDeg, sampler, outProfile)`, that fills a caller-owned
  `std::vector<ProfileSample>&` via `clear()` (which drops elements but keeps
  capacity) instead of returning a new one. A caller that keeps one buffer alive
  across frames and reuses it for each line-of-sight query allocates nothing once
  that buffer's capacity has grown to the longest profile it will ever need — the
  exact per-frame query shape a real-time runtime needs. The original by-value
  overload is now a two-line wrapper over this one, so every existing caller
  (batch, viewshed, tests) is unchanged.
- **Physical-quantity-unit identifier names, and Big-O/thread-safety documentation**
  — every public struct field and function parameter that had lost its unit or frame
  now states it: `GeoPoint::latitudeDeg`/`longitudeDeg`, `ProfileSample::elevationM`,
  `LineOfSightResult::blockingElevationM`/`clearanceDeficitM`, `observerHeightAgl`/
  `targetHeightAgl` (replacing `hA`/`hB`/`observerHeight`), `spacingDeg` (replacing
  bare `spacing`), and `IElevationSampler::GetElevation(latitudeDeg, longitudeDeg)`
  across every implementation (`FakeElevationSampler`, `MultiTileElevationSampler`,
  `RasterBlockElevationSampler`, `RealElevationSampler`). Internal locals carrying
  the same flagged quantities (`totalDistanceDeg`/`totalDistanceM`, `curvatureDropM`,
  and the like) were renamed alongside them for consistency within each function.
  Every public entry point in `TerrainCore`/`TerrainReader` that runs per frame or
  handles a collection now documents its Big-O and its thread-safety guarantee in a
  comment directly above it — e.g. `ComputeViewshedNaive` states
  O(gridRows × gridCols × samples per profile) and single-thread-only, and
  `GetTerrainProfile`'s scratch-buffer overload states its O(sample count) and the
  condition under which concurrent callers are safe.
- **A stated threading position for the viewshed** — `ComputeViewshedFast`'s
  boundary rays are applied to the grid in a fixed, sequential order, and where
  two rays visit the same cell, the later one in that order wins. That "last ray
  wins" reduction is deterministic only because ray order never varies; naively
  parallelising the ray loop would let completion order decide the result
  instead. The decision, stated directly above `ComputeViewshedFast` in
  `Viewshed.h`: **deliberately serial, not order-independent** — there is no
  per-frame pressure to change it, since a viewshed call is a planning-time cost
  rather than a per-frame one, and the comment states what would have
  to change (an order-independent per-cell reduction, e.g. a locked or
  atomic-compare-and-swap running max of slope) before that loop could safely be
  threaded. `ComputeViewshedNaive` has no such hazard today — each cell is
  written exactly once by its own independent call — and the comment above it
  says so.
- **A frame-safe line-of-sight path vs. a batch viewshed path, labelled in the
  headers** — the split already existed structurally (the scratch-buffer overload
  above is the frame-safe half; the viewsheds and `ComputeBatchLineOfSight` were
  always the batch half); this makes it explicit where a reader is looking at the code.
  `TerrainProfile.h`, `LineOfSight.h` and `Viewshed.h` each carry a
  `=== FRAME-SAFE LINE-OF-SIGHT PATH ===` or `=== BATCH PATH ===` banner comment
  above every public entry point, cross-referencing its counterpart: pair
  `GetTerrainProfile`'s in-place overload with `ComputeLineOfSight`/
  `ComputeFresnelClearance` for a per-frame query that allocates nothing;
  `ComputeBatchLineOfSight` and both viewsheds are the batch path, each
  allocating a profile per query/cell/ray, meant for a one-off batch or a
  planning-time call, never a per-frame loop.

A host application that already provides its own great-circle distance primitive
can still use it in place of this library's — `EarthRadiusM` (`TerrainProfile.h`)
is exposed precisely so a caller can stay consistent with a different distance
source without the two disagreeing on which sphere they're measuring over — but
that is now an optional substitution, not a correctness requirement.