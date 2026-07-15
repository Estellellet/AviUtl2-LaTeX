#include "PersistentRenderCache.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <new>
#include <sstream>
#include <system_error>
#include <utility>

#include "ImageLoader.h"

namespace {

constexpr std::size_t kMaximumPersistentStepCount = 32;
constexpr std::uint64_t kMaximumPersistentResidentPixelCount =
    kMaximumDecodedPixelCount * 4ULL;
constexpr char kPersistentImageFormat[] = "alpha8";
constexpr char kPersistentMaskFormat[] = "monochrome-alpha-mask-v1";
constexpr char kManifestFileName[] = "manifest.json";

struct LayerManifest {
    std::string file_name;
    int width = 0;
    int height = 0;
    std::uint64_t byte_count = 0;
    std::uint64_t nonzero_alpha_pixels = 0;
    std::string checksum;
};

struct CacheManifest {
    std::string cache_version;
    std::string cache_key;
    std::string template_name;
    int render_dpi = 0;
    std::size_t step_count = 0;
    int canvas_width = 0;
    int canvas_height = 0;
    std::string image_format;
    std::string mask_format;
    std::vector<LayerManifest> layers;
};

void set_status(
    PersistentRenderCacheStatus value,
    PersistentRenderCacheStatus* output) {
    if (output != nullptr) {
        *output = value;
    }
}

std::string to_utf8(std::wstring_view value) {
    if (value.empty()) {
        return {};
    }
    const int count = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (count <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(count), '\0');
    if (WideCharToMultiByte(
            CP_UTF8,
            WC_ERR_INVALID_CHARS,
            value.data(),
            static_cast<int>(value.size()),
            result.data(),
            count,
            nullptr,
            nullptr) != count) {
        return {};
    }
    return result;
}

std::uint64_t fnv1a_bytes(std::span<const unsigned char> bytes) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char byte : bytes) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::uint64_t fnv1a_string(std::string_view value) {
    return fnv1a_bytes(std::span(
        reinterpret_cast<const unsigned char*>(value.data()), value.size()));
}

std::string hexadecimal(std::uint64_t value) {
    std::ostringstream output;
    output << std::hex << std::nouppercase << std::setw(16)
           << std::setfill('0') << value;
    return output.str();
}

std::wstring hexadecimal_wide(std::uint64_t value) {
    const std::string narrow = hexadecimal(value);
    return std::wstring(narrow.begin(), narrow.end());
}

std::string json_escape(std::string_view value) {
    std::string output;
    output.reserve(value.size());
    constexpr char digits[] = "0123456789abcdef";
    for (const unsigned char value_byte : value) {
        switch (value_byte) {
        case '"': output += "\\\""; break;
        case '\\': output += "\\\\"; break;
        case '\b': output += "\\b"; break;
        case '\f': output += "\\f"; break;
        case '\n': output += "\\n"; break;
        case '\r': output += "\\r"; break;
        case '\t': output += "\\t"; break;
        default:
            if (value_byte < 0x20) {
                output += "\\u00";
                output.push_back(digits[value_byte >> 4]);
                output.push_back(digits[value_byte & 0x0f]);
            } else {
                output.push_back(static_cast<char>(value_byte));
            }
            break;
        }
    }
    return output;
}

class ManifestReader {
public:
    explicit ManifestReader(std::string_view input) : input_(input) {}

    bool read(CacheManifest& manifest) {
        if (!symbol('{') || !named_string("cache_version", manifest.cache_version) ||
            !comma() || !named_string("cache_key", manifest.cache_key) ||
            !comma() || !named_string("template", manifest.template_name) ||
            !comma() || !named_int("render_dpi", manifest.render_dpi) ||
            !comma() || !named_size("step_count", manifest.step_count) ||
            !comma() || !named_int("canvas_width", manifest.canvas_width) ||
            !comma() || !named_int("canvas_height", manifest.canvas_height) ||
            !comma() || !named_string("image_format", manifest.image_format) ||
            !comma() || !named_string("mask_format", manifest.mask_format) ||
            !comma() || !key("step_layers") || !symbol('[')) {
            return false;
        }
        manifest.layers.clear();
        skip_space();
        if (!peek(']')) {
            for (;;) {
                LayerManifest layer;
                if (!read_layer(layer)) {
                    return false;
                }
                manifest.layers.push_back(std::move(layer));
                skip_space();
                if (peek(']')) {
                    break;
                }
                if (!comma()) {
                    return false;
                }
            }
        }
        if (!symbol(']') || !symbol('}')) {
            return false;
        }
        skip_space();
        return position_ == input_.size();
    }

private:
    void skip_space() {
        while (position_ < input_.size() &&
               std::isspace(static_cast<unsigned char>(input_[position_])) != 0) {
            ++position_;
        }
    }

    bool peek(char value) {
        skip_space();
        return position_ < input_.size() && input_[position_] == value;
    }

    bool symbol(char value) {
        if (!peek(value)) {
            return false;
        }
        ++position_;
        return true;
    }

    bool comma() { return symbol(','); }

    bool string(std::string& output) {
        skip_space();
        if (position_ >= input_.size() || input_[position_] != '"') {
            return false;
        }
        ++position_;
        output.clear();
        while (position_ < input_.size()) {
            const unsigned char value =
                static_cast<unsigned char>(input_[position_++]);
            if (value == '"') {
                return true;
            }
            if (value < 0x20) {
                return false;
            }
            if (value != '\\') {
                output.push_back(static_cast<char>(value));
                continue;
            }
            if (position_ >= input_.size()) {
                return false;
            }
            const char escaped = input_[position_++];
            switch (escaped) {
            case '"': output.push_back('"'); break;
            case '\\': output.push_back('\\'); break;
            case '/': output.push_back('/'); break;
            case 'b': output.push_back('\b'); break;
            case 'f': output.push_back('\f'); break;
            case 'n': output.push_back('\n'); break;
            case 'r': output.push_back('\r'); break;
            case 't': output.push_back('\t'); break;
            default:
                // This writer emits UTF-8 directly and uses \u only for ASCII
                // control bytes.  Reject other escape forms rather than trying
                // to normalize attacker-controlled paths.
                return false;
            }
        }
        return false;
    }

    bool key(std::string_view expected) {
        std::string actual;
        return string(actual) && actual == expected && symbol(':');
    }

    bool named_string(std::string_view name, std::string& output) {
        return key(name) && string(output);
    }

    bool unsigned_number(std::uint64_t& output) {
        skip_space();
        const std::size_t start = position_;
        while (position_ < input_.size() &&
               input_[position_] >= '0' && input_[position_] <= '9') {
            ++position_;
        }
        if (position_ == start) {
            return false;
        }
        const auto result = std::from_chars(
            input_.data() + start, input_.data() + position_, output);
        return result.ec == std::errc{} && result.ptr == input_.data() + position_;
    }

    bool named_u64(std::string_view name, std::uint64_t& output) {
        return key(name) && unsigned_number(output);
    }

    bool named_size(std::string_view name, std::size_t& output) {
        std::uint64_t parsed = 0;
        if (!named_u64(name, parsed) ||
            parsed > (std::numeric_limits<std::size_t>::max)()) {
            return false;
        }
        output = static_cast<std::size_t>(parsed);
        return true;
    }

    bool named_int(std::string_view name, int& output) {
        std::uint64_t parsed = 0;
        if (!named_u64(name, parsed) ||
            parsed > static_cast<std::uint64_t>((std::numeric_limits<int>::max)())) {
            return false;
        }
        output = static_cast<int>(parsed);
        return true;
    }

    bool read_layer(LayerManifest& layer) {
        return symbol('{') &&
            named_string("file", layer.file_name) && comma() &&
            named_int("width", layer.width) && comma() &&
            named_int("height", layer.height) && comma() &&
            named_u64("byte_count", layer.byte_count) && comma() &&
            named_u64("nonzero_alpha_pixels", layer.nonzero_alpha_pixels) && comma() &&
            named_string("fnv1a64", layer.checksum) && symbol('}');
    }

    std::string_view input_;
    std::size_t position_ = 0;
};

std::string serialize_manifest(const CacheManifest& manifest) {
    std::ostringstream output;
    output << "{\n"
        << "  \"cache_version\": \"" << json_escape(manifest.cache_version) << "\",\n"
        << "  \"cache_key\": \"" << json_escape(manifest.cache_key) << "\",\n"
        << "  \"template\": \"" << json_escape(manifest.template_name) << "\",\n"
        << "  \"render_dpi\": " << manifest.render_dpi << ",\n"
        << "  \"step_count\": " << manifest.step_count << ",\n"
        << "  \"canvas_width\": " << manifest.canvas_width << ",\n"
        << "  \"canvas_height\": " << manifest.canvas_height << ",\n"
        << "  \"image_format\": \"" << kPersistentImageFormat << "\",\n"
        << "  \"mask_format\": \"" << kPersistentMaskFormat << "\",\n"
        << "  \"step_layers\": [\n";
    for (std::size_t index = 0; index < manifest.layers.size(); ++index) {
        const LayerManifest& layer = manifest.layers[index];
        output << "    {\"file\": \"" << json_escape(layer.file_name)
            << "\", \"width\": " << layer.width
            << ", \"height\": " << layer.height
            << ", \"byte_count\": " << layer.byte_count
            << ", \"nonzero_alpha_pixels\": " << layer.nonzero_alpha_pixels
            << ", \"fnv1a64\": \"" << layer.checksum << "\"}";
        if (index + 1 != manifest.layers.size()) {
            output << ',';
        }
        output << '\n';
    }
    output << "  ]\n}\n";
    return output.str();
}

bool has_reparse_point(const std::filesystem::path& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

bool safe_existing_directory(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::is_directory(path, error) && !error &&
        !has_reparse_point(path);
}

bool safe_regular_file(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::is_regular_file(path, error) && !error &&
        !std::filesystem::is_symlink(path, error) && !error &&
        !has_reparse_point(path);
}

bool valid_layer_file_name(std::string_view name) {
    if (name.size() < 12 || name.size() > 96 ||
        name.find("..") != std::string_view::npos) {
        return false;
    }
    for (const unsigned char value : name) {
        const bool ascii_alphanumeric =
            (value >= 'a' && value <= 'z') ||
            (value >= 'A' && value <= 'Z') ||
            (value >= '0' && value <= '9');
        if (!(ascii_alphanumeric || value == '-' || value == '_' || value == '.')) {
            return false;
        }
    }
    return name.starts_with("step-") && name.ends_with(".alpha");
}

bool within_root(
    const std::filesystem::path& root,
    const std::filesystem::path& candidate) {
    std::error_code root_error;
    std::error_code candidate_error;
    const std::filesystem::path normalized_root =
        std::filesystem::absolute(root, root_error).lexically_normal();
    const std::filesystem::path normalized_candidate =
        std::filesystem::absolute(candidate, candidate_error).lexically_normal();
    if (root_error || candidate_error) {
        return false;
    }
    auto root_iterator = normalized_root.begin();
    auto candidate_iterator = normalized_candidate.begin();
    for (; root_iterator != normalized_root.end();
         ++root_iterator, ++candidate_iterator) {
        if (candidate_iterator == normalized_candidate.end() ||
            _wcsicmp(root_iterator->c_str(), candidate_iterator->c_str()) != 0) {
            return false;
        }
    }
    return candidate_iterator != normalized_candidate.end();
}

std::filesystem::path persistent_root(const std::filesystem::path& cache_root) {
    return cache_root / L"persistent-render-cache-restore-v1";
}

bool prepare_cache_directory(
    const std::filesystem::path& cache_root,
    std::wstring_view key,
    bool create,
    std::filesystem::path& directory,
    PersistentRenderCacheStatus& status,
    std::string& error_message) {
    if (!is_valid_persistent_render_cache_key(key)) {
        status = PersistentRenderCacheStatus::InvalidKey;
        error_message = "restore key is not a 16-character hexadecimal hash";
        return false;
    }
    std::error_code error;
    const bool cache_root_exists = std::filesystem::exists(cache_root, error);
    if (error) {
        status = PersistentRenderCacheStatus::IoError;
        error_message = "failed to inspect the configured cache root";
        return false;
    }
    if (cache_root_exists && !safe_existing_directory(cache_root)) {
        status = PersistentRenderCacheStatus::UnsafePath;
        error_message = "configured cache root is not a safe directory";
        return false;
    }
    if (create) {
        if (!cache_root_exists) {
            std::filesystem::create_directories(cache_root, error);
            if (error || !safe_existing_directory(cache_root)) {
                status = PersistentRenderCacheStatus::IoError;
                error_message = "failed to create a safe configured cache root";
                return false;
            }
        }
    } else if (!cache_root_exists) {
        status = PersistentRenderCacheStatus::NotFound;
        error_message = "configured cache root does not exist";
        return false;
    }
    const std::filesystem::path root = persistent_root(cache_root);
    if (create) {
        error.clear();
        std::filesystem::create_directories(root, error);
        if (error) {
            status = PersistentRenderCacheStatus::IoError;
            error_message = "failed to create the persistent cache root";
            return false;
        }
    }
    if (!safe_existing_directory(root)) {
        status = create
            ? PersistentRenderCacheStatus::UnsafePath
            : PersistentRenderCacheStatus::NotFound;
        error_message = create
            ? "persistent cache root is not a safe directory"
            : "persistent cache root does not exist";
        return false;
    }
    directory = root / std::wstring(key);
    if (!within_root(root, directory)) {
        status = PersistentRenderCacheStatus::UnsafePath;
        error_message = "cache directory escapes the persistent cache root";
        return false;
    }
    if (create) {
        if (std::filesystem::exists(directory, error) && has_reparse_point(directory)) {
            status = PersistentRenderCacheStatus::UnsafePath;
            error_message = "cache key directory is a reparse point";
            return false;
        }
        error.clear();
        std::filesystem::create_directories(directory, error);
        if (error || !safe_existing_directory(directory)) {
            status = PersistentRenderCacheStatus::IoError;
            error_message = "failed to create a safe cache key directory";
            return false;
        }
    } else if (!safe_existing_directory(directory)) {
        status = PersistentRenderCacheStatus::NotFound;
        error_message = "persistent cache key directory does not exist";
        return false;
    }
    return true;
}

bool write_file_atomically(
    const std::filesystem::path& final_path,
    std::span<const unsigned char> bytes) {
    const std::filesystem::path temporary = final_path.wstring() + L".tmp";
    if (has_reparse_point(temporary) || has_reparse_point(final_path)) {
        return false;
    }
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            return false;
        }
        output.write(
            reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
        output.flush();
        if (!output.good()) {
            output.close();
            DeleteFileW(temporary.c_str());
            return false;
        }
    }
    if (!MoveFileExW(
            temporary.c_str(),
            final_path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}

bool write_string_atomically(
    const std::filesystem::path& final_path,
    const std::string& value) {
    return write_file_atomically(
        final_path,
        std::span(
            reinterpret_cast<const unsigned char*>(value.data()), value.size()));
}

bool read_file_limited(
    const std::filesystem::path& path,
    std::uint64_t maximum_size,
    std::vector<unsigned char>& bytes) {
    if (!safe_regular_file(path)) {
        return false;
    }
    std::error_code error;
    const std::uintmax_t size = std::filesystem::file_size(path, error);
    if (error || size > maximum_size ||
        size > (std::numeric_limits<std::size_t>::max)()) {
        return false;
    }
    bytes.resize(static_cast<std::size_t>(size));
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return false;
    }
    input.read(
        reinterpret_cast<char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    return input.good() ||
        (input.eof() && static_cast<std::size_t>(input.gcount()) == bytes.size());
}

bool validate_dimensions(
    int width,
    int height,
    std::size_t step_count,
    std::uint64_t& canvas_pixels,
    std::string& error_message) {
    if (width <= 0 || height <= 0 ||
        width > static_cast<int>(kMaximumDecodedImageWidth) ||
        height > static_cast<int>(kMaximumDecodedImageHeight)) {
        error_message = "persistent cache canvas dimensions exceed configured limits";
        return false;
    }
    canvas_pixels = static_cast<std::uint64_t>(width) *
        static_cast<std::uint64_t>(height);
    if (canvas_pixels > kMaximumDecodedPixelCount ||
        step_count > kMaximumPersistentStepCount || step_count == 0) {
        error_message = "persistent cache step count or canvas pixel count is invalid";
        return false;
    }
    const std::uint64_t resident_image_count =
        static_cast<std::uint64_t>(step_count) * 2ULL + 1ULL;
    if (canvas_pixels > kMaximumPersistentResidentPixelCount /
            resident_image_count) {
        error_message = "persistent cache total resident pixel count exceeds its limit";
        return false;
    }
    return true;
}

bool valid_render_dpi(int value) {
    return value == 600 || value == 1200 || value == 2400;
}

bool valid_utf8(std::string_view value) {
    std::size_t index = 0;
    while (index < value.size()) {
        const unsigned char first = static_cast<unsigned char>(value[index++]);
        if (first <= 0x7f) {
            continue;
        }
        std::uint32_t code_point = 0;
        std::size_t continuation_count = 0;
        std::uint32_t minimum = 0;
        if (first >= 0xc2 && first <= 0xdf) {
            code_point = first & 0x1f;
            continuation_count = 1;
            minimum = 0x80;
        } else if (first >= 0xe0 && first <= 0xef) {
            code_point = first & 0x0f;
            continuation_count = 2;
            minimum = 0x800;
        } else if (first >= 0xf0 && first <= 0xf4) {
            code_point = first & 0x07;
            continuation_count = 3;
            minimum = 0x10000;
        } else {
            return false;
        }
        if (continuation_count > value.size() - index) {
            return false;
        }
        for (std::size_t count = 0; count < continuation_count; ++count) {
            const unsigned char next =
                static_cast<unsigned char>(value[index++]);
            if ((next & 0xc0) != 0x80) {
                return false;
            }
            code_point = (code_point << 6) | (next & 0x3f);
        }
        if (code_point < minimum || code_point > 0x10ffff ||
            (code_point >= 0xd800 && code_point <= 0xdfff)) {
            return false;
        }
    }
    return true;
}

bool valid_template_name(std::string_view value) {
    if (value.empty() || value.size() > 128 || !valid_utf8(value)) {
        return false;
    }
    return std::none_of(value.begin(), value.end(), [](unsigned char byte) {
        return byte < 0x20 || byte == 0x7f;
    });
}

bool rebuild_cumulative_images(
    PersistentRenderCacheSnapshot& snapshot,
    std::string& error_message) {
    const std::uint64_t pixel_count64 =
        static_cast<std::uint64_t>(snapshot.canvas_width) *
        static_cast<std::uint64_t>(snapshot.canvas_height);
    const std::size_t pixel_count = static_cast<std::size_t>(pixel_count64);
    try {
        snapshot.transparent_image =
            std::make_shared<const RenderedImage>(RenderedImage{
                snapshot.canvas_width,
                snapshot.canvas_height,
                std::vector<PIXEL_RGBA>(
                    pixel_count, PIXEL_RGBA{ 0, 0, 0, 0 })
            });
        snapshot.cumulative_images.clear();
        snapshot.cumulative_images.reserve(snapshot.step_layers.size() + 1);
        snapshot.cumulative_images.push_back(snapshot.transparent_image);
        std::vector<PIXEL_RGBA> accumulated(
            pixel_count, PIXEL_RGBA{ 255, 255, 255, 0 });
        for (const auto& layer : snapshot.step_layers) {
            for (std::size_t index = 0; index < pixel_count; ++index) {
                accumulated[index].a = (std::max)(
                    accumulated[index].a, layer->pixels[index].a);
            }
            snapshot.cumulative_images.push_back(
                std::make_shared<const RenderedImage>(RenderedImage{
                    snapshot.canvas_width,
                    snapshot.canvas_height,
                    accumulated
                }));
        }
    } catch (const std::bad_alloc&) {
        error_message = "not enough memory to rebuild persistent cumulative images";
        return false;
    }
    return true;
}

} // namespace

std::wstring make_persistent_render_cache_key(
    std::wstring_view identity_material) {
    const std::string identity = to_utf8(identity_material);
    const std::string versioned = identity + "\n" + kPersistentRenderCacheVersion;
    return hexadecimal_wide(fnv1a_string(versioned));
}

bool is_valid_persistent_render_cache_key(std::wstring_view key) {
    if (key.size() != 16) {
        return false;
    }
    return std::all_of(key.begin(), key.end(), [](wchar_t value) {
        return (value >= L'0' && value <= L'9') ||
            (value >= L'a' && value <= L'f');
    });
}

const char* persistent_render_cache_status_name(
    PersistentRenderCacheStatus status) {
    switch (status) {
    case PersistentRenderCacheStatus::Success: return "success";
    case PersistentRenderCacheStatus::NotFound: return "not_found";
    case PersistentRenderCacheStatus::InvalidKey: return "invalid_key";
    case PersistentRenderCacheStatus::UnsafePath: return "unsafe_path";
    case PersistentRenderCacheStatus::InvalidManifest: return "invalid_manifest";
    case PersistentRenderCacheStatus::IoError: return "io_error";
    case PersistentRenderCacheStatus::ResourceLimit: return "resource_limit";
    case PersistentRenderCacheStatus::OutOfMemory: return "out_of_memory";
    }
    return "unknown";
}

bool save_persistent_render_cache(
    const std::filesystem::path& cache_root,
    const PersistentRenderCacheMetadata& metadata,
    std::span<const std::shared_ptr<const RenderedImage>> step_layers,
    std::string& error_message,
    PersistentRenderCacheStatus* status_output) {
    error_message.clear();
    PersistentRenderCacheStatus status = PersistentRenderCacheStatus::IoError;
    set_status(status, status_output);
    if (step_layers.empty() || step_layers.size() > kMaximumPersistentStepCount) {
        status = PersistentRenderCacheStatus::ResourceLimit;
        error_message = "persistent cache step count is out of range";
        set_status(status, status_output);
        return false;
    }
    if (!is_valid_persistent_render_cache_key(metadata.cache_key)) {
        status = PersistentRenderCacheStatus::InvalidKey;
        error_message = "persistent cache key is invalid";
        set_status(status, status_output);
        return false;
    }
    if (!valid_render_dpi(metadata.render_dpi) ||
        !valid_template_name(metadata.template_name)) {
        status = PersistentRenderCacheStatus::InvalidManifest;
        error_message = "persistent cache template or render DPI is invalid";
        set_status(status, status_output);
        return false;
    }
    const auto& first = step_layers.front();
    if (!first) {
        status = PersistentRenderCacheStatus::InvalidManifest;
        error_message = "persistent cache contains a null first layer";
        set_status(status, status_output);
        return false;
    }
    std::uint64_t canvas_pixels = 0;
    if (!validate_dimensions(
            first->width, first->height, step_layers.size(),
            canvas_pixels, error_message)) {
        status = PersistentRenderCacheStatus::ResourceLimit;
        set_status(status, status_output);
        return false;
    }

    std::filesystem::path directory;
    if (!prepare_cache_directory(
            cache_root, metadata.cache_key, true, directory,
            status, error_message)) {
        set_status(status, status_output);
        return false;
    }

    CacheManifest manifest;
    try {
        manifest.cache_version = kPersistentRenderCacheVersion;
        manifest.cache_key = to_utf8(metadata.cache_key);
        manifest.template_name = metadata.template_name;
        manifest.render_dpi = metadata.render_dpi;
        manifest.step_count = step_layers.size();
        manifest.canvas_width = first->width;
        manifest.canvas_height = first->height;
        manifest.image_format = kPersistentImageFormat;
        manifest.mask_format = kPersistentMaskFormat;
        manifest.layers.reserve(step_layers.size());
        for (std::size_t index = 0; index < step_layers.size(); ++index) {
            const auto& layer = step_layers[index];
            if (!layer || layer->width != first->width ||
                layer->height != first->height ||
                layer->pixels.size() != static_cast<std::size_t>(canvas_pixels)) {
                status = PersistentRenderCacheStatus::InvalidManifest;
                error_message = "persistent step layers do not share one exact canvas";
                set_status(status, status_output);
                return false;
            }
            std::vector<unsigned char> alpha(static_cast<std::size_t>(canvas_pixels));
            std::uint64_t nonzero = 0;
            for (std::size_t pixel = 0; pixel < alpha.size(); ++pixel) {
                alpha[pixel] = layer->pixels[pixel].a;
                if (alpha[pixel] != 0) {
                    ++nonzero;
                }
            }
            if (nonzero == 0) {
                status = PersistentRenderCacheStatus::InvalidManifest;
                error_message = "persistent cache step layer is completely transparent";
                set_status(status, status_output);
                return false;
            }
            const std::string checksum = hexadecimal(fnv1a_bytes(alpha));
            std::ostringstream file_name;
            file_name << "step-" << std::setw(2) << std::setfill('0')
                      << (index + 1) << '-' << checksum << ".alpha";
            const std::string file = file_name.str();
            const std::filesystem::path layer_path =
                directory / std::filesystem::path(file);
            if (!within_root(directory, layer_path) ||
                !write_file_atomically(layer_path, alpha)) {
                status = PersistentRenderCacheStatus::IoError;
                error_message = "failed to atomically write a persistent alpha layer";
                set_status(status, status_output);
                return false;
            }
            manifest.layers.push_back(LayerManifest{
                file,
                layer->width,
                layer->height,
                canvas_pixels,
                nonzero,
                checksum
            });
        }
    } catch (const std::bad_alloc&) {
        status = PersistentRenderCacheStatus::OutOfMemory;
        error_message = "not enough memory to serialize persistent alpha layers";
        set_status(status, status_output);
        return false;
    }

    // manifest.json is the commit marker.  It is written only after every
    // referenced immutable alpha file has been published successfully.
    std::string serialized_manifest;
    try {
        serialized_manifest = serialize_manifest(manifest);
    } catch (const std::bad_alloc&) {
        status = PersistentRenderCacheStatus::OutOfMemory;
        error_message = "not enough memory to serialize persistent manifest.json";
        set_status(status, status_output);
        return false;
    }
    if (!write_string_atomically(
            directory / std::filesystem::path(kManifestFileName),
            serialized_manifest)) {
        status = PersistentRenderCacheStatus::IoError;
        error_message = "failed to atomically publish persistent manifest.json";
        set_status(status, status_output);
        return false;
    }
    status = PersistentRenderCacheStatus::Success;
    set_status(status, status_output);
    return true;
}

bool load_persistent_render_cache(
    const std::filesystem::path& cache_root,
    std::wstring_view cache_key,
    PersistentRenderCacheSnapshot& snapshot,
    std::string& error_message,
    PersistentRenderCacheStatus* status_output) {
    snapshot = {};
    error_message.clear();
    PersistentRenderCacheStatus status = PersistentRenderCacheStatus::IoError;
    set_status(status, status_output);
    std::filesystem::path directory;
    if (!prepare_cache_directory(
            cache_root, cache_key, false, directory,
            status, error_message)) {
        set_status(status, status_output);
        return false;
    }
    const std::filesystem::path manifest_path =
        directory / std::filesystem::path(kManifestFileName);
    if (!within_root(directory, manifest_path) || !safe_regular_file(manifest_path)) {
        status = PersistentRenderCacheStatus::NotFound;
        error_message = "persistent manifest.json does not exist";
        set_status(status, status_output);
        return false;
    }
    CacheManifest manifest;
    try {
        std::vector<unsigned char> manifest_bytes;
        constexpr std::uint64_t kMaximumManifestSize = 256ULL * 1024ULL;
        if (!read_file_limited(manifest_path, kMaximumManifestSize, manifest_bytes)) {
            status = PersistentRenderCacheStatus::InvalidManifest;
            error_message = "persistent manifest.json is unavailable or too large";
            set_status(status, status_output);
            return false;
        }
        const std::string manifest_text(
            reinterpret_cast<const char*>(manifest_bytes.data()), manifest_bytes.size());
        if (!ManifestReader(manifest_text).read(manifest)) {
            status = PersistentRenderCacheStatus::InvalidManifest;
            error_message = "persistent manifest.json is not valid JSON for this cache version";
            set_status(status, status_output);
            return false;
        }
    } catch (const std::bad_alloc&) {
        status = PersistentRenderCacheStatus::OutOfMemory;
        error_message = "not enough memory to read persistent manifest.json";
        set_status(status, status_output);
        return false;
    }
    if (
        manifest.cache_version != kPersistentRenderCacheVersion ||
        manifest.cache_key != to_utf8(cache_key) ||
        !valid_render_dpi(manifest.render_dpi) ||
        !valid_template_name(manifest.template_name) ||
        manifest.image_format != kPersistentImageFormat ||
        manifest.mask_format != kPersistentMaskFormat ||
        manifest.step_count != manifest.layers.size()) {
        status = PersistentRenderCacheStatus::InvalidManifest;
        error_message = "persistent manifest fields are invalid or unsupported";
        set_status(status, status_output);
        return false;
    }
    std::uint64_t canvas_pixels = 0;
    if (!validate_dimensions(
            manifest.canvas_width,
            manifest.canvas_height,
            manifest.step_count,
            canvas_pixels,
            error_message)) {
        status = PersistentRenderCacheStatus::ResourceLimit;
        set_status(status, status_output);
        return false;
    }

    try {
        snapshot.metadata.cache_key = std::wstring(cache_key);
        snapshot.metadata.template_name = manifest.template_name;
        snapshot.metadata.render_dpi = manifest.render_dpi;
        snapshot.canvas_width = manifest.canvas_width;
        snapshot.canvas_height = manifest.canvas_height;
        snapshot.step_layers.clear();
        snapshot.step_layers.reserve(manifest.step_count);
        for (const LayerManifest& layer : manifest.layers) {
            if (!valid_layer_file_name(layer.file_name) ||
                layer.width != manifest.canvas_width ||
                layer.height != manifest.canvas_height ||
                layer.byte_count != canvas_pixels ||
                layer.checksum.size() != 16 ||
                layer.nonzero_alpha_pixels == 0 ||
                layer.nonzero_alpha_pixels > canvas_pixels) {
                status = PersistentRenderCacheStatus::InvalidManifest;
                error_message = "persistent step layer metadata is invalid";
                set_status(status, status_output);
                snapshot = {};
                return false;
            }
            const std::filesystem::path layer_path =
                directory / std::filesystem::path(layer.file_name);
            if (!within_root(directory, layer_path)) {
                status = PersistentRenderCacheStatus::UnsafePath;
                error_message = "persistent layer path escapes its key directory";
                set_status(status, status_output);
                snapshot = {};
                return false;
            }
            if (!safe_regular_file(layer_path)) {
                status = PersistentRenderCacheStatus::UnsafePath;
                error_message =
                    "persistent alpha layer is not a safe regular file";
                set_status(status, status_output);
                snapshot = {};
                return false;
            }
            std::vector<unsigned char> alpha;
            if (!read_file_limited(layer_path, canvas_pixels, alpha) ||
                alpha.size() != canvas_pixels ||
                hexadecimal(fnv1a_bytes(alpha)) != layer.checksum) {
                status = PersistentRenderCacheStatus::InvalidManifest;
                error_message = "persistent alpha layer is missing, truncated, or corrupt";
                set_status(status, status_output);
                snapshot = {};
                return false;
            }
            std::uint64_t nonzero = 0;
            RenderedImage restored{
                manifest.canvas_width,
                manifest.canvas_height,
                std::vector<PIXEL_RGBA>(
                    static_cast<std::size_t>(canvas_pixels),
                    PIXEL_RGBA{ 255, 255, 255, 0 })
            };
            for (std::size_t pixel = 0; pixel < alpha.size(); ++pixel) {
                restored.pixels[pixel].a = alpha[pixel];
                if (alpha[pixel] != 0) {
                    ++nonzero;
                }
            }
            if (nonzero != layer.nonzero_alpha_pixels) {
                status = PersistentRenderCacheStatus::InvalidManifest;
                error_message = "persistent alpha layer content count is inconsistent";
                set_status(status, status_output);
                snapshot = {};
                return false;
            }
            snapshot.step_layers.push_back(
                std::make_shared<const RenderedImage>(std::move(restored)));
        }
        if (!rebuild_cumulative_images(snapshot, error_message)) {
            status = PersistentRenderCacheStatus::OutOfMemory;
            set_status(status, status_output);
            snapshot = {};
            return false;
        }
    } catch (const std::bad_alloc&) {
        status = PersistentRenderCacheStatus::OutOfMemory;
        error_message = "not enough memory to restore persistent render cache";
        set_status(status, status_output);
        snapshot = {};
        return false;
    }
    status = PersistentRenderCacheStatus::Success;
    set_status(status, status_output);
    return true;
}
