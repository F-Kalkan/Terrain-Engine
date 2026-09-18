# Adapting toward a real-time host application

[ENGINE.md](ENGINE.md) describes the library as a standalone tool. This page covers
further changes aimed at a different consumer: a real-time simulation or
visualization runtime that would embed this library rather than call it from the
command line. That kind of host typically supplies terrain asynchronously in
batches instead of one point at a time, tracks altitude in a specific vertical
reference frame, and runs a line-of-sight query many times per second on a budget
that cannot tolerate a heap allocation per call. The changes below address those
constraints directly.

- **Vertical datum, as a type** — see "Vertical datum" under "Geometric model and
  assumptions" in [ENGINE.md](ENGINE.md). A host typically holds an airborne platform's altitude as a height
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
  "Orientation note" in [ENGINE.md](ENGINE.md) describes for the file-backed viewshed PGM output.
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