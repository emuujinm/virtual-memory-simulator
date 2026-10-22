cmake_minimum_required(VERSION 3.16)
project(sim LANGUAGES CXX)

add_executable(sim src/main.cpp)
target_include_directories(sim PRIVATE include)
target_compile_features(sim PRIVATE cxx_std_17)

if (MSVC)
    target_compile_options(sim PRIVATE /W4)
else()
    target_compile_options(sim PRIVATE -Wall -Wextra)
endif()
