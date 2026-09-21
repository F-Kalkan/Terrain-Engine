# Notes: interface/algorithm friction and what I'd change

## Where the interface fought the algorithm

- **The sampler interface is a single point query (`GetElevation(lat, lon)`), but the
  fast viewshed wants directional sweeps.** The interface never had to know about rays
  or sweeps — it just answers "what's the elevation here" — so the fast algorithm still
  ends up calling it once per sampled point along every ray, one point at a time. A
  batched or ray-aware query would let a real reader do smarter I/O (e.g. reading a
  strip of a `.hgt` file at once instead of one point at a time), but that would have
  leaked the *reader's* concerns back into the *core* interface — the classic trap of
  writing the reader first and shaping the interface around a file format. I kept the
  interface pure and paid for it in the fast algorithm's call count instead.

- **`k` (the curvature/refraction coefficient) started as a local constant inside
  `ComputeLineOfSight`, then had to be retrofitted as a parameter into three function
  signatures** (`ComputeLineOfSight`, `ComputeViewshedNaive`, `ComputeViewshedFast`)
  and re-threaded through every call site, including the CLI's argument parsing. It was
  always clear that someone would want to set it, and that should have been read as
  "expose it all the way to the CLI," not just "make it a function default." Doing it
  from the first line would have cost nothing; doing it after the fact touched code in
  four files.

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
viewshed grid's longitude spacing (see `docs/ENGINE.md`, "Viewshed grid shape") so a
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

The target was 30 m data, and the repository only ever had the 3-arcsecond (~90 m)
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
  API contract's claim (docs/ENGINE.md) that nothing throws across the library
  boundary. Such a grid now comes back empty.
- **A word where a number belongs.** The CLI parsed with `std::stod`/`std::stoi`, which
  throw; nothing caught them. Every argument now goes through a parser that returns
  nothing instead of throwing, and the CLI names the argument it couldn't use. The batch
  reader had the same problem in a quieter form: a bad first value left the spacing
  uninitialised, and a query cut short at the end of the file was silently dropped.

What these share is that none of the tests asked. Every test fed the code inputs
shaped the way the code expected, which checks the arithmetic and says nothing about
what happens on the first input shaped otherwise.

## Update: a target height for the viewshed

Both viewsheds asked one question of every cell: can the observer see the ground there? A
viewshed for a radio link or a lookout asks something else -- can the observer see a mast, a
vehicle or a standing person in that cell -- so both now take a target height, the last
parameter, defaulting to 0 m above ground. As a `DatumHeight`, like the observer's, not a
bare `double`: the reason heights carry their datum everywhere else applies here too, and a
target in a datum the terrain can't be put on is refused the same way (every cell but the
observer's Degraded), never guessed.

Naive needed nothing new: it already runs a full line of sight per cell, and passes the target
height to it. Fast needed care. Its ray keeps a horizon, the steepest slope from the observer's
eye to the terrain seen so far, and calls a cell visible when that cell's slope reaches it. With
a target height there are two slopes per cell: the terrain's, which is what raises the horizon
for every cell further out, and the target's, which is what this cell's answer is about. Letting
the target's slope raise the horizon would be wrong -- the next cell's mast stands on the
ground, not on this one -- and a test puts that mistake back and watches it fail.

With the default, the two slopes are the same number and the old code path is taken exactly: the
command line's viewshed images came out byte for byte the same as before the change, for three
observers and both interpolation modes. At a large target height the visible region can end in
open ground rather than at a ridge, and fast then disagrees with naive on a few boundary cells
for the same reason it does on ridgelines (its rays sample a cell away from the cell's centre);
the test allows that on the boundary and nowhere else.

In the DLL, `te_viewshed_query` gained `target_height_above_ground_m` as its last field. The
app and the DLL ship together, so there is no older caller to keep working; a DLL meant to be
called by programs built against an earlier header would need a size field or a new function
instead.

## The DLL boundary

The desktop test bench reaches the engine only through `TerrainEngineApi.dll`, called
from .NET. Everything about that boundary follows from two facts: nothing C++-specific
survives the crossing, and the caller is a program someone is clicking around in, so no
input may take it down.

- **Plain C, nothing else.** The header is `extern "C"`: `int32_t`, `double`, UTF-16 and
  UTF-8 strings, and structs of fixed-size fields. Padding is spelled out as `reserved`
  fields and the structs are ordered so no hidden padding appears, which lets .NET's
  sequential layout map them field for field. Booleans are `int32_t`, never `bool`, whose
  size C and .NET don't agree on.
- **A tile is a number, not a pointer.** A pointer handed to .NET can't be checked: a
  stale or invented one reads freed memory. Handles are keys into a table instead, never
  reused, so a closed, zero or made-up handle is simply not found and comes back as
  `TE_ERROR_INVALID_HANDLE`. The table holds shared pointers, so closing a tile while a
  viewshed is still running on it lets that run finish before the tile is freed.
- **Failures are a code and a sentence.** Every function returns a result code; the
  matching message is kept per thread and says in plain English what is allowed ("The
  spacing must be a number greater than 0 m."), because the app shows it as it is. Every
  exported body runs inside one catch-all, so neither `std::bad_alloc` nor anything else
  escapes into .NET, where an unwinding C++ exception ends the process.
- **Validation lives at the boundary, not in the engine.** The engine's behaviour doesn't
  change in this work, so the DLL refuses what the engine was never meant to see: a
  spacing, radius or `k` at or below zero, a point outside the open tile, a viewshed
  within 0.1° of a pole (where `cos(latitude)` sends the column step to infinity), and
  sizes that would exhaust memory (a million path samples, a 4001-cell-wide grid). A
  missing, empty, folder-shaped or cut-short `.hgt` is told apart in the message, never
  reported as a tile of voids.
- **Who frees what is never in doubt.** Strings the DLL hands out are static or per
  thread and must not be freed. Arrays the DLL hands out are allocated with `malloc` and
  released with `te_free`, never by the caller's allocator: the DLL links the C runtime
  statically, so its heap is its own.
- **One call per path.** Profile, line of sight and Fresnel clearance come from a single
  `te_analyze_path` over one profile, with every sample carrying the terrain, the
  curvature-corrected terrain, the sight line and the Fresnel radius. The app draws its
  chart from those numbers and computes none of its own, so the chart and the verdict
  can't drift apart. To make that possible the engine's curvature, sight-line and Fresnel
  formulas moved into named functions that the engine itself now calls too -- one
  implementation, with the CLI's output byte-for-byte unchanged.
- **Metres in, degrees reported back.** People think in metres; the engine samples in
  degrees. The DLL converts, and returns the degree spacing it used so any query made in
  the app can be repeated exactly with `TerrainEngine.exe`.
- **Progress without threads in the DLL.** A viewshed runs on whatever thread calls it,
  reporting progress through a C callback on that same thread; the app runs it off the
  UI thread and marshals the reports back. Stopping is the callback's return value. The
  engine gained an optional `ViewshedProgress` parameter for this, defaulting to nothing,
  so every existing caller runs exactly as before.
- **Both interpolation modes, one open file.** A sampler's interpolation mode is fixed at
  construction, and changing it on a shared sampler would break its thread-safety. A tile
  therefore holds one sampler per mode, the second copied from the first in memory -- twice
  the tile's size (about 52 MB for a 1-arcsecond tile) in exchange for no locking and no
  second read of the file.
- **No runtime to install.** The DLL is built with `/MT`, so its only dependency is
  `KERNEL32.dll`: a release runs on a clean Windows machine without the Visual C++
  Redistributable.

## TerrainBench, the app on top of it

- **Its own folder.** The .NET solution lives in `app/`, not beside the C++ projects:
  `Directory.Build.props` is picked up by every MSBuild project under it, `.vcxproj` files
  included, and the app's settings have no business in the engine's build.
- **Where "no geometry" draws its line.** The rule is that the app computes no terrain
  geometry, and the tempting cases are the small ones. The map places the tile's square
  degree on screen and converts a pixel to a latitude and longitude linearly -- laying out a
  picture -- but its proportions come from the post spacings the DLL reports, and the scale
  bar's metres from a great-circle distance the DLL computes. The chart plots, and never
  derives, the curvature-corrected terrain, the sight line and the Fresnel radius: they
  arrive per sample from `te_analyze_path`. Metres become degrees inside the DLL, which also
  returns the degrees it used.
- **Numbers never pick up a decimal comma.** The first probe of the DLL from .NET, on a
  Turkish Windows, printed the spacing as `0,0002697964817756191`, and the CLI rightly
  refused it. Every number the app writes -- copied results, CSV, the command line it
  offers -- is formatted invariantly, and a test runs that code under `tr-TR`. Typing still
  accepts either form, since nobody should need to know which one is expected.
- **k typed as 4/3.** `1.3333333333333333` is what the engine's default really is, and it is
  unreadable. Fields accept a simple fraction and read it exactly, so the default shows as
  `4/3` and is still the same double the CLI uses.
- **Comparing with the previous run.** A very large `k` should visibly change a 30 km
  viewshed. Measured on the sample tile, it doesn't, much: with a 2 m observer, 8,583
  of the 4,000,000 cells in the whole square grid change (0.21%; 8,135 of them inside the
  30 km circle the app draws and counts); even at 300 m it is 1.87%. Drawn at map size, picking one
  cell per screen pixel, scattered changes like that simply vanish. Two changes fix it
  without touching the engine: the viewshed can be compared with the previous run, marking
  every changed cell and counting them, and an overlay larger than the screen is averaged
  down rather than sampled, so a lone changed cell still tints its pixel.
- **Viewsheds off the window's thread.** A run goes to a worker with `Task.Run`; the DLL's
  progress callback reports through a `Progress<double>` created on the UI thread, and
  Cancel sets a token the callback turns into "stop". A headless test starts a naive 30 km
  run, waits for progress, switches tabs while it runs, and cancels it.
- **Labels that explain themselves.** Field names are in Title Case and carry their unit in
  lighter text beside them; a unitless field shows none rather than "(no unit)". Every field
  that needs more than its name has a "?" whose tooltip says, in plain words, what the value
  does to the answer -- what k is, why the Fresnel zone needs a frequency, what a target
  height means -- so the panel stays short and the explanation is one hover away.
- **A viewshed at the tile's edge.** A radius can reach past the tile, where there is no
  data. Drawn as it came from the engine, that part of the disc was a large purple block of
  "no confident answer" which said nothing about the terrain. The disc is now clipped to the
  tile, the legend counts only the cells that are drawn -- inside the circle and on the
  tile -- and the summary says, in a sentence, that part of the circle lies beyond the tile
  and isn't drawn or counted. The engine's own counts for the whole grid are unchanged and
  still go into Copy Results, so nothing is hidden from a bug report.
- **Tests against the real thing.** The interop tests write the engine's hand-checkable
  cases out as `.hgt` tiles, because the DLL only reads tiles, and check one query against
  `TerrainEngine.exe`'s own output. The window tests run the real DLL too; only dialogs, the
  clipboard and the settings file are faked.
- **App tests that CI never ran.** `app/global.json` selects Microsoft.Testing.Platform as
  the test runner, but the SDK looks for `global.json` from the current folder, and
  `build.ps1` ran `dotnet test app/TerrainBench.sln` from the repository root. There the
  setting was never seen, the older runner found no tests in the xUnit v3 projects, and
  reported success -- so every CI run and release up to v1.1.0 built the app's tests
  without running one. They passed when run from `app/`, as they had been locally, so no
  release shipped a failing test; but nothing guaranteed it. `build.ps1` now runs every
  `dotnet` command for the app from `app/`, and the platform fails a run that finds no
  tests.
- **Pinned build image.** CI and releases run on `windows-2025` rather than
  `windows-latest`, so a runner image update can't swap the Visual C++ toolset under a
  release. Actions are pinned to commit SHAs.
- **An installer beside the zip.** The portable zip stays; the MSI holds the same
  published files, built by WiX Toolset 5 from `installer/TerrainBench.wxs`. It installs
  per user under `%LOCALAPPDATA%\Programs\TerrainBench`: a tool for trying the engine
  shouldn't need an administrator, and a per-user install can't touch anything outside
  the user's own profile.
  WiX comes in as a local dotnet tool pinned in `.config/dotnet-tools.json`, so a build machine
  needs nothing more than the .NET SDK it already has; version 5 rather than 6, whose licence
  terms changed. The first build had no wizard at all -- a progress bar that vanished, with no
  word that it had worked -- so it now has the usual pages: an Options page with a desktop
  shortcut, off unless ticked, and a Finish page that starts the app, ticked. The desktop box
  has to come before the install, since the shortcut is one of the things installed. WiX's
  ready-made wizards all include a licence page or a folder choice, so the page order is
  written out in the `.wxs` from WiX's own dialogs. The package's upgrade code is fixed,
  so installing a newer version replaces the older one in place. CI builds the installer
  on every push, so a packaging break is red before a tag finds it.
- **A tour that has you do it.** The first version was six pages of text on a card in the
  middle of the window, and reading about Ctrl + click is not the same as doing it. Each
  page now points at a real control: the window dims around a gap over it, a ring and an
  arrow mark it, and using it moves the tour on, so the first minute ends with a tile open,
  a line of sight checked and a viewshed run. The dimming takes the clicks everywhere but
  the gap, so a stray click can't lose the thread, while the keyboard stays free: the
  control a page waits for has the focus, and the map's Enter places the observer as a
  click does. Where the card goes is a small pure function beside the window code (beside
  the control on the side with room, else below or above, else inside the map), tested
  without a window. The pages and what each waits for live in a view model, tested
  without one too; a page whose step already holds, like opening a tile when one is open,
  is skipped, and whether the tour has been seen is one more remembered setting, absent
  from settings written before the tour, so an existing user sees it once too.
