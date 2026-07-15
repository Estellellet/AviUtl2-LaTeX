#include "AppPaths.h"

#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>

#include <system_error>

namespace {

bool known_folder(REFKNOWNFOLDERID id, std::filesystem::path& value) {
    PWSTR raw = nullptr;
    const HRESULT result = SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &raw);
    if (FAILED(result) || raw == nullptr) {
        if (raw != nullptr) {
            CoTaskMemFree(raw);
        }
        return false;
    }
    value = raw;
    CoTaskMemFree(raw);
    return true;
}

bool make_directory(const std::filesystem::path& path, std::wstring& error) {
    std::error_code code;
    std::filesystem::create_directories(path, code);
    if (code || !std::filesystem::is_directory(path, code)) {
        error = L"ディレクトリを作成できません: " + path.wstring();
        return false;
    }
    return true;
}

} // namespace

bool resolve_app_paths(AppPaths& paths, std::wstring& error) {
    std::filesystem::path roaming;
    std::filesystem::path local;
    if (!known_folder(FOLDERID_RoamingAppData, roaming)) {
        error = L"Roaming AppDataを取得できません";
        return false;
    }
    if (!known_folder(FOLDERID_LocalAppData, local)) {
        error = L"Local AppDataを取得できません";
        return false;
    }
    paths.roaming_root = roaming / L"AviUtl2LaTeX";
    paths.settings_file = paths.roaming_root / L"settings.json";
    paths.local_root = local / L"AviUtl2LaTeX";
    paths.cache_root = paths.local_root / L"cache";
    paths.work_root = paths.local_root / L"work";
    paths.logs_root = paths.local_root / L"logs";
    paths.latest_log = paths.logs_root / L"latest.log";
    return true;
}

bool ensure_runtime_directories(const AppPaths& paths, std::wstring& error) {
    return make_directory(paths.roaming_root, error) &&
        make_directory(paths.cache_root, error) &&
        make_directory(paths.work_root, error) &&
        make_directory(paths.logs_root, error);
}

