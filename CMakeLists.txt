
cmake_minimum_required(VERSION 3.16)

project(techlaser_simulator
    VERSION 1.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Curses REQUIRED)

add_executable(techlaser_simulator
    src/main.cpp
)

target_include_directories(techlaser_simulator PRIVATE
    ${CURSES_INCLUDE_DIRS}
)

target_link_libraries(techlaser_simulator PRIVATE
    ${CURSES_LIBRARIES}
)

target_compile_options(techlaser_simulator PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

set_target_properties(techlaser_simulator PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin"
)
