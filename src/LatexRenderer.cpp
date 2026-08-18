#include "LatexRenderer.h"

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <mutex>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include "ImageLoader.h"
#include "ProcessRunner.h"
#include "AppPaths.h"

namespace {

constexpr std::uintmax_t kMaximumPngFileSize = 256ULL * 1024ULL * 1024ULL;
constexpr std::uintmax_t kMaximumProcessOutputBytes = 16ULL * 1024ULL * 1024ULL;
constexpr char kRenderCacheVersion[] = "document-source-v1";
std::recursive_mutex latest_log_mutex;

std::string to_utf8(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(size), '\0');
    if (WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
            result.data(), size, nullptr, nullptr) != size) {
        return {};
    }
    return result;
}

std::uint64_t fnv1a(const std::string& value) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char byte : value) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::wstring hash_string(std::uint64_t hash) {
    std::wostringstream stream;
    stream << std::hex << std::setw(16) << std::setfill(L'0') << hash;
    return stream.str();
}

bool write_binary_file(const std::filesystem::path& path, const std::string& data) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(data.data(), static_cast<std::streamsize>(data.size()));
    return output.good();
}

std::string read_binary_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return "(log file unavailable)\n";
    }
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

void append_process_report(
    std::ofstream& log,
    const char* title,
    const ProcessResult& result,
    const std::filesystem::path& output_path) {
    log << "\n[" << title << "]\n";
    log << "command: " << to_utf8(result.command_line) << "\n";
    log << "started: " << (result.started ? "yes" : "no") << "\n";
    log << "timed_out: " << (result.timed_out ? "yes" : "no") << "\n";
    log << "output_truncated: " <<
        (result.output_truncated ? "yes" : "no") << "\n";
    if (result.started) {
        log << "exit_code: " << result.exit_code << "\n";
    }
    if (!result.error.empty()) {
        log << "error: " << to_utf8(result.error) << "\n";
    }
    log << "output:\n" << read_binary_file(output_path) << "\n";
}

} // namespace

void append_latest_log(const std::string& message) {
    std::lock_guard log_lock(latest_log_mutex);
    AppPaths paths;
    std::wstring path_error;
    if (!resolve_app_paths(paths, path_error) ||
        !ensure_runtime_directories(paths, path_error)) {
        return;
    }
    std::ofstream latest(paths.latest_log, std::ios::binary | std::ios::app);
    latest << message;
    if (!message.empty() && message.back() != '\n') {
        latest << '\n';
    }
}

void reset_latest_log(const std::string& message) {
    std::lock_guard log_lock(latest_log_mutex);
    AppPaths paths;
    std::wstring path_error;
    if (!resolve_app_paths(paths, path_error) ||
        !ensure_runtime_directories(paths, path_error)) {
        return;
    }
    std::ofstream latest(paths.latest_log, std::ios::binary | std::ios::trunc);
    latest << message;
    if (!message.empty() && message.back() != '\n') {
        latest << '\n';
    }
}

std::wstring make_render_cache_key(
    const std::wstring& document_source,
    const std::string& cache_version,
    int render_dpi) {
    const std::string source_utf8 = to_utf8(document_source);
    return hash_string(fnv1a(
        source_utf8 + "\nwhite-alpha-v2\n" + kRenderCacheVersion +
        "\n" + cache_version + "\nrender-dpi=" + std::to_string(render_dpi)));
}

bool render_latex(
    const std::wstring& document_source,
    RenderedImage& image,
    const std::string& cache_version,
    int render_dpi,
    const RenderTools& tools,
    bool crop_to_content,
    unsigned char alpha_threshold,
    std::uint64_t* nonzero_alpha_pixels,
    bool* image_decoded,
    bool* disk_cache_used) {
    // render_latex also streams detailed process reports directly to
    // latest.log. Keep each render report contiguous while allowing the
    // append helper to be called recursively from failure paths.
    std::lock_guard log_lock(latest_log_mutex);
    const bool tikz_render = cache_version.find(
        "tikzpicture-template-monochrome-v1") != std::string::npos;
    if (nonzero_alpha_pixels != nullptr) {
        *nonzero_alpha_pixels = 0;
    }
    if (image_decoded != nullptr) {
        *image_decoded = false;
    }
    if (disk_cache_used != nullptr) {
        *disk_cache_used = false;
    }
    AppPaths paths;
    std::wstring path_error;
    if (!resolve_app_paths(paths, path_error) ||
        !ensure_runtime_directories(paths, path_error)) {
        return false;
    }
    if (!std::filesystem::is_regular_file(tools.lualatex)) {
        append_latest_log("failed_stage: lualatex_not_found\n");
        return false;
    }
    if (!std::filesystem::is_regular_file(tools.mutool)) {
        append_latest_log("failed_stage: mutool_not_found\n");
        return false;
    }
    std::error_code error;

    const std::string source_utf8 = to_utf8(document_source);
    if (!document_source.empty() && source_utf8.empty()) {
        return false;
    }
    const std::wstring key = make_render_cache_key(
        document_source, cache_version, render_dpi);
    const std::filesystem::path work_directory = paths.work_root / key;
    std::filesystem::create_directories(work_directory, error);
    if (error) {
        return false;
    }

    const std::filesystem::path tex_path = work_directory / L"main.tex";
    const std::filesystem::path pdf_path = work_directory / L"main.pdf";
    const std::filesystem::path png_path = work_directory / L"main.png";
    const std::filesystem::path cached_png_path = paths.cache_root / (key + L".png");
    const std::filesystem::path lualatex_log_path = paths.logs_root / (L"lualatex-" + key + L".log");
    const std::filesystem::path mutool_log_path = paths.logs_root / (L"mutool-" + key + L".log");
    const std::filesystem::path latest_log_path = paths.latest_log;

    if (std::filesystem::is_regular_file(cached_png_path, error)) {
        ImageProcessingStats cached_stats;
        if (load_and_process_png(cached_png_path, image, cached_stats,
                crop_to_content, alpha_threshold) &&
            cached_stats.nonzero_alpha_pixels > 0) {
            if (nonzero_alpha_pixels != nullptr) *nonzero_alpha_pixels = cached_stats.nonzero_alpha_pixels;
            if (image_decoded != nullptr) *image_decoded = true;
            if (disk_cache_used != nullptr) *disk_cache_used = true;
            append_latest_log("disk_cache_used: yes\ncache_used: yes\n");
            return true;
        }
        error.clear();
        std::filesystem::remove(cached_png_path, error);
        append_latest_log("disk_cache_invalid: yes\ncache_fallback: regenerate\n");
    }

    std::filesystem::remove(pdf_path, error);
    error.clear();
    std::filesystem::remove(png_path, error);

    const std::string& document = source_utf8;

    std::ofstream latest(latest_log_path, std::ios::binary | std::ios::app);
    latest << "source_key: " << to_utf8(key) << "\n";
    latest << "cache_key: " << to_utf8(key)
           << "|render-dpi=" << render_dpi << "\n";
    latest << "render_dpi: " << render_dpi << "\n";
    latest << "actual_mutool_render_dpi: " << render_dpi << "\n";
    latest << "cache_key_contains_dpi: yes\n";
    if (!write_binary_file(tex_path, document)) {
        latest << "failed_stage: main_tex_write\n";
        latest << "error: failed to write UTF-8 main.tex\n";
        return false;
    }
    latest << "\n[Generated main.tex]\n";
    latest << document;

    latest << "\n[LuaLaTeX configuration]\n";
    latest << "executable: " << to_utf8(tools.lualatex.wstring()) << "\n";
    latest << "working_directory: " << to_utf8(work_directory.wstring()) << "\n";
    latest << "input_file: " << to_utf8(tex_path.wstring()) << "\n";
    latest << "output_directory: " << to_utf8(work_directory.wstring()) << "\n";

    const ProcessResult latex_result = run_process(
        tools.lualatex,
        {
            L"--interaction=nonstopmode",
            L"--halt-on-error",
            L"--file-line-error",
            L"--no-shell-escape",
            L"--output-directory=" + work_directory.wstring(),
            tex_path.wstring()
        },
        work_directory,
        lualatex_log_path,
        30000,
        std::stop_token{},
        kMaximumProcessOutputBytes);
    append_process_report(latest, "LuaLaTeX", latex_result, lualatex_log_path);
    latest.flush();
    error.clear();
    const std::uintmax_t pdf_file_size = std::filesystem::file_size(pdf_path, error);
    if (error) {
        latest << "pdf_file_size: unavailable\n";
    } else {
        latest << "pdf_file_size: " << pdf_file_size << "\n";
    }
    if (!latex_result.started || latex_result.timed_out || latex_result.exit_code != 0 ||
        !std::filesystem::is_regular_file(pdf_path)) {
        latest << "failed_stage: " << (!latex_result.started ? "lualatex_launch" : "latex_compile") << "\n";
        latest << "result: LuaLaTeX failed or main.pdf was not generated\n";
        return false;
    }

    const ProcessResult mutool_result = run_process(
        tools.mutool,
        {
            L"draw",
            L"-q",
            L"-o", png_path.wstring(),
            L"-F", L"png",
            L"-c", L"rgb",
            L"-r", std::to_wstring(render_dpi),
            pdf_path.wstring(),
            L"1"
        },
        work_directory,
        mutool_log_path,
        30000,
        std::stop_token{},
        kMaximumProcessOutputBytes);
    append_process_report(latest, "MuPDF", mutool_result, mutool_log_path);
    latest.flush();
    error.clear();
    const std::uintmax_t png_file_size = std::filesystem::file_size(png_path, error);
    if (error) {
        latest << "png_file_size: unavailable\n";
    } else {
        latest << "png_file_size: " << png_file_size << "\n";
    }
    if (!mutool_result.started || mutool_result.timed_out || mutool_result.exit_code != 0 ||
        !std::filesystem::is_regular_file(png_path)) {
        latest << "failed_stage: " << (!mutool_result.started ? "mutool_launch" : "pdf_render") << "\n";
        latest << "result: MuPDF failed or main.png was not generated\n";
        return false;
    }
    if (error || png_file_size > kMaximumPngFileSize) {
        latest << "failed_stage: "
               << (tikz_render ? "tikz_image_limit" : "png_file_size_limit")
               << "\n";
        latest << "result: PNG file is unavailable or exceeds the 268435456-byte limit\n";
        if (tikz_render) {
            latest << "image_width: unavailable\n";
            latest << "image_height: unavailable\n";
            latest << "pixel_count: unavailable\n";
            latest << "configured_limits: width=" << kMaximumDecodedImageWidth
                   << ",height=" << kMaximumDecodedImageHeight
                   << ",pixels=" << kMaximumDecodedPixelCount
                   << ",png_bytes=" << kMaximumPngFileSize << "\n";
        }
        return false;
    }

    ImageProcessingStats image_stats;
    const bool image_succeeded = load_and_process_png(
        png_path, image, image_stats, crop_to_content, alpha_threshold);
    if (nonzero_alpha_pixels != nullptr) {
        *nonzero_alpha_pixels = image_stats.nonzero_alpha_pixels;
    }
    if (image_decoded != nullptr) {
        *image_decoded = image_stats.source_width > 0 && image_stats.source_height > 0;
    }
    latest << "decoded_source_width: " << image_stats.source_width << "\n";
    latest << "decoded_source_height: " << image_stats.source_height << "\n";
    latest << "decoded_width_limit: " << kMaximumDecodedImageWidth << "\n";
    latest << "decoded_height_limit: " << kMaximumDecodedImageHeight << "\n";
    latest << "decoded_pixel_count: "
           << static_cast<std::uint64_t>(image_stats.source_width) *
                image_stats.source_height << "\n";
    latest << "decoded_pixel_count_limit: " << kMaximumDecodedPixelCount << "\n";
    latest << "nonzero_alpha_pixels: " << image_stats.nonzero_alpha_pixels << "\n";
    latest << "maximum_alpha: " << image_stats.maximum_alpha << "\n";
    if (image_stats.has_cropped_rectangle) {
        latest << "cropped_rectangle: "
               << image_stats.left << "," << image_stats.top << ","
               << image_stats.right << "," << image_stats.bottom << "\n";
    } else {
        latest << "cropped_rectangle: none\n";
    }
    latest << "final_image_size: " << image.width << "x" << image.height << "\n";

    if (!image_succeeded || image_stats.nonzero_alpha_pixels == 0) {
        latest << "failed_stage: "
               << (tikz_render && image_stats.failure_stage == "decoded_image_size_limit"
                    ? "tikz_image_limit"
                    : (image_stats.failure_stage.empty()
                        ? "png_image_processing"
                        : image_stats.failure_stage))
               << "\n";
        latest << "result: WIC PNG decoding or image processing failed\n";
        if (image_stats.failure_stage == "decoded_image_size_limit") {
            latest << "error: decoded PNG exceeds the width, height, or pixel-count limit\n";
            if (tikz_render) {
                latest << "image_width: " << image_stats.source_width << "\n";
                latest << "image_height: " << image_stats.source_height << "\n";
                latest << "pixel_count: "
                       << static_cast<std::uint64_t>(image_stats.source_width) *
                            image_stats.source_height << "\n";
                latest << "configured_limits: width=" << kMaximumDecodedImageWidth
                       << ",height=" << kMaximumDecodedImageHeight
                       << ",pixels=" << kMaximumDecodedPixelCount << "\n";
            }
        }
        return false;
    }

    latest << "failed_stage: none\n";
    latest << "result: success\n";
    latest << "image_size: " << image.width << "x" << image.height << "\n";
    const auto cache_temporary = cached_png_path.wstring() + L".tmp";
    error.clear();
    std::filesystem::copy_file(png_path, cache_temporary,
        std::filesystem::copy_options::overwrite_existing, error);
    if (!error && MoveFileExW(cache_temporary.c_str(), cached_png_path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        latest << "disk_cache_written: yes\n";
    } else {
        DeleteFileW(cache_temporary.c_str());
        latest << "disk_cache_written: no\n";
    }
    return true;
}
