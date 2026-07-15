#pragma once

#include <stop_token>
#include <string>

#include "ToolSettings.h"

struct DiagnosticReport {
    std::wstring summary;
    std::wstring details;
    bool success = false;
    bool cancelled = false;
    bool timed_out = false;
    std::wstring lualatex_version;
    std::wstring mutool_version;
};

DiagnosticReport run_environment_diagnostics(const ToolSettings& settings);

// Intended to be called from the environment-settings dialog's joinable
// worker thread. The settings value must be a worker-owned copy captured when
// diagnostics starts; cancellation terminates the active diagnostic process
// tree through ProcessRunner's Job Object.
DiagnosticReport run_environment_diagnostics(
    const ToolSettings& settings,
    std::stop_token stop_token);
