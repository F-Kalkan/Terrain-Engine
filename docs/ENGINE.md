# The engine: model, accuracy and API

The reference for the C++ library behind `TerrainEngine.exe`, `TerrainEngineApi.dll` and
TerrainBench. The [README](../README.md) gives the overview.

## Components

`TerrainCore` contains the elevation-sampler interface (`IElevationSampler`), four
in-memory implementations of it (`FakeElevationSampler`, a tiny index-addressed grid for sampler tests,
`MultiTileElevationSampler`, and the raster-block pair `RasterBlockViewElevationSampler` /
`RasterBlockElevationSampler` — see
[INTEGRATION.md](INTEGRATION.md)), the three algorithms (`GetTerrainProfile`,
`ComputeLineOfSight`, `ComputeViewshedNaive`/`ComputeViewshedFast`), the minimum visible
height built on them (`MinimumVisibleHeight.h`, see "Minimum visible height" below), the
prepared observer that answers many targets a second from one place (`PreparedObserver.h`,
see "Many answers per second from a fixed observer" below), many pairs on every core
(`LineOfSightPairs.h`, `Threads.h`), the boxes of data queries will read (`QueryExtent.h`, see
"Data not given, and the data a query will read" below), and
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
- **Voids, and data not given**: missing ground is never treated as zero or sea level,
  and it comes in two kinds, told apart at every layer. A **void** is a hole in data the
  engine was given (a `-32768` post, a raster cell flagged invalid); **data not given** is
  ground it never had (off the tile, outside a raster block or window, no tile registered).
  `IElevationSampler::Sample` says which (`ElevationData`); `GetElevation` still answers
  `nullopt` for both. A profile sample marks the second with `dataNotGiven`; a line of sight
  reports `EndpointMissing` or `VoidInProfile` for a void and `DataNotGiven` for ground not
  given; a viewshed or minimum-visible-height cell is `Degraded` for a void and
  `DataNotGiven` for ground not given; a prepared observer's target likewise. Where a path
  meets both, the void wins: nothing loaded will fill it, whereas `DataNotGiven` means that
  given the data, the path may yet be answered. Never a silent "visible".

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
- Threaded where the work is many independent lines of sight -- line of sight for many pairs,
  the naive viewshed and the exact minimum visible height -- and bit-identical at every thread
  count (see "Many pairs on every core" below); everything else runs on the calling thread.
- **Inputs outside the domain are refused, by the library itself** -- not only by the DLL
  and the CLI in front of it, since code that links the library goes through neither.
  Each entry point answers such an input with a value naming the problem
  (`InputProblem`, `TerrainProfile.h`), never a confident answer and never undefined
  behaviour:
  - **Spacing** that is zero, negative, NaN or infinite, or so fine that the path's
    interval count won't fit in an `int`. `GetTerrainProfile` returns an empty profile
    (its in-place overload also returns the reason; `CheckProfileRequest` says it for
    either), on which `ComputeLineOfSight` and `ComputeFresnelClearance` report
    `EmptyOrSingleSampleProfile`. A spacing of zero or less once came back as a
    two-sample profile -- the endpoints alone -- and a confident "visible" across
    terrain that blocks the view at 30 m.
  - **Coordinates** that are NaN or infinite, or a latitude beyond ±90° (`CheckPoint`).
    Every sampler answers such a coordinate, or a finite one far off its data, with no
    elevation, checked before anything is cast to an `int`.
  - **Heights** that are NaN or infinite, or carry such an undulation; **k** that is zero,
    negative, NaN or infinite; a **Fresnel frequency** likewise. The line of sight and
    Fresnel clearance report `ComputationStatus::InvalidInput` with the reason in
    `inputProblem`; `ComputeBatchLineOfSight` does the same per query, answering the rest.
    A very large k -- curvature switched off -- is still a k.
  - **A viewshed grid that would reach a pole**: `LongitudeSpacingForLatitude` divides by
    `cos(latitude)`, and a row at or past a pole has no longitude to lay its columns
    along. Both viewsheds, like for every other problem above, return an empty grid with
    `ViewshedResult::inputProblem` set (`CheckViewshedRequest`).
  One test per class of input, through the library rather than the DLL, in
  [TESTS.md](TESTS.md).

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
- Bit-identical across thread counts: `ComputeLineOfSightPairs`, `ComputeViewshedNaive` and
  `ComputeMinimumVisibleHeightReference` give the same answers, to the bit, on 1, 2, 3, 7
  threads and one per hardware thread -- each pair or cell is its own computation, written to
  its own place, with nothing summed or ordered across threads -- and the tests compare them
  field by field (see "Many pairs on every core" below).

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

## Minimum visible height

**The question.** For one observer and a radius: for every cell, the lowest height above
the cell's ground at which a target standing on its centre is seen -- 0 where the ground
itself is. A viewshed answers "is a target this high seen?" for one height; this answers it
for all of them. `ComputeMinimumVisibleHeightReference` and `ComputeMinimumVisibleHeightFast`
(`MinimumVisibleHeight.h`) return a `MinimumVisibleHeightResult` with, per cell:

- `state` -- the viewshed of the ground itself: `Visible` where the ground is seen,
  `NotVisible` where only a target above it is, and `NotCovered` or `Degraded`, for the same
  reasons the viewshed gives them, where there is no confident answer;
- `heightAboveGroundM` -- the answer; NaN where there is no confident answer, and infinity
  where no height is seen (an observer whose eye is below the ground under it sees nothing);
- `groundM` -- the ground under the cell's centre that the answer was computed on, in
  `terrainDatum`. `MinimumVisibleAbsoluteHeightM` gives the same answer as an absolute
  height: that ground plus the height.

`ViewshedAtTargetHeight(result, H)` turns it back into the viewshed for a target `H` above
every cell. `MinimumVisibleHeightAlongProfile` answers the same question for one path.
Through the DLL it is `te_minimum_visible_height`, over the same grid, checks and refusals
as `te_viewshed`, with the states and the heights as two arrays; TerrainBench draws it as a
map layer with a legend in metres ([TERRAINBENCH.md](TERRAINBENCH.md)).

**The model.** A target `D` away with its eye at height `T` is seen when its
curvature-adjusted slope is at least that of every point in front of it (see "Fast vs.
naive viewshed" above for `s`):

```
(T - eye) / D  -  D / 2kR  >=  S,      S = the largest s(d) = (h(d) - eye) / d - d / 2kR, d < D
```

Solved for `T`, the lowest target eye height seen is

```
T = eye + D * S + D^2 / 2kR,           and above the ground g under the target: max(0, T - g)
```

This is `ComputeLineOfSight`'s test turned round. The terrain at `d` blocks the line to `T`
when, raised by the curvature drop, it stands above the line:
`h(d) + d (D - d) / 2kR > eye + (d / D)(T - eye)`. Multiplied by `D / d` and rearranged, that
is `T < eye + D s(d) + D^2 / 2kR`, one sample at a time; the largest of these is the answer.

**On a smooth sphere** -- level ground at 0 m everywhere, an eye `h` above it -- every term
has a closed form. There `s(d) = -h / d - d / 2kR`, whose derivative `h / d^2 - 1 / 2kR` is
zero at

```
d_h = sqrt(2kR h)                      the horizon
```

`s` rises up to `d_h` and falls after it. So a target inside the horizon has nothing in front
of it steeper than itself, and the ground there is seen: the answer is 0. Beyond it,
`S = s(d_h) = -h / d_h - d_h / 2kR = -2 d_h / 2kR`, since `h = d_h^2 / 2kR`. Then

```
T = h - 2 D d_h / 2kR + D^2 / 2kR = (d_h^2 - 2 D d_h + D^2) / 2kR = (D - d_h)^2 / 2kR
```

-- the familiar radio-horizon result: past the horizon, the height needed grows with the
square of the distance beyond it. For a 10 m eye and `k` = 4/3, `2kR` = 16,989,333 m and
`d_h` = 13,034 m; a target 30 km away must stand (16,966 m)^2 / 16,989,333 m = 16.94 m above
the ground. The engine finds `S` among its samples rather than at `d_h` itself, and the
sample nearest `d_h` is at most half a spacing `s` from it, where `s(d)` falls short of its
peak by at most `1/2 |s''| (s/2)^2`, with `s''(d) = -2h / d^3`. The answer is `D` times that
short. `TestMinimumVisibleHeightOnASmoothSphereMatchesTheClosedForm` holds both versions to
the closed form, with its tolerances derived that way: 10^-4 m along single paths at 30 m out
to 50 km (measured, at most 4.2 x 10^-5 m), and 0.03 m over a grid of 300 m cells (measured,
at most 2.1 mm).

**Exact, not nearly.** `ComputeLineOfSight` compares a sight line with the terrain; the
formula above compares slopes. On paper they are the same test; in doubles they part in the
last bits, so near the answer the formula can say "seen" where the line of sight says
"blocked". The formula is therefore only the estimate. Whether a target is seen can only go
from "no" to "yes" as it rises -- every step from its height to the verdict preserves order,
rounding included -- so there is one smallest double at which the line of sight first says
"yes", and that is the answer. `SmallestHeightSeen` walks to it from the estimate:
non-negative doubles are ordered like their bit patterns, so it steps up or down through them
doubling the step until the answer is between two heights, then halves the gap. The
estimate is usually some hundreds of doubles off -- its terms are large and cancel -- so a
cell whose ground is hidden takes a median of 16 line-of-sight calls at (36.5, -111.5) and
10 at (36.55861, -111.81361), 2 km on the 1-arcsecond tile, and a cell whose ground is seen
takes one. Building the profile costs more than all of them: the reference takes 1.0-1.6
times naive's time at 2 and 5 km, and 1.4 times at 30 km. The result is that
`ViewshedAtTargetHeight(reference, H)` is, cell for cell, `ComputeViewshedNaive` with a
target of height `H`, at every height -- `TestMinimumVisibleHeightThresholdedIsTheViewshedAtThatHeight`
asks at every height the grid holds and at the double just below each. The fast version is
settled the same way by the fast viewshed's own test, so it is, cell for cell,
`ComputeViewshedFast` at every height.

**The reference and the fast version.** The reference answers every cell along its own exact
line, with `MinimumVisibleHeightAlongProfile` on the profile naive would build for it; it is
kept as the reference the way naive is. The fast version shares the fast viewshed's rays
(`AnswerEachCellFromFastHorizons`, `Viewshed.h`): each cell's horizon is the blend of the two
rays either side of it, and the formula and the search above turn it into a height. Measured
against the reference the way the fast viewshed is measured against naive -- `CompareViewsheds`
on the viewsheds both give for a target of 0, 2, 10, 30 and 100 m, with the same tolerances --
at the same observers (30 m cells, 2 m eye, `k` 4/3, nearest, 1-arcsecond tile; every figure
printed by `TestMinimumVisibleHeightFastAgreesWithTheReferenceAtThreeObservers`):

| Observer, radius | Differ at 0 m | 2 m | 10 m | 30 m | 100 m | Worst off the edge | Heights apart: median, 90th, 99th percentile |
|---|---|---|---|---|---|---|---|
| (36.5, -111.5), 2 km | 25.5% | 10.9% | 3.83% | 1.07% | 0.45% | 1.25% | 0.21 m, 1.27 m, 10.4 m |
| (36.5, -111.5), 5 km | 19.2% | 7.46% | 2.55% | 0.77% | 0.35% | 1.71% | 0.24 m, 1.12 m, 11.2 m |
| (36.86361, -111.30861), 2 km | 2.40% | 1.64% | 1.40% | 1.05% | 1.28% | 0.34% | 0 m, 2.82 m, 20.8 m |
| (36.86361, -111.30861), 5 km | 7.36% | 3.21% | 1.49% | 0.67% | 0.29% | 0.86% | 0.002 m, 1.60 m, 9.77 m |
| (36.55861, -111.81361), 2 km | 11.3% | 6.96% | 2.63% | 1.71% | 1.02% | 1.19% | 0.14 m, 6.45 m, 32.5 m |
| (36.55861, -111.81361), 5 km | 13.5% | 5.79% | 1.43% | 0.73% | 0.56% | 0.90% | 0.15 m, 3.22 m, 31.0 m |

"Differ" is over the cells either one sees, as for the viewsheds. At 0 m this is the fast
viewshed's comparison with naive exactly. A taller target sees over the terrain that decides
the edge at ground level, and the two agree more closely as it rises. Every run is within the
fast viewshed's tolerances at every height, with at least 85% of differences on the
reference's edge. The heights themselves are close for most cells and far apart for a few:
where a ray beside the cell's line crosses a ridge that the line itself misses, or the other
way round, the two answers are on different sides of it -- the same cells the viewsheds
disagree on.

**Performance** (the machine above, Release build; `TerrainEngine.exe benchmark minheight`
and `benchmark viewshed`, three runs each, one process per run):

| 30 km radius @ 30 m spacing (2000×2000) | Tile | Wall time | Peak memory |
|---|---|---|---|
| Fast viewshed | 3-arcsecond | ~1.68–1.76 s | ~61.7 MB |
| Fast minimum visible height | 3-arcsecond | ~1.95–1.97 s | ~125.3–125.6 MB |
| Fast viewshed | 1-arcsecond | ~1.64–1.71 s | ~83.8 MB |
| Fast minimum visible height | 1-arcsecond | ~1.87–1.88 s | ~147.3–147.6 MB |

The time added is small: the rays are the fast viewshed's, and a cell whose ground is
hidden takes a few more comparisons. The memory added is the answer itself -- a height and
a ground in doubles, and a state, for each of four million cells -- held beside the rays.
The reference, run once in a benchmark-only program on the 3-arcsecond tile, took **340 s**
for the same grid, against 1.52 s for the fast version in the same program (~220x); the
naive viewshed took 240 s there. With both answers held at once the program peaked at
168.6 MB. At that radius the two differ, at 0 m, on 13.7% of the cells either sees, and
3.13% off the edge -- just past the 3% the tests hold the 3-arcsecond tile to at 2 km. That
is the fast viewshed's own figure at 30 km on the coarser tile, which the tests don't run:
the reference takes six minutes there.

## Many answers per second from a fixed observer

**The question.** A ground observer stays put while its targets move on every update: a
few dozen observers against a few hundred targets, several times a second, each target
anywhere within 50 km and at any height up to 15,000 m above mean sea level. One direct line
of sight at 50 km costs ~170-190 µs from the CLI here, 5,000-6,000 a second, and re-reads the
same terrain every time.

**The approach** (`PreparedObserver.h`). `PrepareObserver` reads the terrain once: rays from
the observer in evenly spaced directions -- as many as it takes for neighbouring rays to be
one sample spacing apart at the radius, 10,472 for 50 km at 30 m -- each keeping its horizon,
the running maximum of the curvature-adjusted slope `s(d)`, one float per sample.
`QueryTarget` then answers a target the way the fast viewshed answers a cell: the target's
bearing picks the two rays either side of it and its distance the sample in front of it; the
two horizons are blended by the bearing between them, the last half spacing in front of the
target left out as in the fast viewshed; and the target is seen when its own slope is at
least that horizon. A target below the ground under it is hidden, as `ComputeLineOfSight`
answers it. A distance, a bearing, a ground read, two table reads and a comparison: no
allocation, and nothing that grows with the distance. Heights may be given in any datum the
terrain can be put on; a target has the viewshed's no-answer states -- `Degraded` for a void
on either ray before it or under it, or a height that can't be put on the terrain's datum,
`NotCovered` beyond the prepared radius -- and one the query refuses says why, in
`TargetAnswer::inputProblem`. `ComputeLineOfSight` stays in the API as the reference.

**Measured against the line of sight.** `DATA/prepared_observer_targets.csv` lists 15,000
seeded targets for each of the fast/naive comparison's three observers on the 1-arcsecond
tile, uniform over the 50 km disc within the tile: a third 0-10 m above the ground, a third
10-500 m above it, a third up to 15,000 m above mean sea level (the list is written out,
rather than regenerated, because the standard library fixes the random engine but not the
distributions). Each is answered by `QueryTarget` and by `ComputeLineOfSight` along its own
line at 30 m, and compared by height class with `CompareViewsheds`' measure: over the
targets either one sees, each direction apart, a difference on the reference's edge when the
line of sight answers the same target moved 30 m north, east, south or west the other way.
Every figure is printed by `TestPreparedObserverAgreesWithLineOfSightOverTheListedTargets`:

| Observer, targets | Line of sight sees | Prepared only | Line of sight only | Differ, of targets either sees | Off the edge |
|---|---|---|---|---|---|
| (36.5, -111.5), 0-10 m above ground | 6 | 0 | 0 | 0% | 0% |
| (36.5, -111.5), 10-500 m above ground | 327 | 1 | 0 | 0.30% | 0% |
| (36.5, -111.5), up to 15,000 m | 3,961 | 1 | 0 | 0.03% | 0% |
| (36.86361, -111.30861), 0-10 m above ground | 1,515 | 17 | 21 | 2.48% | 0.13% |
| (36.86361, -111.30861), 10-500 m above ground | 4,073 | 1 | 1 | 0.05% | 0% |
| (36.86361, -111.30861), up to 15,000 m | 4,424 | 0 | 0 | 0% | 0% |
| (36.55861, -111.81361), 0-10 m above ground | 605 | 8 | 8 | 2.61% | 0% |
| (36.55861, -111.81361), 10-500 m above ground | 1,958 | 3 | 3 | 0.31% | 0% |
| (36.55861, -111.81361), up to 15,000 m | 4,265 | 0 | 0 | 0% | 0% |

5,000 targets per row. The observer at (36.5, -111.5) sees little of the ground around it (5%
of its 2 km viewshed), so its first row is small.

Every run is within the fast viewshed's tolerances, by a wide margin. Near the ground the
two differ on the same kind of cell the viewsheds do, the edge of what can be seen; in the
air there is almost nothing in front of a target for two neighbouring rays to see
differently. One target in 45,000 is confident in one answer only: it stands 5 m inside the
tile's western edge, and a ray beside its line leaves the tile -- runs out of data -- 1.5 m
before reaching it. The test allows fewer than 1 in 1,000.

Two design choices were measured and neither turned out load-bearing. Leaving the last half
spacing out of the horizon, as the fast viewshed does, lowers the worst ground-level
disagreement from 3.02% to 2.56% and the part off the edge from 0.59% to 0.26% (a scratch
program over the same three observers); blending the two rays against taking the nearer one
moves a handful of targets either way (2.49% against 2.56% at worst, 0.53% against 0.26% off
the edge). With rays one sample spacing apart at the radius, and closer everywhere inside it,
two neighbouring rays rarely see differently. Both are kept, for the numbers and to answer a
target the way the fast viewshed answers a cell -- but taking either out keeps the tests
green, and they aren't claimed to be more than that.

**Performance** (the machine above, Release build; `TerrainEngine.exe benchmark observer`,
three runs on each tile, one process per run): an observer 2 m above the tile's centre,
prepared out to 50 km at 30 m; a million queries over 10,000 targets spread evenly over the
disc on the tile, alternately 2 m above the ground and 5,000 m above sea level; and, for
comparison, 1,000 direct lines of sight 50 km long.

| Tile | Preparation | Peak memory after it | Queries a second, one core | Direct line of sight at 50 km |
|---|---|---|---|---|
| 1-arcsecond | ~1.72 s | ~96.3 MB | ~5.93 million | ~190 µs (~5,270 a second) |
| 3-arcsecond | ~1.70-1.72 s | ~74.2-74.3 MB | ~6.32-6.39 million | ~167-168 µs (~5,950 a second) |

The prepared observer holds 66.6 MB of horizons (10,472 rays of 1,667 floats); the rest of the
peak is the tile. A query costs ~160-170 ns, whatever the target's distance, against
~170-190 µs for a line of sight at 50 km: some 1,100 times as many answers a second, and about
sixty times the 100,000 asked for. Both figures come from the same CLI process, which (see
Performance above) runs the library somewhat slower than a program built for nothing else,
so the ratio is the figure to read. Nothing is allocated per query:
`TestPreparedObserverQueriesAllocateNothing` counts the executable's own `operator new` over
10,000 queries of every kind -- seen, hidden, past the radius, without a confident answer and
refused -- and finds none.

## Many pairs on every core

**The question.** Line of sight for N observers against M targets, on every core, with the
same answer at any thread count.

**The approach** (`LineOfSightPairs.h`, `Threads.h`). `ComputeLineOfSightPairs` gives each
pair its own profile and its own `ComputeLineOfSight`, and writes the answer to that pair's
own place in the result. Threads take pairs sixteen at a time from a shared counter, so one
that draws short paths simply takes more; each keeps its own profile buffer; the calling
thread is one of them. Nothing is summed, sorted or shared between pairs, so there is no
order for a thread count to change: the answer is, field by field, what
`ComputeBatchLineOfSight` gives on one thread. The naive viewshed and the exact minimum
visible height take a thread count too and spread their rows the same way -- each cell is
already its own line of sight -- reporting progress only on the calling thread, so a callback
never runs on a thread its caller didn't start, and stopping every thread when it asks to
stop. The samplers are read from every thread at once, which each of them allows once
constructed. The DLL runs its naive viewshed and exact minimum visible height on one thread
per hardware thread; the fast versions, under two seconds for 30 km, stay on one.

**Bit-identical.** `TestLineOfSightPairsAreTheSameAtEveryThreadCount` answers 8 observers
against 241 targets on the 1-arcsecond tile -- on the ground and in the air, some past the
tile's edge, one not on the Earth -- on 1, 2, 3 and 7 threads and one per hardware thread, and
compares every field of every answer with the one-thread batch to the bit.
`TestReferenceGridsAreTheSameAtEveryThreadCount` does the same for the naive viewshed and the
exact minimum visible height over 1 km, and checks that the threads asked for really read the
terrain -- three asked for, three seen. Each defect put back is caught: a profile buffer shared
by every thread, or one buffer for every thread of the reference, crashes the suite, the
threads resizing it under each other; a pair's observer taken by the wrong index, progress
reported from another thread, a stop that doesn't reach the others and a thread count quietly
ignored each turn a test red.

**Throughput** (the machine above, 8 cores and 16 threads; `TerrainEngine.exe benchmark pairs`,
24 observers against 400 targets up to 75 km apart on the 1-arcsecond tile, half the targets
2 m above the ground and half 5,000 m above sea level; each run checked against the
one-thread run):

| Threads | Time | Pairs a second | Speed-up |
|---|---|---|---|
| 1 | ~1.31-1.34 s | ~7,190-7,330 | 1x |
| 2 | ~0.64-0.66 s | ~14,600-14,900 | 2.0x |
| 4 | ~0.31-0.33 s | ~29,300-30,900 | 4.1x |
| 8 | ~0.18-0.19 s | ~51,300-52,400 | 7.1x |
| 16 | ~0.12 s | ~79,100-79,200 | 10.9x |

Up to eight threads each core does its share; past eight, the second thread on each core adds
about half as much again. The same benchmark's naive viewshed over 5 km goes from ~1.73-1.75 s
on one thread to ~0.18 s on 16; over 30 km on the 3-arcsecond tile, in a benchmark-only
program, from 240.3 s to 24.2 s.

## Data not given, and the data a query will read

**The question.** A real-time host loads elevation for a region rather than reading a file
on demand. It needs two things from the engine: to hear, when a query runs past the data it
was given, that this is what happened -- not a void in the data, which no loading will fill --
and to be able to ask, before running a line of sight, a viewshed or a minimum visible height,
which ground the query will read, so that exactly that can be loaded first.

**Telling the two apart.** Each sampler says, point by point, whether it holds a value, a void
or nothing at all (`IElevationSampler::Sample`, see "Voids, and data not given" above), and
the answer travels with the profile to every result. A void, once met, wins over data not
given, since loading more can't fill it; `DataNotGiven` therefore means that, given the data,
the answer may come.

**The data a query will read** (`QueryExtent.h`). `LineOfSightExtent`, `NaiveViewshedExtent`
(the naive viewshed and the exact minimum visible height read the same) and
`FastViewshedExtent` (the fast viewshed and the fast minimum visible height) return the box of
latitude and longitude around every point the query will read an elevation at -- not an
estimate: its edges are points it reads, to the bit. They are worked out without reading any
data, from the same arithmetic the query uses. Along a great circle, longitude moves one way,
so a profile's longitudes span its ends; latitude can turn once -- between two points on one
parallel the circle bulges toward the pole, by some 120 m over 60 km at 60 degrees north --
and with `z = sin(latitude)`, the profile's `z(t) = z_a sin((1-t)w)/sin w + z_b sin(tw)/sin w`
turns where `tan(tw) = (z_b - z_a cos w) / (z_a sin w)`, so the samples either side of that
point are checked too. A profile's box is six points at most; the naive viewshed's, one
profile's per cell; the fast viewshed's, one per boundary ray -- its cells' centres lie within
the boundary cells', which are where its rays end. `ProfileExtent` is O(1),
`NaiveViewshedExtent` O(cells), `FastViewshedExtent` O(rows + columns). A request the query
would refuse gets an empty box carrying the query's own reason (`GeoExtent::inputProblem`).

**Loading exactly that** (`RealElevationSampler::PostsCovering`, `Window`). For the tile
reader, `PostsCovering` turns a box into the posts it needs -- the nearest post to each point,
or with bilinear interpolation the four around it -- with `Sample`'s own arithmetic, and
`Window` gives a sampler holding only those posts, which finds a point's post exactly as the
whole tile does and calls any other point on the tile data not given. A raster block can be a
window into a larger raster the same way (`RasterBlockGeometry::rowOffset`, `colOffset`).

**Measured.** `TestQueryExtentsAreTheBoxesTheQueriesRead` runs each query through a sampler
that records every point it is asked for, and the recorded box must equal the reported one to
the bit -- for profiles north-south, diagonal, reversed, a few metres long and 60 km east-west
at 60 degrees north (where a box around the ends alone would miss the bulge), and for all four
grids. `TestAQueryGivenOnlyItsExtentAnswersAsOnTheWholeTile` is the acceptance: on the
1-arcsecond tile, each query -- five lines of sight, the naive viewshed and exact minimum
visible height over 1 km, the fast viewshed and fast minimum visible height over 3 km, and the
fast viewshed read bilinearly -- run on a window of only its reported posts gives, to the bit,
the answer it gives on the whole tile; and with the window a post short on each side in turn,
every answer that changes becomes `DataNotGiven`, and none becomes a void.

## Void handling and degraded results

Asserted by tests (`TestVoidPointIsDegraded`, `TestViewshedDetectsVoid`,
`TestViewshedsAgreeWhenObserverIsUnknown`, `TestDataNotGivenIsToldApartFromAVoid`). When
nothing is known about the observer, both viewsheds and both minimum visible heights answer
every cell for that one reason, the observer's own cell included: `Degraded` when it stands
on a void or its height can't be put on the terrain's datum, `DataNotGiven` when the ground
under it wasn't given. A missing or
unreadable elevation file is also distinguished from a genuine void:
`RealElevationSampler::IsLoaded()` reports load failure explicitly — a missing file, or
one whose size isn't a whole square of posts, as a download cut short would be — and the CLI refuses
to run rather than silently treating every query as void.

## API contract

Every public result type (`ProfileSample`, `LineOfSightResult`, `ViewshedResult`) is a
plain struct of value types. `ProfileSample::elevationM` and `LineOfSightResult`'s
optional fields use `std::optional` for absence, not a sentinel value.
`ViewshedResult::visible` instead uses an explicit `CellVisibility` enum
(`NotCovered` / `Degraded` / `Visible` / `NotVisible` / `DataNotGiven`) — a single `optional<bool>`
cannot distinguish "no ray ever reached this cell" from "a ray reached it but crossed
a void partway", and a caller needs to tell those two apart. Similarly, a height
crossing the API boundary (`observerHeight`/`targetHeight`) is never a bare `double` —
it is a `DatumHeight` — and every `ProfileSample` carries the datum of its own
elevation, so `ComputeLineOfSight(profile, observerHeight, targetHeight, k)` and
`ComputeFresnelClearance(profile, observerHeight, targetHeight, frequencyHz, k)` take
no separate terrain-datum argument that could disagree with the profile. A query that
can't be put on one datum is rejected as a value
(`LineOfSightResult::status == ComputationStatus::DatumRejected`), not silently
computed or thrown, and an input outside a function's domain likewise, as
`ComputationStatus::InvalidInput` with an `InputProblem` saying which -- see "Validity
envelope" above. A viewshed asked for a grid with no rows or no columns returns an
empty `ViewshedResult`, with no problem: it simply has no cells. Both viewsheds take an optional progress callback that can stop
them early, which marks the result `cancelled`; without one they run exactly as before.
`MinimumVisibleHeightResult` follows the same rules: the viewshed's states for cells
without a confident answer, NaN heights there rather than a sentinel, the inputs the
viewsheds refuse refused with the same `InputProblem`, and the same progress and
cancellation.
Nothing in `TerrainCore` or `TerrainReader` throws across its
own boundary, other than the standard library's `std::bad_alloc` if memory runs out.

## Rendered output

`profile_output.pgm` and `viewshed_output.pgm` (produced by the built-in benchmark) and
`cli_viewshed_output.pgm` (produced by the `viewshed` CLI command) — plain P5 binary
PGM, written by a single self-contained writer (`ImageWriter.h`), no graphics
dependency. In a viewshed, white is visible, black hidden, light grey (192) past the data
the engine was given, and mid grey (128) a void on the way or a cell not reached.

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
