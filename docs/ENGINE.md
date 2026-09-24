# The engine: model, accuracy and API

The reference for the C++ library behind `TerrainEngine.exe`, `TerrainEngineApi.dll` and
TerrainBench. The [README](../README.md) gives the overview.

## Components

`TerrainCore` contains the elevation-sampler interface (`IElevationSampler`), four
in-memory implementations of it (`FakeElevationSampler`, a tiny index-addressed grid for sampler tests,
`MultiTileElevationSampler`, and the raster-block pair `RasterBlockViewElevationSampler` /
`RasterBlockElevationSampler` — see
[INTEGRATION.md](INTEGRATION.md)), the three algorithms (`GetTerrainProfile`,
`ComputeLineOfSight`, `ComputeViewshedNaive`/`ComputeViewshedFast`), and
`CompareViewsheds` (`ViewshedAgreement.h`), which measures an approximate viewshed
against its exact reference -- see "Fast vs. naive viewshed" below. It has no
include path to `TerrainReader` and cannot see `RealElevationSampler.h`.

`TerrainReader` contains `RealElevationSampler`, a reader for SRTM `.hgt` tiles —
3-arcsecond (1201×1201, ~90 m) or 1-arcsecond (3601×3601, ~30 m), the post count
inferred from the file size, and a file whose size isn't a whole square of posts
refused — with big-endian 16-bit signed elevations and void = `-32768`. It reports a
loaded tile's posts per side, void count and south-west corner, and takes its path as a
`std::filesystem::path`, so a folder or file name with non-ASCII characters opens too.

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
| 30 km-radius viewshed @ 30 m spacing (2000×2000) | 3-arcsecond  | ~1.70–1.81 s  | ~61.7 MB    |
| 50 km profile @ 30 m spacing (1,668 samples)     | 1-arcsecond  | ~0.42 ms      | ~55 MB      |
| 30 km-radius viewshed @ 30 m spacing (2000×2000) | 1-arcsecond  | ~1.69–1.70 s  | ~83.7 MB    |

The two viewshed rows moved with the fast viewshed's current algorithm, which answers
every cell from its own centre between two rays and keeps every ray's horizon to do it
(see "Fast vs. naive viewshed" below). Measured the same day, on the same machine, the
previous algorithm took ~1.22 s and ~24 MB (3-arcsecond) and ~1.19 s and ~55 MB
(1-arcsecond); the extra memory is those horizons, one float per ray sample. The earlier
~1.01–1.08 s recorded here for that algorithm came from a quieter run of the machine; the
same-day pair is the one to compare.

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

**What each one does.** Naive answers every cell along its own line: a profile from the
observer to the cell's centre, and `ComputeLineOfSight` on it. Fast casts one ray to each
cell on the grid's boundary and keeps, along each, the horizon: the largest
curvature-adjusted slope `s(d) = (h - eye) / d - d / 2kR` of the terrain so far. That is
`ComputeLineOfSight`'s own test with the target's distance cancelled out
(`CurvatureAdjustedSlope`, `Viewshed.h`), so one running maximum along a ray serves every
cell beside it. Every cell is then answered from its own centre -- its ground, its
distance, a target standing on it -- against the horizon of the two rays either side of
it, blended by the cell's direction between them. The last half cell of a ray before the
target is left out of its horizon: that terrain lies in the target's own cell, beside the
line to its centre. The rays pass beside most cells rather than through their centres, so
fast is an approximation, and the question is how far from naive it is, and where.

**The measure** (`CompareViewsheds`, `ViewshedAgreement.h`). Differences are counted over
the cells both answer with confidence, and divided by the cells *either* finds visible --
not by every cell. Most of a grid is usually hidden, and a rate over every cell mostly
measures that: at (36.5, -111.5) on the 1-arcsecond tile 95% of the 2 km grid is hidden,
and a "viewshed" answering NotVisible everywhere differs on 4.80% of all cells, but on
100% of the cells either sees. The counts behind each rate are kept, one direction at a
time. Each differing cell is also placed: **on naive's visibility edge** when some
confident 8-neighbour gets the other answer from naive -- fast drew the boundary one cell
off -- or **off the edge**, where no neighbour agrees with fast.

A one-cell shift of the boundary is about what naive's own answer is worth there. Moving
the observer 1 m north changes naive on 1.3-6.4% of the cells either run sees, and 5 m on
6.9-26% (the same observers and radii as below). So the tolerance has two parts: the whole
disagreement, and the part off the edge.

**Measured** -- 30 m cells, 2 m above ground at both ends, `k` 4/3, nearest; every figure
comes from `TestFastViewshedAgreesWithNaiveAtThreeObservers`, which prints them:

| Observer, radius, tile | Naive sees | Fast only | Naive only | Differ, of cells either sees | Off naive's edge | On the edge |
|---|---|---|---|---|---|---|
| (36.5, -111.5), 2 km, 1" | 850 | 107 | 137 | 25.5% | 1.25% | 95.1% |
| (36.5, -111.5), 5 km, 1" | 6,361 | 535 | 788 | 19.2% | 1.71% | 91.1% |
| (36.86361, -111.30861), 2 km, 1" | 9,254 | 90 | 134 | 2.4% | 0.34% | 85.7% |
| (36.86361, -111.30861), 5 km, 1" | 56,599 | 2,041 | 2,275 | 7.4% | 0.86% | 88.4% |
| (36.55861, -111.81361), 2 km, 1" | 7,054 | 372 | 465 | 11.3% | 1.19% | 89.5% |
| (36.55861, -111.81361), 5 km, 1" | 31,573 | 2,212 | 2,343 | 13.5% | 0.90% | 93.3% |
| (36.5, -111.5), 2 km, 3" | 1,996 | 218 | 185 | 18.2% | 2.62% | 85.6% |

The three 1-arcsecond observers see little (5-6% of the grid), about half, and in between.
Each run asserts: the whole disagreement under **30%**; the part off the edge under **2%**
on the 1-arcsecond tile and **3%** on the 3-arcsecond one, whose 90 m posts, read at 30 m,
stand as terraces three cells wide; at least **80%** of differences on the edge; and a
viewshed with one answer everywhere failing the same tolerance. So the disagreement is not
concentrated on ridgelines, as this section used to argue without measuring, but on the
edge of the visible region, balanced between the two directions: fast is neither
systematically optimistic nor pessimistic.

**The change behind these numbers.** Until this version fast judged each cell by the first
sample of whichever single ray passed nearest, at that sample's own position and height --
up to half a cell from the centre naive tests. On the same runs, measured the same way, it
differed on 41.7%, 32.7%, 3.3%, 10.9%, 17.7% and 23.4% of the cells either sees on the
1-arcsecond tile, and 29.1% on the 3-arcsecond one; off the edge, 2.60%, 3.07%, 0.45%,
1.33%, 1.55%, 1.70% and 3.54%. Asking about the centre, leaving the target's own half cell
out of the horizon and blending two rays took the whole disagreement down by about a third
at every run, and the part off the edge by a quarter to a half. [NOTES.md](../NOTES.md) has
the experiments and why these three changes.

Under the old measure, over every confident cell, the figure recorded here was 3.86% on the
3-arcsecond tile and 2.54% on the 1-arcsecond one, at (36.5, -111.5), 2 km; its earlier
history on the 3-arcsecond tile (4.85% and 4.29% after two early attempts to shrink it,
4.65% with the grid's latitude correction, 3.92% with great-circle distances, 3.60% once
samples sharing a cell reached the horizon, 3.86% once the sample count rounded up) is in
[NOTES.md](../NOTES.md) and isn't comparable with the rates above.

**Performance side of the same comparison** (the machine above, Release build, the same
real tiles; CLI timings, so the caveat above applies to both columns alike):

| Radius / grid / tile | Naive | Fast | Speed-up |
|---|---|---|---|
| 2 km / 133×133 / 3-arcsecond | ~121–138 ms | ~8.9–9.5 ms | ~13.6–15.5x |
| 2 km / 133×133 / 1-arcsecond | ~129–131 ms | ~8.3–9.6 ms | ~13.7–15.8x |
| 30 km / 2000×2000 / 3-arcsecond | ~240.3 s (one run) | ~1.32 s (same program) | ~183x |

The 30 km naive viewshed takes about four minutes and was measured on the 3-arcsecond
tile only, in a benchmark-only program with the fast one beside it -- where fast takes
1.32 s rather than the CLI's ~1.75 s, for the reason given under Performance above. The
previous fast viewshed was about 1.45 times quicker than this one (1.22 s against 1.75 s
from the CLI at 30 km, measured the same day), and the speed-ups at 2 km were ~22x before
this version.

Naive's cost grows faster than fast's as the radius grows: it runs one full profile and
line of sight per cell, so its work scales with roughly (cell count) × (average profile
length), while fast casts rays only to the boundary and then answers each cell with a
lookup, so its work scales closer to the boundary's perimeter × profile length plus the
cell count. That is why the speed-up is larger at 30 km than at 2 km.

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
empty `ViewshedResult`. Both viewsheds take an optional progress callback that can stop
them early, which marks the result `cancelled`; without one they run exactly as before.
Nothing in `TerrainCore` or `TerrainReader` throws across its
own boundary, other than the standard library's `std::bad_alloc` if memory runs out.

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

## Beyond the three questions

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
