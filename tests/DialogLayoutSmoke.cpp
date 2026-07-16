#include <windows.h>

#include <array>
#include <iostream>

#include "DialogLayout.h"

namespace {

bool inside(const RECT& value, const RECT& work) {
    return value.left >= work.left && value.top >= work.top &&
        value.right <= work.right && value.bottom <= work.bottom &&
        value.right > value.left && value.bottom > value.top;
}

bool run_case(
    const char* name, const RECT& work, UINT dpi,
    SIZE logical_size, const RECT& anchor) {
    const SIZE desired{
        dialog_layout::scale_for_dpi(logical_size.cx, dpi),
        dialog_layout::scale_for_dpi(logical_size.cy, dpi)};
    const RECT result = dialog_layout::center_and_clamp_window_rect(
        anchor, desired, work, dialog_layout::scale_for_dpi(12, dpi));
    if (!inside(result, work)) {
        std::cerr << name << " escaped the work area\n";
        return false;
    }
    return true;
}

}  // namespace

int main() {
    if (dialog_layout::scale_for_dpi(100, 96) != 100 ||
        dialog_layout::scale_for_dpi(100, 120) != 125 ||
        dialog_layout::scale_for_dpi(100, 144) != 150 ||
        dialog_layout::scale_for_dpi(100, 192) != 200) {
        std::cerr << "DPI scaling produced an unexpected value\n";
        return 1;
    }
    struct TestCase {
        const char* name;
        RECT work;
        UINT dpi;
        SIZE logical_size;
        RECT anchor;
    };
    const std::array cases{
        TestCase{"A", {0, 0, 1920, 1040}, 96, {610, 410}, {200, 100, 1700, 900}},
        TestCase{"B", {0, 0, 1536, 824}, 120, {610, 410}, {100, 50, 1400, 760}},
        TestCase{"C", {0, 0, 1280, 680}, 144, {610, 410}, {0, 0, 1280, 680}},
        TestCase{"D", {0, 0, 1024, 728}, 192, {610, 410}, {0, 0, 1024, 728}},
        TestCase{"E", {-1920, 0, 0, 1040}, 96, {736, 600}, {-1700, 100, -200, 900}},
        TestCase{"F", {0, 0, 1280, 720}, 144, {480, 510}, {-300, -200, 500, 500}},
        TestCase{"G", {0, 0, 1024, 728}, 192, {1200, 900}, {0, 0, 1024, 728}},
    };
    for (const auto& value : cases) {
        if (!run_case(value.name, value.work, value.dpi,
                value.logical_size, value.anchor)) {
            return 1;
        }
    }
    return 0;
}
