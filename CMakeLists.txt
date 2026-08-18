cmake_minimum_required(VERSION 3.25)

project(AviUtl2LaTeX VERSION 0.2.0 LANGUAGES CXX)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/CHANGELOG.md" AVIUTL2_LATEX_CHANGELOG)
if(NOT AVIUTL2_LATEX_CHANGELOG MATCHES "## ${PROJECT_VERSION}([^0-9]|$)")
    message(FATAL_ERROR
        "CHANGELOG.md does not contain the PROJECT_VERSION ${PROJECT_VERSION}")
endif()

set(AVIUTL2_LATEX_BUILD_ID "local" CACHE STRING
    "Release build identifier supplied by CI")
set(AVIUTL2_LATEX_BUILD_TIMESTAMP "unknown" CACHE STRING
    "Reproducible build timestamp supplied by CI")

if(NOT MSVC)
    message(FATAL_ERROR "AviUtl2LaTeX requires MSVC.")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "AviUtl2LaTeX must be built for x64.")
endif()

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Version.rc.in"
    "${CMAKE_CURRENT_BINARY_DIR}/Version.rc"
    @ONLY
)
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/GeneratedVersion.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/GeneratedVersion.h"
    @ONLY
)

add_library(AviUtl2LaTeX MODULE
    src/AppPaths.cpp
    src/AviUtl2LaTeX.cpp
    src/DialogLayout.cpp
    src/DocumentTemplateBuilder.cpp
    src/EnvironmentDiagnostics.cpp
    src/ImageLoader.cpp
    src/JapaneseFontConfig.cpp
    src/LatexRenderer.cpp
    src/PersistentRenderCache.cpp
    src/PluginInfo.cpp
    src/ProcessRunner.cpp
    src/StepParser.cpp
    src/ToolSettings.cpp
    src/UiDialogs.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/Version.rc"
)

target_compile_features(AviUtl2LaTeX PRIVATE cxx_std_20)

target_include_directories(AviUtl2LaTeX PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/external/aviutl2_sdk"
    "${CMAKE_CURRENT_BINARY_DIR}"
)

target_compile_options(AviUtl2LaTeX PRIVATE
    /W4
    /permissive-
    /utf-8
    /Zc:__cplusplus
    /wd4828
    /wd4201
)

target_link_libraries(AviUtl2LaTeX PRIVATE
    windowscodecs
    ole32
    shell32
    dwrite
    comctl32
)

include(CTest)
if(BUILD_TESTING)
    add_executable(PersistentRenderCacheSmoke
        tests/PersistentRenderCacheSmoke.cpp
        src/PersistentRenderCache.cpp
    )
    target_compile_features(PersistentRenderCacheSmoke PRIVATE cxx_std_20)
    target_include_directories(PersistentRenderCacheSmoke PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/external/aviutl2_sdk"
    )
    target_compile_options(PersistentRenderCacheSmoke PRIVATE
        /W4
        /permissive-
        /utf-8
        /Zc:__cplusplus
        /wd4828
        /wd4201
    )
    add_test(
        NAME persistent_render_cache_smoke
        COMMAND PersistentRenderCacheSmoke
            "${CMAKE_CURRENT_BINARY_DIR}/persistent-cache-smoke-data"
    )

    add_executable(ProcessRunnerSmoke
        tests/process-runner/ProcessRunnerSmoke.cpp
        src/ProcessRunner.cpp
    )
    target_compile_features(ProcessRunnerSmoke PRIVATE cxx_std_20)
    target_include_directories(ProcessRunnerSmoke PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_compile_options(ProcessRunnerSmoke PRIVATE
        /W4
        /permissive-
        /utf-8
        /Zc:__cplusplus
    )
    add_test(NAME process_runner_smoke COMMAND ProcessRunnerSmoke)

    add_executable(PluginInfoSmoke
        tests/PluginInfoSmoke.cpp
        src/PluginInfo.cpp
        src/AppPaths.cpp
        src/ToolSettings.cpp
    )
    target_compile_features(PluginInfoSmoke PRIVATE cxx_std_20)
    target_include_directories(PluginInfoSmoke PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/external/aviutl2_sdk"
        "${CMAKE_CURRENT_BINARY_DIR}"
    )
    target_compile_options(PluginInfoSmoke PRIVATE
        /W4 /permissive- /utf-8 /Zc:__cplusplus /wd4828 /wd4201)
    target_link_libraries(PluginInfoSmoke PRIVATE shell32 ole32)
    add_test(NAME plugin_info_smoke COMMAND PluginInfoSmoke)

    add_executable(DialogLayoutSmoke
        tests/DialogLayoutSmoke.cpp
        src/DialogLayout.cpp
    )
    target_compile_features(DialogLayoutSmoke PRIVATE cxx_std_20)
    target_include_directories(DialogLayoutSmoke PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_compile_options(DialogLayoutSmoke PRIVATE
        /W4 /permissive- /utf-8 /Zc:__cplusplus /wd4828)
    target_link_libraries(DialogLayoutSmoke PRIVATE user32)
    add_test(NAME dialog_layout_smoke COMMAND DialogLayoutSmoke)

    function(add_japanese_font_config_smoke target source test_name)
        add_executable(${target}
            ${source}
            src/JapaneseFontConfig.cpp
        )
        target_compile_features(${target} PRIVATE cxx_std_20)
        target_include_directories(${target} PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests"
        )
        target_compile_options(${target} PRIVATE
            /W4 /WX /permissive- /utf-8 /Zc:__cplusplus /wd4828)
        add_test(
            NAME ${test_name}
            COMMAND ${target}
                "${CMAKE_CURRENT_BINARY_DIR}/font-config-smoke-data")
    endfunction()

    add_japanese_font_config_smoke(
        JapaneseFontConfigSmoke
        tests/JapaneseFontConfigSmoke.cpp
        japanese_font_config_smoke)
    add_japanese_font_config_smoke(
        FontConfigMigrationSmoke
        tests/FontConfigMigrationSmoke.cpp
        font_config_migration_smoke)
    add_japanese_font_config_smoke(
        CacheKeyCompletenessSmoke
        tests/CacheKeyCompletenessSmoke.cpp
        cache_key_completeness_smoke)

    add_executable(StepParserSmoke
        tests/StepParserSmoke.cpp
        src/StepParser.cpp
    )
    target_compile_features(StepParserSmoke PRIVATE cxx_std_20)
    target_include_directories(StepParserSmoke PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_compile_options(StepParserSmoke PRIVATE
        /W4 /WX /permissive- /utf-8 /Zc:__cplusplus /wd4828)
    add_test(NAME step_parser_smoke COMMAND StepParserSmoke)

    add_executable(DocumentMixedLineBreakSmoke
        tests/DocumentMixedLineBreakSmoke.cpp
        src/DocumentTemplateBuilder.cpp
        src/JapaneseFontConfig.cpp
        src/ProcessRunner.cpp
        src/StepParser.cpp
    )
    target_compile_features(DocumentMixedLineBreakSmoke PRIVATE cxx_std_20)
    target_include_directories(DocumentMixedLineBreakSmoke PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_compile_options(DocumentMixedLineBreakSmoke PRIVATE
        /W4 /WX /permissive- /utf-8 /Zc:__cplusplus /wd4828)
    add_test(
        NAME document_mixed_line_break_smoke
        COMMAND DocumentMixedLineBreakSmoke)

    add_executable(SdkStringLifetimeSmoke
        tests/SdkStringLifetimeSmoke.cpp
    )
    target_compile_features(SdkStringLifetimeSmoke PRIVATE cxx_std_20)
    target_include_directories(SdkStringLifetimeSmoke PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_compile_options(SdkStringLifetimeSmoke PRIVATE
        /W4 /WX /permissive- /utf-8 /Zc:__cplusplus /wd4828)
    add_test(NAME sdk_string_lifetime_smoke COMMAND SdkStringLifetimeSmoke)

    set(AVIUTL2_LATEX_TEST_LUALATEX "" CACHE FILEPATH
        "LuaLaTeX executable for opt-in font isolation integration tests")
    set(AVIUTL2_LATEX_TEST_MUTOOL "" CACHE FILEPATH
        "mutool executable for opt-in font isolation integration tests")
    set(AVIUTL2_LATEX_TEST_CORPORATE_FONT_FAMILY "" CACHE STRING
        "Optional installed Corporate Logo family for font isolation tests")
    set(AVIUTL2_LATEX_TEST_JAPANESE_FONT_FILE "" CACHE FILEPATH
        "Optional Japanese OTF/TTF/TTC file for font isolation tests")
    if(AVIUTL2_LATEX_TEST_LUALATEX)
        add_test(
            NAME document_mixed_line_break_integration
            COMMAND DocumentMixedLineBreakSmoke
                "${AVIUTL2_LATEX_TEST_LUALATEX}"
                "${CMAKE_CURRENT_BINARY_DIR}/document-mixed-line-break")
        set_tests_properties(
            document_mixed_line_break_integration PROPERTIES
            LABELS "latex_integration"
            TIMEOUT 300)
    endif()
    if(AVIUTL2_LATEX_TEST_LUALATEX AND AVIUTL2_LATEX_TEST_MUTOOL)
        add_executable(JapaneseMathFontIsolationSmoke
            tests/JapaneseMathFontIsolationSmoke.cpp
            src/JapaneseFontConfig.cpp
            src/ProcessRunner.cpp
        )
        target_compile_features(JapaneseMathFontIsolationSmoke PRIVATE cxx_std_20)
        target_include_directories(JapaneseMathFontIsolationSmoke PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src"
        )
        target_compile_options(JapaneseMathFontIsolationSmoke PRIVATE
            /W4 /WX /permissive- /utf-8 /Zc:__cplusplus)
        add_test(
            NAME japanese_math_font_isolation_smoke
            COMMAND JapaneseMathFontIsolationSmoke
                "${AVIUTL2_LATEX_TEST_LUALATEX}"
                "${AVIUTL2_LATEX_TEST_MUTOOL}"
                "${CMAKE_CURRENT_BINARY_DIR}/japanese-math-font-isolation"
                "${AVIUTL2_LATEX_TEST_CORPORATE_FONT_FAMILY}"
                "${AVIUTL2_LATEX_TEST_JAPANESE_FONT_FILE}")
        set_tests_properties(japanese_math_font_isolation_smoke PROPERTIES
            LABELS "latex_integration"
            TIMEOUT 180)
    endif()
endif()

set_target_properties(AviUtl2LaTeX PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
    PREFIX ""
    OUTPUT_NAME "AviUtl2LaTeX"
    SUFFIX ".auf2"
)

set(AVIUTL2_LATEX_RUNTIME_PLUGIN_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/runtime/data/Plugin/AviUtl2LaTeX"
    CACHE PATH "Development runtime directory receiving the built plugin")
option(AVIUTL2_LATEX_DEPLOY_RUNTIME
    "Copy and verify the built plugin in the local development runtime" ON)
if(AVIUTL2_LATEX_DEPLOY_RUNTIME)
    add_custom_command(TARGET AviUtl2LaTeX POST_BUILD
        COMMAND "${CMAKE_COMMAND}"
            -DDEPLOY_SOURCE=$<TARGET_FILE:AviUtl2LaTeX>
            -DDEPLOY_DIRECTORY=${AVIUTL2_LATEX_RUNTIME_PLUGIN_DIR}
            -DDEPLOY_FILE_NAME=$<TARGET_FILE_NAME:AviUtl2LaTeX>
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/DeployPlugin.cmake"
        VERBATIM
    )
endif()

set(AVIUTL2_LATEX_DIST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/dist")
add_custom_target(package_catalog
    COMMAND "${CMAKE_COMMAND}"
        -DPLUGIN_FILE=$<TARGET_FILE:AviUtl2LaTeX>
        -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
        -DDIST_DIR=${AVIUTL2_LATEX_DIST_DIR}
        -DPROJECT_VERSION=${PROJECT_VERSION}
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/PackageCatalog.cmake"
    DEPENDS AviUtl2LaTeX
    VERBATIM
)
