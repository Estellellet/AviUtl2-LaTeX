#pragma once

#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "LatexRenderer.h"

inline constexpr char kPersistentRenderCacheVersion[] =
    "persistent-render-cache-restore-v1";

enum class PersistentRenderCacheStatus {
    Success,
    NotFound,
    InvalidKey,
    UnsafePath,
    InvalidManifest,
    IoError,
    ResourceLimit,
    OutOfMemory
};

struct PersistentRenderCacheMetadata {
    std::wstring cache_key;
    std::string template_name;
    int render_dpi = 0;
};

struct PersistentRenderCacheSnapshot {
    PersistentRenderCacheMetadata metadata;
    int canvas_width = 0;
    int canvas_height = 0;
    std::vector<std::shared_ptr<const RenderedImage>> step_layers;
    std::vector<std::shared_ptr<const RenderedImage>> cumulative_images;
    std::shared_ptr<const RenderedImage> transparent_image;
};

// The returned key is always exactly 16 lower-case hexadecimal characters.
// Callers should include every successful compile input in identity_material.
std::wstring make_persistent_render_cache_key(
    std::wstring_view identity_material);

bool is_valid_persistent_render_cache_key(std::wstring_view key);

const char* persistent_render_cache_status_name(
    PersistentRenderCacheStatus status);

// Writes all alpha layers before atomically publishing manifest.json.  A caller
// must update the object's saved restore key only after this function succeeds.
bool save_persistent_render_cache(
    const std::filesystem::path& cache_root,
    const PersistentRenderCacheMetadata& metadata,
    std::span<const std::shared_ptr<const RenderedImage>> step_layers,
    std::string& error_message,
    PersistentRenderCacheStatus* status = nullptr);

// Performs disk I/O and in-memory alpha composition only.  It never launches
// LuaLaTeX, MuPDF, font discovery, or environment diagnostics.
bool load_persistent_render_cache(
    const std::filesystem::path& cache_root,
    std::wstring_view cache_key,
    PersistentRenderCacheSnapshot& snapshot,
    std::string& error_message,
    PersistentRenderCacheStatus* status = nullptr);
