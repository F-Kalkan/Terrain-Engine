# Notes: interface/algorithm friction and what I'd change

## Where the interface fought the algorithm

- **The sampler interface is a single point query (`GetElevation(lat, lon)`), but the
  fast viewshed wants directional sweeps.** The interface never had to know about rays
  or sweeps — it just answers "what's the elevation here" — so the fast algorithm still
  ends up calling it once per sampled point along every ray, one point at a time. A
  batched or ray-aware query would let a real reader do smarter I/O (e.g. reading a
  strip of a `.hgt` file at once instead of one point at a time), but that would have
  leaked the *reader's* concerns back into the *core* interface — exactly what the
  spec warns against ("if you write the reader first you will shape the interface
  around a file format"). I kept the interface pure and paid for it in the fast
  algorithm's call count instead.

- **`k` (the curvature/refraction coefficient) started as a local constant inside
  `ComputeLineOfSight`, then had to be retrofitted as a parameter into three function
  signatures** (`ComputeLineOfSight`, `ComputeViewshedNaive`, `ComputeViewshedFast`)
  and re-threaded through every call site, including the CLI's argument parsing. The
  spec said outright that "someone will want to set it" — that sentence should have
  been read as "expose it all the way to the CLI," not just "make it a function
  default." Doing it from the first line would have cost nothing; doing it after the
  fact touched code in four files.

- **`assert()` is a silent no-op once `NDEBUG` is defined** (i.e. in every Release
  build). All ten hand-checkable tests were written with `assert(condition);` followed
  by an unconditional `std::cout << "PASS"`, which meant the Release build's test
  output was printing "PASS" regardless of whether the assertion actually held —
  the tests were checking nothing in exactly the build configuration used for the
  performance numbers. This wasn't caught by reading documentation; it took
  deliberately compiling a two-line reproduction with and without `NDEBUG` to see the
  difference in behaviour. Replaced with a small `Expect(condition, name)` helper that
  does a plain runtime `if/else` — no macro, so it can't be compiled away.

- **Getting a genuinely separate "build target" for the reader cost more tooling
  ceremony than design effort.** The actual C++ separation (an interface, a header
  the reader includes and the core doesn't) took one file. Making Visual Studio's
  project system enforce it — a header-only static library needs at least one `.cpp`
  file before the linker will even produce a `.lib`, precompiled headers had to be
  explicitly turned off, and every include path and project reference had to be wired
  by hand — was pure MSBuild friction unrelated to the actual architecture.

## What made the fast viewshed only approximately match the naive one

The fast algorithm casts rays only to the grid's boundary cells and assigns every
interior cell's visibility from whichever ray passes nearest it, rather than that
cell's own exact-direction line. That's a genuine algorithmic trade-off, not a bug —
and it doesn't get better with more precision. I tried two fixes: casting many more,
angularly denser rays, and switching the profile's sample-count rounding from `floor`
to `ceil`. Both made the mismatch against naive *worse* (from ~3.66% up to 4.85%, then
4.29%), because finer sampling just measures the already-slightly-wrong swept ray's
answer more precisely — it doesn't make that ray point at the cell being asked about.
The mismatch is inherent to "assign by nearest ray" and only goes away by computing an
exact per-cell direction, which is the naive algorithm by definition.

A third change moved the ratio again, this time for an unrelated reason: fixing the
viewshed grid's longitude spacing (see `README.md`, "Viewshed grid shape") so a
"30 km radius" grid is a real-world circle instead of an ellipse also changed the real
spacing between the fast algorithm's boundary rays. The ratio went from 3.58% to
4.65% — still under the 5% tolerance, but the fix wasn't chosen to improve or preserve
that number, it was chosen because the old grid shape was wrong regardless of what it
did to the mismatch ratio. Worth recording so a future reader doesn't mistake the
shift for noise or a regression.

## What I'd change about the API if I started again

- Make `k`, the interpolation mode, and the (currently unmodeled) vertical datum all
  explicit, first-class constructor or query parameters from day one, not something
  added later as a default argument.
- Give grid coordinates and geodetic coordinates distinct types instead of both being
  a bare `{double, double}` `GeoPoint` — the synthetic tests' grid indices and real
  latitude/longitude shared a type only because it happened to compile, not because
  they meant the same thing. (The tests have since stopped passing grid indices where
  degrees are expected; the type still doesn't stop anyone doing it.)
- Document the vertical-datum gap as a loud, unmissable part of the API (a comment at
  minimum, ideally a named parameter forcing the caller to state which convention
  their heights use) instead of a README paragraph nobody has to read before calling
  the function.

## Update: the vertical datum regret above is now fixed

The two bullets above turned into an actual change: `hA`/`hB`/`observerHeight` are
now `DatumHeight` (a value plus a `VerticalDatum`), every `IElevationSampler`
declares its own datum through `GetDatum()`, and a query is rejected as a value
(`ComputationStatus::DatumRejected`) rather than silently combined whenever the
heights and the terrain can't be put on one datum. Worth naming honestly what
this did and did not close: the *type* is now unmissable (a caller cannot pass a
bare `double` and forget which convention it's in), but there is still no real
geoid-undulation data source behind `ConvertHeightBetweenDatums` -- the function is
correct and tested, but a caller who actually has an ellipsoidal height still needs
to get the undulation for their coordinate from somewhere else first. Typing the
problem away is not the same as solving the data-sourcing half of it.

## Update: heights beyond "above ground"

Accepting only `HeightAboveGround` was safe, but it pushed the conversion a real host
needs — an ellipsoidal platform altitude against orthometric terrain — back onto the
caller, outside the type meant to guard it. Heights may now be orthometric or
ellipsoidal too, carrying their own geoid undulation (absent, never zero, unless
given), and the terrain's datum travels on each profile sample instead of as a
separate argument that could disagree with the profile it describes.

## Update: rounding the sample count up after all

The section above records trying `ceil` instead of `floor` for the profile's sample
count and rejecting it because the fast/naive mismatch got worse. That was measured
under a distance model that was itself wrong, and against the wrong question. The
right question is whether a ray visits every cell it crosses, and with `floor` it
often didn't: a viewshed's axis rays are exact multiples of the spacing by
construction, floating-point noise turned 1000.0 into 999.9999..., and one sample —
one cell's worth of terrain, void or not — silently disappeared. Measured directly at
36.5°N: 1998 of 2000 east-west axis rays and 1000 of 2000 north-south ones came out a
sample short. `ceil` with a tiny relative tolerance fixes that without adding spurious
samples (0 short and 0 extra on the same 4000 rays). The fast/naive mismatch moved from
3.60% to 3.86% — still under tolerance, and not the number to optimise when the
alternative is skipping cells.

## Update: the tests that couldn't fail

Running the suite against a copy of the code with Earth curvature switched off inside
both viewsheds — the very defect the viewshed work had fixed — left every check green.
Two reasons. `FakeElevationSampler` reads latitude/longitude as grid indices while
`GetTerrainProfile` treats them as degrees, so a 5×5 test grid is ~445 km across; at
~111 km a cell, curvature decided every viewshed cell and the terrain the tests were
built around played no part. And several line-of-sight tests only asserted
`isVisible`, which a rejected query leaves at its default of `true`, so a query that
computed nothing still passed. Viewshed tests now run on 30 m rasters aligned with the
viewshed grid, curvature has a test that fails if `k` stops mattering, and every
"visible" assertion also checks the computation succeeded. The line-of-sight tests had
the same scale problem in a quieter form: they built profiles on the degree-read grid
and then overwrote every sample's distance with a hand-picked one, so they never saw
the distances `GetTerrainProfile` actually produces. They now run on 30 m grids with
the profile left as built; halving every distance in `GetTerrainProfile` used to be
caught only by the two tests that check distances directly, and now also fails the
wall test. The lesson that generalises:
a green suite says nothing until the defect it guards against has been put back and
the suite has gone red.

## Update: the 30 m data

The task asked for 30 m data, and the repository only ever had the 3-arcsecond (~90 m)
tile, so every "30 m" benchmark was a sampling spacing laid over 90 m posts. The
1-arcsecond tile for the same square degree now goes through the same reader unchanged
— it infers the post count from the file size — and through the same benchmark and
fast-vs-naive tolerance check.

Two things were worth writing down. The two tiles don't agree post for post: only 7.5%
of shared posts are identical and the mean difference is 6.9 m, which looked like a
misplaced grid until shifting the comparison one fine post in each direction made it
worse every time. They are different products — SRTM v2.1 against the reprocessed,
void-filled SRTMGL1 v3 — and on canyon terrain a post's exact footprint can move the
answer by hundreds of metres. And the finer data changed the fast-vs-naive mismatch
(3.86% to 2.54%) but barely changed the timing (~1.03 s to ~1.07 s for the 30 km
viewshed): the cost follows the number of samples, not what they read. Memory is what
grew, to ~55 MB, because the tile itself is nine times larger.

## Update: a raster view instead of a copy

The raster-block sampler was meant to be a view over a block the caller already holds,
and it was a container: it took nested per-row vectors of `std::optional<double>`, so a
flat row-major reply with a parallel validity array had to be copied in cell by cell.
Nothing was wrong with the answers, which is why it was easy to leave. It is now split
in two: `RasterBlockViewElevationSampler` reads the flat elevation and validity arrays
in place, and `RasterBlockElevationSampler` owns the same flat layout for callers
without a buffer of their own. Both map a coordinate to a cell through one function,
`RasterBlockCellIndex`, rather than two copies of the same arithmetic. The price of a
view is a lifetime rule the type can't enforce — the arrays must outlive the sampler
and stay unchanged while it is being read — so it is written down where the class is
declared. The test that proves it's a view changes the buffer after construction and
expects the next query to see it; a sampler that quietly copied would still pass every
other test.

## Update: three inputs that crashed instead of being refused

Probing the edges the test suite never reached turned up three ways to take the process
down, none of them a wrong answer and all of them worse than one.

- **A tile file cut short.** The reader took the side of the grid as the rounded square
  root of the post count, so a file one post short still looked like a 1201×1201 tile,
  reported itself loaded, and read its south-east corner from past the end of the data —
  an assertion in Debug, a silent garbage elevation in Release. This one mattered more
  once tiles were being downloaded by hand. A tile's size must now be exactly a square
  of posts, or it isn't loaded.
- **A grid with no cells.** Both viewsheds set the observer's own cell unconditionally,
  so a zero-sized grid wrote into an empty result, and a negative size turned into an
  enormous one on its way into `std::vector::resize`, which throws — contradicting the
  README's claim that nothing throws across the library boundary. Such a grid now comes
  back empty.
- **A word where a number belongs.** The CLI parsed with `std::stod`/`std::stoi`, which
  throw; nothing caught them. Every argument now goes through a parser that returns
  nothing instead of throwing, and the CLI names the argument it couldn't use. The batch
  reader had the same problem in a quieter form: a bad first value left the spacing
  uninitialised, and a query cut short at the end of the file was silently dropped.

What these share is that none of the tests asked. Every test fed the code inputs
shaped the way the code expected, which checks the arithmetic and says nothing about
what happens on the first input shaped otherwise.