#include <windows.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <iostream>
#include <array>
#include <string>

using Microsoft::WRL::ComPtr;

std::string utf8(const std::wstring& value) {
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
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        result.data(),
        size,
        nullptr,
        nullptr);
    return result;
}

std::wstring name_for_locale(
    IDWriteLocalizedStrings* names,
    const wchar_t* locale,
    bool& found) {
    UINT32 index = 0;
    BOOL exists = FALSE;
    names->FindLocaleName(locale, &index, &exists);
    found = exists != FALSE;
    if (!found) {
        return {};
    }
    UINT32 length = 0;
    names->GetStringLength(index, &length);
    std::wstring value(static_cast<std::size_t>(length) + 1, L'\0');
    names->GetString(index, value.data(), length + 1);
    value.resize(length);
    return value;
}

int wmain() {
    ComPtr<IDWriteFactory> factory;
    if (FAILED(DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(factory.GetAddressOf())))) {
        return 1;
    }
    ComPtr<IDWriteFontCollection> collection;
    if (FAILED(factory->GetSystemFontCollection(&collection, FALSE))) {
        return 2;
    }
    wchar_t locale[LOCALE_NAME_MAX_LENGTH]{};
    if (!GetUserDefaultLocaleName(locale, LOCALE_NAME_MAX_LENGTH)) {
        wcscpy_s(locale, L"en-us");
    }
    for (UINT32 index = 0; index < collection->GetFontFamilyCount(); ++index) {
        ComPtr<IDWriteFontFamily> family;
        ComPtr<IDWriteLocalizedStrings> names;
        if (FAILED(collection->GetFontFamily(index, &family)) ||
            FAILED(family->GetFamilyNames(&names))) {
            continue;
        }
        bool ui_found = false;
        bool english_found = false;
        const std::wstring ui = name_for_locale(names.Get(), locale, ui_found);
        const std::wstring english =
            name_for_locale(names.Get(), L"en-us", english_found);
        const std::wstring searchable = ui + L" " + english;
        if (english != L"Arial" &&
            searchable.find(L"Yu Gothic") == std::wstring::npos &&
            searchable.find(L"Meiryo") == std::wstring::npos &&
            searchable.find(L"Corporate") == std::wstring::npos &&
            searchable.find(L"游ゴシック") == std::wstring::npos &&
            searchable.find(L"メイリオ") == std::wstring::npos) {
            continue;
        }
        std::wstring dwrite_name = ui_found ? ui : english;
        UINT32 found_index = 0;
        BOOL found_name = FALSE;
        if (dwrite_name.empty() || FAILED(collection->FindFamilyName(
                dwrite_name.c_str(), &found_index, &found_name)) ||
            found_name == FALSE || found_index != index) {
            dwrite_name = english;
        }
        ComPtr<IDWriteFont> font;
        ComPtr<IDWriteFontFace> face;
        const HRESULT font_result = family->GetFirstMatchingFont(
            DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            DWRITE_FONT_STYLE_NORMAL,
            &font);
        const HRESULT face_result = SUCCEEDED(font_result)
            ? font->CreateFontFace(&face)
            : font_result;
        static constexpr std::array<UINT32, 5> test_points{
            L'漢', L'あ', L'ア', L'A', L'1'
        };
        std::array<UINT16, test_points.size()> glyphs{};
        const bool glyphs_available = SUCCEEDED(face_result) &&
            SUCCEEDED(face->GetGlyphIndices(
                test_points.data(),
                static_cast<UINT32>(test_points.size()),
                glyphs.data()));
        ComPtr<IDWriteTextFormat> format;
        const bool format_available = SUCCEEDED(font_result) &&
            !dwrite_name.empty() &&
            SUCCEEDED(factory->CreateTextFormat(
                dwrite_name.c_str(),
                collection.Get(),
                font->GetWeight(),
                font->GetStyle(),
                font->GetStretch(),
                24.0F,
                L"ja-jp",
                &format));
        std::cout << "locale=" << utf8(locale)
                  << " display=" << utf8(ui_found ? ui : L"(missing)")
                  << " en-US=" << utf8(english_found ? english : L"(missing)")
                  << " dwrite=" << utf8(dwrite_name)
                  << " weight=" << (font ? static_cast<int>(font->GetWeight()) : -1)
                  << " glyphs=";
        if (glyphs_available) {
            for (const UINT16 glyph : glyphs) {
                std::cout << (glyph != 0 ? '1' : '0');
            }
        } else {
            std::cout << "error";
        }
        std::cout << " text-format=" << (format_available ? "ok" : "failed")
                  << '\n';
    }
    return 0;
}
