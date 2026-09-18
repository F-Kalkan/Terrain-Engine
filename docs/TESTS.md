# The engine's test suite

53 hand-checkable test functions, plus a real-data tolerance assertion per tile:

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
  character, and reported as zero for a file that didn't load; one loaded tile answering in the other
  interpolation mode exactly as a fresh load in that mode would; and the in-place profile
  overload reusing a caller-owned buffer without growing it.
- **Viewshed target height** (the wall scene of the fast/naive test): straight east of a
  2 m observer, a target 180 m out behind a 50 m wall 90 m out clears it once its height
  reaches 98 m, worked by hand -- both viewsheds hide it at 97.9 m and show it at 98.1 m, and
  at the default of 0 m (the ground itself) hide it as they always did; a taller target only
  ever reveals cells, never hides one or raises the horizon for the cells behind it, and an
  explicit 0 m is the default cell for cell; fast matches naive on every cell off the wall
  at 0 m and 30 m, and at 120 m, where the visible region ends in the open field, disagrees
  only on that boundary, for the reason the fast/naive section of [ENGINE.md](ENGINE.md) gives for ridgelines; and a
  target height in a datum the terrain can't be put on leaves every cell but the observer's
  Degraded in both, never guessed.
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
