cmake_minimum_required(VERSION 3.25)
project(FontSelectionProbe LANGUAGES CXX)

add_executable(enumerate_directwrite enumerate-directwrite.cpp)
target_compile_features(enumerate_directwrite PRIVATE cxx_std_17)
target_compile_options(enumerate_directwrite PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(enumerate_directwrite PRIVATE dwrite)
