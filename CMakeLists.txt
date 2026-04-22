cmake_minimum_required(VERSION 3.16)
project(xp_music_player CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# ---------------------------------------------------------------------------
# Dependencies
#
# Linux: install libglfw3-dev + libtag1-dev (or equivalent) via your package
#        manager; both are picked up below.
# Windows: recommended to use vcpkg ->
#     vcpkg install glfw3 taglib
#     cmake -B build -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
# ---------------------------------------------------------------------------

find_package(OpenGL REQUIRED)

# GLFW: prefer find_package (vcpkg/conan/config), fall back to pkg-config.
find_package(glfw3 CONFIG QUIET)
if(NOT glfw3_FOUND)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(GLFW glfw3)
    endif()
endif()

# TagLib: prefer find_package (vcpkg exports TagLib::tag), fall back to pkg-config.
find_package(TagLib CONFIG QUIET)
if(NOT TagLib_FOUND)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(TAGLIB taglib)
    endif()
endif()

if(NOT (TARGET glfw OR GLFW_FOUND))
    message(FATAL_ERROR "GLFW not found. On Linux: apt install libglfw3-dev. On Windows: vcpkg install glfw3 (and pass -DCMAKE_TOOLCHAIN_FILE=...).")
endif()
if(NOT (TARGET TagLib::tag OR TAGLIB_FOUND))
    message(FATAL_ERROR "TagLib not found. On Linux: apt install libtag1-dev. On Windows: vcpkg install taglib.")
endif()

# ---------------------------------------------------------------------------
# Dear ImGui (vendored)
# ---------------------------------------------------------------------------
set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/third_party/imgui)
add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)
target_link_libraries(imgui PUBLIC OpenGL::GL)
if(TARGET glfw)
    target_link_libraries(imgui PUBLIC glfw)
else()
    target_include_directories(imgui PUBLIC ${GLFW_INCLUDE_DIRS})
    target_link_libraries(imgui PUBLIC ${GLFW_LIBRARIES})
    target_link_directories(imgui PUBLIC ${GLFW_LIBRARY_DIRS})
endif()

# ---------------------------------------------------------------------------
# App
# ---------------------------------------------------------------------------
add_executable(music_player
    src/main.cpp
    src/miniaudio_impl.cpp
    src/stb_image_impl.cpp
    src/player.cpp
    src/library.cpp
    src/theme.cpp
)
set_target_properties(music_player PROPERTIES WIN32_EXECUTABLE FALSE)

target_include_directories(music_player PRIVATE src third_party)
target_link_libraries(music_player PRIVATE imgui OpenGL::GL)

if(TARGET TagLib::tag)
    target_link_libraries(music_player PRIVATE TagLib::tag)
else()
    target_include_directories(music_player PRIVATE ${TAGLIB_INCLUDE_DIRS})
    target_link_libraries(music_player PRIVATE ${TAGLIB_LIBRARIES})
    target_link_directories(music_player PRIVATE ${TAGLIB_LIBRARY_DIRS})
endif()

if(WIN32)
    target_link_libraries(music_player PRIVATE opengl32)
    target_compile_definitions(music_player PRIVATE UNICODE _UNICODE)
else()
    target_link_libraries(music_player PRIVATE pthread dl m)
endif()

if(MSVC)
    target_compile_options(music_player PRIVATE /W3 /utf-8)
    target_compile_options(imgui PRIVATE /utf-8)
else()
    target_compile_options(music_player PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
