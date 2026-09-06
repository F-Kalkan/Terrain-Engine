#pragma once
#include <vector>
#include <fstream>
#include <string>
#include "TerrainProfile.h"
#include "Viewshed.h"

inline void WriteViewshedPGM(const ViewshedResult& viewshed, std::string filePath)
{
    int rows = (int)viewshed.visible.size();
    int cols = (int)viewshed.visible[0].size();

    std::ofstream file(filePath, std::ios::binary);
    file << "P5\n" << cols << " " << rows << "\n255\n";

    for (int row = 0; row < rows; row++)
    {
        for (int col = 0; col < cols; col++)
        {
            unsigned char pixel;
            switch (viewshed.visible[row][col])
            {
            case CellVisibility::Visible:
                pixel = 255;
                break;
            case CellVisibility::NotVisible:
                pixel = 0;
                break;
            default: // Degraded or NotCovered -- both render as "unknown"
                pixel = 128;
                break;
            }
            file.write((char*)&pixel, 1);
        }
    }
}

inline void WriteProfilePGM(const std::vector<ProfileSample>& profile, std::string filePath, int imageHeight = 200)
{
    int width = (int)profile.size();

    double minElev = 1e18;
    double maxElev = -1e18;
    for (const auto& sample : profile)
    {
        if (sample.elevation.has_value())
        {
            if (*sample.elevation < minElev) minElev = *sample.elevation;
            if (*sample.elevation > maxElev) maxElev = *sample.elevation;
        }
    }

    double range = maxElev - minElev;
    if (range == 0) range = 1;

    std::vector<unsigned char> pixels(width * imageHeight, 255);

    for (int x = 0; x < width; x++)
    {
        if (!profile[x].elevation.has_value()) continue;

        double normalized = (*profile[x].elevation - minElev) / range;
        int y = imageHeight - 1 - (int)(normalized * (imageHeight - 1));

        if (y < 0) y = 0;
        if (y >= imageHeight) y = imageHeight - 1;

        pixels[y * width + x] = 0;
    }

    std::ofstream file(filePath, std::ios::binary);
    file << "P5\n" << width << " " << imageHeight << "\n255\n";
    file.write((char*)pixels.data(), pixels.size());
}