using System.Runtime.InteropServices;
using TerrainBench.Engine.Interop;

namespace TerrainBench.Engine;

/// <summary>
/// <see cref="ITerrainEngine"/> over TerrainEngineApi.dll. Every call returns an
/// <see cref="EngineResult{T}"/>; the DLL's own messages are passed through unchanged, since they
/// are written to be shown to a user.
/// </summary>
public sealed class NativeTerrainEngine : ITerrainEngine
{
    private NativeTerrainEngine(string version, string commit)
    {
        Version = version;
        Commit = commit;
    }

    public string Version { get; }

    public string Commit { get; }

    /// <summary>Loads the DLL, or says in plain English why it couldn't be loaded.</summary>
    public static EngineResult<ITerrainEngine> Load()
    {
        try
        {
            var version = Utf8(NativeMethods.te_engine_version());
            var commit = Utf8(NativeMethods.te_engine_commit());
            return EngineResult<ITerrainEngine>.Ok(new NativeTerrainEngine(version, commit));
        }
        catch (DllNotFoundException)
        {
            return EngineResult<ITerrainEngine>.Fail(EngineErrorKind.EngineMissing,
                "The terrain engine, TerrainEngineApi.dll, is missing from the TerrainBench folder. Extract the whole release zip again.");
        }
        catch (EntryPointNotFoundException)
        {
            return EngineResult<ITerrainEngine>.Fail(EngineErrorKind.EngineMissing,
                "TerrainEngineApi.dll in the TerrainBench folder is from a different version. Extract the whole release zip again.");
        }
        catch (BadImageFormatException)
        {
            return EngineResult<ITerrainEngine>.Fail(EngineErrorKind.EngineMissing,
                "TerrainEngineApi.dll in the TerrainBench folder isn't a 64-bit Windows library. Extract the whole release zip again.");
        }
    }

    public EngineResult<ITile> OpenTile(string path, double southWestLatitudeDeg, double southWestLongitudeDeg)
    {
        int code = NativeMethods.te_tile_open(path ?? string.Empty, southWestLatitudeDeg, southWestLongitudeDeg, out ulong handle);
        if (code != NativeMethods.Ok) return EngineResult<ITile>.Fail(LastError(code));

        code = NativeMethods.te_tile_get_info(handle, out var native);
        if (code != NativeMethods.Ok)
        {
            var error = LastError(code);
            NativeMethods.te_tile_close(handle);
            return EngineResult<ITile>.Fail(error);
        }

        var info = new TileInfo(
            native.SouthWestLatitudeDeg, native.SouthWestLongitudeDeg,
            native.NorthEastLatitudeDeg, native.NorthEastLongitudeDeg,
            native.PostsPerSide, native.VoidCount,
            native.PostSpacingArcsec, native.PostSpacingNorthSouthM, native.PostSpacingEastWestM);

        return EngineResult<ITile>.Ok(new NativeTile(handle, path!, info));
    }

    public EngineResult<double> DistanceM(double latitude1Deg, double longitude1Deg, double latitude2Deg, double longitude2Deg)
    {
        int code = NativeMethods.te_distance_m(latitude1Deg, longitude1Deg, latitude2Deg, longitude2Deg, out double distance);
        return code == NativeMethods.Ok ? EngineResult<double>.Ok(distance) : EngineResult<double>.Fail(LastError(code));
    }

    public EngineResult<double> SpacingToDegrees(double spacingM)
    {
        int code = NativeMethods.te_spacing_m_to_deg(spacingM, out double spacingDeg);
        return code == NativeMethods.Ok ? EngineResult<double>.Ok(spacingDeg) : EngineResult<double>.Fail(LastError(code));
    }

    internal static EngineError LastError(int code)
    {
        var message = Utf8(NativeMethods.te_last_error_message());
        if (string.IsNullOrWhiteSpace(message)) message = "The engine reported an error without a description.";
        return new EngineError((EngineErrorKind)code, message);
    }

    private static string Utf8(IntPtr text) => Marshal.PtrToStringUTF8(text) ?? string.Empty;

    private sealed class NativeTile : ITile
    {
        private ulong _handle;

        public NativeTile(ulong handle, string path, TileInfo info)
        {
            _handle = handle;
            Path = path;
            Info = info;
        }

        public string Path { get; }

        public TileInfo Info { get; }

        // A disposed tile's handle is 0, which the DLL reports as an invalid handle rather than
        // touching anything.
        private ulong Handle => Interlocked.Read(ref _handle);

        public EngineResult<double?> Elevation(double latitudeDeg, double longitudeDeg, Interpolation interpolation)
        {
            int code = NativeMethods.te_tile_get_elevation(Handle, latitudeDeg, longitudeDeg, (int)interpolation, out double elevation, out int hasValue);
            if (code != NativeMethods.Ok) return EngineResult<double?>.Fail(LastError(code));
            return EngineResult<double?>.Ok(hasValue != 0 ? elevation : null);
        }

        public EngineResult<TilePosts> CopyPosts()
        {
            int code = NativeMethods.te_tile_copy_posts(Handle, out IntPtr elevationsPtr, out IntPtr validPtr, out int side);
            if (code != NativeMethods.Ok) return EngineResult<TilePosts>.Fail(LastError(code));

            try
            {
                int count = checked(side * side);
                var elevations = new float[count];
                var valid = new byte[count];
                Marshal.Copy(elevationsPtr, elevations, 0, count);
                Marshal.Copy(validPtr, valid, 0, count);
                return EngineResult<TilePosts>.Ok(new TilePosts(side, elevations, valid));
            }
            finally
            {
                NativeMethods.te_free(elevationsPtr);
                NativeMethods.te_free(validPtr);
            }
        }

        public EngineResult<PathAnalysis> AnalyzePath(PathQuery query)
        {
            var native = new NativeMethods.PathQueryNative
            {
                ObserverLatitudeDeg = query.ObserverLatitudeDeg,
                ObserverLongitudeDeg = query.ObserverLongitudeDeg,
                ObserverHeightAboveGroundM = query.ObserverHeightAboveGroundM,
                TargetLatitudeDeg = query.TargetLatitudeDeg,
                TargetLongitudeDeg = query.TargetLongitudeDeg,
                TargetHeightAboveGroundM = query.TargetHeightAboveGroundM,
                SpacingM = query.SpacingM,
                RefractionK = query.RefractionK,
                FrequencyMHz = query.FrequencyMHz,
                Interpolation = (int)query.Interpolation,
            };

            int code = NativeMethods.te_analyze_path(Handle, ref native, out var result, out IntPtr samplesPtr);
            if (code != NativeMethods.Ok) return EngineResult<PathAnalysis>.Fail(LastError(code));

            try
            {
                bool eyesKnown = result.EyeHeightsKnown != 0;
                bool fresnel = result.FresnelComputed != 0;
                int size = Marshal.SizeOf<NativeMethods.PathSampleNative>();
                var samples = new PathSample[result.SampleCount];

                for (int i = 0; i < samples.Length; i++)
                {
                    var s = Marshal.PtrToStructure<NativeMethods.PathSampleNative>(samplesPtr + i * size);
                    bool has = s.HasElevation != 0;
                    samples[i] = new PathSample(
                        s.LatitudeDeg,
                        s.LongitudeDeg,
                        s.DistanceM,
                        has ? s.ElevationM : null,
                        has ? s.CurvatureCorrectedElevationM : null,
                        eyesKnown ? s.SightLineHeightM : null,
                        fresnel ? s.FirstFresnelRadiusM : 0.0);
                }

                var status = (ComputationStatus)result.LosStatus;
                BlockingPoint? blocking = status == ComputationStatus.Ok && result.HasBlockingPoint != 0
                    ? new BlockingPoint(
                        result.BlockingLatitudeDeg,
                        result.BlockingLongitudeDeg,
                        result.BlockingElevationM,
                        result.BlockingDistanceM,
                        result.BlockingSampleIndex,
                        result.ClearanceDeficitM,
                        (TerrainFeature)result.BlockingFeature)
                    : null;

                FresnelClearance? clearance = fresnel
                    ? new FresnelClearance((ComputationStatus)result.FresnelStatus, result.MinClearanceFraction, result.WorstSampleIndex, result.WavelengthM)
                    : null;

                return EngineResult<PathAnalysis>.Ok(new PathAnalysis(
                    result.SpacingDeg,
                    result.TotalDistanceM,
                    eyesKnown ? result.ObserverEyeHeightM : null,
                    eyesKnown ? result.TargetEyeHeightM : null,
                    status,
                    result.IsVisible != 0,
                    blocking,
                    clearance,
                    samples));
            }
            finally
            {
                NativeMethods.te_free(samplesPtr);
            }
        }

        public EngineResult<ViewshedMap> Viewshed(ViewshedQuery query, IProgress<double>? progress, CancellationToken cancellation)
        {
            var native = new NativeMethods.ViewshedQueryNative
            {
                ObserverLatitudeDeg = query.ObserverLatitudeDeg,
                ObserverLongitudeDeg = query.ObserverLongitudeDeg,
                ObserverHeightAboveGroundM = query.ObserverHeightAboveGroundM,
                RadiusKm = query.RadiusKm,
                SpacingM = query.SpacingM,
                RefractionK = query.RefractionK,
                Interpolation = (int)query.Interpolation,
                Algorithm = (int)query.Algorithm,
                TargetHeightAboveGroundM = query.TargetHeightAboveGroundM,
            };

            // An exception must never unwind into the DLL: anything thrown while reporting stops the run.
            NativeMethods.ProgressCallback callback = (fraction, _) =>
            {
                try
                {
                    progress?.Report(fraction);
                    return cancellation.IsCancellationRequested ? 1 : 0;
                }
                catch
                {
                    return 1;
                }
            };

            int code = NativeMethods.te_viewshed(Handle, ref native, callback, IntPtr.Zero, out var grid, out IntPtr cellsPtr);
            GC.KeepAlive(callback);
            if (code != NativeMethods.Ok) return EngineResult<ViewshedMap>.Fail(LastError(code));

            try
            {
                int count = checked(grid.Rows * grid.Cols);
                var bytes = new byte[count];
                Marshal.Copy(cellsPtr, bytes, 0, count);
                var cells = new CellState[count];
                for (int i = 0; i < count; i++) cells[i] = (CellState)bytes[i];

                return EngineResult<ViewshedMap>.Ok(new ViewshedMap(
                    grid.Rows, grid.Cols, grid.ObserverRow, grid.ObserverCol,
                    grid.SpacingDeg, grid.ColStepDeg,
                    grid.SouthWestCellLatitudeDeg, grid.SouthWestCellLongitudeDeg,
                    cells));
            }
            finally
            {
                NativeMethods.te_free(cellsPtr);
            }
        }

        public void Dispose()
        {
            ulong handle = Interlocked.Exchange(ref _handle, 0);
            if (handle != 0) NativeMethods.te_tile_close(handle);
        }
    }
}
