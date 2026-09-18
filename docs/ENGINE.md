# The engine: model, accuracy and API

The reference for the C++ library behind `TerrainEngine.exe`, `TerrainEngineApi.dll` and
TerrainBench. The [README](../README.md) gives the overview.

## Components

`TerrainCore` contains the elevation-sampler interface (`IElevationSampler`), four
in-memory implementations of it (`FakeElevationSampler`, a tiny index-addressed grid for sampler tests,
`MultiTileElevationSampler`, and the raster-block pair `RasterBlockViewElevationSampler` /
`RasterBlockElevationSampler` — see
[INTEGRATION.md](INTEGRATION.md)), and the three algorithms (`GetTerrainProfile`,
`ComputeLineOfSight`, `ComputeViewshedNaive`/`ComputeViewshedFast`). It has no
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
4.85% then 4.29% — see [NOTES.md](../NOTES.md) for why); up to 4.65% when the viewshed grid's
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
3-arcsecond one at a finer grid (see [TESTS.md](TESTS.md)), so the ridgelines the disagreement
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
