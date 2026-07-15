#pragma once

#include <windows.h>

#include <filesystem>
#include <optional>
#include <string>

#include "PluginInfo.h"

struct SystemFontSelection {
    bool use_default = false;
    std::wstring display_name;
    std::wstring fontspec_family_name;
};

void set_dialog_owner(HWND owner);
void show_environment_settings_dialog();
void show_information_dialog(const InformationDialogSnapshot& snapshot);
std::optional<SystemFontSelection> show_system_font_dialog(
    const std::wstring& current_font,
    bool current_is_default);
std::optional<std::filesystem::path> show_font_file_dialog(
    const std::filesystem::path& current_file);
