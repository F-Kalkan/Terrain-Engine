# The engine's test suite

80 test functions, hand-checkable apart from the real-data comparisons, which hold the
fast viewshed against naive, the fast minimum visible height against its reference and the
prepared observer against the line of sight, at several observers:

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
  back empty from both, rather than written into or resized to an enormous size; and progress reported in order, and a stop request
  honoured, by both algorithms without changing a single cell.

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
  the viewshed's longitude spacing correcting for latitude; and the shared curvature-drop,
  sight-line and Fresnel-radius formulas against hand-worked values.
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
  too many) refused rather than read past their end; a tile's own facts (posts per
  side, void count, south-west corner) reported for a file whose name holds a non-ASCII
  character, and reported as zero for a file that didn't load; one loaded tile answering
  in the other interpolation mode exactly as a fresh load in that mode would; the
  reader's bilinear interpolation against a value worked by hand (177.5 m), at a point
  off the middle of its four posts -- in the middle the row and column fractions are
  equal, and swapping them goes unseen; and the in-place profile overload reusing a
  caller-owned buffer without growing it.
- **Viewshed target height** (the wall scene of the fast/naive test): straight east of a
  2 m observer, a target 180 m out behind a 50 m wall 90 m out clears it once its height
  reaches 98 m, worked by hand -- both viewsheds hide it at 97.9 m and show it at 98.1 m, and
  at the default of 0 m (the ground itself) hide it as they always did; a taller target only
  ever reveals cells, never hides one or raises the horizon for the cells behind it, and an
  explicit 0 m is the default cell for cell; fast matches naive on every cell off the wall
  at 0 m and 30 m, and at 120 m, where the visible region ends in the open field, disagrees
  only on that boundary, where the fast/naive section of [ENGINE.md](ENGINE.md) measures
  such differences on real terrain; and a target height in a datum the terrain can't be
  put on leaves every cell but the observer's Degraded in both, never guessed.
- **Viewshed agreement** (`CompareViewsheds`, the measure the fast viewshed is held to):
  on 7x7 viewsheds drawn as text, a 3x3 block drawn one column off, with a hole and a
  stray cell, gives the counts worked by hand -- 4 cells each way, 6 of 8 differences on
  the reference's edge, 13 cells either sees -- and a neighbour with no confident answer
  never makes a cell an edge cell; "hidden everywhere" and "visible everywhere" fail the
  tolerance on a grid where, counted over every cell, "hidden everywhere" would differ
  on only 18%; the block moved one cell has no difference off the edge; and grids of
  different sizes, or ragged ones, are refused rather than partly compared.
- **Command line**: the number parser behind every CLI argument accepting real
  numbers and refusing text, trailing characters, `nan`, `inf` and out-of-range
  values, instead of throwing.
- **Inputs the library refuses** (one test per class, called on the library directly,
  not through the DLL or the CLI):
  - *spacing*: a 20 km path over level ground with a ridge across it halfway is blocked
    at 30 m; at a spacing of 0, -30 m, NaN, infinity and 1 x 10^-6 m (2 x 10^10
    intervals) the profile comes back empty with the reason -- a buffer filled
    beforehand is emptied -- and the line of sight, Fresnel clearance and batch built on
    it are not Ok, where each once answered "visible";
  - *coordinates*: NaN, infinite and past-the-pole points are refused by the profile, the
    batch (answering its other queries) and both viewsheds, and every sampler -- the
    `.hgt` reader in both interpolation modes, the fake grid (an empty one too), the
    raster block and the multi-tile one -- answers NaN, infinite and ±10^300
    coordinates with no elevation;
  - *heights*: NaN and infinite heights and undulations are refused as that, not as a
    datum problem, by the line of sight at either end, Fresnel clearance and both
    viewsheds, and the datum helpers convert them to nothing;
  - *k and frequency*: k of 0, -4/3, NaN and infinity, and a Fresnel frequency of 0,
    negative, NaN or infinite, are refused by name everywhere they are taken, while
    k = 10^12 (curvature off) is still answered;
  - *a grid at a pole*: a viewshed grid is refused at a pole even as one cell, and 0.2
    degrees from either pole when its rows would reach 0.27 degrees, and laid out 0.5
    degrees from it when they wouldn't; a latitude of 91 and a spacing of 0 are refused,
    and a grid with no rows is still simply empty.
- **Minimum visible height** (both the reference and the fast version, unless one is named):
  - *the wall, by hand*: in the wall scene the lowest target seen 120 m east, just behind the
    wall, is 66.000212 m, and 180 m east 98.000954 m, worked out in the test from the sight
    line over the wall and the curvature drop; in front of the wall and on it, 0; each cell
    carries its own ground, the wall's top on the wall;
  - *a smooth sphere*: over level ground only curvature hides anything, and past the horizon
    `d_h = sqrt(2kR h)` the answer is `(D - d_h)^2 / 2kR` (derived in
    [ENGINE.md](ENGINE.md)); along single paths out to 50 km from eyes of 10 and 100 m, and
    over every cell of a 101x101 grid of 300 m cells, within tolerances derived in the test
    from how far the nearest sample can be from the horizon -- and exactly 0 inside it;
  - *exactly the viewshed*: asked for a target of height H, the reference gives, cell for
    cell, the naive viewshed at H, and the fast version the fast viewshed at H -- at every
    height the grid holds and at the double just below each, on the wall scene with a void
    and cells off the data, and on the 1-arcsecond tile over 1 km;
  - *no answer, and refusals*: an observer on a void leaves every cell Degraded as in the
    viewsheds, with no height or ground; an observer whose eye is below its ground needs an
    infinite height everywhere, and a target 1 km up is still hidden, as naive says; the
    absolute answer is the cell's ground plus its height; spacing, heights, k, coordinates
    and a grid reaching a pole are refused with the viewsheds' reasons, a grid with no rows
    is empty; a target height below the ground or not a number is refused when thresholding;
    progress runs from 0 to 1 without changing a cell, and a stop is honoured;
  - *fast against the reference* (skipped when the 1-arcsecond tile isn't present): at the
    three observers of the fast/naive comparison, 2 km and 5 km, the viewsheds both give for
    targets of 0, 2, 10, 30 and 100 m are within the fast viewshed's tolerances, with at
    least 80% of differences on the reference's edge, and one answer everywhere fails; each
    run prints its counts and how far the heights are apart.
- **A prepared observer** (`PrepareObserver` and `QueryTarget`):
  - *by hand*: in the wall scene, a target 180 m out is hidden at 97.9 m and seen at 98.1 m,
    above the ground or above sea level; the ground in front of the wall is seen and behind
    it hidden; an aircraft 10 km up is seen in any direction; a target below the ground is
    hidden, 1 m under the wall's top too, where its slope alone would clear everything; a
    target past the prepared radius has no answer;
  - *the listed targets* (skipped without the 1-arcsecond tile): `DATA/prepared_observer_targets.csv`,
    15,000 seeded targets for each of three observers over a 50 km disc -- a third near the
    ground, a third 10-500 m up, a third up to 15,000 m above sea level -- answered by the
    prepared observer and by the line of sight along each target's own line, within the fast
    viewshed's tolerances for every observer and height class, with at least 80% of the
    differences on the edge (a target moved 30 m is answered the other way), fewer than 1 in
    1,000 confident in one answer only, and one answer everywhere failing; and at least
    100,000 queries a second;
  - *no allocation*: 10,000 queries of every kind -- seen, hidden, past the radius, without a
    confident answer and refused -- make no call to the executable's `operator new`;
  - *no answer, and refusals*: a void on the rays before a target or under it, an observer on
    a void and a height with no datum to put it on leave no confident answer; a target not on
    the Earth (not a number, or past a pole) or with no height is refused with the reason, as
    is every target of a preparation refused for its spacing or radius; progress runs from 0
    to 1 without changing a ray, and a stop is honoured.
- **Threads** (skipped without the 1-arcsecond tile, but for the refusals):
  - *many pairs*: 8 observers against 241 targets -- on the ground and in the air, some past
    the tile's edge, one not on the Earth -- answered on 1, 2, 3, 7 threads and one per
    hardware thread, every field of every answer the same, to the bit, as the one-thread
    batch; each run on as many threads as asked, and three threads asked for seen reading the
    terrain; a spacing of zero refuses every pair with its reason, and no observers or no
    targets is an empty answer;
  - *the exact grids*: the naive viewshed and the exact minimum visible height over 1 km, the
    same grid to the bit on 1, 2, 3 threads and one per hardware thread, progress reported on
    the calling thread only, and a stop at the first report stopping every thread.
- **Data not given, and the data a query reads**:
  - *told apart*: every sampler says a void from ground it wasn't given -- the raster block and
    a view over one (a cell with no value; outside it) and a window of it, the `.hgt` reader (a `-32768` post; off
    the tile, a window of it -- holding its own posts, not a copy of the tile's -- a tile that
    didn't load), the multi-tile sampler (no tile) and the
    fake grid -- while one answering only `GetElevation` calls every gap a void; the line of
    sight answers across a void `VoidInProfile`, off the data `DataNotGiven`, onto a void
    `EndpointMissing`, and across a void and off the data both, `VoidInProfile` (a void, once
    met, wins); both viewsheds and both minimum visible heights answer the ring past their
    data for the reason each line gives -- naive line by line, fast with both reasons present --
    and every cell from an observer off the data `DataNotGiven`, from one on a void `Degraded`;
    a prepared observer's targets likewise, and a target height with no datum `Degraded`
    whatever is known of the observer, as in a viewshed;
  - *the box a query reads*: run through a sampler that records every point asked for, a
    query reads exactly the box it reported beforehand, to the bit -- profiles north-south,
    diagonal, reversed, a few metres, and 60 km east-west at 60 degrees north, bulging over
    50 m past a box around its ends; and the naive and fast viewsheds and both minimum visible
    heights -- while a request the query would refuse reads nothing, and its box says why;
  - *only that box* (skipped without the 1-arcsecond tile): five lines of sight, the naive
    viewshed and exact minimum visible height over 1 km, and the fast viewshed (nearest and
    bilinear) and fast minimum visible height over 3 km answer on a window holding only the
    posts their box needs exactly as on the whole tile, to the bit; a post short on any side,
    every answer that changes becomes `DataNotGiven`, never a void.
- **Heights in every datum**:
  - *below the ground*: a target at 200 m above sea level over a 300 m plateau, and an eye at
    50 m above sea level on 100 m ground -- the fast viewshed matches naive cell for cell,
    hiding the plateau's cells and everything the buried eye would see but its own cell; the
    fast minimum visible height's states match the reference's and every height is infinite
    in both; a prepared observer answers the same;
  - *on the command line*: a bare number is above the ground, and `:agl`, `:msl` and
    `:hae:<undulation>` say their datum; `:hae` without an undulation is refused as such,
    and an empty, unknown or extra part, or a word for a number, each with its own reason;
    every datum has words for a printed height.
  The same heights through the DLL, `TerrainEngine.exe` and TerrainBench are the app's
  `DatumTests` ([TERRAINBENCH.md](TERRAINBENCH.md)).
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
- **Real data** (`TestFastViewshedAgreesWithNaiveAtThreeObservers`, skipped per tile when
  it isn't present): on the 1-arcsecond tile at (36.5, -111.5), (36.86361, -111.30861) and
  (36.55861, -111.81361), each at 2 km and 5 km, and on the 3-arcsecond tile at
  (36.5, -111.5), 2 km -- 30 m cells, 2 m above ground, `k` 4/3, nearest -- the fast
  viewshed is within tolerance of naive, at least 80% of their differences lie on naive's
  visibility edge, and a viewshed with one answer everywhere fails the same tolerance.
  Each run prints its counts in both directions; [ENGINE.md](ENGINE.md) tabulates them.

Each correctness fix above was confirmed by putting the defect back in a scratch copy
of the code and watching its test fail: curvature forced inert inside both viewsheds,
the profile's sample count rounded down again, a void skipped when its sample shares a
cell with the previous one, the sight line interpolated by sample index, the observer's
own cell marked visible unconditionally, an ellipsoidal height used without its
undulation, every profile distance halved, and the tile reader placing its grid one
post off or assuming 3-arcsecond post spacing for a 1-arcsecond file, and the raster
view copying its buffer at construction or ignoring its validity flags. The fast viewshed
and its measure were checked the same way: a fast viewshed answering NotVisible
everywhere, answering from the nearest ray alone, letting the target's own cell into its
horizon, leaving curvature out of its slope, ignoring the target height or the voids on
its rays, reading one ray twice, or reading a ray's horizon over its whole length; and a
measure counted over every cell, taking every difference for an edge one, or taking a
neighbour with no confident answer for an edge. So were the refusals: each check on
spacing, sample count, latitude, height, undulation, k, frequency, the batch's per-query
path and the grid at a pole taken out in turn, and the datum conversion let take NaN,
turns a test red -- and a viewshed with its spacing check taken out crashes the suite
outright, laying out a grid at a spacing of zero. And so was the minimum visible height,
thirteen defects one at a time: either version answering with the formula alone instead of
settling it, the search returning the last height not seen instead of the first seen, the
fast version leaving curvature out of the target's slope, the reference calling every
answered cell hidden, either version taking the observer's ground for the cell's,
thresholding with "above" instead of "at or above", the reference's refusal, its observer
cell's height, its progress reports or its no-answer state taken out, and the refusal of a
target below the ground taken out. And so was the prepared observer: curvature left out of a
target's slope, the voids on its rays ignored, a target below the ground allowed, no radius
check, the target's own sample let into its horizon, the bearing taken from the target, a
quarter of the rays, a query that allocates, and a refused target answered each turn a test
red; an unknown observer answered anyway crashes the suite, reading rays it never cast. Two
design choices survive being taken out -- the blend of two rays and the half-spacing margin
-- and [ENGINE.md](ENGINE.md) says why neither is claimed to matter. And so were the threads:
a pair's observer taken by the wrong index, progress reported from another thread, a stop
that doesn't reach the other threads and a thread count quietly ignored each turn a test red,
and a profile buffer shared by every thread crashes the suite. The thread count ignored first
survived -- the test compared the threads used with the same function that picked them -- so
it now works the number out itself and watches which threads read the terrain. And so was
the split between a void and data not given: a profile that forgets which gaps weren't given;
data not given winning over a void, on a path or in the fast viewshed; the tile reader, the
raster block or a view over one calling ground off it a void; a profile's box from its ends
alone; the fast box without its rays; a nearest window covered by flooring; a window that
ignores its own bounds; a window holding a copy of the whole tile; naive answering each line
from an unknown observer; and a prepared observer forgetting data not given on its rays or
under a target, or answering for an unknown observer before a target height with no datum --
each turns a test red. The
view over a raster first survived, the test checking only the block that owns its cells; it
checks the view too now. And so were heights in every datum: the fast viewshed seeing a
target in the ground or out of an eye in it, the fast minimum visible height and the prepared
observer seeing out of it, and the command line reading `msl` as above the ground or letting
`hae` go without its undulation, each turn one of these tests red; through the DLL and the
app, `DatumTests` catches each surface's own. A suite that stays green with the defect
restored verifies nothing.

All of the above pass identically in Debug and Release.
