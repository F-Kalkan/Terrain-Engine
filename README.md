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
2. **Line of sight** — can an observer at point A see a target at point B, each at a
   stated height (above the ground, above mean sea level, or above the ellipsoid)?
   If blocked, the position, elevation, and clearance deficit of the blocking point.
3. **Viewshed** — from one observer, which cells within a radius are visible, as a
   raster mask.

## Quick start

1. Clone this repository and open `TerrainEngine.sln` in Visual Studio 2022.
2. Build the solution (`x64`, `Release` recommended for real use).
3. Run `TerrainEngine.exe` with no arguments — this runs the 46-case test suite and a
   small demo against the included sample tile (`DATA/N36W112.hgt`, a 3-arcsecond
   stretch of the Grand Canyon), and writes `profile_output.pgm` / `viewshed_output.pgm`
   you can open in any image viewer that supports PGM (e.g. IrfanView, GIMP). If the
   1-arcsecond (~30 m) tile for the same square degree is at `DATA/SRTM1/N36W112.hgt`
   (NASA SRTMGL1 v003, `N36W112.SRTMGL1.hgt.zip`, unzipped), the tests and benchmark
   run against it too; without it, those parts skip.
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

`TerrainCore` contains the elevation-sampler interface (`IElevationSampler`), four
in-memory implementations of it (`FakeElevationSampler`, a tiny index-addressed grid for sampler tests,
`MultiTileElevationSampler`, and the raster-block pair `RasterBlockViewElevationSampler` /
`RasterBlockElevationSampler` — see "Adapting
toward a real-time host application" below), and the three algorithms (`GetTerrainProfile`,
`ComputeLineOfSight`, `ComputeViewshedNaive`/`ComputeViewshedFast`). It has no
include path to `TerrainReader` and cannot see `RealElevationSampler.h`.

`TerrainReader` contains `RealElevationSampler`, a reader for SRTM `.hgt` tiles —
3-arcsecond (1201×1201, ~90 m) or 1-arcsecond (3601×3601, ~30 m), the post count
inferred from the file size, and a file whose size isn't a whole square of posts
refused — with big-endian 16-bit signed elevations and void = `-32768`.

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
  the included tile's latitude (36.5°N) at a "30 km radius", measuring the real
  distance from observer to each grid edge through `GetTerrainProfile`: south and
  west reach exactly 30,000 m, north and east reach 29,970 m. That 30 m asymmetry
  is the same on both axes — an off-by-one from the grid's `gridSize / 2` integer
  division giving one more step on the south/west side than the north/east side —
  and is unrelated to the latitude correction; the point this verifies is that
  north/south and east/west reach the *same* pair of distances (30,000 m and
  29,970 m on both axes) rather than the ~24 km an uncorrected ellipse would give
  on the east/west axis.
- **Vertical datum**: modeled as an explicit, closed set (`VerticalDatum`, in
  `VerticalDatum.h`) — `EllipsoidalHae`, `OrthometricMsl`, `PressureAltitude`,
  `HeightAboveGround`, and `Unknown`. Every `IElevationSampler` declares the datum
  its elevations are in via `GetDatum()` (`RealElevationSampler` → `OrthometricMsl`,
  since SRTM is EGM96-referenced), and `GetTerrainProfile` stamps that datum onto
  every `ProfileSample` (`elevationDatum`), so the terrain's datum travels with the
  profile rather than being passed alongside it. Observer and target heights are a
  `DatumHeight` — a value, its datum, and optionally the local geoid undulation —
  given either as `HeightAboveGround` (added to the terrain under that end of the
  path) or as an absolute `OrthometricMsl` or `EllipsoidalHae` height. An absolute
  height in a different datum from the terrain is converted with
  `ConvertHeightBetweenDatums` using the undulation carried on the `DatumHeight`.
  That undulation is **required** for such a conversion, never defaulted to zero,
  because "zero undulation" silently asserts the point sits exactly on the geoid. A
  query is **rejected as a value** (`status` set to `ComputationStatus::DatumRejected`,
  never an exception) whenever the heights and the terrain can't be put on one datum:
  terrain declared `Unknown`, `HeightAboveGround` or `PressureAltitude`; a profile
  mixing datums; a height in `PressureAltitude` or `Unknown`; or an
  ellipsoidal↔orthometric move with no undulation to make it with.
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
- **Sampling**: the number of intervals along a path is the great-circle distance
  divided by the requested spacing, rounded **up**, so the effective spacing is never
  larger than `spacingDeg`; a profile always has at least 2 samples (the two
  endpoints). Rounding down used to make the effective spacing larger than asked
  whenever the distance wasn't an exact multiple of it — and a viewshed's axis rays are
  exact multiples by construction, where floating-point noise turned 1000.0 into
  999.9999… and a whole sample was dropped, leaving a cell on the ray unvisited. A tiny
  relative tolerance keeps the same noise from adding a spurious extra sample in the
  other direction. Sampling at the data's own spacing therefore visits every cell;
  sampling more coarsely than the data still can't promise to catch a feature narrower
  than the spacing (`TestNarrowSpikeCanFallBetweenSamples`).
- **Voids**: a void (source `-32768`, or a query outside the tile) is never treated as
  zero or sea level. It surfaces as `std::optional` at every layer: `GetElevation`
  returns `nullopt`, `LineOfSightResult::status` becomes `ComputationStatus::VoidInProfile`,
  and a viewshed cell whose line crosses one becomes `CellVisibility::Degraded` —
  never a silent "visible".

## Validity envelope

- SRTM 3-arcsecond (~90 m) and 1-arcsecond (~30 m) tiles (1°×1°), each addressed by
  its south-west corner. `RealElevationSampler` reads one tile; `MultiTileElevationSampler` stitches
  several (see "Tile boundaries" below).
- Distances up to the tested 50 km (profile) / 30 km radius (viewshed). The
  great-circle distance calculation itself has no inherent range limit, but the
  curvature model used for visibility (an effective-Earth-radius approximation,
  standard for line-of-sight and radio-propagation calculations) is not validated
  beyond the ranges tested here, and neither model handles a path that crosses the
  antimeridian (±180° longitude).
- No threading — determinism across thread counts is satisfied vacuously.
- **Unvalidated inputs, not exercised by any test or CLI path today:**
  - `spacingDeg <= 0` passed to `GetTerrainProfile` divides by zero
    (`sampleCount = totalDistanceM / (spacingDeg * metersPerDegree)`). The CLI
    refuses a non-positive spacing before calling it, and no test passes one.
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
`/fp:precise`, the real Grand Canyon tile at both resolutions (`DATA/N36W112.hgt` and
`DATA/SRTM1/N36W112.hgt`):

| Operation                                        | Tile         | Wall time     | Peak memory |
|--------------------------------------------------|--------------|---------------|-------------|
| 50 km profile @ 30 m spacing (1,668 samples)     | 3-arcsecond  | ~0.25–0.43 ms | ~10.7 MB    |
| 30 km-radius viewshed @ 30 m spacing (2000×2000) | 3-arcsecond  | ~1.01–1.04 s  | ~23.6 MB    |
| 50 km profile @ 30 m spacing (1,668 samples)     | 1-arcsecond  | ~0.42 ms      | ~55 MB      |
| 30 km-radius viewshed @ 30 m spacing (2000×2000) | 1-arcsecond  | ~1.07–1.08 s  | ~54.7 MB    |

The 1-arcsecond tile's higher peak memory is the tile, not the computation: 3601×3601
posts is ~25.9 MB of 16-bit elevations, nine times the 3-arcsecond tile, and the reader
briefly holds the file's raw bytes and the decoded elevations together while loading.
(The first 1-arcsecond profile run took 0.77 ms, a cold start; the next two took 0.42 ms.)

These come from the `TerrainEngine.exe` CLI, whose one translation unit also holds the
whole test suite, and they have moved twice. Switching from a flat-plane distance
approximation to a true great-circle one made the viewshed roughly 2.4x slower per
sample — trigonometry costs more than the handful of multiplications it replaced.
Later, the CLI's 30 km viewshed went from ~640 ms to ~1.02 s, but that jump is not the
library getting slower. Timed from a benchmark-only program with nothing else in the
executable, the same 30 km viewshed takes ~625–645 ms with the library as it was and
~640–675 ms as it is now — a few percent for the extra samples that rounding the sample
count up adds, and for samples sharing a cell with the previous one no longer being
skipped. The rest of the CLI's increase arrived with the test suite growing several-fold
inside the same executable. Each library change was also removed one at a time (the
sample-count rounding, the same-cell handling, the per-sample datum, the larger profile
sample, a pre-sized profile buffer) and none of them accounted for it; the most likely
cause is how the compiler optimises the benchmark's code path inside a much larger
binary. The profile figure barely moves either way because it does so little work.

Each row was measured as its own process (`TerrainEngine.exe benchmark profile ...` /
`TerrainEngine.exe benchmark viewshed ...`), so the two peak-memory figures are
independent — Windows' `PeakWorkingSetSize` is a running high-water mark for the whole
process and can't be reset mid-run, so measuring both operations in one process would
have let the first operation's peak leak into the second's number.

The `30 m` in each row is the sampling spacing passed to the CLI. On the 3-arcsecond
tile (~90 m posts) that oversamples between real data points via the chosen
interpolation mode rather than adding information the data doesn't have. On the
1-arcsecond tile it is the data's own resolution: posts are one arcsecond apart,
~30.9 m north-south and ~24.8 m east-west at this latitude. Wall time barely moves
between the two, because the work is set by how many samples are taken, not by what
they read.

## Fast vs. naive viewshed: accuracy and performance

On a 2 km-radius viewshed over the real SRTM tile (133×133 = 17,689 grid cells total,
all 17,689 of them valid and confident in both algorithms, **0 excluded**), the fast
(boundary-ray-sweep) algorithm disagrees with the naive (one-LOS-per-cell) algorithm
on **682 cells (3.86%)** over the 3-arcsecond tile and **450 cells (2.54%)** over the
1-arcsecond tile, each asserted to stay under a 5% tolerance
(`FastViewshedMatchesNaive on real SRTM data within stated tolerance`, once per tile,
in the CLI's test/benchmark output, which also prints the excluded-cell count directly
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
column spacing was corrected for latitude; down to 3.92% when the distance
calculation switched from a flat-plane approximation to a true great-circle one,
which brought both algorithms' independently-derived sightlines into closer
agreement on the ridgelines where the old approximation's error was concentrated;
down again to 3.60% when the fast viewshed stopped skipping samples that land in the
same cell as the previous one, so their terrain now reaches its horizon; and up to
the current 3.86% when the profile's sample count started rounding up, so that no ray
skips a cell. That last move is not a regression: rounding down made the two
algorithms agree slightly more often while silently dropping a sample on most
viewshed axis rays. It is comfortably under the 5% tolerance, which is worth knowing
rather than discovering under a slightly different tile or radius. The history above
is all on the 3-arcsecond tile. The 1-arcsecond figure comes from the same code over
different terrain data: that tile is a separately reprocessed product, not the
3-arcsecond one at a finer grid (see "Test suite"), so the ridgelines the disagreement
concentrates on are not identical between the two.

**Performance side of the same comparison** (Release build, the same real tiles):

| Radius / grid / tile                        | Naive              | Fast              | Speed-up    |
|---------------------------------------------|--------------------|-------------------|-------------|
| 2 km / 133×133 / 3-arcsecond                | ~107.6–109.5 ms    | ~4.93–5.00 ms     | ~21.8–21.9x |
| 2 km / 133×133 / 1-arcsecond                | ~109.4 ms (one run) | ~5.06 ms (one run) | ~21.6x     |
| 30 km / 2000×2000 / 3-arcsecond             | ~317.6 s (one run) | ~1.01–1.04 s      | ~310x       |

The 30 km naive viewshed takes over five minutes and was measured on the 3-arcsecond
tile only.

These are CLI timings too, with the same caveat as the table above: sharing the
executable with the test suite inflates both columns together (naive and fast each rose
by roughly the same ~55% when it grew). The speed-up ratio is the figure to read here —
both algorithms run in the same binary, so whatever that costs, it costs both.

Naive's cost grows faster than fast's as the radius grows: it runs one full profile +
line-of-sight per cell, so its total work scales with roughly (cell count) ×
(average profile length), both of which grow with radius, while fast only casts rays
to the boundary, so its work scales closer to the boundary's perimeter × profile
length. That is why the speed-up is larger at 30 km than at 2 km — and it is the
number this project was missing: the performance table above used to report the fast
viewshed's time in isolation, with no naive baseline next to it, so the actual
speed-up this design buys was never stated.

## Void handling and degraded results

Asserted by tests (`TestVoidPointIsDegraded`, `TestViewshedDetectsVoid`,
`TestViewshedsAgreeWhenObserverIsUnknown`). When nothing is known about the observer —
it stands on a void, or its height can't be put on the terrain's datum — both viewsheds
return every cell `Degraded`, the observer's own cell included. A missing or
unreadable elevation file is also distinguished from a genuine void:
`RealElevationSampler::IsLoaded()` reports load failure explicitly — a missing file, or
one whose size isn't a whole square of posts, as a download cut short would be — and the CLI refuses
to run rather than silently treating every query as void.

## API contract

Every public result type (`ProfileSample`, `LineOfSightResult`, `ViewshedResult`) is a
plain struct of value types. `ProfileSample::elevationM` and `LineOfSightResult`'s
optional fields use `std::optional` for absence, not a sentinel value.
`ViewshedResult::visible` instead uses an explicit `CellVisibility` enum
(`NotCovered` / `Degraded` / `Visible` / `NotVisible`) — a single `optional<bool>`
cannot distinguish "no ray ever reached this cell" from "a ray reached it but crossed
a void partway", and a caller needs to tell those two apart. Similarly, a height
crossing the API boundary (`observerHeight`/`targetHeight`) is never a bare `double` —
it is a `DatumHeight` — and every `ProfileSample` carries the datum of its own
elevation, so `ComputeLineOfSight(profile, observerHeight, targetHeight, k)` and
`ComputeFresnelClearance(profile, observerHeight, targetHeight, frequencyHz, k)` take
no separate terrain-datum argument that could disagree with the profile. A query that
can't be put on one datum is rejected as a value
(`LineOfSightResult::status == ComputationStatus::DatumRejected`), not silently
computed or thrown. A viewshed asked for a grid with no rows or no columns returns an
empty `ViewshedResult`. Nothing in `TerrainCore` or `TerrainReader` throws across its
own boundary, other than the standard library's `std::bad_alloc` if memory runs out.

## Building

Open `TerrainEngine.sln` in Visual Studio 2022, build the `x64` platform (`Debug` or
`Release`). `TerrainCore` and `TerrainReader` build as static libraries; `TerrainEngine`
links both and produces `TerrainEngine.exe`.

## CLI usage
```
TerrainEngine.exe # run the 46-case test suite + demo
TerrainEngine.exe benchmark <profile|viewshed> <hgtFile> <swLat> <swLon>
TerrainEngine.exe profile <hgtFile> <swLat> <swLon> <aLat> <aLon> <bLat> <bLon> <spacing> [nearest|bilinear]
TerrainEngine.exe los <hgtFile> <swLat> <swLon> <aLat> <aLon> <bLat> <bLon> <spacing> <hA> <hB> [k] [nearest|bilinear]
TerrainEngine.exe viewshed <hgtFile> <swLat> <swLon> <obsLat> <obsLon> <gridSize> <spacing> <height> [k] [nearest|bilinear]
TerrainEngine.exe fresnel <hgtFile> <swLat> <swLon> <aLat> <aLon> <bLat> <bLon> <spacing> <hA> <hB> <frequencyMHz> [k]
TerrainEngine.exe batch <hgtFile> <swLat> <swLon> <queriesFile> [k]
```
Example, using the included Grand Canyon tile: TerrainEngine.exe los DATA/N36W112.hgt 36.0 -112.0 36.3 -111.5 36.35 -111.45 0.0002694 2.0 2.0

Every numeric argument is checked before anything runs: text that isn't a whole, finite
number, or a spacing, `k`, frequency or grid size that isn't greater than zero, is
reported by the argument's name with exit code 1. A `batch` queries file is a positive
spacing followed by six numbers per query (`aLat aLon bLat bLon hA hB`); a word where a
number belongs, or a query cut short at the end of the file, is reported the same way
rather than silently skipped.

## Test suite

46 hand-checkable test functions, plus a real-data tolerance assertion per tile:

- **Line of sight and profile arithmetic** (30 m grids or hand-built profiles, no file
  on disk): flat plateau, wall, curvature, void, determinism,
  symmetric-hill reciprocity, observer below a rim, target on a far slope, Fresnel
  partial obstruction, batch line of sight against individual calls, blocking-feature
  classification and its invariance to sample spacing, interpolation modes on a
  ridgeline, empty and single-sample profile guards, and a sight line interpolated by
  distance rather than by sample index on an unevenly spaced profile. Every assertion
  that a line is visible, or that two results agree, also asserts the computation
  succeeded — a rejected query leaves `isVisible` at its default of `true`, and would
  otherwise pass without computing anything. Terrain grids are laid on the ground in
  30 m cells and run through `GetTerrainProfile` unmodified, so the distances these
  tests see are the real great-circle ones; hand-built profiles place each sample
  where its stated distance says it is.
- **Viewsheds at real scale** (`RasterBlockElevationSampler`, 30 m cells aligned with
  the viewshed grid): a grid larger than its data (off-data cells degraded, the rest
  confident); fast against naive behind a wall, alongside the same scene without the
  wall hiding nothing; void propagation behind a hole on a straight ray and on every
  sample of a diagonal ray; and both algorithms agreeing when the observer stands on a
  void or its height can't be converted; and a grid with no rows or no columns coming
  back empty from both, rather than written into or resized to an enormous size.

No test that computes a profile, a line of sight or a viewshed reads a synthetic grid
as degrees, which would be ~111 km per cell — a scale at which Earth curvature, not
terrain, decides every answer.
- **Earth curvature**: a 20 km viewshed over flat ground at 1 km cells must see strictly
  more cells with curvature switched off than with it on — the assertion that fails if
  `k` ever stops reaching the geometry.
- **Distance and sampling**: great-circle distances against values exact on the sphere
  (one degree of arc, a quarter circle, equator to pole); a great-circle path bulging
  toward the pole where degree-space interpolation would not; a profile never sampled
  more coarsely than requested (exact multiples along a meridian and a parallel, and a
  non-multiple); a narrow spike seen at the data's spacing and missed at a coarser one;
  and the viewshed's longitude spacing correcting for latitude.
- **Datums**: an ellipsoidal↔orthometric round trip with a known undulation; the same
  line of sight given as above-ground, orthometric and ellipsoidal heights producing
  the identical blocked-by-15-m answer; rejection of an ellipsoidal height without an
  undulation, a pressure altitude, terrain declared above-ground or `Unknown`, and a
  profile mixing datums; and `RealElevationSampler` always declaring `OrthometricMsl`.
- **Samplers**: a multi-tile seam, by point query and by a profile crossing it; a
  resident raster block with a positive row step, a negative row step (proving the
  signed step — not an unwritten convention — decides row order), a void cell passed
  through, and a full line-of-sight run against it; a view over a flat row-major buffer
  and its validity flags agreeing with the owning sampler cell for cell, and seeing a
  change made to the buffer after construction; a 2×2 `.hgt` written with a real
  void sentinel and read back through `RealElevationSampler`; tile files whose size
  isn't a whole square of posts (empty, one post, a post short, an odd byte, a post
  too many) refused rather than read past their end; and the in-place profile
  overload reusing a caller-owned buffer without growing it.
- **Command line**: the number parser behind every CLI argument accepting real
  numbers and refusing text, trailing characters, `nan`, `inf` and out-of-range
  values, instead of throwing.
- **1-arcsecond data** (skipped when `DATA/SRTM1/N36W112.hgt` isn't present): 200
  consecutive posts read through `GetTerrainProfile` at the tile's own spacing must
  each return exactly the value stored in the file at that row and column; and the 1-
  and 3-arcsecond tiles, read at every interior 3-arcsecond post, must agree to within
  10 m on average (6.92 m measured) and agree best with no offset — shifting the fine
  tile one post north, south, east or west makes the agreement worse. The two are
  separate products (SRTM v2.1 and the reprocessed, void-filled SRTMGL1 v3) and are not
  expected to match exactly: 7.5% of shared posts are identical, 76.6% agree within
  5 m, and 2% differ by more than 50 m, the largest by 717 m.
- **Frozen golden output**: `TestProfileMatchesFrozenOracle` regenerates a real 2.9 km
  profile across the included tile at its ~90 m spacing (33 samples) and diffs it field
  by field against `DATA/oracle_profile.csv`. It reads `DATA/` relative to the working
  directory and skips, rather than fails, when the data isn't there; a damaged row fails
  the test and names the row, rather than throwing and taking the suite down with it.
- **Real data**: a tolerance assertion comparing naive and fast viewshed output over
  each Grand Canyon tile present (`FastViewshedMatchesNaive on real SRTM data within
  stated tolerance`, printed by `RunWallTimeBenchmark`).

Each correctness fix above was confirmed by putting the defect back in a scratch copy
of the code and watching its test fail: curvature forced inert inside both viewsheds,
the profile's sample count rounded down again, a void skipped when its sample shares a
cell with the previous one, the sight line interpolated by sample index, the observer's
own cell marked visible unconditionally, an ellipsoidal height used without its
undulation, every profile distance halved, and the tile reader placing its grid one
post off or assuming 3-arcsecond post spacing for a 1-arcsecond file, and the raster
view copying its buffer at construction or ignoring its validity flags. A suite that
stays green with the defect restored verifies nothing.

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
  the result's status is `ComputationStatus::NothingEvaluated` rather than a false
  "fully clear".

- **Batch line of sight** — `ComputeBatchLineOfSight` (in `LineOfSight.h`) takes a list
  of `BatchLineOfSightQuery` entries (`observer`, `observerHeight`, `target`,
  `targetHeight`, the heights as `DatumHeight`s), a single shared spacing and
  `IElevationSampler`, returning one
  `LineOfSightResult` per query. The shared
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
  assumptions" above. A host typically holds an airborne platform's altitude as a height
  above the ellipsoid while its terrain is orthometric. An observer or target can be
  given in either datum, with the local geoid undulation the conversion needs, and the
  library converts it itself — the arithmetic a caller would otherwise have to do
  outside the very type meant to guard it. The terrain's datum rides on every profile
  sample, so it can't be supplied inconsistently with the profile.
- **A raster-block `IElevationSampler`** — `RasterBlockViewElevationSampler` (in
  `IElevationSampler.h`) is a view over a block of elevation data someone else already
  holds, in the shape a raster reply arrives in: one flat row-major elevation array
  plus a parallel per-cell validity array, read in place with nothing copied. A
  validity flag of 0 is a void and the elevation beside it is never read, so per-cell
  validity is carried through rather than flattened. The view is non-owning: the
  arrays must outlive it and must not change while a query or viewshed is running.
  The block is described by a `RasterBlockGeometry` — the **centre** of cell (0, 0) as
  origin plus **signed** per-row/per-column degree steps — with no assumed resolution
  (unlike `RealElevationSampler`, which infers its size from a `.hgt` file's byte
  count) and no hardcoded 1°×1° box (unlike `MultiTileElevationSampler`). Lookups
  round to the nearest cell centre, which is what a raster reply means by its origin.
  `RasterBlockElevationSampler` is the owning counterpart, for data that doesn't
  already live in a buffer (the tests build their grids with it); it stores the same
  flat layout and maps coordinates to cells through the same `RasterBlockCellIndex`,
  so the two cannot disagree. The view reads elevations as `double` and flags as
  bytes; a buffer of another element type would need the class templated on it, or
  one conversion at the boundary. Answering a query is index arithmetic, not I/O. The row
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
  `LineOfSightResult::blockingElevationM`/`clearanceDeficitM`, `observerHeight`/
  `targetHeight` as `DatumHeight`s whose type carries the frame (replacing bare
  `hA`/`hB` doubles), `spacingDeg` (replacing
  bare `spacing`), and `IElevationSampler::GetElevation(latitudeDeg, longitudeDeg)`
  across every implementation (`FakeElevationSampler`, `MultiTileElevationSampler`,
  `RasterBlockViewElevationSampler`, `RasterBlockElevationSampler`, `RealElevationSampler`). Internal locals carrying
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