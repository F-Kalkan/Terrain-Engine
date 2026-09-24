#include "TerrainEngineApi.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>

#include "TerrainProfile.h"
#include "LineOfSight.h"
#include "Viewshed.h"
#include "RealElevationSampler.h"

// Filled in by the build (build.ps1 passes the version and the commit); a plain build falls back to
// these. They arrive unquoted -- quotes don't survive MSBuild's command line intact -- and are turned
// into strings here. build.ps1 only lets letters, digits, '.', '_' and '-' through.
#define TE_STRINGIFY_TOKENS(tokens) #tokens
#define TE_STRINGIFY(tokens) TE_STRINGIFY_TOKENS(tokens)
#ifdef TE_ENGINE_VERSION_TOKENS
#define TE_ENGINE_VERSION TE_STRINGIFY(TE_ENGINE_VERSION_TOKENS)
#else
#define TE_ENGINE_VERSION "0.0.0-dev"
#endif
#ifdef TE_ENGINE_COMMIT_TOKENS
#define TE_ENGINE_COMMIT TE_STRINGIFY(TE_ENGINE_COMMIT_TOKENS)
#else
#define TE_ENGINE_COMMIT "unknown"
#endif

namespace
{
    // ---- Errors -------------------------------------------------------------------

    thread_local std::string lastErrorMessage;

    int32_t Succeed()
    {
        lastErrorMessage.clear();
        return TE_OK;
    }

    int32_t Fail(int32_t code, const std::string& message)
    {
        lastErrorMessage = message;
        return code;
    }

    // Runs one exported function's body so that no exception can cross the boundary.
    template <typename Body>
    int32_t Guarded(Body body)
    {
        try
        {
            return body();
        }
        catch (const std::bad_alloc&)
        {
            return Fail(TE_ERROR_OUT_OF_MEMORY, "There isn't enough memory for this request. Try a smaller radius or a larger spacing.");
        }
        catch (const std::exception& e)
        {
            return Fail(TE_ERROR_INTERNAL, std::string("The engine hit an unexpected error: ") + e.what());
        }
        catch (...)
        {
            return Fail(TE_ERROR_INTERNAL, "The engine hit an unexpected error.");
        }
    }

    std::string Number(double value)
    {
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "%.6g", value);
        return buffer;
    }

    std::string ToUtf8(const std::wstring& text)
    {
        if (text.empty()) return {};
        int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0, nullptr, nullptr);
        std::string utf8(length, '\0');
        WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), utf8.data(), length, nullptr, nullptr);
        return utf8;
    }

    // Arrays handed to the caller come from malloc, so te_free can release them without
    // knowing their type.
    template <typename T>
    T* AllocateArray(size_t count)
    {
        void* memory = std::malloc((count == 0 ? 1 : count) * sizeof(T));
        if (memory == nullptr) throw std::bad_alloc();
        return static_cast<T*>(memory);
    }

    // ---- Tiles --------------------------------------------------------------------

    struct Tile
    {
        Tile(const std::filesystem::path& path, double southWestLatitudeDeg, double southWestLongitudeDeg)
            : nearest(path, southWestLatitudeDeg, southWestLongitudeDeg, InterpolationMode::Nearest),
              bilinear(nearest.WithInterpolationMode(InterpolationMode::Bilinear))
        {
        }

        RealElevationSampler& Sampler(int32_t interpolation)
        {
            return interpolation == TE_INTERPOLATION_BILINEAR ? bilinear : nearest;
        }

        RealElevationSampler nearest;
        RealElevationSampler bilinear;
    };

    std::mutex registryMutex;
    std::unordered_map<te_tile, std::shared_ptr<Tile>> openTiles;
    std::atomic<te_tile> nextTileHandle{ 1 };

    std::shared_ptr<Tile> FindTile(te_tile handle)
    {
        std::lock_guard<std::mutex> lock(registryMutex);
        auto found = openTiles.find(handle);
        return found == openTiles.end() ? nullptr : found->second;
    }

    const char* InvalidHandleMessage = "That tile isn't open: it was closed, or never opened.";

    // ---- Validation ---------------------------------------------------------------
    // Each returns an empty string when the input is acceptable, or a message saying
    // what is allowed.

    std::string PointProblem(const char* what, double latitudeDeg, double longitudeDeg)
    {
        if (!std::isfinite(latitudeDeg) || !std::isfinite(longitudeDeg))
            return std::string("The ") + what + " position must be given as numbers.";
        if (latitudeDeg < -90.0 || latitudeDeg > 90.0)
            return std::string("The ") + what + " latitude must be between -90 and 90 degrees.";
        if (longitudeDeg < -180.0 || longitudeDeg > 180.0)
            return std::string("The ") + what + " longitude must be between -180 and 180 degrees.";
        return {};
    }

    std::string OutsideTileProblem(const char* what, double latitudeDeg, double longitudeDeg, const RealElevationSampler& sampler)
    {
        double south = sampler.SouthWestLatitudeDeg();
        double west = sampler.SouthWestLongitudeDeg();
        if (latitudeDeg >= south && latitudeDeg <= south + 1.0 && longitudeDeg >= west && longitudeDeg <= west + 1.0) return {};
        return std::string("The ") + what + " (" + Number(latitudeDeg) + ", " + Number(longitudeDeg)
            + ") is outside the open tile, which covers latitude " + Number(south) + " to " + Number(south + 1.0)
            + " and longitude " + Number(west) + " to " + Number(west + 1.0) + ".";
    }

    std::string PositiveProblem(const char* what, double value, const char* unit)
    {
        if (!std::isfinite(value) || value <= 0.0)
            return std::string("The ") + what + " must be a number greater than 0" + unit + ".";
        return {};
    }

    std::string HeightProblem(const char* what, double value)
    {
        if (!std::isfinite(value) || value < 0.0 || value > 100000.0)
            return std::string("The ") + what + " height above ground must be between 0 and 100000 m.";
        return {};
    }

    std::string InterpolationProblem(int32_t interpolation)
    {
        if (interpolation != TE_INTERPOLATION_NEAREST && interpolation != TE_INTERPOLATION_BILINEAR)
            return "The interpolation mode must be nearest or bilinear.";
        return {};
    }

    double MetersPerDegree()
    {
        return EarthRadiusM * DegToRad;
    }

    // Index of the sample a result's point was copied from, or -1.
    int32_t SampleIndexOf(const std::vector<ProfileSample>& profile, const std::optional<GeoPoint>& point)
    {
        if (!point.has_value()) return -1;
        for (size_t i = 0; i < profile.size(); i++)
        {
            if (profile[i].point.latitudeDeg == point->latitudeDeg && profile[i].point.longitudeDeg == point->longitudeDeg)
                return (int32_t)i;
        }
        return -1;
    }
}

const char* te_engine_version(void)
{
    return TE_ENGINE_VERSION;
}

const char* te_engine_commit(void)
{
    return TE_ENGINE_COMMIT;
}

const char* te_last_error_message(void)
{
    return lastErrorMessage.c_str();
}

void te_free(void* pointer)
{
    std::free(pointer);
}

int32_t te_tile_open(const wchar_t* path, double south_west_latitude_deg, double south_west_longitude_deg, te_tile* out_tile)
{
    return Guarded([&]() -> int32_t {
        if (out_tile == nullptr) return Fail(TE_ERROR_INVALID_ARGUMENT, "out_tile must not be NULL.");
        *out_tile = 0;

        if (path == nullptr || *path == L'\0')
            return Fail(TE_ERROR_INVALID_ARGUMENT, "No tile file was given.");
        if (!std::isfinite(south_west_latitude_deg) || south_west_latitude_deg < -90.0 || south_west_latitude_deg > 89.0)
            return Fail(TE_ERROR_INVALID_ARGUMENT, "The south-west corner latitude must be between -90 and 89 degrees.");
        if (!std::isfinite(south_west_longitude_deg) || south_west_longitude_deg < -180.0 || south_west_longitude_deg > 179.0)
            return Fail(TE_ERROR_INVALID_ARGUMENT, "The south-west corner longitude must be between -180 and 179 degrees.");

        std::filesystem::path filePath(path);
        std::string shownName = ToUtf8(filePath.filename().wstring());
        std::error_code error;

        if (!std::filesystem::exists(filePath, error))
            return Fail(TE_ERROR_LOAD_FAILED, "The file " + shownName + " doesn't exist.");
        if (std::filesystem::is_directory(filePath, error))
            return Fail(TE_ERROR_LOAD_FAILED, shownName + " is a folder, not a tile file.");
        std::uintmax_t bytes = std::filesystem::file_size(filePath, error);
        if (error)
            return Fail(TE_ERROR_LOAD_FAILED, "The file " + shownName + " couldn't be read.");
        if (bytes == 0)
            return Fail(TE_ERROR_LOAD_FAILED, "The file " + shownName + " is empty.");

        auto tile = std::make_shared<Tile>(filePath, south_west_latitude_deg, south_west_longitude_deg);
        if (!tile->nearest.IsLoaded())
        {
            std::uintmax_t posts = bytes / 2;
            std::uintmax_t side = (std::uintmax_t)std::llround(std::sqrt((double)posts));
            bool wholeTile = bytes % 2 == 0 && side >= 2 && side * side == posts;
            if (wholeTile)
                return Fail(TE_ERROR_LOAD_FAILED, "The file " + shownName + " couldn't be read.");
            return Fail(TE_ERROR_LOAD_FAILED, "The file " + shownName + " isn't a complete .hgt tile (" + std::to_string(bytes)
                + " bytes). A tile holds a square grid of 2-byte heights, such as 1201 x 1201 or 3601 x 3601; this one may have been cut short while downloading.");
        }

        te_tile handle = nextTileHandle.fetch_add(1);
        {
            std::lock_guard<std::mutex> lock(registryMutex);
            openTiles.emplace(handle, std::move(tile));
        }
        *out_tile = handle;
        return Succeed();
    });
}

int32_t te_tile_close(te_tile tile)
{
    return Guarded([&]() -> int32_t {
        std::shared_ptr<Tile> closing;
        {
            std::lock_guard<std::mutex> lock(registryMutex);
            auto found = openTiles.find(tile);
            if (found == openTiles.end()) return Fail(TE_ERROR_INVALID_HANDLE, InvalidHandleMessage);
            closing = std::move(found->second);
            openTiles.erase(found);
        }
        // The tile itself is freed here, outside the lock, unless a running call still holds it.
        closing.reset();
        return Succeed();
    });
}

int32_t te_tile_get_info(te_tile tile, te_tile_info* out_info)
{
    return Guarded([&]() -> int32_t {
        if (out_info == nullptr) return Fail(TE_ERROR_INVALID_ARGUMENT, "out_info must not be NULL.");
        *out_info = te_tile_info{};

        std::shared_ptr<Tile> found = FindTile(tile);
        if (!found) return Fail(TE_ERROR_INVALID_HANDLE, InvalidHandleMessage);

        const RealElevationSampler& sampler = found->nearest;
        double south = sampler.SouthWestLatitudeDeg();
        double west = sampler.SouthWestLongitudeDeg();
        double postDeg = 1.0 / (sampler.PostsPerSide() - 1);
        double middleLatitude = south + 0.5;
        double middleLongitude = west + 0.5;

        te_tile_info info{};
        info.south_west_latitude_deg = south;
        info.south_west_longitude_deg = west;
        info.north_east_latitude_deg = south + 1.0;
        info.north_east_longitude_deg = west + 1.0;
        info.posts_per_side = sampler.PostsPerSide();
        info.void_count = sampler.VoidCount();
        info.post_spacing_arcsec = 3600.0 * postDeg;
        info.post_spacing_north_south_m = GreatCircleDistanceM(GeoPoint{ middleLatitude, middleLongitude }, GeoPoint{ middleLatitude + postDeg, middleLongitude });
        info.post_spacing_east_west_m = GreatCircleDistanceM(GeoPoint{ middleLatitude, middleLongitude }, GeoPoint{ middleLatitude, middleLongitude + postDeg });
        *out_info = info;
        return Succeed();
    });
}

int32_t te_tile_get_elevation(te_tile tile, double latitude_deg, double longitude_deg, int32_t interpolation, double* out_elevation_m, int32_t* out_has_value)
{
    return Guarded([&]() -> int32_t {
        if (out_elevation_m == nullptr || out_has_value == nullptr)
            return Fail(TE_ERROR_INVALID_ARGUMENT, "out_elevation_m and out_has_value must not be NULL.");
        *out_elevation_m = 0.0;
        *out_has_value = 0;

        std::shared_ptr<Tile> found = FindTile(tile);
        if (!found) return Fail(TE_ERROR_INVALID_HANDLE, InvalidHandleMessage);

        std::string problem = InterpolationProblem(interpolation);
        if (problem.empty()) problem = PointProblem("point", latitude_deg, longitude_deg);
        if (!problem.empty()) return Fail(TE_ERROR_INVALID_ARGUMENT, problem);
        problem = OutsideTileProblem("point", latitude_deg, longitude_deg, found->nearest);
        if (!problem.empty()) return Fail(TE_ERROR_OUTSIDE_TILE, problem);

        std::optional<double> elevation = found->Sampler(interpolation).GetElevation(latitude_deg, longitude_deg);
        if (elevation.has_value())
        {
            *out_elevation_m = *elevation;
            *out_has_value = 1;
        }
        return Succeed();
    });
}

int32_t te_tile_copy_posts(te_tile tile, float** out_elevations_m, uint8_t** out_valid, int32_t* out_posts_per_side)
{
    return Guarded([&]() -> int32_t {
        if (out_elevations_m == nullptr || out_valid == nullptr || out_posts_per_side == nullptr)
            return Fail(TE_ERROR_INVALID_ARGUMENT, "out_elevations_m, out_valid and out_posts_per_side must not be NULL.");
        *out_elevations_m = nullptr;
        *out_valid = nullptr;
        *out_posts_per_side = 0;

        std::shared_ptr<Tile> found = FindTile(tile);
        if (!found) return Fail(TE_ERROR_INVALID_HANDLE, InvalidHandleMessage);

        const std::vector<int16_t>& posts = found->nearest.Posts();
        std::unique_ptr<float, decltype(&std::free)> elevations(AllocateArray<float>(posts.size()), &std::free);
        std::unique_ptr<uint8_t, decltype(&std::free)> valid(AllocateArray<uint8_t>(posts.size()), &std::free);
        for (size_t i = 0; i < posts.size(); i++)
        {
            bool isVoid = posts[i] == -32768;
            elevations.get()[i] = isVoid ? 0.0f : (float)posts[i];
            valid.get()[i] = isVoid ? 0 : 1;
        }

        *out_posts_per_side = found->nearest.PostsPerSide();
        *out_elevations_m = elevations.release();
        *out_valid = valid.release();
        return Succeed();
    });
}

int32_t te_distance_m(double latitude1_deg, double longitude1_deg, double latitude2_deg, double longitude2_deg, double* out_distance_m)
{
    return Guarded([&]() -> int32_t {
        if (out_distance_m == nullptr) return Fail(TE_ERROR_INVALID_ARGUMENT, "out_distance_m must not be NULL.");
        *out_distance_m = 0.0;

        std::string problem = PointProblem("first", latitude1_deg, longitude1_deg);
        if (problem.empty()) problem = PointProblem("second", latitude2_deg, longitude2_deg);
        if (!problem.empty()) return Fail(TE_ERROR_INVALID_ARGUMENT, problem);

        *out_distance_m = GreatCircleDistanceM(GeoPoint{ latitude1_deg, longitude1_deg }, GeoPoint{ latitude2_deg, longitude2_deg });
        return Succeed();
    });
}

int32_t te_spacing_m_to_deg(double spacing_m, double* out_spacing_deg)
{
    return Guarded([&]() -> int32_t {
        if (out_spacing_deg == nullptr) return Fail(TE_ERROR_INVALID_ARGUMENT, "out_spacing_deg must not be NULL.");
        *out_spacing_deg = 0.0;

        std::string problem = PositiveProblem("spacing", spacing_m, " m");
        if (!problem.empty()) return Fail(TE_ERROR_INVALID_ARGUMENT, problem);

        *out_spacing_deg = spacing_m / MetersPerDegree();
        return Succeed();
    });
}

int32_t te_analyze_path(te_tile tile, const te_path_query* query, te_path_result* out_result, te_path_sample** out_samples)
{
    return Guarded([&]() -> int32_t {
        if (query == nullptr || out_result == nullptr || out_samples == nullptr)
            return Fail(TE_ERROR_INVALID_ARGUMENT, "query, out_result and out_samples must not be NULL.");
        *out_result = te_path_result{};
        *out_samples = nullptr;

        std::shared_ptr<Tile> found = FindTile(tile);
        if (!found) return Fail(TE_ERROR_INVALID_HANDLE, InvalidHandleMessage);

        const te_path_query& q = *query;
        std::string problem = PointProblem("observer", q.observer_latitude_deg, q.observer_longitude_deg);
        if (problem.empty()) problem = PointProblem("target", q.target_latitude_deg, q.target_longitude_deg);
        if (problem.empty()) problem = HeightProblem("observer", q.observer_height_above_ground_m);
        if (problem.empty()) problem = HeightProblem("target", q.target_height_above_ground_m);
        if (problem.empty()) problem = PositiveProblem("spacing", q.spacing_m, " m");
        if (problem.empty()) problem = PositiveProblem("refraction factor k", q.refraction_k, "");
        if (problem.empty() && (!std::isfinite(q.frequency_mhz) || q.frequency_mhz < 0.0))
            problem = "The frequency must be a number of MHz, or 0 to skip Fresnel clearance.";
        if (problem.empty()) problem = InterpolationProblem(q.interpolation);
        if (!problem.empty()) return Fail(TE_ERROR_INVALID_ARGUMENT, problem);

        problem = OutsideTileProblem("observer", q.observer_latitude_deg, q.observer_longitude_deg, found->nearest);
        if (problem.empty()) problem = OutsideTileProblem("target", q.target_latitude_deg, q.target_longitude_deg, found->nearest);
        if (!problem.empty()) return Fail(TE_ERROR_OUTSIDE_TILE, problem);

        GeoPoint observer{ q.observer_latitude_deg, q.observer_longitude_deg };
        GeoPoint target{ q.target_latitude_deg, q.target_longitude_deg };
        double totalDistanceM = GreatCircleDistanceM(observer, target);
        if (totalDistanceM / q.spacing_m > TE_MAX_PATH_SAMPLES)
            return Fail(TE_ERROR_INVALID_ARGUMENT, "The spacing is too small for a path this long: it would take more than "
                + std::to_string(TE_MAX_PATH_SAMPLES) + " samples. Use a spacing of at least " + Number(totalDistanceM / TE_MAX_PATH_SAMPLES) + " m.");

        double spacingDeg = q.spacing_m / MetersPerDegree();
        RealElevationSampler& sampler = found->Sampler(q.interpolation);
        std::vector<ProfileSample> profile = GetTerrainProfile(observer, target, spacingDeg, sampler);

        DatumHeight observerHeight{ q.observer_height_above_ground_m, VerticalDatum::HeightAboveGround };
        DatumHeight targetHeight{ q.target_height_above_ground_m, VerticalDatum::HeightAboveGround };
        LineOfSightResult los = ComputeLineOfSight(profile, observerHeight, targetHeight, q.refraction_k);

        te_path_result result{};
        result.spacing_deg = spacingDeg;
        result.total_distance_m = profile.back().distanceFromStartM;
        result.sample_count = (int32_t)profile.size();

        bool eyesKnown = profile.front().elevationM.has_value() && profile.back().elevationM.has_value();
        if (eyesKnown)
        {
            VerticalDatum terrainDatum = profile.front().elevationDatum;
            std::optional<double> observerEye = EyeHeightInTerrainDatum(observerHeight, *profile.front().elevationM, terrainDatum);
            std::optional<double> targetEye = EyeHeightInTerrainDatum(targetHeight, *profile.back().elevationM, terrainDatum);
            eyesKnown = observerEye.has_value() && targetEye.has_value();
            if (eyesKnown)
            {
                result.observer_eye_height_m = *observerEye;
                result.target_eye_height_m = *targetEye;
            }
        }
        result.eye_heights_known = eyesKnown ? 1 : 0;

        result.los_status = (int32_t)los.status;
        result.is_visible = los.isVisible ? 1 : 0;
        result.blocking_feature = (int32_t)los.blockingFeature;
        result.blocking_sample_index = SampleIndexOf(profile, los.blockingPoint);
        result.has_blocking_point = los.blockingPoint.has_value() ? 1 : 0;
        if (los.blockingPoint.has_value())
        {
            result.blocking_latitude_deg = los.blockingPoint->latitudeDeg;
            result.blocking_longitude_deg = los.blockingPoint->longitudeDeg;
            result.blocking_elevation_m = los.blockingElevationM.value_or(0.0);
            if (result.blocking_sample_index >= 0) result.blocking_distance_m = profile[result.blocking_sample_index].distanceFromStartM;
        }
        result.clearance_deficit_m = los.clearanceDeficitM;
        result.worst_sample_index = -1;

        double wavelengthM = 0.0;
        if (q.frequency_mhz > 0.0)
        {
            double frequencyHz = q.frequency_mhz * 1e6;
            FresnelClearanceResult fresnel = ComputeFresnelClearance(profile, observerHeight, targetHeight, frequencyHz, q.refraction_k);
            wavelengthM = WavelengthM(frequencyHz);
            result.fresnel_computed = 1;
            result.fresnel_status = (int32_t)fresnel.status;
            result.has_worst_point = fresnel.worstPoint.has_value() ? 1 : 0;
            result.worst_sample_index = SampleIndexOf(profile, fresnel.worstPoint);
            result.min_clearance_fraction = fresnel.minClearanceFraction;
            result.wavelength_m = wavelengthM;
        }

        te_path_sample* samples = AllocateArray<te_path_sample>(profile.size());
        for (size_t i = 0; i < profile.size(); i++)
        {
            const ProfileSample& p = profile[i];
            double d1M = p.distanceFromStartM;
            double d2M = result.total_distance_m - d1M;

            te_path_sample s{};
            s.latitude_deg = p.point.latitudeDeg;
            s.longitude_deg = p.point.longitudeDeg;
            s.distance_m = d1M;
            if (p.elevationM.has_value())
            {
                s.has_elevation = 1;
                s.elevation_m = *p.elevationM;
                s.curvature_corrected_elevation_m = *p.elevationM + CurvatureDropM(d1M, d2M, q.refraction_k);
            }
            if (eyesKnown)
            {
                s.sight_line_height_m = SightLineHeightM(result.observer_eye_height_m, result.target_eye_height_m, d1M, result.total_distance_m);
            }
            if (result.fresnel_computed && d1M > 0 && d2M > 0)
            {
                s.first_fresnel_radius_m = FirstFresnelRadiusM(wavelengthM, d1M, d2M, result.total_distance_m);
            }
            samples[i] = s;
        }

        *out_result = result;
        *out_samples = samples;
        return Succeed();
    });
}

int32_t te_viewshed(te_tile tile, const te_viewshed_query* query, te_progress_callback progress, void* user_data, te_viewshed_grid* out_grid, uint8_t** out_cells)
{
    return Guarded([&]() -> int32_t {
        if (query == nullptr || out_grid == nullptr || out_cells == nullptr)
            return Fail(TE_ERROR_INVALID_ARGUMENT, "query, out_grid and out_cells must not be NULL.");
        *out_grid = te_viewshed_grid{};
        *out_cells = nullptr;

        std::shared_ptr<Tile> found = FindTile(tile);
        if (!found) return Fail(TE_ERROR_INVALID_HANDLE, InvalidHandleMessage);

        const te_viewshed_query& q = *query;
        std::string problem = PointProblem("observer", q.observer_latitude_deg, q.observer_longitude_deg);
        if (problem.empty() && std::abs(q.observer_latitude_deg) > 89.9)
            problem = "A viewshed can't be laid out within 0.1 degree of a pole, where lines of longitude meet. Choose an observer latitude between -89.9 and 89.9 degrees.";
        if (problem.empty()) problem = HeightProblem("observer", q.observer_height_above_ground_m);
        if (problem.empty()) problem = HeightProblem("target", q.target_height_above_ground_m);
        if (problem.empty()) problem = PositiveProblem("radius", q.radius_km, " km");
        if (problem.empty()) problem = PositiveProblem("spacing", q.spacing_m, " m");
        if (problem.empty()) problem = PositiveProblem("refraction factor k", q.refraction_k, "");
        if (problem.empty()) problem = InterpolationProblem(q.interpolation);
        if (problem.empty() && q.algorithm != TE_ALGORITHM_FAST && q.algorithm != TE_ALGORITHM_NAIVE)
            problem = "The viewshed algorithm must be fast or naive.";
        if (!problem.empty()) return Fail(TE_ERROR_INVALID_ARGUMENT, problem);

        problem = OutsideTileProblem("observer", q.observer_latitude_deg, q.observer_longitude_deg, found->nearest);
        if (!problem.empty()) return Fail(TE_ERROR_OUTSIDE_TILE, problem);

        // The same arithmetic the command-line benchmark uses to size a viewshed grid.
        double metersPerDegree = MetersPerDegree();
        double radiusDeg = (q.radius_km * 1000.0) / metersPerDegree;
        double spacingDeg = q.spacing_m / metersPerDegree;
        double cellsAcross = 2 * radiusDeg / spacingDeg;
        if (!(cellsAcross >= 1.0))
            return Fail(TE_ERROR_INVALID_ARGUMENT, "The radius is smaller than half a cell. Use a radius of at least " + Number(q.spacing_m / 2000.0) + " km, or a smaller spacing.");
        if (cellsAcross > TE_MAX_VIEWSHED_CELLS_PER_SIDE)
            return Fail(TE_ERROR_INVALID_ARGUMENT, "That grid would be more than " + std::to_string(TE_MAX_VIEWSHED_CELLS_PER_SIDE)
                + " cells across. Use a spacing of at least " + Number(2000.0 * q.radius_km / TE_MAX_VIEWSHED_CELLS_PER_SIDE) + " m, or a smaller radius.");
        int gridSize = (int)cellsAcross;

        ViewshedProgress report;
        if (progress != nullptr)
        {
            report = [progress, user_data](double fractionDone) { return progress(fractionDone, user_data) == 0; };
        }

        GeoPoint observer{ q.observer_latitude_deg, q.observer_longitude_deg };
        DatumHeight observerHeight{ q.observer_height_above_ground_m, VerticalDatum::HeightAboveGround };
        DatumHeight targetHeight{ q.target_height_above_ground_m, VerticalDatum::HeightAboveGround };
        RealElevationSampler& sampler = found->Sampler(q.interpolation);
        ViewshedResult viewshed = q.algorithm == TE_ALGORITHM_NAIVE
            ? ComputeViewshedNaive(observer, observerHeight, gridSize, gridSize, spacingDeg, sampler, q.refraction_k, report, targetHeight)
            : ComputeViewshedFast(observer, observerHeight, gridSize, gridSize, spacingDeg, sampler, q.refraction_k, report, targetHeight);

        if (viewshed.cancelled) return Fail(TE_ERROR_CANCELLED, "The viewshed was cancelled.");

        // The checks above leave one refusal to the engine: a grid whose rows would reach
        // a pole, which depends on the observer, the radius and the spacing together.
        if (viewshed.inputProblem == InputProblem::GridBeyondPole)
            return Fail(TE_ERROR_INVALID_ARGUMENT, "That viewshed would reach the pole, where lines of longitude meet. Use a smaller radius, or an observer further from the pole.");
        if (viewshed.inputProblem != InputProblem::None)
            return Fail(TE_ERROR_INVALID_ARGUMENT, std::string("The viewshed can't be computed: ") + InputProblemToString(viewshed.inputProblem) + ".");

        int center = gridSize / 2;
        double colStepDeg = LongitudeSpacingForLatitude(spacingDeg, observer.latitudeDeg);
        uint8_t* cells = AllocateArray<uint8_t>((size_t)gridSize * (size_t)gridSize);
        for (int row = 0; row < gridSize; row++)
        {
            for (int col = 0; col < gridSize; col++)
            {
                cells[(size_t)row * gridSize + col] = (uint8_t)viewshed.visible[row][col];
            }
        }

        te_viewshed_grid grid{};
        grid.rows = gridSize;
        grid.cols = gridSize;
        grid.observer_row = center;
        grid.observer_col = center;
        grid.spacing_deg = spacingDeg;
        grid.col_step_deg = colStepDeg;
        grid.south_west_cell_latitude_deg = observer.latitudeDeg + (0 - center) * spacingDeg;
        grid.south_west_cell_longitude_deg = observer.longitudeDeg + (0 - center) * colStepDeg;

        *out_grid = grid;
        *out_cells = cells;
        return Succeed();
    });
}
