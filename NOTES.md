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
  latitude/longitude currently share a type only because it happens to compile, not
  because they mean the same thing.
- Document the vertical-datum gap as a loud, unmissable part of the API (a comment at
  minimum, ideally a named parameter forcing the caller to state which convention
  their heights use) instead of a README paragraph nobody has to read before calling
  the function.