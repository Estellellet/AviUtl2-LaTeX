#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <iomanip>
#include <locale>
#include <memory>
#include <mutex>
#include <new>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "filter2.h"
#include "plugin2.h"
#include "AppPaths.h"
#include "LatexRenderer.h"
#include "PersistentRenderCache.h"
#include "PluginInfo.h"
#include "ToolSettings.h"
#include "UiDialogs.h"

bool func_proc_video(FILTER_PROC_VIDEO* video);
void request_compile(EDIT_SECTION* edit);
void request_environment_settings(EDIT_SECTION* edit);
void request_information(EDIT_SECTION* edit);
void request_font_selection(EDIT_SECTION* edit);
void request_font_file_selection(EDIT_SECTION* edit);
int resolve_japanese_font_mode(const char* serialized_value, int fallback);

namespace {

constexpr std::size_t kMaximumStepCount = 32;
constexpr char kTemplateCacheVersion[] =
    "tikzpicture-template-monochrome-v1";
constexpr char kAlignRelationSpacingCacheVersion[] =
    "align-relation-spacing-fix-v1";
constexpr unsigned char kHiddenLayerAlphaThreshold = 2;
constexpr unsigned char kContentAlphaThreshold = 1;
constexpr double kStepEpsilon = 1.0e-9;

enum class LatexTemplate : int {
    InlineMath = 0,
    AlignStar = 1,
    EquationStar = 2,
    Document = 3,
    TikzPicture = 4
};

enum class JapaneseFontMode : int {
    Default = 0,
    FontName = 1,
    FontFile = 2
};

enum class JapaneseSpacingMode : int {
    Auto = 0,
    Uniform = 1,
    FontMetrics = 2
};

enum class ParagraphAlignment : int {
    Left = 0,
    Justify = 1,
    Center = 2,
    Right = 3
};

enum class CompileSource {
    Generated,
    Cache
};

using ImagePointer = std::shared_ptr<const RenderedImage>;
using ImageList = std::vector<ImagePointer>;

struct ContentBounds {
    int left = 0;
    int top = 0;
    int right = -1;
    int bottom = -1;

    bool valid() const {
        return right >= left && bottom >= top;
    }
};

struct LayerCropStats {
    ContentBounds original_bounds;
    std::uint64_t nonzero_alpha_pixels = 0;
    int cropped_destination_x = 0;
    int cropped_destination_y = 0;
};

struct JapaneseDocumentConfiguration {
    bool enabled = false;
    JapaneseFontMode font_mode = JapaneseFontMode::Default;
    std::wstring font_name;
    std::wstring font_file;
    std::wstring normalized_font_directory;
    std::wstring normalized_font_filename;
    bool font_file_exists = false;
    std::uintmax_t font_file_size = 0;
    std::wstring font_file_last_write_time = L"not-applicable";
    JapaneseSpacingMode spacing_requested = JapaneseSpacingMode::Auto;
    JapaneseSpacingMode spacing_effective = JapaneseSpacingMode::Auto;
    bool spacing_options_applied = false;
    std::wstring spacing_jfm = L"not-specified";
    std::wstring spacing_kerning = L"not-specified";
    bool spacing_palt = false;
    bool spacing_kern_feature = false;
    std::wstring spacing_jfont_options;
    std::wstring generated_script = L"not-specified";
    std::wstring generated_raw_features = L"not-specified";
    std::wstring generated_setmainfont;
    std::wstring generated_setmainjfont;
    std::wstring preamble;
    std::wstring cache_material;
    std::wstring error_message;

    bool valid() const {
        return error_message.empty();
    }
};

struct DocumentLayoutConfiguration {
    double raw_minipage_width_cm = 0.0;
    double minipage_width_cm = 0.0;
    std::wstring formatted_minipage_width_cm = L"0.0";
    ParagraphAlignment paragraph_alignment = ParagraphAlignment::Left;

    bool minipage_enabled() const {
        return minipage_width_cm > 0.0;
    }
};

struct TikzConfiguration {
    std::string raw_libraries;
    std::string normalized_libraries;
    std::wstring preamble;
    std::wstring cache_material;
    std::string error_message;
    std::size_t library_count = 0;

    bool valid() const {
        return error_message.empty();
    }
};

struct EffectOutputBuffer {
    std::mutex mutex;
    RenderedImage image;
};

struct PersistentObjectData {
    // FILTER_ITEM_DATA is the SDK's non-displayed, per-object storage item.
    // Store only the fixed-size hash; never persist an absolute cache path.
    char last_successful_cache_key[17]{};
};

static_assert(sizeof(PersistentObjectData) <= 1024);

struct FontObjectData {
    static constexpr unsigned char kSchemaVersion = 1;
    // Zero distinguishes objects created before this hidden item existed.
    unsigned char schema_version = 0;
    unsigned char mode = 0; // 0=Default, 1=FontName, 2=FontFile
    unsigned char reserved[2]{};
    std::array<char, 192> fontspec_family_name{};
    std::array<char, 700> font_file_path{};
    std::array<char, 120> display_name{};
};

static_assert(sizeof(FontObjectData) <= 1024);

struct ObjectState {
    int layer = -1;
    int frame_start = -1;
    int frame_end = -1;
    int compiled_render_dpi = 0;
    std::shared_ptr<const ImageList> images;
    std::shared_ptr<const ImageList> step_layers;
    ImagePointer common_transparent;
    ImagePointer colored_source;
    ImagePointer colored_image;
    unsigned char colored_r = 0;
    unsigned char colored_g = 0;
    unsigned char colored_b = 0;
    std::shared_ptr<EffectOutputBuffer> effect_output =
        std::make_shared<EffectOutputBuffer>();
    std::unordered_map<std::wstring, ImagePointer> source_cache;
    bool restore_attempted = false;
    std::wstring restored_cache_key;
    // FILTER_ITEM_DATA writes are formally supported from filter processing
    // callbacks. A successful manual compile queues the new key here; the
    // next evaluation commits it for exactly this object before rendering.
    std::wstring pending_persistent_key;
    std::optional<FontObjectData> pending_font_data;
    PersistentRenderCacheStatus restore_result =
        PersistentRenderCacheStatus::NotFound;
    LastOperationInfo last_operation;
};

std::mutex state_mutex;
std::unordered_map<std::int64_t, ObjectState> object_states;
std::atomic<std::int64_t> last_object_id = -1;
EDIT_HANDLE* host_edit_handle = nullptr;

ImagePointer transparent_image() {
    static const auto image = std::make_shared<const RenderedImage>(RenderedImage{
        1,
        1,
        std::vector<PIXEL_RGBA>{ PIXEL_RGBA{ 0, 0, 0, 0 } }
    });
    return image;
}

std::wstring persistent_key_from_data(const PersistentObjectData* data) {
    if (data == nullptr || data->last_successful_cache_key[16] != '\0') {
        return {};
    }
    std::wstring key;
    key.reserve(16);
    for (std::size_t index = 0; index < 16; ++index) {
        const unsigned char value = static_cast<unsigned char>(
            data->last_successful_cache_key[index]);
        if (!((value >= '0' && value <= '9') ||
              (value >= 'a' && value <= 'f'))) {
            return {};
        }
        key.push_back(static_cast<wchar_t>(value));
    }
    return is_valid_persistent_render_cache_key(key) ? key : std::wstring{};
}

bool store_persistent_key(
    PersistentObjectData* data,
    std::wstring_view key) {
    if (data == nullptr || !is_valid_persistent_render_cache_key(key)) {
        return false;
    }
    std::fill_n(data->last_successful_cache_key, 17, '\0');
    for (std::size_t index = 0; index < key.size(); ++index) {
        data->last_successful_cache_key[index] =
            static_cast<char>(key[index]);
    }
    return true;
}

bool is_valid_cached_image(const ImagePointer& image) {
    if (!image || image->width <= 0 || image->height <= 0) {
        return false;
    }
    const std::size_t width = static_cast<std::size_t>(image->width);
    const std::size_t height = static_cast<std::size_t>(image->height);
    return width <= (std::numeric_limits<std::size_t>::max)() / height &&
        image->pixels.size() >= width * height;
}

ImagePointer create_colored_image(
    const ImagePointer& source,
    unsigned char red,
    unsigned char green,
    unsigned char blue) {
    if (!source) {
        return {};
    }
    try {
        RenderedImage colored{ source->width, source->height, source->pixels };
        for (auto& pixel : colored.pixels) {
            pixel.r = red;
            pixel.g = green;
            pixel.b = blue;
        }
        return std::make_shared<const RenderedImage>(std::move(colored));
    } catch (const std::bad_alloc&) {
        return {};
    }
}

bool prepare_effect_output(
    const ImagePointer& base,
    const ImagePointer& layer,
    int effect,
    int reveal_direction,
    double progress,
    unsigned char red,
    unsigned char green,
    unsigned char blue,
    RenderedImage& output) {
    if (!base || !layer || base->width <= 0 || base->height <= 0 ||
        base->width != layer->width || base->height != layer->height) {
        return false;
    }
    const std::size_t width = static_cast<std::size_t>(base->width);
    const std::size_t height = static_cast<std::size_t>(base->height);
    if (width > (std::numeric_limits<std::size_t>::max)() / height) {
        return false;
    }
    const std::size_t pixel_count = width * height;
    if (base->pixels.size() < pixel_count || layer->pixels.size() < pixel_count) {
        return false;
    }

    try {
        if (output.width != base->width || output.height != base->height ||
            output.pixels.size() != pixel_count) {
            output.width = base->width;
            output.height = base->height;
            output.pixels.resize(pixel_count);
        }
    } catch (const std::bad_alloc&) {
        return false;
    }

    progress = (std::clamp)(progress, 0.0, 1.0);
    if (effect == 1) {
        for (std::size_t index = 0; index < pixel_count; ++index) {
            const auto faded_layer_alpha = static_cast<unsigned char>((std::clamp)(
                std::lround(layer->pixels[index].a * progress), 0L, 255L));
            const unsigned char alpha = (std::max)(
                base->pixels[index].a, faded_layer_alpha);
            output.pixels[index] = PIXEL_RGBA{
                red, green, blue, alpha
            };
        }
        return true;
    }

    if (progress >= 1.0) {
        for (std::size_t index = 0; index < pixel_count; ++index) {
            output.pixels[index] = PIXEL_RGBA{
                red,
                green,
                blue,
                (std::max)(base->pixels[index].a, layer->pixels[index].a)
            };
        }
        return true;
    }

    int left = layer->width;
    int top = layer->height;
    int right = -1;
    int bottom = -1;
    for (int y = 0; y < layer->height; ++y) {
        for (int x = 0; x < layer->width; ++x) {
            const std::size_t index = static_cast<std::size_t>(y) * width + x;
            if (layer->pixels[index].a > kHiddenLayerAlphaThreshold) {
                left = (std::min)(left, x);
                top = (std::min)(top, y);
                right = (std::max)(right, x);
                bottom = (std::max)(bottom, y);
            }
        }
    }

    for (std::size_t index = 0; index < pixel_count; ++index) {
        output.pixels[index] = PIXEL_RGBA{
            red, green, blue, base->pixels[index].a
        };
    }
    if (right < left || bottom < top) {
        return true;
    }

    const int diff_width = right - left + 1;
    const int diff_height = bottom - top + 1;
    const int visible_width = static_cast<int>(std::floor(diff_width * progress));
    const int visible_height = static_cast<int>(std::floor(diff_height * progress));
    for (int y = top; y <= bottom; ++y) {
        for (int x = left; x <= right; ++x) {
            bool visible = false;
            switch (reveal_direction) {
            case 1:
                visible = x > right - visible_width;
                break;
            case 2:
                visible = y < top + visible_height;
                break;
            case 3:
                visible = y > bottom - visible_height;
                break;
            default:
                visible = x < left + visible_width;
                break;
            }
            if (visible) {
                const std::size_t index = static_cast<std::size_t>(y) * width + x;
                output.pixels[index] = PIXEL_RGBA{
                    red,
                    green,
                    blue,
                    (std::max)(base->pixels[index].a, layer->pixels[index].a)
                };
            }
        }
    }
    return true;
}

std::wstring trim_copy(const std::wstring& value) {
    std::size_t first = 0;
    while (first < value.size() && std::iswspace(value[first])) {
        ++first;
    }
    std::size_t last = value.size();
    while (last > first && std::iswspace(value[last - 1])) {
        --last;
    }
    return value.substr(first, last - first);
}

std::wstring format_decimal_one_place(double value) {
    std::wostringstream output;
    output.imbue(std::locale::classic());
    output << std::fixed << std::setprecision(1) << value;
    return output.str();
}

std::string format_raw_number(double value) {
    if (std::isnan(value)) {
        return "nan";
    }
    if (std::isinf(value)) {
        return value < 0.0 ? "-inf" : "inf";
    }
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << value;
    return output.str();
}

DocumentLayoutConfiguration resolve_document_layout(
    double raw_width_cm,
    ParagraphAlignment paragraph_alignment) {
    DocumentLayoutConfiguration configuration;
    configuration.raw_minipage_width_cm = raw_width_cm;
    configuration.minipage_width_cm = std::isfinite(raw_width_cm)
        ? (std::clamp)(raw_width_cm, 0.0, 100.0)
        : 0.0;
    configuration.formatted_minipage_width_cm =
        format_decimal_one_place(configuration.minipage_width_cm);
    configuration.paragraph_alignment = paragraph_alignment;
    return configuration;
}

std::vector<std::wstring> split_steps(const std::wstring& source) {
    std::vector<std::wstring> steps;
    std::vector<std::wstring> current_lines;

    const auto flush_step = [&steps, &current_lines]() {
        std::wstring step;
        for (std::size_t index = 0; index < current_lines.size(); ++index) {
            if (index != 0) {
                step.push_back(L'\n');
            }
            step += current_lines[index];
        }
        if (!trim_copy(step).empty()) {
            steps.push_back(std::move(step));
        }
        current_lines.clear();
    };

    std::wistringstream input(source);
    std::wstring line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == L'\r') {
            line.pop_back();
        }
        if (trim_copy(line) == L"%<step>") {
            flush_step();
        } else {
            current_lines.push_back(std::move(line));
        }
    }
    flush_step();
    return steps;
}

std::string narrow_hash(const std::wstring& hash) {
    std::string result;
    result.reserve(hash.size());
    for (const wchar_t character : hash) {
        result.push_back(static_cast<char>(character));
    }
    return result;
}

std::wstring widen_ascii(const char* value) {
    std::wstring result;
    if (value == nullptr) {
        return result;
    }
    while (*value != '\0') {
        result.push_back(static_cast<unsigned char>(*value));
        ++value;
    }
    return result;
}

std::string to_utf8_for_log(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (size <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(size), '\0');
    if (WideCharToMultiByte(
            CP_UTF8,
            WC_ERR_INVALID_CHARS,
            value.data(),
            static_cast<int>(value.size()),
            result.data(),
            size,
            nullptr,
            nullptr) != size) {
        return {};
    }
    return result;
}

bool set_compile_status(
    EDIT_SECTION* edit,
    OBJECT_HANDLE object,
    const wchar_t* status) {
    if (edit == nullptr || object == nullptr || status == nullptr) {
        return false;
    }
    const std::string serialized_status = to_utf8_for_log(status);
    return !serialized_status.empty() && edit->set_object_item_value(
        object, L"LaTeX", L"状態", serialized_status.c_str());
}

class CompileStatusScope {
public:
    CompileStatusScope(
        EDIT_SECTION* edit,
        OBJECT_HANDLE object,
        std::int64_t object_id,
        std::wstring template_name,
        int render_dpi,
        std::wstring font_display_name,
        std::wstring font_file_name)
        : edit_(edit), object_(object), object_id_(object_id) {
        set_compile_status(edit_, object_, L"コンパイル中");
        operation_.status = L"コンパイル中";
        operation_.template_name = std::move(template_name);
        operation_.render_dpi = render_dpi;
        operation_.font_display_name = std::move(font_display_name);
        operation_.font_file_name = std::filesystem::path(
            std::move(font_file_name)).filename().wstring();
        operation_.timestamp = current_local_timestamp();
        if (object_id_ >= 0) {
            std::lock_guard state_lock(state_mutex);
            object_states[object_id_].last_operation = operation_;
        }
    }

    ~CompileStatusScope() {
        if (finished_) {
            return;
        }
        const bool updated = set_compile_status(edit_, object_, L"失敗");
        append_latest_log(
            "status_display: " + to_utf8_for_log(L"失敗") + "\n"
            "status_item_update: " +
                std::string(updated ? "success\n" : "failed\n") +
            "compile_source: none\n"
            "cache_used: no\n"
            "exit_code: not-applicable-or-see-process-log\n"
            "error_summary: compilation failed; previous image retained\n");
        if (object_id_ >= 0) {
            const LastOperationInfo failure =
                inspect_latest_compile_failure(operation_);
            std::lock_guard state_lock(state_mutex);
            object_states[object_id_].last_operation = failure;
        }
    }

    void succeed(CompileSource source) {
        const bool from_cache = source == CompileSource::Cache;
        const wchar_t* status = from_cache
            ? L"成功（キャッシュ）"
            : L"成功（再生成）";
        const bool updated = set_compile_status(edit_, object_, status);
        append_latest_log(
            "status_display: " + to_utf8_for_log(status) + "\n"
            "status_item_update: " +
                std::string(updated ? "success\n" : "failed\n") +
            "compile_source: " +
                std::string(from_cache ? "cache\n" : "generated\n") +
            "cache_used: " +
                std::string(from_cache ? "yes\n" : "no\n"));
        operation_.status = status;
        operation_.failed_stage = L"なし";
        operation_.error_category = UserErrorCategory::None;
        operation_.error_summary.clear();
        operation_.exit_code = 0;
        operation_.cache_known = true;
        operation_.cache_used = from_cache;
        operation_.timestamp = current_local_timestamp();
        if (object_id_ >= 0) {
            std::lock_guard state_lock(state_mutex);
            object_states[object_id_].last_operation = operation_;
        }
        finished_ = true;
    }

private:
    EDIT_SECTION* edit_ = nullptr;
    OBJECT_HANDLE object_ = nullptr;
    std::int64_t object_id_ = -1;
    LastOperationInfo operation_;
    bool finished_ = false;
};

std::string trim_ascii_copy(const std::string& value) {
    const std::size_t first = value.find_first_not_of(" \t");
    if (first == std::string::npos) {
        return {};
    }
    const std::size_t last = value.find_last_not_of(" \t");
    return value.substr(first, last - first + 1);
}

TikzConfiguration build_tikz_configuration(
    LatexTemplate selected_template,
    const std::string& raw_libraries) {
    TikzConfiguration configuration;
    if (selected_template != LatexTemplate::TikzPicture) {
        return configuration;
    }

    configuration.raw_libraries = raw_libraries;
    if (raw_libraries.find_first_of("\r\n\\{}%#$;") != std::string::npos) {
        configuration.error_message =
            "TikZ library list contains a forbidden character";
        return configuration;
    }

    std::vector<std::string> libraries;
    std::size_t begin = 0;
    while (begin <= raw_libraries.size()) {
        const std::size_t comma = raw_libraries.find(',', begin);
        const std::size_t end = comma == std::string::npos
            ? raw_libraries.size()
            : comma;
        const std::string library = trim_ascii_copy(
            raw_libraries.substr(begin, end - begin));
        if (!library.empty()) {
            const bool valid_name = std::all_of(
                library.begin(),
                library.end(),
                [](unsigned char character) {
                    return (character >= 'A' && character <= 'Z') ||
                        (character >= 'a' && character <= 'z') ||
                        (character >= '0' && character <= '9') ||
                        character == '.' || character == '-' || character == '_';
                });
            if (!valid_name) {
                configuration.error_message =
                    "TikZ library names may contain only letters, digits, '.', '-', and '_'";
                return configuration;
            }
            libraries.push_back(library);
        }
        if (comma == std::string::npos) {
            break;
        }
        begin = comma + 1;
    }

    std::sort(libraries.begin(), libraries.end());
    libraries.erase(
        std::unique(libraries.begin(), libraries.end()), libraries.end());
    configuration.library_count = libraries.size();
    for (const auto& library : libraries) {
        if (!configuration.normalized_libraries.empty()) {
            configuration.normalized_libraries.push_back(',');
        }
        configuration.normalized_libraries += library;
    }

    configuration.preamble = L"\\usepackage{tikz}\n";
    if (!configuration.normalized_libraries.empty()) {
        configuration.preamble += L"\\usetikzlibrary{";
        configuration.preamble.append(
            configuration.normalized_libraries.begin(),
            configuration.normalized_libraries.end());
        configuration.preamble += L"}\n";
    }
    configuration.cache_material =
        L"template=tikzpicture\n"
        L"tikz-libraries=";
    configuration.cache_material.append(
        configuration.normalized_libraries.begin(),
        configuration.normalized_libraries.end());
    configuration.cache_material +=
        L"\ntikz-template-generation=standalone-tikz-v1"
        L"\ntikz-step-scope=opacity-scope-v1"
        L"\ntikz-render-mode=monochrome-alpha-mask";
    // ImageLoader converts rendered RGB luminance to an alpha mask; the
    // AviUtl2 text-color item supplies the final RGB, so TikZ colors are not
    // preserved in this initial template implementation.
    return configuration;
}

ParagraphAlignment paragraph_alignment_from_selection(int value) {
    switch (value) {
    case 1:
        return ParagraphAlignment::Justify;
    case 2:
        return ParagraphAlignment::Center;
    case 3:
        return ParagraphAlignment::Right;
    default:
        return ParagraphAlignment::Left;
    }
}

ParagraphAlignment paragraph_alignment_from_serialized_value(
    const std::string& value) {
    if (value == to_utf8_for_log(L"両端揃え") || value == "1") {
        return ParagraphAlignment::Justify;
    }
    if (value == to_utf8_for_log(L"中央揃え") || value == "2") {
        return ParagraphAlignment::Center;
    }
    if (value == to_utf8_for_log(L"右揃え") || value == "3") {
        return ParagraphAlignment::Right;
    }
    return ParagraphAlignment::Left;
}

const char* paragraph_alignment_name(ParagraphAlignment alignment) {
    switch (alignment) {
    case ParagraphAlignment::Justify:
        return "justify";
    case ParagraphAlignment::Center:
        return "center";
    case ParagraphAlignment::Right:
        return "right";
    default:
        return "left";
    }
}

const wchar_t* paragraph_alignment_command(ParagraphAlignment alignment) {
    switch (alignment) {
    case ParagraphAlignment::Justify:
        return L"\\justifying";
    case ParagraphAlignment::Center:
        return L"\\Centering";
    case ParagraphAlignment::Right:
        return L"\\RaggedLeft";
    default:
        return L"\\RaggedRight";
    }
}

JapaneseFontMode resolve_japanese_font_mode(int value) {
    switch (value) {
    case 1:
        return JapaneseFontMode::FontName;
    case 2:
        return JapaneseFontMode::FontFile;
    default:
        return JapaneseFontMode::Default;
    }
}

const wchar_t* japanese_font_mode_name(JapaneseFontMode mode) {
    switch (mode) {
    case JapaneseFontMode::FontName:
        return L"font-name";
    case JapaneseFontMode::FontFile:
        return L"font-file";
    default:
        return L"default";
    }
}

JapaneseSpacingMode japanese_spacing_mode_from_selection(int value) {
    switch (value) {
    case 1:
        return JapaneseSpacingMode::Uniform;
    case 2:
        return JapaneseSpacingMode::FontMetrics;
    default:
        return JapaneseSpacingMode::Auto;
    }
}

JapaneseSpacingMode japanese_spacing_mode_from_serialized_value(
    const std::string& value) {
    if (value == to_utf8_for_log(L"自動") || value == "0") {
        return JapaneseSpacingMode::Auto;
    }
    if (value == to_utf8_for_log(L"均等") || value == "1") {
        return JapaneseSpacingMode::Uniform;
    }
    if (value == to_utf8_for_log(L"フォント準拠") || value == "2") {
        return JapaneseSpacingMode::FontMetrics;
    }
    return JapaneseSpacingMode::Auto;
}

bool is_legacy_custom_spacing_value(const std::string& value) {
    return value == to_utf8_for_log(L"カスタム") || value == "3" ||
        value == "custom" || value == "Custom";
}

const wchar_t* japanese_spacing_mode_name(JapaneseSpacingMode mode) {
    switch (mode) {
    case JapaneseSpacingMode::Uniform:
        return L"uniform";
    case JapaneseSpacingMode::FontMetrics:
        return L"font-metrics";
    default:
        return L"auto";
    }
}

void configure_japanese_spacing(
    JapaneseDocumentConfiguration& configuration,
    JapaneseSpacingMode requested_mode) {
    configuration.spacing_requested = requested_mode;
    if (configuration.font_mode == JapaneseFontMode::Default) {
        return;
    }

    configuration.spacing_effective =
        requested_mode == JapaneseSpacingMode::Auto
        ? JapaneseSpacingMode::FontMetrics
        : requested_mode;
    configuration.spacing_options_applied = true;
    if (configuration.spacing_effective == JapaneseSpacingMode::Uniform) {
        configuration.spacing_jfm = L"ujis";
        configuration.spacing_kerning = L"off";
        configuration.spacing_jfont_options =
            L"  YokoFeatures={JFM=ujis},\n"
            L"  Kerning=Off\n";
        return;
    }

    configuration.spacing_jfm = L"propw";
    configuration.spacing_kerning = L"on";
    configuration.spacing_palt = true;
    configuration.spacing_kern_feature = true;
    configuration.generated_script = L"Default";
    configuration.generated_raw_features = L"+palt,+kern";
    configuration.spacing_jfont_options =
        L"  YokoFeatures={JFM=propw},\n"
        L"  Kerning=On,\n"
        L"  Script=Default,\n"
        L"  RawFeature={+palt,+kern}\n";
}

const wchar_t* japanese_spacing_effective_name(
    const JapaneseDocumentConfiguration& configuration) {
    if (!configuration.spacing_options_applied) {
        return L"default";
    }
    return japanese_spacing_mode_name(configuration.spacing_effective);
}

bool contains_unsafe_font_name_character(const std::wstring& value) {
    return value.find_first_of(L"\r\n{}%#\\&$^~_") != std::wstring::npos;
}

bool contains_unsafe_font_path_character(const std::wstring& value) {
    return value.find_first_of(L"\r\n{}%#&$^~") != std::wstring::npos;
}

std::wstring lowercase_copy(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t character) {
        return static_cast<wchar_t>(std::towlower(character));
    });
    return value;
}

JapaneseDocumentConfiguration build_japanese_document_configuration(
    LatexTemplate selected_template,
    bool japanese_requested,
    int font_mode_value,
    JapaneseSpacingMode spacing_mode,
    const std::wstring& requested_font_name,
    const std::wstring& requested_font_file) {
    JapaneseDocumentConfiguration configuration;
    if (selected_template != LatexTemplate::Document) {
        return configuration;
    }

    configuration.enabled = japanese_requested;
    configuration.font_mode = resolve_japanese_font_mode(font_mode_value);
    configuration.spacing_requested = spacing_mode;
    configuration.font_name = trim_copy(requested_font_name);
    configuration.font_file = trim_copy(requested_font_file);
    configuration.cache_material =
        L"japanese_enabled=" + std::to_wstring(configuration.enabled ? 1 : 0);
    if (!configuration.enabled) {
        return configuration;
    }
    configure_japanese_spacing(configuration, spacing_mode);
    configuration.cache_material +=
        L"\nfont_mode=" + std::wstring(japanese_font_mode_name(configuration.font_mode)) +
        L"\njapanese-spacing-requested=" +
            japanese_spacing_mode_name(configuration.spacing_requested) +
        L"\njapanese-spacing-effective=" +
            japanese_spacing_effective_name(configuration) +
        L"\njapanese-jfm=" +
            (configuration.spacing_options_applied
                ? configuration.spacing_jfm
                : L"default") +
        L"\njapanese-kerning=" +
            (configuration.spacing_options_applied
                ? configuration.spacing_kerning
                : L"default") +
        L"\njapanese-palt=" +
            std::wstring(configuration.spacing_palt ? L"on" : L"off") +
        L"\njapanese-kern-feature=" +
            std::wstring(configuration.spacing_kern_feature ? L"on" : L"off");
    configuration.preamble =
        L"\\usepackage{fontspec}\n"
        L"\\usepackage{luatexja}\n"
        L"\\usepackage{luatexja-fontspec}\n";
    if (configuration.font_mode == JapaneseFontMode::Default) {
        return configuration;
    }

    if (configuration.font_mode == JapaneseFontMode::FontName) {
        if (configuration.font_name.empty()) {
            configuration.error_message = L"日本語フォント名が空です";
            return configuration;
        }
        if (contains_unsafe_font_name_character(requested_font_name)) {
            configuration.error_message =
                L"日本語フォント名にTeXへ安全に渡せない文字が含まれています";
            return configuration;
        }
        configuration.generated_setmainfont =
            L"\\setmainfont{" + configuration.font_name + L"}\n";
        configuration.preamble += configuration.generated_setmainfont;
        if (configuration.spacing_options_applied) {
            configuration.generated_setmainjfont =
                L"\\setmainjfont[\n" + configuration.spacing_jfont_options +
                L"]{" + configuration.font_name + L"}\n";
        } else {
            configuration.generated_setmainjfont =
                L"\\setmainjfont{" + configuration.font_name + L"}\n";
        }
        configuration.preamble += configuration.generated_setmainjfont;
        configuration.cache_material += L"\nfont_name=" + configuration.font_name;
        return configuration;
    }

    if (configuration.font_file.empty()) {
        configuration.error_message = L"日本語フォントファイルが空です";
        return configuration;
    }
    if (contains_unsafe_font_path_character(requested_font_file)) {
        configuration.error_message =
            L"日本語フォントファイルのパスにTeXへ安全に渡せない文字が含まれています";
        return configuration;
    }

    std::error_code error;
    std::filesystem::path font_path(configuration.font_file);
    font_path = std::filesystem::absolute(font_path, error).lexically_normal();
    if (error) {
        configuration.error_message = L"日本語フォントファイルの絶対パスを取得できません";
        return configuration;
    }
    configuration.font_file = font_path.wstring();
    configuration.font_file_exists =
        std::filesystem::exists(font_path, error) && !error;
    if (!configuration.font_file_exists ||
        !std::filesystem::is_regular_file(font_path, error) || error) {
        configuration.error_message =
            L"日本語フォントファイルが存在しないか、通常ファイルではありません";
        return configuration;
    }

    const std::wstring extension = lowercase_copy(font_path.extension().wstring());
    if (extension != L".otf" && extension != L".ttf" && extension != L".ttc") {
        configuration.error_message =
            L"日本語フォントファイルの拡張子は.otf、.ttf、.ttcのみ対応しています";
        return configuration;
    }

    configuration.font_file_size = std::filesystem::file_size(font_path, error);
    if (error) {
        configuration.error_message = L"日本語フォントファイルのサイズを取得できません";
        return configuration;
    }
    const auto last_write_time = std::filesystem::last_write_time(font_path, error);
    configuration.font_file_last_write_time = error
        ? L"unavailable"
        : std::to_wstring(last_write_time.time_since_epoch().count());

    configuration.normalized_font_directory =
        font_path.parent_path().generic_wstring();
    if (!configuration.normalized_font_directory.empty() &&
        configuration.normalized_font_directory.back() != L'/') {
        configuration.normalized_font_directory.push_back(L'/');
    }
    configuration.normalized_font_filename = font_path.filename().generic_wstring();
    if (contains_unsafe_font_path_character(configuration.normalized_font_directory) ||
        contains_unsafe_font_path_character(configuration.normalized_font_filename)) {
        configuration.error_message =
            L"日本語フォントファイルのパスにTeXへ安全に渡せない文字が含まれています";
        return configuration;
    }

    configuration.generated_setmainfont =
        L"\\setmainfont[\n  Path={" + configuration.normalized_font_directory +
        L"}\n]{" + configuration.normalized_font_filename + L"}\n";
    configuration.preamble += configuration.generated_setmainfont;
    if (configuration.spacing_options_applied) {
        configuration.generated_setmainjfont =
            L"\\setmainjfont[\n  Path={" +
            configuration.normalized_font_directory + L"},\n" +
            configuration.spacing_jfont_options + L"]{" +
            configuration.normalized_font_filename + L"}\n";
    } else {
        configuration.generated_setmainjfont =
            L"\\setmainjfont[\n  Path={" +
            configuration.normalized_font_directory + L"}\n]{" +
            configuration.normalized_font_filename + L"}\n";
    }
    configuration.preamble += configuration.generated_setmainjfont;
    configuration.cache_material +=
        L"\nnormalized_font_file=" + configuration.normalized_font_directory +
            configuration.normalized_font_filename +
        L"\nfont_file_size=" + std::to_wstring(configuration.font_file_size) +
        L"\nfont_file_last_write_time=" + configuration.font_file_last_write_time;
    return configuration;
}

bool parse_environment_command(
    const std::wstring& source,
    std::size_t position,
    const wchar_t* command,
    std::size_t& command_end,
    std::wstring& environment_name) {
    const std::wstring prefix = std::wstring(L"\\") + command + L"{";
    if (source.compare(position, prefix.size(), prefix) != 0) {
        return false;
    }
    const std::size_t name_start = position + prefix.size();
    const std::size_t closing_brace = source.find(L'}', name_start);
    if (closing_brace == std::wstring::npos) {
        return false;
    }
    environment_name = source.substr(name_start, closing_brace - name_start);
    command_end = closing_brace + 1;
    return true;
}

std::wstring apply_math_structure_color(
    const std::wstring& source,
    bool visible,
    bool split_alignment_cells) {
    const std::wstring color = visible ? L"black" : L"white";
    std::wstring output;
    std::vector<std::wstring> environment_stack;
    int brace_depth = 0;
    bool in_comment = false;
    bool color_group_open = false;

    const auto open_color_group = [&]() {
        if (!color_group_open) {
            output += L"{\\color{";
            output += color;
            output += L"}";
            color_group_open = true;
        }
    };
    const auto close_color_group = [&]() {
        if (color_group_open) {
            output.push_back(L'}');
            color_group_open = false;
        }
    };

    for (std::size_t index = 0; index < source.size();) {
        const wchar_t character = source[index];
        if (in_comment) {
            output.push_back(character);
            ++index;
            if (character == L'\n') {
                in_comment = false;
            }
            continue;
        }
        if (character == L'%') {
            in_comment = true;
            output.push_back(character);
            ++index;
            continue;
        }
        if (character == L'\\') {
            std::size_t command_end = 0;
            std::wstring environment_name;
            if (parse_environment_command(
                    source, index, L"begin", command_end, environment_name)) {
                open_color_group();
                output.append(source, index, command_end - index);
                environment_stack.push_back(std::move(environment_name));
                index = command_end;
                continue;
            }
            if (parse_environment_command(
                    source, index, L"end", command_end, environment_name)) {
                open_color_group();
                output.append(source, index, command_end - index);
                if (!environment_stack.empty() &&
                    environment_stack.back() == environment_name) {
                    environment_stack.pop_back();
                }
                index = command_end;
                continue;
            }
            if (index + 1 < source.size() && source[index + 1] == L'\\') {
                const bool outer_row_break =
                    environment_stack.empty() && brace_depth == 0;
                if (outer_row_break) {
                    close_color_group();
                } else {
                    open_color_group();
                }
                output.append(L"\\\\");
                index += 2;
                if (outer_row_break) {
                    if (index < source.size() && source[index] == L'*') {
                        output.push_back(source[index++]);
                    }
                    if (index < source.size() && source[index] == L'[') {
                        while (index < source.size()) {
                            const wchar_t option_character = source[index++];
                            output.push_back(option_character);
                            if (option_character == L']') {
                                break;
                            }
                        }
                    }
                }
                continue;
            }
            open_color_group();
            output.push_back(character);
            ++index;
            if (index < source.size()) {
                output.push_back(source[index++]);
            }
            continue;
        }
        if (character == L'{') {
            open_color_group();
            ++brace_depth;
        } else if (character == L'}' && brace_depth > 0) {
            --brace_depth;
        }
        if (character == L'&' && split_alignment_cells &&
            environment_stack.empty() && brace_depth == 0) {
            close_color_group();
            output.push_back(character);
            ++index;
            continue;
        }
        if (!std::iswspace(character)) {
            open_color_group();
        }
        output.push_back(character);
        ++index;
    }
    if (in_comment && color_group_open) {
        output.push_back(L'\n');
    }
    close_color_group();
    return output;
}

std::wstring apply_alignment_cell_color(
    const std::wstring& source,
    bool visible) {
    const std::wstring color = visible ? L"black" : L"white";
    std::wstring output;
    std::vector<std::wstring> environment_stack;
    int brace_depth = 0;
    bool in_comment = false;
    bool color_needed = true;

    const auto emit_color = [&]() {
        if (color_needed) {
            output += L"\\color{";
            output += color;
            output += L"}";
            color_needed = false;
        }
    };

    for (std::size_t index = 0; index < source.size();) {
        const wchar_t character = source[index];
        if (in_comment) {
            output.push_back(character);
            ++index;
            if (character == L'\n') {
                in_comment = false;
            }
            continue;
        }
        if (character == L'%') {
            in_comment = true;
            output.push_back(character);
            ++index;
            continue;
        }
        if (character == L'\\') {
            std::size_t command_end = 0;
            std::wstring environment_name;
            if (parse_environment_command(
                    source, index, L"begin", command_end, environment_name)) {
                emit_color();
                output.append(source, index, command_end - index);
                environment_stack.push_back(std::move(environment_name));
                index = command_end;
                continue;
            }
            if (parse_environment_command(
                    source, index, L"end", command_end, environment_name)) {
                emit_color();
                output.append(source, index, command_end - index);
                if (!environment_stack.empty() &&
                    environment_stack.back() == environment_name) {
                    environment_stack.pop_back();
                }
                index = command_end;
                continue;
            }
            if (index + 1 < source.size() && source[index + 1] == L'\\') {
                const bool outer_row_break =
                    environment_stack.empty() && brace_depth == 0;
                if (!outer_row_break) {
                    emit_color();
                }
                output.append(L"\\\\");
                index += 2;
                if (outer_row_break) {
                    if (index < source.size() && source[index] == L'*') {
                        output.push_back(source[index++]);
                    }
                    if (index < source.size() && source[index] == L'[') {
                        while (index < source.size()) {
                            const wchar_t option_character = source[index++];
                            output.push_back(option_character);
                            if (option_character == L']') {
                                break;
                            }
                        }
                    }
                    color_needed = true;
                }
                continue;
            }
            emit_color();
            output.push_back(character);
            ++index;
            if (index < source.size()) {
                output.push_back(source[index++]);
            }
            continue;
        }
        if (character == L'{' || character == L'}') {
            emit_color();
            if (character == L'{') {
                ++brace_depth;
            } else if (brace_depth > 0) {
                --brace_depth;
            }
        }
        if (character == L'&' &&
            environment_stack.empty() && brace_depth == 0) {
            output.push_back(character);
            color_needed = true;
            ++index;
            continue;
        }
        if (!std::iswspace(character)) {
            emit_color();
        }
        output.push_back(character);
        ++index;
    }
    return output;
}

bool create_globally_cropped_layers(
    const ImageList& source_images,
    int render_dpi,
    std::shared_ptr<ImageList>& padded_images,
    ImagePointer& padded_transparent,
    int& original_canvas_width,
    int& original_canvas_height,
    ContentBounds& global_content_bounds,
    int& padding_px,
    int& final_canvas_width,
    int& final_canvas_height,
    std::vector<LayerCropStats>& layer_stats) {
    original_canvas_width = 1;
    original_canvas_height = 1;
    for (const auto& image : source_images) {
        if (!image || image->width <= 0 || image->height <= 0) {
            return false;
        }
        const auto source_pixel_count =
            static_cast<std::size_t>(image->width) * static_cast<std::size_t>(image->height);
        if (image->pixels.size() < source_pixel_count) {
            return false;
        }
        original_canvas_width = (std::max)(original_canvas_width, image->width);
        original_canvas_height = (std::max)(original_canvas_height, image->height);
    }

    global_content_bounds = ContentBounds{
        original_canvas_width, original_canvas_height, -1, -1
    };
    layer_stats.clear();
    layer_stats.resize(source_images.size());
    for (std::size_t layer_index = 0; layer_index < source_images.size(); ++layer_index) {
        const auto& source = source_images[layer_index];
        const int source_offset_x = (original_canvas_width - source->width) / 2;
        auto& stats = layer_stats[layer_index];
        stats.original_bounds = ContentBounds{
            original_canvas_width, original_canvas_height, -1, -1
        };
        for (int y = 0; y < source->height; ++y) {
            for (int x = 0; x < source->width; ++x) {
                const auto source_index =
                    static_cast<std::size_t>(y) * source->width + x;
                if (source->pixels[source_index].a <= kContentAlphaThreshold) {
                    continue;
                }
                ++stats.nonzero_alpha_pixels;
                const int normalized_x = source_offset_x + x;
                stats.original_bounds.left =
                    (std::min)(stats.original_bounds.left, normalized_x);
                stats.original_bounds.top =
                    (std::min)(stats.original_bounds.top, y);
                stats.original_bounds.right =
                    (std::max)(stats.original_bounds.right, normalized_x);
                stats.original_bounds.bottom =
                    (std::max)(stats.original_bounds.bottom, y);
                global_content_bounds.left =
                    (std::min)(global_content_bounds.left, normalized_x);
                global_content_bounds.top =
                    (std::min)(global_content_bounds.top, y);
                global_content_bounds.right =
                    (std::max)(global_content_bounds.right, normalized_x);
                global_content_bounds.bottom =
                    (std::max)(global_content_bounds.bottom, y);
            }
        }
    }
    if (!global_content_bounds.valid()) {
        return false;
    }

    padding_px = (std::max)(
        2, static_cast<int>(std::ceil(static_cast<double>(render_dpi) * 2.0 / 72.0)));
    const std::int64_t content_width =
        static_cast<std::int64_t>(global_content_bounds.right) -
        global_content_bounds.left + 1;
    const std::int64_t content_height =
        static_cast<std::int64_t>(global_content_bounds.bottom) -
        global_content_bounds.top + 1;
    const std::int64_t calculated_width = content_width + 2LL * padding_px;
    const std::int64_t calculated_height = content_height + 2LL * padding_px;
    if (calculated_width <= 0 || calculated_height <= 0 ||
        calculated_width > (std::numeric_limits<int>::max)() ||
        calculated_height > (std::numeric_limits<int>::max)() ||
        static_cast<std::uint64_t>(calculated_width) >
            (std::numeric_limits<std::size_t>::max)() /
            static_cast<std::uint64_t>(calculated_height)) {
        return false;
    }
    final_canvas_width = static_cast<int>(calculated_width);
    final_canvas_height = static_cast<int>(calculated_height);
    const auto canvas_pixel_count =
        static_cast<std::size_t>(final_canvas_width) *
        static_cast<std::size_t>(final_canvas_height);
    try {
        padded_images = std::make_shared<ImageList>();
        padded_images->reserve(source_images.size());
        padded_transparent = std::make_shared<const RenderedImage>(RenderedImage{
            final_canvas_width,
            final_canvas_height,
            std::vector<PIXEL_RGBA>(canvas_pixel_count, PIXEL_RGBA{ 0, 0, 0, 0 })
        });

        for (std::size_t index = 0; index < source_images.size(); ++index) {
            const auto& source = source_images[index];
            const int source_offset_x = (original_canvas_width - source->width) / 2;
            const int destination_x =
                source_offset_x - global_content_bounds.left + padding_px;
            const int destination_y = -global_content_bounds.top + padding_px;
            layer_stats[index].cropped_destination_x = destination_x;
            layer_stats[index].cropped_destination_y = destination_y;

            RenderedImage padded{
                final_canvas_width,
                final_canvas_height,
                std::vector<PIXEL_RGBA>(canvas_pixel_count, PIXEL_RGBA{ 0, 0, 0, 0 })
            };
            for (int y = 0; y < source->height; ++y) {
                for (int x = 0; x < source->width; ++x) {
                    const int cropped_x = destination_x + x;
                    const int cropped_y = destination_y + y;
                    if (cropped_x < 0 || cropped_x >= final_canvas_width ||
                        cropped_y < 0 || cropped_y >= final_canvas_height) {
                        continue;
                    }
                    const auto source_index =
                        static_cast<std::size_t>(y) * source->width + x;
                    const auto destination_index =
                        static_cast<std::size_t>(cropped_y) * final_canvas_width + cropped_x;
                    padded.pixels[destination_index] = source->pixels[source_index];
                }
            }
            padded_images->push_back(
                std::make_shared<const RenderedImage>(std::move(padded)));
        }
    } catch (const std::bad_alloc&) {
        append_latest_log("padded_image_created: no\n");
        return false;
    }
    return true;
}

bool create_cumulative_images(
    const ImageList& step_layers,
    const ImagePointer& transparent_layer,
    std::shared_ptr<ImageList>& cumulative_images) {
    try {
        cumulative_images = std::make_shared<ImageList>();
        cumulative_images->reserve(step_layers.size() + 1);
        if (!transparent_layer) {
            return false;
        }
        cumulative_images->push_back(transparent_layer);
        if (step_layers.empty()) {
            return true;
        }
        const int width = step_layers.front()->width;
        const int height = step_layers.front()->height;
        const auto pixel_count = static_cast<std::size_t>(width) * height;
        std::vector<PIXEL_RGBA> accumulated(
            pixel_count, PIXEL_RGBA{ 255, 255, 255, 0 });
        for (const auto& layer : step_layers) {
            if (!layer || layer->width != width || layer->height != height ||
                layer->pixels.size() < pixel_count) {
                return false;
            }
            for (std::size_t index = 0; index < pixel_count; ++index) {
                accumulated[index].a = (std::max)(
                    accumulated[index].a, layer->pixels[index].a);
            }
            cumulative_images->push_back(
                std::make_shared<const RenderedImage>(RenderedImage{
                    width, height, accumulated
                }));
        }
    } catch (const std::bad_alloc&) {
        return false;
    }
    return true;
}

class LatexTemplateBuilder {
public:
    static LatexTemplate from_selection(int value) {
        switch (value) {
        case 1:
            return LatexTemplate::AlignStar;
        case 2:
            return LatexTemplate::EquationStar;
        case 3:
            return LatexTemplate::Document;
        case 4:
            return LatexTemplate::TikzPicture;
        default:
            return LatexTemplate::InlineMath;
        }
    }

    static LatexTemplate from_serialized_value(
        const std::string& value,
        int fallback_selection) {
        if (value == to_utf8_for_log(L"インライン数式")) {
            return LatexTemplate::InlineMath;
        }
        if (value == "align*") {
            return LatexTemplate::AlignStar;
        }
        if (value == "equation*") {
            return LatexTemplate::EquationStar;
        }
        if (value == "document" || value == "gather*" || value == "multline*") {
            return LatexTemplate::Document;
        }
        if (value == "tikzpicture") {
            return LatexTemplate::TikzPicture;
        }
        return from_selection(fallback_selection);
    }

    static int id(LatexTemplate selected_template) {
        return static_cast<int>(selected_template);
    }

    static const wchar_t* name(LatexTemplate selected_template) {
        switch (selected_template) {
        case LatexTemplate::InlineMath:
            return L"インライン数式";
        case LatexTemplate::AlignStar:
            return L"align*";
        case LatexTemplate::EquationStar:
            return L"equation*";
        case LatexTemplate::Document:
            return L"document";
        case LatexTemplate::TikzPicture:
            return L"tikzpicture";
        default:
            return L"インライン数式";
        }
    }

    static std::wstring build_step_layer_document(
        LatexTemplate selected_template,
        const std::vector<std::wstring>& steps,
        std::size_t target_step,
        const JapaneseDocumentConfiguration& japanese_configuration,
        const DocumentLayoutConfiguration& layout_configuration,
        const TikzConfiguration& tikz_configuration) {
        std::wstring body;
        for (std::size_t index = 0; index < steps.size(); ++index) {
            if (!body.empty() && selected_template != LatexTemplate::InlineMath) {
                body.push_back(L'\n');
            }
            const bool visible = index == target_step;
            if (selected_template == LatexTemplate::TikzPicture) {
                body += L"\\begin{scope}[opacity=";
                body += visible ? L"1" : L"0";
                body += L"]\n";
                body += steps[index];
                body += L"\n\\end{scope}";
            } else if (selected_template == LatexTemplate::InlineMath) {
                body += L"{\\color{";
                body += visible ? L"black" : L"white";
                body += L"}";
                body += steps[index];
                body += L"}";
            } else if (selected_template == LatexTemplate::AlignStar) {
                body += apply_alignment_cell_color(steps[index], visible);
            } else if (selected_template == LatexTemplate::Document) {
                body += L"\\begingroup\\color{";
                body += visible ? L"black" : L"white";
                body += L"}\n";
                body += steps[index];
                body += L"\n\\endgroup";
            } else {
                body += apply_math_structure_color(steps[index], visible, false);
            }
        }
        return build_document(
            selected_template,
            body,
            selected_template == LatexTemplate::Document
                ? japanese_configuration.preamble
                : std::wstring{},
            layout_configuration,
            tikz_configuration);
    }

private:
    static std::wstring common_preamble(LatexTemplate selected_template) {
        if (selected_template == LatexTemplate::TikzPicture) {
            return
                L"\\documentclass[tikz,border=2pt]{standalone}\n"
                L"\\usepackage{amsmath}\n"
                L"\\usepackage{amssymb}\n"
                L"\\usepackage{xcolor}\n";
        }
        return
            L"\\documentclass[12pt,border=2pt,preview]{standalone}\n"
            L"\\usepackage{amsmath}\n"
            L"\\usepackage{amssymb}\n"
            L"\\usepackage{xcolor}\n";
    }

    static std::wstring build_document(
        LatexTemplate selected_template,
        const std::wstring& body,
        const std::wstring& additional_preamble,
        const DocumentLayoutConfiguration& layout_configuration,
        const TikzConfiguration& tikz_configuration) {
        std::wstring document = common_preamble(selected_template);
        document += additional_preamble;
        if (selected_template == LatexTemplate::TikzPicture) {
            document += tikz_configuration.preamble;
        }
        if (selected_template == LatexTemplate::Document) {
            if (layout_configuration.minipage_enabled()) {
                document += L"\\usepackage{ragged2e}\n";
            }
            document +=
                L"\\newcommand{\\AviUtlSetDisplaySpacing}{%\n"
                L"  \\setlength{\\abovedisplayskip}{0.5\\baselineskip}%\n"
                L"  \\setlength{\\abovedisplayshortskip}{0.5\\baselineskip}%\n"
                L"  \\setlength{\\belowdisplayskip}{0.5\\baselineskip}%\n"
                L"  \\setlength{\\belowdisplayshortskip}{0.5\\baselineskip}%\n"
                L"}\n"
                L"\\AddToHook{env/minipage/begin}{%\n"
                L"  \\AviUtlSetDisplaySpacing\n"
                L"}\n";
        }
        document += L"\\begin{document}\n";
        if (selected_template == LatexTemplate::Document) {
            document += L"\\AviUtlSetDisplaySpacing\n";
        }
        if (selected_template == LatexTemplate::InlineMath) {
            document += L"\\(";
        } else if (selected_template == LatexTemplate::TikzPicture) {
            document += L"\\begin{tikzpicture}\n";
        } else if (selected_template != LatexTemplate::Document) {
            document += L"\\begin{";
            document += name(selected_template);
            document += L"}\n";
        } else if (layout_configuration.minipage_enabled()) {
            document += L"\\noindent\n\\begin{minipage}{";
            document += layout_configuration.formatted_minipage_width_cm;
            document += L"cm}\n";
            document += paragraph_alignment_command(
                layout_configuration.paragraph_alignment);
            document += L"\n";
        }
        document += body;
        if (selected_template == LatexTemplate::InlineMath) {
            document += L"\\)\n";
        } else if (selected_template == LatexTemplate::TikzPicture) {
            document += L"\n\\end{tikzpicture}\n";
        } else if (selected_template != LatexTemplate::Document) {
            document += L"\n\\end{";
            document += name(selected_template);
            document += L"}\n";
        } else if (layout_configuration.minipage_enabled()) {
            document += L"\n\\end{minipage}\n";
        } else {
            document += L"\n";
        }
        document += L"\\end{document}\n";
        return document;
    }
};

std::int64_t find_target_object_id(EDIT_SECTION* edit, OBJECT_HANDLE object) {
    if (edit == nullptr || object == nullptr) {
        return -1;
    }
    const OBJECT_LAYER_FRAME position = edit->get_object_layer_frame(object);
    std::lock_guard state_lock(state_mutex);

    const std::int64_t recent_id = last_object_id.load(std::memory_order_relaxed);
    if (const auto recent = object_states.find(recent_id); recent != object_states.end()) {
        const auto& state = recent->second;
        if (state.layer == position.layer && state.frame_start == position.start &&
            state.frame_end == position.end) {
            return recent_id;
        }
    }

    for (const auto& [object_id, state] : object_states) {
        if (state.layer == position.layer && state.frame_start == position.start &&
            state.frame_end == position.end) {
            return object_id;
        }
    }
    return -1;
}

} // namespace

auto latex_source = FILTER_ITEM_TEXT(L"LaTeXソース", L"E=mc^2");
FILTER_ITEM_SELECT::ITEM display_mode_items[] = {
    { L"全表示", 0 },
    { L"累積表示", 1 },
    { nullptr }
};
auto display_mode = FILTER_ITEM_SELECT(L"表示モード", 0, display_mode_items);
auto display_step = FILTER_ITEM_TRACK(L"表示ステップ", 1.0, 0.0, 32.0, 0.01);
auto compile_button = FILTER_ITEM_BUTTON(L"コンパイル", request_compile);
auto compile_status = FILTER_ITEM_STRING(L"状態", L"未実行");
FILTER_ITEM_SELECT::ITEM template_items[] = {
    { L"インライン数式", 0 },
    { L"align*", 1 },
    { L"equation*", 2 },
    { L"document", 3 },
    { L"tikzpicture", 4 },
    { nullptr }
};
auto template_selection = FILTER_ITEM_SELECT(L"テンプレート", 0, template_items);
auto japanese_settings_group =
    FILTER_ITEM_GROUP(L"日本語設定（documentのみ有効）");
auto japanese_enabled = FILTER_ITEM_CHECK(L"日本語対応", false);
auto japanese_font_in_use = FILTER_ITEM_STRING(L"使用中", L"既定");
auto japanese_font_select_button =
    FILTER_ITEM_BUTTON(L"フォント選択", request_font_selection);
auto japanese_font_file_button =
    FILTER_ITEM_BUTTON(L"ファイル読込", request_font_file_selection);
FILTER_ITEM_SELECT::ITEM japanese_spacing_items[] = {
    { L"自動", 0 },
    { L"均等", 1 },
    { L"フォント準拠", 2 },
    { nullptr }
};
auto japanese_spacing =
    FILTER_ITEM_SELECT(L"和文字間", 0, japanese_spacing_items);
auto japanese_settings_group_end = FILTER_ITEM_GROUP(L"");
auto tikz_settings_group =
    FILTER_ITEM_GROUP(L"TikZ設定（tikzpictureのみ有効）");
auto tikz_libraries = FILTER_ITEM_STRING(
    L"ライブラリ", L"arrows.meta,positioning,calc");
auto tikz_settings_group_end = FILTER_ITEM_GROUP(L"");
auto text_color = FILTER_ITEM_COLOR(L"文字色", 0xffffff);
FILTER_ITEM_SELECT::ITEM render_dpi_items[] = {
    { L"600", 0 },
    { L"1200", 1 },
    { L"2400", 2 },
    { nullptr }
};
auto render_dpi = FILTER_ITEM_SELECT(L"描画DPI", 1, render_dpi_items);
auto document_settings_group =
    FILTER_ITEM_GROUP(L"document設定（documentのみ有効）");
auto minipage_width = FILTER_ITEM_TRACK(
    L"幅", 0.0, 0.0, 100.0, 0.1);
FILTER_ITEM_SELECT::ITEM paragraph_alignment_items[] = {
    { L"左揃え", 0 },
    { L"両端揃え", 1 },
    { L"中央揃え", 2 },
    { L"右揃え", 3 },
    { nullptr }
};
auto paragraph_alignment = FILTER_ITEM_SELECT(
    L"揃え",
    0,
    paragraph_alignment_items);
auto document_settings_group_end = FILTER_ITEM_GROUP(L"");
FILTER_ITEM_SELECT::ITEM transition_effect_items[] = {
    { L"即時", 0 },
    { L"フェード", 1 },
    { L"リビール", 2 },
    { nullptr }
};
auto transition_effect = FILTER_ITEM_SELECT(L"切替効果", 0, transition_effect_items);
FILTER_ITEM_SELECT::ITEM reveal_direction_items[] = {
    { L"左から", 0 },
    { L"右から", 1 },
    { L"上から", 2 },
    { L"下から", 3 },
    { nullptr }
};
auto reveal_direction = FILTER_ITEM_SELECT(L"リビール方向", 0, reveal_direction_items);
auto cumulative_settings_group =
    FILTER_ITEM_GROUP(L"累積表示設定（全表示では無効）");
auto cumulative_settings_group_end = FILTER_ITEM_GROUP(L"");
auto reveal_settings_group =
    FILTER_ITEM_GROUP(L"リビール設定（リビール時のみ有効）");
auto reveal_settings_group_end = FILTER_ITEM_GROUP(L"");
auto environment_group = FILTER_ITEM_GROUP(L"環境");
auto environment_settings_button =
    FILTER_ITEM_BUTTON(L"環境設定", request_environment_settings);
auto information_button = FILTER_ITEM_BUTTON(L"情報", request_information);
auto environment_group_end = FILTER_ITEM_GROUP(L"");
auto internal_group = FILTER_ITEM_GROUP(L"内部設定", false);
auto internal_group_end = FILTER_ITEM_GROUP(L"");
auto update_id = FILTER_ITEM_CHECK(L"更新ID", false);
auto persistent_object_data =
    FILTER_ITEM_DATA<PersistentObjectData>(L"永続レンダーキャッシュ");
auto font_object_data =
    FILTER_ITEM_DATA<FontObjectData>(L"フォント内部データ");

void* items[] = {
    &latex_source,
    &compile_button,
    &compile_status,
    &template_selection,
    &text_color,
    &render_dpi,
    &display_mode,
    &document_settings_group,
    &minipage_width,
    &paragraph_alignment,
    &document_settings_group_end,
    &japanese_settings_group,
    &japanese_enabled,
    &japanese_font_in_use,
    &japanese_font_select_button,
    &japanese_font_file_button,
    &japanese_spacing,
    &japanese_settings_group_end,
    &tikz_settings_group,
    &tikz_libraries,
    &tikz_settings_group_end,
    &cumulative_settings_group,
    &display_step,
    &transition_effect,
    &cumulative_settings_group_end,
    &reveal_settings_group,
    &reveal_direction,
    &reveal_settings_group_end,
    &environment_group,
    &environment_settings_button,
    &information_button,
    &environment_group_end,
    &internal_group,
    &update_id,
    &persistent_object_data,
    &font_object_data,
    &internal_group_end,
    nullptr
};

FILTER_PLUGIN_TABLE filter_plugin_table = {
    FILTER_PLUGIN_TABLE::FLAG_VIDEO | FILTER_PLUGIN_TABLE::FLAG_INPUT,
    L"LaTeX",
    L"LaTeX",
    L"AviUtl2 LaTeX media object prototype",
    items,
    func_proc_video,
    nullptr
};

EXTERN_C __declspec(dllexport) FILTER_PLUGIN_TABLE* GetFilterPluginTable(void) {
    return &filter_plugin_table;
}

EXTERN_C __declspec(dllexport) DWORD RequiredVersion() {
    // FILTER_ITEM_BUTTON callbacks with refreshed object values are required.
    return 2003300;
}

EXTERN_C __declspec(dllexport) void RegisterPlugin(HOST_APP_TABLE* host) {
    if (host == nullptr) {
        return;
    }
    host_edit_handle = host->create_edit_handle();
    if (host_edit_handle != nullptr) {
        set_dialog_owner(host_edit_handle->get_host_app_window());
    }
}

EXTERN_C __declspec(dllexport) void UninitializePlugin() {
    host_edit_handle = nullptr;
    set_dialog_owner(nullptr);
}

std::wstring from_utf8_for_object_value(const char* value) {
    if (value == nullptr || *value == '\0') {
        return {};
    }
    const int size = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value, -1, nullptr, 0);
    if (size <= 1) {
        return {};
    }
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, value, -1,
            result.data(), size) == 0) {
        return {};
    }
    result.resize(static_cast<std::size_t>(size - 1));
    return result;
}

std::optional<std::string> get_object_value_copy(
    EDIT_SECTION* edit,
    OBJECT_HANDLE object,
    const wchar_t* current_name,
    const wchar_t* legacy_name = nullptr,
    bool* legacy_used = nullptr) {
    if (legacy_used != nullptr) {
        *legacy_used = false;
    }
    if (edit == nullptr || object == nullptr || current_name == nullptr) {
        return std::nullopt;
    }

    // The SDK guarantees a returned LPCSTR only until the next string-returning
    // SDK call on the same thread. Copy every value before requesting another.
    const char* current_pointer = edit->get_object_item_value(
        object, L"LaTeX", current_name);
    std::optional<std::string> current;
    if (current_pointer != nullptr) {
        current.emplace(current_pointer);
        if (!current->empty() || legacy_name == nullptr) {
            return current;
        }
    }

    if (legacy_name != nullptr) {
        const char* legacy_pointer = edit->get_object_item_value(
            object, L"LaTeX", legacy_name);
        if (legacy_pointer != nullptr) {
            std::string legacy(legacy_pointer);
            if (!legacy.empty()) {
                if (legacy_used != nullptr) {
                    *legacy_used = true;
                }
                return legacy;
            }
        }
    }
    return current;
}

std::string object_handle_for_log(OBJECT_HANDLE object) {
    std::ostringstream stream;
    stream << "0x" << std::hex <<
        reinterpret_cast<std::uintptr_t>(object);
    return stream.str();
}

struct OwnedFontSettings {
    int mode = 0;
    std::string family_name;
    std::string file_path;
    std::string display_name = to_utf8_for_log(L"既定");
    bool needs_migration = false;
};

template<std::size_t Size>
std::optional<std::string> fixed_field_value(
    const std::array<char, Size>& field) {
    const auto end = std::find(field.begin(), field.end(), '\0');
    if (end == field.end()) return std::nullopt;
    return std::string(field.begin(), end);
}

template<std::size_t Size>
bool set_fixed_field(std::array<char, Size>& field, const std::string& value) {
    if (value.size() >= field.size()) return false;
    field.fill('\0');
    std::copy(value.begin(), value.end(), field.begin());
    return true;
}

std::optional<OwnedFontSettings> font_settings_from_data(
    const FontObjectData* data) {
    if (data == nullptr ||
        data->schema_version != FontObjectData::kSchemaVersion ||
        data->mode > 2) {
        return std::nullopt;
    }
    const auto family = fixed_field_value(data->fontspec_family_name);
    const auto file = fixed_field_value(data->font_file_path);
    const auto display = fixed_field_value(data->display_name);
    if (!family || !file || !display) return std::nullopt;
    OwnedFontSettings result;
    result.mode = data->mode;
    result.family_name = *family;
    result.file_path = *file;
    result.display_name = display->empty()
        ? to_utf8_for_log(L"既定") : *display;
    return result;
}

std::optional<FontObjectData> make_font_object_data(
    const OwnedFontSettings& settings) {
    if (settings.mode < 0 || settings.mode > 2) return std::nullopt;
    FontObjectData data;
    data.schema_version = FontObjectData::kSchemaVersion;
    data.mode = static_cast<unsigned char>(settings.mode);
    if (!set_fixed_field(data.fontspec_family_name, settings.family_name) ||
        !set_fixed_field(data.font_file_path, settings.file_path) ||
        !set_fixed_field(data.display_name, settings.display_name)) {
        return std::nullopt;
    }
    return data;
}

bool queue_font_object_data(
    std::int64_t object_id,
    const OwnedFontSettings& settings) {
    const auto data = make_font_object_data(settings);
    if (object_id < 0 || !data) return false;
    std::lock_guard state_lock(state_mutex);
    object_states[object_id].pending_font_data = *data;
    return true;
}

OwnedFontSettings read_owned_font_settings(
    EDIT_SECTION* edit,
    OBJECT_HANDLE object,
    std::int64_t object_id) {
    if (object_id >= 0) {
        std::lock_guard state_lock(state_mutex);
        const auto state = object_states.find(object_id);
        if (state != object_states.end() && state->second.pending_font_data) {
            if (const auto pending = font_settings_from_data(
                    &*state->second.pending_font_data)) {
                return *pending;
            }
        }
    }
    // FILTER_ITEM_BUTTON refreshes DATA::value for the focused object. Copy it
    // immediately and never retain the SDK-owned pointer past this callback.
    if (const auto hidden = font_settings_from_data(font_object_data.value)) {
        return *hidden;
    }

    OwnedFontSettings legacy;
    legacy.needs_migration = true;
    const auto mode = get_object_value_copy(edit, object, L"フォント指定");
    legacy.mode = resolve_japanese_font_mode(
        mode ? mode->c_str() : nullptr, 0);
    if (const auto family = get_object_value_copy(
            edit, object, L"フォント名", L"日本語フォント名")) {
        legacy.family_name = *family;
    }
    if (const auto file = get_object_value_copy(
            edit, object, L"フォントファイル", L"日本語フォントファイル")) {
        legacy.file_path = *file;
    }
    if (const auto display = get_object_value_copy(edit, object, L"使用中");
        display && !display->empty()) {
        legacy.display_name = *display;
    } else if (legacy.mode == 1 && !legacy.family_name.empty()) {
        legacy.display_name = legacy.family_name;
    } else if (legacy.mode == 2 && !legacy.file_path.empty()) {
        legacy.display_name = to_utf8_for_log(
            std::filesystem::path(from_utf8_for_object_value(
                legacy.file_path.c_str())).filename().wstring());
    }
    return legacy;
}

void request_environment_settings(EDIT_SECTION*) {
    if (host_edit_handle != nullptr) {
        set_dialog_owner(host_edit_handle->get_host_app_window());
    }
    show_environment_settings_dialog();
}

void request_information(EDIT_SECTION* edit) {
    if (host_edit_handle != nullptr) {
        set_dialog_owner(host_edit_handle->get_host_app_window());
    }
    InformationDialogSnapshot snapshot;
    OBJECT_HANDLE object = edit != nullptr ? edit->get_focus_object() : nullptr;
    const std::int64_t object_id = find_target_object_id(edit, object);
    if (object_id >= 0) {
        std::lock_guard state_lock(state_mutex);
        const auto found = object_states.find(object_id);
        if (found != object_states.end()) {
            snapshot.last_operation = found->second.last_operation;
        }
    }
    show_information_dialog(snapshot);
}

void request_font_selection(EDIT_SECTION* edit) {
    OBJECT_HANDLE object = edit != nullptr ? edit->get_focus_object() : nullptr;
    if (edit == nullptr || object == nullptr) {
        return;
    }
    const std::int64_t object_id = find_target_object_id(edit, object);
    const OwnedFontSettings previous =
        read_owned_font_settings(edit, object, object_id);
    if (host_edit_handle != nullptr) {
        set_dialog_owner(host_edit_handle->get_host_app_window());
    }
    const auto selected = show_system_font_dialog(
        from_utf8_for_object_value(previous.family_name.c_str()),
        previous.mode == 0);
    if (!selected) {
        return;
    }

    OwnedFontSettings next = previous;
    if (selected->use_default) {
        next.mode = 0;
        next.display_name = to_utf8_for_log(L"既定");
    } else {
        next.mode = 1;
        next.family_name = to_utf8_for_log(selected->fontspec_family_name);
        next.display_name = to_utf8_for_log(
            selected->display_name.empty()
                ? selected->fontspec_family_name
                : selected->display_name);
    }
    const bool data_queued = queue_font_object_data(object_id, next);
    const bool display_set = data_queued && edit->set_object_item_value(
        object, L"LaTeX", L"使用中", next.display_name.c_str());
    if (!display_set && data_queued) {
        queue_font_object_data(object_id, previous);
    }
    append_latest_log(
        "operation: font_selection\n"
        "font_selection_display_name: " + next.display_name + "\n" +
        "font_selection_fontspec_name: " +
            (next.mode == 1 ? next.family_name : "default") + "\n" +
        "font_selection_internal_mode: " +
            std::string(next.mode == 0 ? "Default\n" : "FontName\n") +
        "font_selection_target_object: " + object_handle_for_log(object) + "\n" +
        "font_hidden_data_queued: " +
            std::string(data_queued ? "yes\n" : "no\n") +
        "font_selection_display_set_result: " +
            std::string(display_set ? "success\n" : "failed\n") +
        "font_selection_set_result: " +
            std::string(display_set ? "success\n" : "failed\n"));
}

void request_font_file_selection(EDIT_SECTION* edit) {
    OBJECT_HANDLE object = edit != nullptr ? edit->get_focus_object() : nullptr;
    if (edit == nullptr || object == nullptr) {
        return;
    }
    const std::int64_t object_id = find_target_object_id(edit, object);
    const OwnedFontSettings previous =
        read_owned_font_settings(edit, object, object_id);
    if (host_edit_handle != nullptr) {
        set_dialog_owner(host_edit_handle->get_host_app_window());
    }
    const auto selected = show_font_file_dialog(
        std::filesystem::path(from_utf8_for_object_value(
            previous.file_path.c_str())));
    if (!selected) {
        return;
    }
    const std::string serialized_file = to_utf8_for_log(selected->wstring());
    const std::string display_file =
        to_utf8_for_log(selected->filename().wstring());
    OwnedFontSettings next = previous;
    next.mode = 2;
    next.file_path = serialized_file;
    next.display_name = display_file;
    const bool data_queued = queue_font_object_data(object_id, next);
    const bool display_updated = data_queued && edit->set_object_item_value(
        object, L"LaTeX", L"使用中", display_file.c_str());
    if (!display_updated && data_queued) {
        queue_font_object_data(object_id, previous);
    }
    append_latest_log(
        "operation: font_file_selection\n"
        "font_selection_display_name: " + display_file + "\n" +
        "font_selection_fontspec_name: not-applicable\n"
        "font_selection_internal_mode: FontFile\n"
        "font_selection_target_object: " + object_handle_for_log(object) + "\n" +
        "font_hidden_data_queued: " +
            std::string(data_queued ? "yes\n" : "no\n") +
        "font_selection_display_set_result: " +
            std::string(display_updated ? "success\n" : "failed\n") +
        "font_selection_set_result: " +
            std::string(display_updated ? "success\n" : "failed\n"));
}

int resolve_render_dpi(int selected_value) {
    switch (selected_value) {
    case 0:
        return 600;
    case 1:
        return 1200;
    case 2:
        return 2400;
    default:
        return 1200;
    }
}

int resolve_object_render_dpi(const std::string& serialized_value) {
    int value = 0;
    const auto conversion = std::from_chars(
        serialized_value.data(),
        serialized_value.data() + serialized_value.size(),
        value);
    if (conversion.ec != std::errc{} ||
        conversion.ptr != serialized_value.data() + serialized_value.size()) {
        return 1200;
    }
    switch (value) {
    case 600:
    case 1200:
    case 2400:
        return value;
    default:
        return 1200;
    }
}

double resolve_object_minipage_width(const std::string& serialized_value) {
    double value = 0.0;
    const auto conversion = std::from_chars(
        serialized_value.data(),
        serialized_value.data() + serialized_value.size(),
        value);
    if (conversion.ec != std::errc{} ||
        conversion.ptr != serialized_value.data() + serialized_value.size()) {
        return 0.0;
    }
    return value;
}

bool resolve_object_check(const char* serialized_value, bool fallback) {
    if (serialized_value == nullptr) {
        return fallback;
    }
    const std::string value(serialized_value);
    if (value == "1" || value == "true" ||
        value == to_utf8_for_log(L"ON")) {
        return true;
    }
    if (value == "0" || value == "false" ||
        value == to_utf8_for_log(L"OFF")) {
        return false;
    }
    return fallback;
}

int resolve_japanese_font_mode(const char* serialized_value, int fallback) {
    if (serialized_value == nullptr) {
        return fallback;
    }
    const std::string value(serialized_value);
    if (value == "0" || value == to_utf8_for_log(L"既定")) {
        return 0;
    }
    if (value == "1" || value == to_utf8_for_log(L"フォント名")) {
        return 1;
    }
    if (value == "2" || value == to_utf8_for_log(L"フォントファイル")) {
        return 2;
    }
    return fallback;
}

void commit_pending_font_data(std::int64_t object_id) {
    std::optional<FontObjectData> pending;
    {
        std::lock_guard state_lock(state_mutex);
        const auto state = object_states.find(object_id);
        if (state == object_states.end() || !state->second.pending_font_data) {
            return;
        }
        pending = state->second.pending_font_data;
    }
    if (font_object_data.value == nullptr || !pending) return;

    // filter2.h formally permits FILTER_ITEM_DATA updates from a filter
    // processing callback. The pointer is per-callback and is never retained.
    *font_object_data.value = *pending;
    bool committed = false;
    {
        std::lock_guard state_lock(state_mutex);
        auto& state = object_states[object_id];
        if (state.pending_font_data &&
            state.pending_font_data->schema_version == pending->schema_version &&
            state.pending_font_data->mode == pending->mode &&
            state.pending_font_data->fontspec_family_name ==
                pending->fontspec_family_name &&
            state.pending_font_data->font_file_path == pending->font_file_path &&
            state.pending_font_data->display_name == pending->display_name) {
            state.pending_font_data.reset();
            committed = true;
        }
    }
    if (committed) {
        append_latest_log(
            "operation: commit_font_object_data\n"
            "font_data_schema: 1\n"
            "font_data_update: success\n");
    }
}

void migrate_legacy_font_data_if_needed(
    FILTER_PROC_VIDEO* video,
    std::int64_t object_id) {
    if (video == nullptr || video->edit == nullptr || video->object == nullptr ||
        font_settings_from_data(font_object_data.value).has_value()) {
        return;
    }
    OBJECT_HANDLE object = video->edit->find_object(
        video->object->layer, video->object->frame_s);
    if (object == nullptr) return;
    const OBJECT_LAYER_FRAME position =
        video->edit->get_object_layer_frame(object);
    if (position.layer != video->object->layer ||
        position.start != video->object->frame_s ||
        position.end != video->object->frame_e) {
        return;
    }
    const OwnedFontSettings legacy =
        read_owned_font_settings(video->edit, object, object_id);
    const auto migrated = make_font_object_data(legacy);
    if (!migrated || font_object_data.value == nullptr) return;
    *font_object_data.value = *migrated;
    append_latest_log(
        "operation: migrate_legacy_font_settings\n"
        "font_data_schema: 1\n"
        "font_data_migration: success\n");
}

void commit_pending_persistent_key(std::int64_t object_id) {
    std::wstring pending_key;
    {
        std::lock_guard state_lock(state_mutex);
        const auto state = object_states.find(object_id);
        if (state == object_states.end() ||
            state->second.pending_persistent_key.empty()) {
            return;
        }
        pending_key = state->second.pending_persistent_key;
    }

    // filter2.h explicitly permits updating FILTER_ITEM_DATA::value from a
    // filter processing callback. Do not retain this per-object pointer after
    // the callback returns.
    if (!store_persistent_key(persistent_object_data.value, pending_key)) {
        return;
    }

    bool committed = false;
    {
        std::lock_guard state_lock(state_mutex);
        auto& state = object_states[object_id];
        if (state.pending_persistent_key == pending_key) {
            state.pending_persistent_key.clear();
            state.restore_attempted = true;
            state.restored_cache_key = pending_key;
            state.restore_result = PersistentRenderCacheStatus::Success;
            committed = true;
        }
    }
    if (committed) {
        append_latest_log(
            "operation: commit_persistent_render_cache_key\n"
            "last_successful_cache_key: " +
                to_utf8_for_log(pending_key) + "\n"
            "last_successful_cache_key_update: success\n");
    }
}

void restore_object_images_from_saved_cache(std::int64_t object_id) {
    const std::wstring saved_key =
        persistent_key_from_data(persistent_object_data.value);
    {
        std::lock_guard state_lock(state_mutex);
        auto& state = object_states[object_id];
        if (state.images || state.restore_attempted) {
            return;
        }
        // Claim this one-time initialization before releasing the mutex for
        // disk I/O. Concurrent frame evaluations will use the transparent
        // fallback until this attempt completes instead of opening the files.
        state.restore_attempted = true;
        state.restored_cache_key = saved_key;
        state.restore_result = saved_key.empty()
            ? PersistentRenderCacheStatus::NotFound
            : PersistentRenderCacheStatus::IoError;
    }

    if (saved_key.empty()) {
        return;
    }

    AppPaths paths;
    std::wstring path_error;
    std::string cache_error;
    PersistentRenderCacheStatus cache_status =
        PersistentRenderCacheStatus::IoError;
    PersistentRenderCacheSnapshot snapshot;
    bool restored = false;
    if (resolve_app_paths(paths, path_error)) {
        restored = load_persistent_render_cache(
            paths.cache_root,
            saved_key,
            snapshot,
            cache_error,
            &cache_status);
    } else {
        cache_error = to_utf8_for_log(path_error);
    }

    std::shared_ptr<const ImageList> restored_images;
    std::shared_ptr<const ImageList> restored_layers;
    if (restored) {
        try {
            restored_images = std::make_shared<const ImageList>(
                std::move(snapshot.cumulative_images));
            restored_layers = std::make_shared<const ImageList>(
                std::move(snapshot.step_layers));
        } catch (const std::bad_alloc&) {
            restored = false;
            cache_status = PersistentRenderCacheStatus::OutOfMemory;
            cache_error = "not enough memory to publish restored images";
        }
    }

    bool published = false;
    {
        std::lock_guard state_lock(state_mutex);
        auto& state = object_states[object_id];
        state.restore_result = cache_status;
        // A successful manual compile may have published newer images while
        // disk loading was in progress. Never overwrite that result.
        if (restored && !state.images &&
            state.restored_cache_key == saved_key) {
            state.images = std::move(restored_images);
            state.step_layers = std::move(restored_layers);
            state.common_transparent = std::move(snapshot.transparent_image);
            state.compiled_render_dpi = snapshot.metadata.render_dpi;
            state.colored_source.reset();
            state.colored_image.reset();
            published = true;
        }
    }

    append_latest_log(
        "operation: restore_persistent_render_cache\n"
        "persistent_cache_version: " +
            std::string(kPersistentRenderCacheVersion) + "\n" +
        "restore_cache_key: " + to_utf8_for_log(saved_key) + "\n" +
        "restore_result: " +
            std::string(persistent_render_cache_status_name(cache_status)) + "\n" +
        "restore_images_published: " +
            std::string(published ? "yes\n" : "no\n") +
        "restore_external_processes_started: no\n" +
        "restore_error: " +
            (cache_error.empty() ? std::string("none\n") : cache_error + "\n"));
}

void request_compile(EDIT_SECTION* edit) {
    OBJECT_HANDLE object = edit != nullptr ? edit->get_focus_object() : nullptr;
    const std::int64_t object_id = find_target_object_id(edit, object);
    // The button callback contract refreshes every FILTER_ITEM value for the
    // focused object. Keep this pointer only for this synchronous callback so
    // later video evaluations cannot redirect the shared item descriptor.
    PersistentObjectData* const target_persistent_data =
        persistent_object_data.value;
    const char* object_dpi_value = edit != nullptr && object != nullptr
        ? edit->get_object_item_value(object, L"LaTeX", L"描画DPI")
        : nullptr;
    const std::string dpi_item_raw_value = object_dpi_value != nullptr
        ? object_dpi_value
        : std::to_string(render_dpi.value);
    const int selected_dpi = object_dpi_value != nullptr
        ? resolve_object_render_dpi(dpi_item_raw_value)
        : resolve_render_dpi(render_dpi.value);
    const char* object_template_value = edit != nullptr && object != nullptr
        ? edit->get_object_item_value(object, L"LaTeX", L"テンプレート")
        : nullptr;
    const LatexTemplate selected_template = object_template_value != nullptr
        ? LatexTemplateBuilder::from_serialized_value(
            object_template_value, template_selection.value)
        : LatexTemplateBuilder::from_selection(template_selection.value);
    const std::wstring selected_template_name =
        LatexTemplateBuilder::name(selected_template);
    const char* object_tikz_libraries_value = edit != nullptr && object != nullptr
        ? edit->get_object_item_value(object, L"LaTeX", L"ライブラリ")
        : nullptr;
    const std::string tikz_libraries_raw = object_tikz_libraries_value != nullptr
        ? object_tikz_libraries_value
        : to_utf8_for_log(
            tikz_libraries.value != nullptr ? tikz_libraries.value : L"");
    const TikzConfiguration tikz_configuration =
        build_tikz_configuration(selected_template, tikz_libraries_raw);
    const char* object_minipage_width_value = nullptr;
    if (edit != nullptr && object != nullptr) {
        object_minipage_width_value =
            edit->get_object_item_value(object, L"LaTeX", L"幅");
        if (object_minipage_width_value == nullptr) {
            object_minipage_width_value = edit->get_object_item_value(
                object, L"LaTeX", L"minipage幅(cm)");
        }
        if (object_minipage_width_value == nullptr) {
            object_minipage_width_value = edit->get_object_item_value(
                object,
                L"LaTeX",
                L"minipage幅(cm)（documentのみ、0で無効）");
        }
    }
    const double current_minipage_width = object_minipage_width_value != nullptr
        ? resolve_object_minipage_width(object_minipage_width_value)
        : minipage_width.value;
    const char* object_paragraph_alignment_value = nullptr;
    if (edit != nullptr && object != nullptr) {
        object_paragraph_alignment_value =
            edit->get_object_item_value(object, L"LaTeX", L"揃え");
        if (object_paragraph_alignment_value == nullptr) {
            object_paragraph_alignment_value = edit->get_object_item_value(
                object, L"LaTeX", L"段落揃え");
        }
        if (object_paragraph_alignment_value == nullptr) {
            object_paragraph_alignment_value = edit->get_object_item_value(
                object,
                L"LaTeX",
                L"段落揃え（documentのminipageのみ有効）");
        }
    }
    const std::string paragraph_alignment_raw =
        object_paragraph_alignment_value != nullptr
        ? object_paragraph_alignment_value
        : "(missing)";
    const ParagraphAlignment selected_paragraph_alignment =
        object_paragraph_alignment_value != nullptr
        ? paragraph_alignment_from_serialized_value(paragraph_alignment_raw)
        : paragraph_alignment_from_selection(paragraph_alignment.value);
    const DocumentLayoutConfiguration layout_configuration =
        resolve_document_layout(
            current_minipage_width,
            selected_paragraph_alignment);
    const bool automatic_minipage_enabled =
        selected_template == LatexTemplate::Document &&
        layout_configuration.minipage_enabled();
    const char* object_japanese_spacing_value = edit != nullptr && object != nullptr
        ? edit->get_object_item_value(object, L"LaTeX", L"和文字間")
        : nullptr;
    const std::string japanese_spacing_raw =
        object_japanese_spacing_value != nullptr
        ? object_japanese_spacing_value
        : "(missing)";
    const bool legacy_custom_spacing_fallback =
        is_legacy_custom_spacing_value(japanese_spacing_raw);
    const JapaneseSpacingMode selected_japanese_spacing =
        object_japanese_spacing_value != nullptr
        ? japanese_spacing_mode_from_serialized_value(japanese_spacing_raw)
        : japanese_spacing_mode_from_selection(japanese_spacing.value);
    const auto object_japanese_enabled_value = get_object_value_copy(
        edit, object, L"日本語対応");
    const OwnedFontSettings owned_font_settings =
        read_owned_font_settings(edit, object, object_id);
    const bool font_data_migration_queued =
        owned_font_settings.needs_migration &&
        queue_font_object_data(object_id, owned_font_settings);
    const bool selected_japanese_enabled = resolve_object_check(
        object_japanese_enabled_value.has_value()
            ? object_japanese_enabled_value->c_str()
            : nullptr,
        japanese_enabled.value);
    const int selected_japanese_font_mode = owned_font_settings.mode;
    const std::wstring selected_japanese_font_name =
        from_utf8_for_object_value(owned_font_settings.family_name.c_str());
    const std::wstring selected_japanese_font_file =
        from_utf8_for_object_value(owned_font_settings.file_path.c_str());
    const JapaneseDocumentConfiguration japanese_configuration =
        build_japanese_document_configuration(
            selected_template,
            selected_japanese_enabled,
            selected_japanese_font_mode,
            selected_japanese_spacing,
            selected_japanese_font_name,
            selected_japanese_font_file);
    const bool japanese_font_applied =
        selected_template == LatexTemplate::Document &&
        japanese_configuration.enabled && japanese_configuration.valid();
    const bool latin_font_applied =
        japanese_font_applied &&
        japanese_configuration.font_mode != JapaneseFontMode::Default;
    const std::wstring text_font_name =
        latin_font_applied &&
            japanese_configuration.font_mode == JapaneseFontMode::FontName
        ? japanese_configuration.font_name
        : std::wstring{};
    const std::wstring text_font_file =
        latin_font_applied &&
            japanese_configuration.font_mode == JapaneseFontMode::FontFile
        ? japanese_configuration.font_file
        : std::wstring{};
    std::wstring font_display_name =
        from_utf8_for_object_value(owned_font_settings.display_name.c_str());
    if (font_display_name.empty()) {
        if (japanese_configuration.font_mode == JapaneseFontMode::FontName &&
            !japanese_configuration.font_name.empty()) {
            font_display_name = japanese_configuration.font_name;
        } else if (
            japanese_configuration.font_mode == JapaneseFontMode::FontFile &&
            !japanese_configuration.font_file.empty()) {
            font_display_name = std::filesystem::path(
                japanese_configuration.font_file).filename().wstring();
        } else {
            font_display_name = L"既定";
        }
    }
    const bool font_setting_fallback_used = owned_font_settings.needs_migration;
    const std::string font_setting_fallback_reason =
        owned_font_settings.needs_migration
        ? (font_data_migration_queued
            ? "legacy_values_queued_for_hidden_data"
            : "legacy_values_hidden_data_queue_failed")
        : "none";
    const std::wstring japanese_spacing_effective_log =
        selected_template != LatexTemplate::Document ||
            !japanese_configuration.enabled
        ? L"ignored"
        : japanese_spacing_effective_name(japanese_configuration);

    ToolSettings tool_settings;
    std::wstring tool_settings_error;
    const bool tool_settings_loaded =
        load_tool_settings(tool_settings, tool_settings_error);
    const ResolvedTools resolved_tools = resolve_external_tools(tool_settings);
    const RenderTools render_tools{
        resolved_tools.lualatex_path,
        resolved_tools.mutool_path
    };

    CompileStatusScope compile_status_scope(
        edit,
        object,
        object_id,
        selected_template_name,
        selected_dpi,
        font_display_name,
        japanese_configuration.font_file);
    int compiled_render_dpi_before = 0;
    if (object_id >= 0) {
        std::lock_guard state_lock(state_mutex);
        const auto state = object_states.find(object_id);
        if (state != object_states.end()) {
            compiled_render_dpi_before = state->second.compiled_render_dpi;
        }
    }
    reset_latest_log(
        "compile_trigger: manual\n"
        "execution_mode: synchronous\n"
        "button_callback_started\n"
        "status_display: " + to_utf8_for_log(L"コンパイル中") + "\n" +
        "dpi_item_raw_value: " + dpi_item_raw_value + "\n"
        "selected_render_dpi: " + std::to_string(selected_dpi) + "\n"
        "compiled_render_dpi_before: " +
            std::to_string(compiled_render_dpi_before) + "\n"
        "requested_render_dpi: " + std::to_string(selected_dpi) + "\n"
        "render_dpi: " + std::to_string(selected_dpi) + "\n"
        "lualatex_source: " +
            to_utf8_for_log(resolved_tools.lualatex_source) + "\n"
        "mutool_source: " +
            to_utf8_for_log(resolved_tools.mutool_source) + "\n"
        "lualatex_path: " +
            to_utf8_for_log(resolved_tools.lualatex_path.wstring()) + "\n"
        "mutool_path: " +
            to_utf8_for_log(resolved_tools.mutool_path.wstring()) + "\n"
        "settings_warning: " +
            (tool_settings_error.empty()
                ? std::string("none\n")
                : to_utf8_for_log(tool_settings_error) + "\n") +
        "selected_template: " + to_utf8_for_log(selected_template_name) + "\n"
        "template_id: " +
            std::to_string(LatexTemplateBuilder::id(selected_template)) + "\n"
        "template_default: inline_math\n"
        "tikz_enabled: " +
            std::string(selected_template == LatexTemplate::TikzPicture
                ? "yes\n"
                : "no\n") +
        "tikz_libraries_raw: " + tikz_configuration.raw_libraries + "\n" +
        "tikz_libraries_normalized: " +
            tikz_configuration.normalized_libraries + "\n" +
        "tikz_library_count: " +
            std::to_string(tikz_configuration.library_count) + "\n" +
        "tikz_render_mode: " +
            std::string(selected_template == LatexTemplate::TikzPicture
                ? "monochrome_alpha_mask\n"
                : "not-applicable\n") +
        "tikz_source_colors_preserved: " +
            std::string(selected_template == LatexTemplate::TikzPicture
                ? "no\n"
                : "not-applicable\n") +
        "minipage_width_raw: " +
            format_raw_number(layout_configuration.raw_minipage_width_cm) + "\n" +
        "minipage_width_cm: " +
            to_utf8_for_log(layout_configuration.formatted_minipage_width_cm) + "\n" +
        "minipage_enabled: " +
            std::string(automatic_minipage_enabled
                ? "yes\n"
                : "no\n") +
        "paragraph_alignment_raw: " + paragraph_alignment_raw + "\n"
        "paragraph_alignment: " +
            paragraph_alignment_name(selected_paragraph_alignment) + "\n"
        "automatic_minipage_enabled: " +
            std::string(automatic_minipage_enabled ? "yes\n" : "no\n") +
        "ragged2e_enabled: " +
            std::string(automatic_minipage_enabled ? "yes\n" : "no\n") +
        "paragraph_alignment_command: " +
            (automatic_minipage_enabled
                ? to_utf8_for_log(paragraph_alignment_command(
                    selected_paragraph_alignment)) + "\n"
                : std::string("not-applicable\n")) +
        "removed_templates: gather*,multline*\n"
        "inline_math_wrapper: " +
            std::string(selected_template == LatexTemplate::InlineMath
                ? "\\( ... \\)\n"
                : "not-applicable\n") +
        "inline_displaystyle_added: no\n"
        "japanese_enabled: " +
            std::string(japanese_configuration.enabled ? "yes\n" : "no\n") +
        "japanese_font_mode: " +
            to_utf8_for_log(japanese_font_mode_name(
                japanese_configuration.font_mode)) + "\n" +
        "japanese_font_name: " +
            to_utf8_for_log(japanese_configuration.font_name) + "\n" +
        "japanese_font_file: " +
            to_utf8_for_log(japanese_configuration.font_file) + "\n" +
        "spacing_item_raw_value: " + japanese_spacing_raw + "\n"
        "spacing_item_value_source: " +
            std::string(object_japanese_spacing_value != nullptr
                ? "object_api\n"
                : "filter_item_fallback\n") +
        "spacing_parsed_mode: " +
            to_utf8_for_log(japanese_spacing_mode_name(
                selected_japanese_spacing)) + "\n" +
        "spacing_requested_mode: " +
            to_utf8_for_log(japanese_spacing_mode_name(
                japanese_configuration.spacing_requested)) + "\n" +
        "spacing_effective_mode: " +
            to_utf8_for_log(japanese_spacing_effective_log) + "\n" +
        "spacing_legacy_custom_fallback: " +
            std::string(legacy_custom_spacing_fallback ? "auto\n" : "no\n") +
        "japanese_spacing_raw: " + japanese_spacing_raw + "\n"
        "japanese_spacing_requested: " +
            to_utf8_for_log(japanese_spacing_mode_name(
                japanese_configuration.spacing_requested)) + "\n" +
        "japanese_spacing_effective: " +
            to_utf8_for_log(japanese_spacing_effective_log) + "\n" +
        "japanese_jfm: " +
            to_utf8_for_log(japanese_configuration.spacing_jfm) + "\n" +
        "japanese_kerning: " +
            to_utf8_for_log(japanese_configuration.spacing_kerning) + "\n" +
        "japanese_palt: " +
            std::string(japanese_configuration.spacing_palt ? "on\n" : "off\n") +
        "japanese_kern_feature: " +
            std::string(japanese_configuration.spacing_kern_feature
                ? "on\n"
                : "off\n") +
        "generated_jfm: " +
            to_utf8_for_log(japanese_configuration.spacing_jfm) + "\n" +
        "generated_kerning: " +
            to_utf8_for_log(japanese_configuration.spacing_kerning) + "\n" +
        "generated_script: " +
            to_utf8_for_log(japanese_configuration.generated_script) + "\n" +
        "generated_raw_features: " +
            to_utf8_for_log(
                japanese_configuration.generated_raw_features) + "\n" +
        "generated_setmainfont:\n" +
            (japanese_configuration.generated_setmainfont.empty()
                ? std::string("not-generated\n")
                : to_utf8_for_log(
                    japanese_configuration.generated_setmainfont)) +
        "generated_setmainjfont:\n" +
            (japanese_configuration.generated_setmainjfont.empty()
                ? std::string("not-generated\n")
                : to_utf8_for_log(
                    japanese_configuration.generated_setmainjfont)) +
        "font_setting_fallback_used: " +
            std::string(font_setting_fallback_used ? "yes\n" : "no\n") +
        "font_setting_fallback_reason: " +
            font_setting_fallback_reason + "\n" +
        "font_mode: " +
            to_utf8_for_log(japanese_font_mode_name(
                japanese_configuration.font_mode)) + "\n" +
        "font_source_mode: " +
            to_utf8_for_log(japanese_font_mode_name(
                japanese_configuration.font_mode)) + "\n" +
        "font_display_name: " + to_utf8_for_log(font_display_name) + "\n" +
        "fontspec_family_name: " +
            to_utf8_for_log(japanese_configuration.font_name) + "\n" +
        "font_file_path: " +
            to_utf8_for_log(japanese_configuration.font_file) + "\n" +
        "font_name: " +
            to_utf8_for_log(japanese_configuration.font_name) + "\n" +
        "font_file: " +
            to_utf8_for_log(japanese_configuration.font_file) + "\n" +
        "normalized_font_directory: " +
            to_utf8_for_log(japanese_configuration.normalized_font_directory) + "\n" +
        "normalized_font_filename: " +
            to_utf8_for_log(japanese_configuration.normalized_font_filename) + "\n" +
        "font_file_exists: " +
            std::string(japanese_configuration.font_file_exists ? "yes\n" : "no\n") +
        "font_file_size: " +
            std::to_string(japanese_configuration.font_file_size) + "\n" +
        "font_file_last_write_time: " +
            to_utf8_for_log(japanese_configuration.font_file_last_write_time) + "\n" +
        "latin_font_applied: " +
            std::string(latin_font_applied ? "yes\n" : "no\n") +
        "japanese_font_applied: " +
            std::string(japanese_font_applied ? "yes\n" : "no\n") +
        "text_font_name: " + to_utf8_for_log(text_font_name) + "\n" +
        "text_font_file: " + to_utf8_for_log(text_font_file) + "\n" +
        "document_display_spacing_enabled: " +
            std::string(selected_template == LatexTemplate::Document
                ? "yes\n"
                : "no\n") +
        "minipage_display_spacing_hook: " +
            std::string(selected_template == LatexTemplate::Document
                ? "env/minipage/begin\n"
                : "not-applicable\n") +
        "display_above_skip: " +
            std::string(selected_template == LatexTemplate::Document
                ? "0.5baselineskip\n"
                : "not-applicable\n") +
        "display_below_skip: " +
            std::string(selected_template == LatexTemplate::Document
                ? "0.5baselineskip\n"
                : "not-applicable\n") +
        "template_cache_version: " +
            std::string(selected_template == LatexTemplate::AlignStar
                ? kAlignRelationSpacingCacheVersion
                : kTemplateCacheVersion) + "\n" +
        "cache_version: " +
            std::string(selected_template == LatexTemplate::AlignStar
                ? kAlignRelationSpacingCacheVersion
                : kTemplateCacheVersion) + "\n" +
        "cache_key_contains_dpi: yes\n");
    if (object_id < 0) {
        append_latest_log(
            "step_count: 0\n"
            "render_success: no\n"
            "failed_stage: target_object\n"
            "failed_step: target_object\n"
            "image_list_published: no\n"
            "compiled_render_dpi_after: " +
                std::to_string(compiled_render_dpi_before) + "\n"
            "compile_button_finished: yes\n");
        return;
    }
    if (!tool_settings_loaded) {
        append_latest_log(
            "failed_stage: settings_load\n"
            "error_summary: " + to_utf8_for_log(tool_settings_error) + "\n"
            "render_success: no\n"
            "image_list_published: no\n"
            "compile_button_finished: yes\n");
        return;
    }
    if (resolved_tools.lualatex_path.empty()) {
        append_latest_log(
            "failed_stage: lualatex_not_found\n"
            "error_summary: LuaLaTeX is not configured or detected\n"
            "render_success: no\n"
            "image_list_published: no\n"
            "compile_button_finished: yes\n");
        return;
    }
    if (resolved_tools.mutool_path.empty()) {
        append_latest_log(
            "failed_stage: mutool_not_found\n"
            "error_summary: mutool is not configured or detected\n"
            "render_success: no\n"
            "image_list_published: no\n"
            "compile_button_finished: yes\n");
        return;
    }
    if (!japanese_configuration.valid()) {
        append_latest_log(
            "failed_stage: japanese_font_configuration\n"
            "error_message: " +
                to_utf8_for_log(japanese_configuration.error_message) + "\n" +
            "render_success: no\n"
            "image_list_published: no\n"
            "compiled_render_dpi_after: " +
                std::to_string(compiled_render_dpi_before) + "\n"
            "compile_button_finished: yes\n");
        return;
    }
    if (!tikz_configuration.valid()) {
        append_latest_log(
            "failed_stage: tikz_library_configuration\n"
            "error_message: " + tikz_configuration.error_message + "\n"
            "render_success: no\n"
            "image_list_published: no\n"
            "compiled_render_dpi_after: " +
                std::to_string(compiled_render_dpi_before) + "\n"
            "compile_button_finished: yes\n");
        return;
    }

    const std::wstring source = latex_source.value != nullptr ? latex_source.value : L"";
    const std::vector<std::wstring> steps = split_steps(source);
    std::wstring persistent_cache_identity =
        L"template-id=" +
        std::to_wstring(LatexTemplateBuilder::id(selected_template)) +
        L"\ntemplate-name=" + selected_template_name +
        L"\nrender-dpi=" + std::to_wstring(selected_dpi) +
        L"\nstep-count=" + std::to_wstring(steps.size()) +
        L"\nsource=" + source;
    if (selected_template == LatexTemplate::AlignStar) {
        append_latest_log(
            "align_relation_spacing_fix_enabled: yes\n"
            "align_relation_spacing_replacement_count: 0\n"
            "align_relation_spacing_operators: none (wrapper-only fix)\n"
            "align_source_before_spacing_fix:\n" + to_utf8_for_log(source) + "\n"
            "align_source_after_spacing_fix:\n" + to_utf8_for_log(source) + "\n");
    }
    append_latest_log(
        "step_count: " + std::to_string(steps.size()) + "\n" +
        "tikz_step_count: " +
            std::string(selected_template == LatexTemplate::TikzPicture
                ? std::to_string(steps.size())
                : "not-applicable") + "\n");
    if (steps.size() > kMaximumStepCount) {
        append_latest_log(
            "render_success: no\n"
            "failed_stage: step_count_limit\n"
            "failed_step: step_count_limit\n"
            "image_list_published: no\n"
            "compiled_render_dpi_after: " +
                std::to_string(compiled_render_dpi_before) + "\n"
            "compile_button_finished: yes\n");
        return;
    }

    auto raw_step_layers = std::make_shared<ImageList>();
    raw_step_layers->reserve(steps.size());
    std::size_t failed_step = 0;
    bool all_layers_from_cache = true;

    for (std::size_t index = 0; index < steps.size(); ++index) {
        const std::wstring layer_document =
            LatexTemplateBuilder::build_step_layer_document(
                selected_template,
                steps,
                index,
                japanese_configuration,
                layout_configuration,
                tikz_configuration);
        std::wstring document_layout_cache_material;
        if (selected_template == LatexTemplate::Document) {
            document_layout_cache_material = L"minipage_width_cm=" +
                layout_configuration.formatted_minipage_width_cm;
            if (layout_configuration.minipage_enabled()) {
                const std::string alignment_name =
                    paragraph_alignment_name(selected_paragraph_alignment);
                document_layout_cache_material +=
                    L"\nparagraph-alignment=" +
                    std::wstring(alignment_name.begin(), alignment_name.end());
            }
        }
        const char* template_cache_version =
            selected_template == LatexTemplate::AlignStar
            ? kAlignRelationSpacingCacheVersion
            : kTemplateCacheVersion;
        const std::string layer_cache_version =
            std::string(template_cache_version) + "-template-" +
            std::to_string(LatexTemplateBuilder::id(selected_template)) + "-" +
            to_utf8_for_log(selected_template_name) + "-step-" +
            std::to_string(index + 1) + "-" +
            to_utf8_for_log(japanese_configuration.cache_material) + "-" +
            to_utf8_for_log(document_layout_cache_material) + "-" +
            to_utf8_for_log(tikz_configuration.cache_material);
        const std::wstring cache_identity =
            source +
            L"\n" + widen_ascii(template_cache_version) + L"\ntemplate_id=" +
            std::to_wstring(LatexTemplateBuilder::id(selected_template)) +
            L"\ntemplate_name=" + selected_template_name + L"\ntarget_step=" +
            std::to_wstring(index + 1) + L"\nrender-dpi=" +
            std::to_wstring(selected_dpi) + L"\n" +
            japanese_configuration.cache_material + L"\n" +
            document_layout_cache_material + L"\n" +
            tikz_configuration.cache_material;
        const std::wstring cache_key =
            make_render_cache_key(layer_document, layer_cache_version, selected_dpi);
        persistent_cache_identity +=
            L"\nstep-layer-" + std::to_wstring(index + 1) + L"=" + cache_key;
        const bool cache_key_contains_spacing =
            cache_identity.find(L"japanese-spacing-requested=") !=
            std::wstring::npos;
        ImagePointer image;
        bool invalid_cached_image = false;
        append_latest_log(
            "\n[Generated " + to_utf8_for_log(selected_template_name) +
            " step layer " + std::to_string(index + 1) + "]\n" +
            to_utf8_for_log(layer_document) + "\n");
        {
            std::lock_guard state_lock(state_mutex);
            auto state = object_states.find(object_id);
            if (state != object_states.end()) {
                auto cached = state->second.source_cache.find(cache_identity);
                if (cached != state->second.source_cache.end()) {
                    if (is_valid_cached_image(cached->second)) {
                        image = cached->second;
                    } else {
                        state->second.source_cache.erase(cached);
                        invalid_cached_image = true;
                    }
                }
            }
        }

        if (invalid_cached_image) {
            append_latest_log(
                "cache_invalid: yes\n"
                "cache_fallback: regenerate\n");
        }

        append_latest_log(
            "step_index: " + std::to_string(index + 1) + "\n" +
            "step_layer_source_hash: " + narrow_hash(cache_key) + "\n" +
            "cache_key:\n" + to_utf8_for_log(cache_identity) + "\n" +
            "cache_hash: " + to_utf8_for_log(cache_key) +
                "|render-dpi=" + std::to_string(selected_dpi) + "\n" +
            "cache_key_contains_dpi: yes\n" +
            "cache_key_contains_spacing: " +
                (cache_key_contains_spacing ? "yes\n" : "no\n") +
            "cache_used: " + (image ? "yes\n" : "no\n"));

        if (!image) {
            RenderedImage rendered;
            std::uint64_t rendered_nonzero_alpha_pixels = 0;
            bool image_decoded = false;
            bool disk_cache_used = false;
            const bool succeeded = render_latex(
                layer_document,
                rendered,
                layer_cache_version,
                selected_dpi,
                render_tools,
                false,
                kHiddenLayerAlphaThreshold,
                &rendered_nonzero_alpha_pixels,
                &image_decoded,
                &disk_cache_used);
            if (!disk_cache_used) {
                all_layers_from_cache = false;
            }
            append_latest_log(
                std::string("render_success: ") + (succeeded ? "yes\n" : "no\n"));
            if (!succeeded) {
                if (image_decoded && rendered_nonzero_alpha_pixels == 0) {
                    append_latest_log(
                        "failed_stage: empty_step_layer\n"
                        "template: " + to_utf8_for_log(selected_template_name) + "\n" +
                        "step_index: " + std::to_string(index + 1) + "\n"
                        "generated_tex:\n" + to_utf8_for_log(layer_document) + "\n");
                }
                failed_step = index + 1;
                break;
            }
            image = std::make_shared<const RenderedImage>(std::move(rendered));
            {
                std::lock_guard state_lock(state_mutex);
                object_states[object_id].source_cache[cache_identity] = image;
            }
        } else {
            append_latest_log("render_success: yes\n");
        }
        raw_step_layers->push_back(std::move(image));
    }

    if (failed_step != 0) {
        append_latest_log(
            "failed_stage: step_render\n"
            "failed_step: " + std::to_string(failed_step) + "\n" +
            "image_list_published: no\n"
            "compiled_render_dpi_after: " +
                std::to_string(compiled_render_dpi_before) + "\n"
            "compile_button_finished: yes\n");
        return;
    }

    std::shared_ptr<ImageList> common_step_layers;
    ImagePointer common_transparent;
    int original_canvas_width = 0;
    int original_canvas_height = 0;
    ContentBounds global_content_bounds;
    int padding_px = 0;
    int final_canvas_width = 0;
    int final_canvas_height = 0;
    std::vector<LayerCropStats> layer_stats;
    const bool layers_created = create_globally_cropped_layers(
        *raw_step_layers,
        selected_dpi,
        common_step_layers,
        common_transparent,
        original_canvas_width,
        original_canvas_height,
        global_content_bounds,
        padding_px,
        final_canvas_width,
        final_canvas_height,
        layer_stats);
    std::shared_ptr<ImageList> cumulative_images;
    const bool cumulative_created = layers_created &&
        create_cumulative_images(
            *common_step_layers, common_transparent, cumulative_images);
    append_latest_log(
        "step_layer_count: " + std::to_string(raw_step_layers->size()) + "\n" +
        "layout_mode: always_top_anchored\n" +
        "template: " + to_utf8_for_log(selected_template_name) + "\n" +
        "render_dpi: " + std::to_string(selected_dpi) + "\n" +
        "original_canvas_width: " + std::to_string(original_canvas_width) + "\n" +
        "original_canvas_height: " + std::to_string(original_canvas_height) + "\n" +
        "global_content_left: " + std::to_string(global_content_bounds.left) + "\n" +
        "global_content_top: " + std::to_string(global_content_bounds.top) + "\n" +
        "global_content_right: " + std::to_string(global_content_bounds.right) + "\n" +
        "global_content_bottom: " + std::to_string(global_content_bounds.bottom) + "\n" +
        "padding_px: " + std::to_string(padding_px) + "\n" +
        "final_canvas_width: " + std::to_string(final_canvas_width) + "\n" +
        "final_canvas_height: " + std::to_string(final_canvas_height) + "\n" +
        "common_canvas_width: " + std::to_string(final_canvas_width) + "\n" +
        "common_canvas_height: " + std::to_string(final_canvas_height) + "\n"
        "layer_generation_method: " +
            std::string(selected_template == LatexTemplate::TikzPicture
                ? "tikz-opacity-scope-full-layout\n"
                : "white-hidden-full-layout\n") +
        "cache_version: " +
            std::string(selected_template == LatexTemplate::AlignStar
                ? kAlignRelationSpacingCacheVersion
                : kTemplateCacheVersion) + "\n");
    if (!layers_created && !global_content_bounds.valid()) {
        append_latest_log(
            "failed_stage: global_content_bounds\n"
            "result: no step layer contained alpha above the content threshold\n");
    }
    if (!cumulative_created) {
        append_latest_log(
            "render_success: no\n"
            "failed_stage: fixed_layout_composition\n"
            "failed_step: fixed_layout_composition\n"
            "image_list_published: no\n"
            "compiled_render_dpi_after: " +
                std::to_string(compiled_render_dpi_before) + "\n"
            "compile_button_finished: yes\n");
        return;
    }

    for (std::size_t index = 0; index < layer_stats.size(); ++index) {
        const auto& stats = layer_stats[index];
        append_latest_log(
            "step_index: " + std::to_string(index + 1) + "\n" +
            "original_layer_bounds: " +
            (stats.original_bounds.valid()
                ? std::to_string(stats.original_bounds.left) + "," +
                    std::to_string(stats.original_bounds.top) + "," +
                    std::to_string(stats.original_bounds.right) + "," +
                    std::to_string(stats.original_bounds.bottom) + "\n"
                : std::string("none\n")));
        append_latest_log(
            "cropped_destination_x: " +
                std::to_string(stats.cropped_destination_x) + "\n" +
            "cropped_destination_y: " +
                std::to_string(stats.cropped_destination_y) + "\n" +
            "nonzero_alpha_pixels: " +
                std::to_string(stats.nonzero_alpha_pixels) + "\n");
    }

    const std::wstring previous_persistent_key =
        persistent_key_from_data(target_persistent_data);
    const std::wstring persistent_cache_key =
        make_persistent_render_cache_key(persistent_cache_identity);
    AppPaths app_paths;
    std::wstring persistent_path_error;
    std::string persistent_cache_error;
    PersistentRenderCacheStatus persistent_cache_status =
        PersistentRenderCacheStatus::IoError;
    bool persistent_cache_saved = false;
    if (resolve_app_paths(app_paths, persistent_path_error) &&
        ensure_runtime_directories(app_paths, persistent_path_error)) {
        persistent_cache_saved = save_persistent_render_cache(
            app_paths.cache_root,
            PersistentRenderCacheMetadata{
                persistent_cache_key,
                to_utf8_for_log(selected_template_name),
                selected_dpi
            },
            *common_step_layers,
            persistent_cache_error,
            &persistent_cache_status);
    } else {
        persistent_cache_status = PersistentRenderCacheStatus::IoError;
        persistent_cache_error = to_utf8_for_log(persistent_path_error);
    }
    const bool persistent_key_queued = persistent_cache_saved;
    append_latest_log(
        "persistent_cache_version: " +
            std::string(kPersistentRenderCacheVersion) + "\n" +
        "persistent_cache_key: " +
            to_utf8_for_log(persistent_cache_key) + "\n" +
        "last_successful_cache_key_before: " +
            (previous_persistent_key.empty()
                ? std::string("none\n")
                : to_utf8_for_log(previous_persistent_key) + "\n") +
        "persistent_cache_save: " +
            std::string(persistent_cache_saved ? "success\n" : "failed\n") +
        "persistent_cache_status: " +
            persistent_render_cache_status_name(persistent_cache_status) + "\n" +
        "persistent_cache_error: " +
            (persistent_cache_error.empty()
                ? std::string("none\n")
                : persistent_cache_error + "\n") +
        "last_successful_cache_key_update: " +
            std::string(persistent_key_queued ? "queued\n" : "unchanged\n"));

    {
        std::lock_guard state_lock(state_mutex);
        auto& state = object_states[object_id];
        state.images = std::move(cumulative_images);
        state.step_layers = std::move(common_step_layers);
        state.common_transparent = std::move(common_transparent);
        state.compiled_render_dpi = selected_dpi;
        state.colored_source.reset();
        state.colored_image.reset();
        state.restore_attempted = true;
        if (persistent_key_queued) {
            state.pending_persistent_key = persistent_cache_key;
        }
        state.restored_cache_key = persistent_key_queued
            ? persistent_cache_key : previous_persistent_key;
        state.restore_result = persistent_key_queued
            ? PersistentRenderCacheStatus::Success
            : persistent_cache_status;
    }

    const bool old_value = update_id.value;
    const bool new_value = !old_value;
    const bool refresh_succeeded = object != nullptr && edit->set_object_item_value(
        object,
        L"LaTeX",
        L"更新ID",
        new_value ? "1" : "0");

    compile_status_scope.succeed(all_layers_from_cache
        ? CompileSource::Cache
        : CompileSource::Generated);

    append_latest_log(
        "failed_step: none\n"
        "image_list_published: yes\n"
        "refresh_item_update: " +
        std::string(refresh_succeeded ? "success\n" : "failed\n") +
        "compiled_render_dpi_after: " + std::to_string(selected_dpi) + "\n" +
        "compile_button_finished: yes\n");
}

bool func_proc_video(FILTER_PROC_VIDEO* video) {
    const std::int64_t object_id = video->object->id;
    last_object_id.store(object_id, std::memory_order_relaxed);

    // The SDK refreshes FILTER_ITEM_DATA::value for this object before the
    // filter callback. Commit a newly compiled key at this SDK-guaranteed
    // write point, then restore exactly the saved last-success key once. Never
    // derive a key from the currently edited source and never run TeX here.
    commit_pending_font_data(object_id);
    migrate_legacy_font_data_if_needed(video, object_id);
    commit_pending_persistent_key(object_id);
    restore_object_images_from_saved_cache(object_id);

    std::shared_ptr<const ImageList> images;
    std::shared_ptr<const ImageList> step_layers;
    ImagePointer common_transparent;
    std::shared_ptr<EffectOutputBuffer> effect_output;
    {
        std::lock_guard state_lock(state_mutex);
        auto& state = object_states[object_id];
        state.layer = video->object->layer;
        state.frame_start = video->object->frame_s;
        state.frame_end = video->object->frame_e;
        images = state.images;
        step_layers = state.step_layers;
        common_transparent = state.common_transparent;
        effect_output = state.effect_output;
    }

    const unsigned char red = text_color.value.r;
    const unsigned char green = text_color.value.g;
    const unsigned char blue = text_color.value.b;
    ImagePointer image;

    if (display_mode.value == 0) {
        if (images && !images->empty()) {
            image = images->back();
        }
    } else {
        const int step_count = step_layers
            ? static_cast<int>(step_layers->size())
            : 0;
        double step_value = (std::clamp)(display_step.value, 0.0, static_cast<double>(step_count));
        const double nearest_integer = std::round(step_value);
        if (std::abs(step_value - nearest_integer) < kStepEpsilon) {
            step_value = nearest_integer;
        }
        const int base_step = static_cast<int>(std::floor(step_value));
        const double progress = step_value - std::floor(step_value);

        if (transition_effect.value == 0 || !images || images->empty()) {
            if (images && !images->empty()) {
                const int selected_step = (std::clamp)(base_step, 0, step_count);
                image = (*images)[static_cast<std::size_t>(selected_step)];
            }
        } else if (base_step >= step_count) {
            image = images->back();
        } else {
            const ImagePointer base_image =
                (*images)[static_cast<std::size_t>(base_step)];
            const ImagePointer added_layer = step_layers &&
                    static_cast<std::size_t>(base_step) < step_layers->size()
                ? (*step_layers)[static_cast<std::size_t>(base_step)]
                : ImagePointer{};
            if (effect_output && base_image && added_layer) {
                std::lock_guard output_lock(effect_output->mutex);
                if (prepare_effect_output(
                        base_image,
                        added_layer,
                        transition_effect.value,
                        transition_effect.value == 2 ? reveal_direction.value : 0,
                        progress,
                        red,
                        green,
                        blue,
                        effect_output->image)) {
                    video->set_image_data(
                        effect_output->image.pixels.data(),
                        effect_output->image.width,
                        effect_output->image.height);
                    return true;
                }
            }
            image = base_image;
        }
    }

    if (!image) {
        image = common_transparent ? common_transparent : transparent_image();
    }

    ImagePointer output_image = image;
    if (!(red == 255 && green == 255 && blue == 255) && image != common_transparent) {
        {
            std::lock_guard state_lock(state_mutex);
            const auto& state = object_states[object_id];
            if (state.colored_source == image && state.colored_image &&
                state.colored_r == red && state.colored_g == green &&
                state.colored_b == blue) {
                output_image = state.colored_image;
            } else {
                output_image.reset();
            }
        }
        if (!output_image) {
            output_image = create_colored_image(image, red, green, blue);
            if (output_image) {
                std::lock_guard state_lock(state_mutex);
                auto& state = object_states[object_id];
                state.colored_source = image;
                state.colored_image = output_image;
                state.colored_r = red;
                state.colored_g = green;
                state.colored_b = blue;
            } else {
                output_image = image;
            }
        }
    }
    video->set_image_data(
        output_image->pixels.data(), output_image->width, output_image->height);
    return true;
}
