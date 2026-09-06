#pragma once
#include <vector>
#include "IElevationSampler.h"
#include <cmath>
#include <optional>


struct GeoPoint
{
    double latitude = 0.0;
    double longitude = 0.0;
};

struct ProfileSample
{
    GeoPoint point;
    std::optional<double> elevation;
    double distanceFromStartM = 0.0;
};

inline std::vector<ProfileSample> GetTerrainProfile(GeoPoint a, GeoPoint b, double spacing, IElevationSampler& sampler)
{
    std::vector<ProfileSample> result;

    const double metersPerDegreeLat = 111320.0;
    const double degToRad = 3.14159265358979323846 / 180.0;
    double midLatRad = ((a.latitude + b.latitude) / 2.0) * degToRad;
    double cosMidLat = cos(midLatRad);

    double totalDistance = sqrt(pow(b.latitude - a.latitude, 2) + pow(b.longitude - a.longitude, 2));

    int sampleCount = (int)(totalDistance / spacing);
    if (sampleCount < 1) sampleCount = 1;

    for (int i = 0; i <= sampleCount; i++)
    {
        double t = (double)i / sampleCount;

        GeoPoint current;
        current.latitude = a.latitude + t * (b.latitude - a.latitude);
        current.longitude = a.longitude + t * (b.longitude - a.longitude);

        double dNorthM = (current.latitude - a.latitude) * metersPerDegreeLat;
        double dEastM = (current.longitude - a.longitude) * metersPerDegreeLat * cosMidLat;
        double distanceFromStartM = sqrt(dNorthM * dNorthM + dEastM * dEastM);

        ProfileSample sample;
        sample.point = current;
        sample.elevation = sampler.GetElevation(current.latitude, current.longitude);
        sample.distanceFromStartM = distanceFromStartM;

        result.push_back(sample);
    }

    return result;
}