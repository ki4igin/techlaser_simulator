cmake_minimum_required(VERSION 3.16)

project(techlaser_simulator LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(techlaser_simulator
    src/main.cpp
)

target_compile_options(techlaser_simulator PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

# Готовый executable кладём в bin/
set_target_properties(techlaser_simulator PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin"
)
