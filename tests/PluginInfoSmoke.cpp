#include <windows.h>

#include <iostream>
#include <string>

#include "PluginInfo.h"
#include "GeneratedVersion.h"

int wmain() {
    InformationDialogSnapshot snapshot;
    snapshot.last_operation.status = L"失敗";
    snapshot.last_operation.template_name = L"document";
    snapshot.last_operation.render_dpi = 1200;
    snapshot.last_operation.failed_stage = L"latex_compile";
    snapshot.last_operation.error_category = UserErrorCategory::LatexCompileFailed;
    snapshot.last_operation.error_summary =
        user_error_message(UserErrorCategory::LatexCompileFailed);
    snapshot.last_operation.font_file_name =
        L"C:\\Users\\private-user\\Fonts\\Example.otf";
    const std::wstring report = build_diagnostic_report(snapshot);
    if (report.find(std::wstring(L"Plugin version: ") +
            AVIUTL2_LATEX_VERSION_W) == std::wstring::npos ||
        report.find(L"Required AviUtl2: 2.00.3300") == std::wstring::npos ||
        report.find(L"Example.otf") == std::wstring::npos ||
        report.find(L"private-user") != std::wstring::npos ||
        report.find(L"C:\\Users") != std::wstring::npos) {
        std::wcerr << report;
        return 1;
    }
    if (classify_user_error(L"lualatex_not_found", L"") !=
            UserErrorCategory::LuaLaTeXNotFound ||
        classify_user_error(L"tikz_image_limit", L"") !=
            UserErrorCategory::ImageTooLarge ||
        classify_user_error(L"latex_compile", L"font not loadable") !=
            UserErrorCategory::FontNotFound ||
        classify_user_error(L"japanese_font_configuration",
            L"font_file_exists: no") != UserErrorCategory::FontFileNotFound ||
        classify_user_error(L"latex_compile", L"process_timed_out: yes") !=
            UserErrorCategory::TimedOut) {
        return 2;
    }
    return 0;
}
