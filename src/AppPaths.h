#pragma once

#include <filesystem>
#include <string>

struct AppPaths {
    std::filesystem::path roaming_root;
    std::filesystem::path settings_file;
    std::filesystem::path local_root;
    std::filesystem::path cache_root;
    std::filesystem::path work_root;
    std::filesystem::path logs_root;
    std::filesystem::path latest_log;
};

bool resolve_app_paths(AppPaths& paths, std::wstring& error);
bool ensure_runtime_directories(const AppPaths& paths, std::wstring& error);

