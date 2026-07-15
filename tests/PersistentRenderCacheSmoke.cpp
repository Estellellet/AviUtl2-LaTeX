#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "PersistentRenderCache.h"

namespace {

std::shared_ptr<const RenderedImage> make_layer(
    int width,
    int height,
    std::initializer_list<std::size_t> visible) {
    RenderedImage value{
        width,
        height,
        std::vector<PIXEL_RGBA>(
            static_cast<std::size_t>(width) * static_cast<std::size_t>(height),
            PIXEL_RGBA{255, 255, 255, 0})
    };
    for (const std::size_t index : visible) {
        value.pixels.at(index).a = 255;
    }
    return std::make_shared<const RenderedImage>(std::move(value));
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) {
        return 2;
    }
    const std::filesystem::path root(argv[1]);
    const std::wstring key =
        make_persistent_render_cache_key(L"smoke-identity");
    const std::vector<std::shared_ptr<const RenderedImage>> layers{
        make_layer(8, 4, {1, 2}),
        make_layer(8, 4, {12, 13, 14})
    };
    std::string error;
    PersistentRenderCacheStatus status{};
    if (!save_persistent_render_cache(
            root,
            PersistentRenderCacheMetadata{key, "document", 1200},
            layers,
            error,
            &status)) {
        std::cerr << "save: " << persistent_render_cache_status_name(status)
                  << ": " << error << '\n';
        return 3;
    }

    PersistentRenderCacheSnapshot restored;
    if (!load_persistent_render_cache(
            root, key, restored, error, &status)) {
        std::cerr << "load: " << persistent_render_cache_status_name(status)
                  << ": " << error << '\n';
        return 4;
    }
    if (restored.step_layers.size() != 2 ||
        restored.cumulative_images.size() != 3 ||
        restored.canvas_width != 8 || restored.canvas_height != 4 ||
        restored.cumulative_images[1]->pixels[1].a != 255 ||
        restored.cumulative_images[1]->pixels[12].a != 0 ||
        restored.cumulative_images[2]->pixels[1].a != 255 ||
        restored.cumulative_images[2]->pixels[12].a != 255) {
        std::cerr << "restored image content mismatch\n";
        return 5;
    }

    PersistentRenderCacheSnapshot rejected;
    if (load_persistent_render_cache(
            root, L"..\\outside-cache", rejected, error, &status) ||
        status != PersistentRenderCacheStatus::InvalidKey) {
        std::cerr << "unsafe key was not rejected\n";
        return 6;
    }

    const std::filesystem::path key_directory =
        root / L"persistent-render-cache-restore-v1" / key;
    std::filesystem::path alpha_file;
    for (const auto& entry : std::filesystem::directory_iterator(key_directory)) {
        if (entry.path().extension() == L".alpha") {
            alpha_file = entry.path();
            break;
        }
    }
    if (alpha_file.empty()) {
        std::cerr << "alpha layer was not created\n";
        return 7;
    }
    {
        std::ofstream corrupt(alpha_file, std::ios::binary | std::ios::trunc);
        corrupt.put('\0');
    }
    if (load_persistent_render_cache(
            root, key, rejected, error, &status) ||
        status != PersistentRenderCacheStatus::InvalidManifest) {
        std::cerr << "corrupt alpha layer was not rejected\n";
        return 8;
    }
    std::string narrow_key;
    narrow_key.reserve(key.size());
    for (const wchar_t value : key) {
        narrow_key.push_back(static_cast<char>(value));
    }
    std::cout << "key=" << narrow_key
              << " steps=" << restored.step_layers.size()
              << " cumulative=" << restored.cumulative_images.size()
              << " status=success\n";
    return 0;
}
