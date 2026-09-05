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
};

inline std::vector<ProfileSample> GetTerrainProfile(GeoPoint a, GeoPoint b, double spacing, IElevationSampler& sampler)
{
    std::vector<ProfileSample> result;

    double totalDistance = sqrt(pow(b.latitude - a.latitude, 2) + pow(b.longitude - a.longitude, 2));

    int sampleCount = (int)(totalDistance / spacing);
    if (sampleCount < 1) sampleCount = 1;

    for (int i = 0; i <= sampleCount; i++)
    {
        double t = (double)i / sampleCount;

        GeoPoint current;
        current.latitude = a.latitude + t * (b.latitude - a.latitude);
        current.longitude = a.longitude + t * (b.longitude - a.longitude);

        ProfileSample sample;   
        sample.point = current;
        sample.elevation = sampler.GetElevation(current.latitude, current.longitude);

        result.push_back(sample);
    }

    return result;
}