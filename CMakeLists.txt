cmake_minimum_required(VERSION 3.25)

project(AviUtl2LaTeX VERSION 0.1.1 LANGUAGES CXX)

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
    src/EnvironmentDiagnostics.cpp
    src/ImageLoader.cpp
    src/LatexRenderer.cpp
    src/PersistentRenderCache.cpp
    src/PluginInfo.cpp
    src/ProcessRunner.cpp
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
