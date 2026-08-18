#include "ImageLoader.h"

#include <windows.h>
#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <new>
#include <vector>

namespace {

template<typename T>
void release_com(T*& value) {
    if (value != nullptr) {
        value->Release();
        value = nullptr;
    }
}

} // namespace

bool load_and_process_png(
    const std::filesystem::path& path,
    RenderedImage& image,
    ImageProcessingStats& stats,
    bool crop_to_content,
    unsigned char alpha_threshold) {
    image = {};
    stats = {};
    stats.failure_stage = "com_initialization";
    const HRESULT initialize_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitialize = SUCCEEDED(initialize_result);
    if (FAILED(initialize_result) && initialize_result != RPC_E_CHANGED_MODE) {
        return false;
    }

    IWICImagingFactory* factory = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;

    bool succeeded = false;
    try {
        do {
        stats.failure_stage = "wic_factory_creation";
        if (FAILED(CoCreateInstance(
                CLSID_WICImagingFactory,
                nullptr,
                CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(&factory)))) {
            break;
        }
        stats.failure_stage = "wic_png_decoder_creation";
        if (FAILED(factory->CreateDecoderFromFilename(
                path.c_str(),
                nullptr,
                GENERIC_READ,
                WICDecodeMetadataCacheOnLoad,
                &decoder))) {
            break;
        }
        stats.failure_stage = "wic_first_frame_decode";
        if (FAILED(decoder->GetFrame(0, &frame))) {
            break;
        }
        stats.failure_stage = "wic_format_converter_creation";
        if (FAILED(factory->CreateFormatConverter(&converter))) {
            break;
        }
        stats.failure_stage = "wic_rgba_conversion";
        if (FAILED(converter->Initialize(
                frame,
                GUID_WICPixelFormat32bppRGBA,
                WICBitmapDitherTypeNone,
                nullptr,
                0.0,
                WICBitmapPaletteTypeCustom))) {
            break;
        }

        UINT width = 0;
        UINT height = 0;
        stats.failure_stage = "decoded_image_size";
        if (FAILED(converter->GetSize(&width, &height)) || width == 0 || height == 0) {
            break;
        }
        stats.source_width = width;
        stats.source_height = height;
        stats.failure_stage = "decoded_image_size_limit";
        if (width > kMaximumDecodedImageWidth ||
            height > kMaximumDecodedImageHeight ||
            static_cast<std::uint64_t>(width) * height > kMaximumDecodedPixelCount) {
            break;
        }

        const UINT stride = width * 4;
        std::vector<BYTE> rgba(static_cast<std::size_t>(stride) * height);
        stats.failure_stage = "wic_pixel_copy";
        if (FAILED(converter->CopyPixels(
                nullptr,
                stride,
                static_cast<UINT>(rgba.size()),
                rgba.data()))) {
            break;
        }

        UINT min_x = width;
        UINT min_y = height;
        UINT max_x = 0;
        UINT max_y = 0;
        bool has_content = false;
        std::vector<unsigned char> alpha(static_cast<std::size_t>(width) * height);

        for (UINT y = 0; y < height; ++y) {
            for (UINT x = 0; x < width; ++x) {
                const std::size_t pixel_index = static_cast<std::size_t>(y) * width + x;
                const std::size_t byte_index = pixel_index * 4;
                const double luminance =
                    0.2126 * rgba[byte_index + 0] +
                    0.7152 * rgba[byte_index + 1] +
                    0.0722 * rgba[byte_index + 2];
                auto value = static_cast<unsigned char>(std::clamp(
                    std::lround(255.0 - luminance),
                    0L,
                    255L));
                if (value <= alpha_threshold) {
                    value = 0;
                }
                alpha[pixel_index] = value;
                stats.maximum_alpha = (std::max)(
                    stats.maximum_alpha,
                    static_cast<unsigned int>(value));
                if (value > 0) {
                    ++stats.nonzero_alpha_pixels;
                    has_content = true;
                    min_x = (std::min)(min_x, x);
                    min_y = (std::min)(min_y, y);
                    max_x = (std::max)(max_x, x);
                    max_y = (std::max)(max_y, y);
                }
            }
        }

        stats.failure_stage = "alpha_conversion";
        if (!has_content) {
            break;
        }

        stats.left = min_x;
        stats.top = min_y;
        stats.right = max_x;
        stats.bottom = max_y;
        stats.has_cropped_rectangle = true;

        if (!crop_to_content) {
            image.width = static_cast<int>(width);
            image.height = static_cast<int>(height);
            image.pixels.resize(static_cast<std::size_t>(width) * height);
            for (std::size_t index = 0; index < alpha.size(); ++index) {
                image.pixels[index] = PIXEL_RGBA{ 255, 255, 255, alpha[index] };
            }
            stats.failure_stage.clear();
            succeeded = true;
            break;
        }

        constexpr UINT padding = 8;
        const UINT content_width = max_x - min_x + 1;
        const UINT content_height = max_y - min_y + 1;
        image.width = static_cast<int>(content_width + padding * 2);
        image.height = static_cast<int>(content_height + padding * 2);
        image.pixels.assign(
            static_cast<std::size_t>(image.width) * image.height,
            PIXEL_RGBA{ 255, 255, 255, 0 });

        for (UINT y = 0; y < content_height; ++y) {
            for (UINT x = 0; x < content_width; ++x) {
                const auto source_index =
                    static_cast<std::size_t>(min_y + y) * width + min_x + x;
                const auto destination_index =
                    static_cast<std::size_t>(y + padding) * image.width + x + padding;
                image.pixels[destination_index] = PIXEL_RGBA{ 255, 255, 255, alpha[source_index] };
            }
        }
        stats.failure_stage.clear();
            succeeded = true;
        } while (false);
    } catch (const std::bad_alloc&) {
        image = {};
        succeeded = false;
        try {
            stats.failure_stage = "image_memory_allocation";
        } catch (...) {
            // Keep COM cleanup deterministic even if the diagnostic string cannot allocate.
        }
    }

    release_com(converter);
    release_com(frame);
    release_com(decoder);
    release_com(factory);
    if (uninitialize) {
        CoUninitialize();
    }
    return succeeded;
}
