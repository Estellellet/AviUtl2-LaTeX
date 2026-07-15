#pragma once

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "filter2.h"

struct RenderedImage {
    int width = 0;
    int height = 0;
    std::vector<PIXEL_RGBA> pixels;
};

struct RenderTools {
    std::filesystem::path lualatex;
    std::filesystem::path mutool;
};

bool render_latex(
    const std::wstring& document_source,
    RenderedImage& image,
    const std::string& cache_version,
    int render_dpi,
    const RenderTools& tools,
    bool crop_to_content = true,
    unsigned char alpha_threshold = 0,
    std::uint64_t* nonzero_alpha_pixels = nullptr,
    bool* image_decoded = nullptr,
    bool* disk_cache_used = nullptr);

void append_latest_log(const std::string& message);
void reset_latest_log(const std::string& message);
std::wstring make_render_cache_key(
    const std::wstring& document_source,
    const std::string& cache_version,
    int render_dpi);
