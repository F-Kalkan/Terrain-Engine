#pragma once
#include <vector>
#include <fstream>
#include <string>
#include "TerrainProfile.h"
#include "Viewshed.h"

// Complexity: O(rows * cols). Thread-safety: single-thread-only (writes to one
// std::ofstream); not a per-frame call.
inline void WriteViewshedPGM(const ViewshedResult& viewshed, std::string filePath)
{
    int rows = (int)viewshed.visible.size();
    int cols = rows > 0 ? (int)viewshed.visible[0].size() : 0;

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
            case CellVisibility::DataNotGiven: // past the data given -- a lighter grey than a void
                pixel = 192;
                break;
            default: // Degraded or NotCovered -- both render as "unknown"
                pixel = 128;
                break;
            }
            file.write((char*)&pixel, 1);
        }
    }
}

// Complexity: O(profile.size() + width * imageHeight). Thread-safety:
// single-thread-only (writes to one std::ofstream); not a per-frame call.
inline void WriteProfilePGM(const std::vector<ProfileSample>& profile, std::string filePath, int imageHeight = 200)
{
    int width = (int)profile.size();

    double minElevM = 1e18;
    double maxElevM = -1e18;
    for (const auto& sample : profile)
    {
        if (sample.elevationM.has_value())
        {
            if (*sample.elevationM < minElevM) minElevM = *sample.elevationM;
            if (*sample.elevationM > maxElevM) maxElevM = *sample.elevationM;
        }
    }

    double rangeM = maxElevM - minElevM;
    if (rangeM == 0) rangeM = 1;

    std::vector<unsigned char> pixels(width * imageHeight, 255);

    for (int x = 0; x < width; x++)
    {
        if (!profile[x].elevationM.has_value()) continue;

        double normalized = (*profile[x].elevationM - minElevM) / rangeM;
        int y = imageHeight - 1 - (int)(normalized * (imageHeight - 1));

        if (y < 0) y = 0;
        if (y >= imageHeight) y = imageHeight - 1;

        pixels[y * width + x] = 0;
    }

    std::ofstream file(filePath, std::ios::binary);
    file << "P5\n" << width << " " << imageHeight << "\n255\n";
    file.write((char*)pixels.data(), pixels.size());
}