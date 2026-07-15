cmake_minimum_required(VERSION 3.25)
project(ProcessRunnerSmoke LANGUAGES CXX)

add_executable(ProcessRunnerSmoke
    ProcessRunnerSmoke.cpp
    ../../src/ProcessRunner.cpp
)
target_compile_features(ProcessRunnerSmoke PRIVATE cxx_std_20)
target_include_directories(ProcessRunnerSmoke PRIVATE ../../src)
target_compile_options(ProcessRunnerSmoke PRIVATE /W4 /permissive- /utf-8)

