cmake_minimum_required(VERSION 3.20)
project(key_toggler LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(key_toggler WIN32
    src/main.cpp
)

target_compile_definitions(key_toggler PRIVATE UNICODE _UNICODE)

target_link_libraries(key_toggler PRIVATE user32 gdi32 dwmapi)
