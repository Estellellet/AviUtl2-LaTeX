#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include "LatexRenderer.h"

inline constexpr std::uint32_t kMaximumDecodedImageWidth = 16384;
inline constexpr std::uint32_t kMaximumDecodedImageHeight = 8192;
inline constexpr std::uint64_t kMaximumDecodedPixelCount = 33554432ULL;

struct ImageProcessingStats {
    unsigned int source_width = 0;
    unsigned int source_height = 0;
    std::uint64_t nonzero_alpha_pixels = 0;
    unsigned int maximum_alpha = 0;
    unsigned int left = 0;
    unsigned int top = 0;
    unsigned int right = 0;
    unsigned int bottom = 0;
    bool has_cropped_rectangle = false;
    std::string failure_stage;
};

bool load_and_process_png(
    const std::filesystem::path& path,
    RenderedImage& image,
    ImageProcessingStats& stats,
    bool crop_to_content = true,
    unsigned char alpha_threshold = 0);
