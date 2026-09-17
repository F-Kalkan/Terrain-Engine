using System.Runtime.InteropServices;

namespace TerrainBench.Engine.Interop;

/// <summary>
/// The C interface of TerrainEngineApi.dll, declared field for field against
/// <c>TerrainEngineApi/TerrainEngineApi.h</c>. Nothing outside <see cref="NativeTerrainEngine"/>
/// calls these directly.
/// </summary>
internal static class NativeMethods
{
    public const string Library = "TerrainEngineApi";

    public const int Ok = 0;
    public const int ErrorInvalidArgument = 1;
    public const int ErrorInvalidHandle = 2;
    public const int ErrorLoadFailed = 3;
    public const int ErrorOutsideTile = 4;
    public const int ErrorCancelled = 5;
    public const int ErrorOutOfMemory = 6;
    public const int ErrorInternal = 7;

    /// <summary>Called on the thread running the viewshed. Return 0 to continue, anything else to stop.</summary>
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate int ProgressCallback(double fractionDone, IntPtr userData);

    [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
    public static extern IntPtr te_engine_version();

    [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
    public static extern IntPtr te_engine_commit();

    [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
    public static extern IntPtr te_last_error_message();

    [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
    public static extern void te_free(IntPtr pointer);

    [DllImport(Library, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
    public static extern int te_tile_open(string path, double southWestLatitudeDeg, double southWestLongitudeDeg, out ulong tile);

    [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
    public static extern int te_tile_close(ulong tile);

    [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
    public static extern int te_tile_get_info(ulong tile, out TileInfoNative info);

    [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
    public static extern int te_tile_get_elevation(ulong tile, double latitudeDeg, double longitudeDeg, int interpolation, out double elevationM, out int hasValue);

    [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
    public static extern int te_tile_copy_posts(ulong tile, out IntPtr elevationsM, out IntPtr valid, out int postsPerSide);

    [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
    public static extern int te_distance_m(double latitude1Deg, double longitude1Deg, double latitude2Deg, double longitude2Deg, out double distanceM);

    [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
    public static extern int te_spacing_m_to_deg(double spacingM, out double spacingDeg);

    [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
    public static extern int te_analyze_path(ulong tile, ref PathQueryNative query, out PathResultNative result, out IntPtr samples);

    [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
    public static extern int te_viewshed(ulong tile, ref ViewshedQueryNative query, ProgressCallback? progress, IntPtr userData, out ViewshedGridNative grid, out IntPtr cells);

    [StructLayout(LayoutKind.Sequential)]
    public struct TileInfoNative
    {
        public double SouthWestLatitudeDeg;
        public double SouthWestLongitudeDeg;
        public double NorthEastLatitudeDeg;
        public double NorthEastLongitudeDeg;
        public int PostsPerSide;
        public int VoidCount;
        public double PostSpacingArcsec;
        public double PostSpacingNorthSouthM;
        public double PostSpacingEastWestM;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct PathQueryNative
    {
        public double ObserverLatitudeDeg;
        public double ObserverLongitudeDeg;
        public double ObserverHeightAboveGroundM;
        public double TargetLatitudeDeg;
        public double TargetLongitudeDeg;
        public double TargetHeightAboveGroundM;
        public double SpacingM;
        public double RefractionK;
        public double FrequencyMHz;
        public int Interpolation;
        public int Reserved;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct PathResultNative
    {
        public double SpacingDeg;
        public double TotalDistanceM;
        public double ObserverEyeHeightM;
        public double TargetEyeHeightM;
        public int EyeHeightsKnown;
        public int SampleCount;
        public int LosStatus;
        public int IsVisible;
        public int HasBlockingPoint;
        public int BlockingFeature;
        public int BlockingSampleIndex;
        public int Reserved0;
        public double BlockingLatitudeDeg;
        public double BlockingLongitudeDeg;
        public double BlockingElevationM;
        public double BlockingDistanceM;
        public double ClearanceDeficitM;
        public int FresnelComputed;
        public int FresnelStatus;
        public int HasWorstPoint;
        public int WorstSampleIndex;
        public double MinClearanceFraction;
        public double WavelengthM;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct PathSampleNative
    {
        public double LatitudeDeg;
        public double LongitudeDeg;
        public double DistanceM;
        public double ElevationM;
        public int HasElevation;
        public int Reserved;
        public double CurvatureCorrectedElevationM;
        public double SightLineHeightM;
        public double FirstFresnelRadiusM;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct ViewshedQueryNative
    {
        public double ObserverLatitudeDeg;
        public double ObserverLongitudeDeg;
        public double ObserverHeightAboveGroundM;
        public double RadiusKm;
        public double SpacingM;
        public double RefractionK;
        public int Interpolation;
        public int Algorithm;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct ViewshedGridNative
    {
        public int Rows;
        public int Cols;
        public int ObserverRow;
        public int ObserverCol;
        public double SpacingDeg;
        public double ColStepDeg;
        public double SouthWestCellLatitudeDeg;
        public double SouthWestCellLongitudeDeg;
    }
}
