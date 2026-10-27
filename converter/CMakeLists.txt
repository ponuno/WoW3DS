cmake_minimum_required(VERSION 3.20)
project(w3dconv CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

include(FetchContent)
# Pass -DFETCHCONTENT_SOURCE_DIR_<NAME>=/path to use local checkouts.
FetchContent_Declare(glm GIT_REPOSITORY https://github.com/g-truc/glm GIT_TAG 1.0.1 GIT_SHALLOW TRUE
                     SOURCE_SUBDIR do-not-add)
FetchContent_Declare(meshoptimizer GIT_REPOSITORY https://github.com/zeux/meshoptimizer GIT_TAG v0.22 GIT_SHALLOW TRUE
                     SOURCE_SUBDIR do-not-add)
FetchContent_Declare(rgetc1 GIT_REPOSITORY https://github.com/richgel999/rg-etc1 GIT_TAG master GIT_SHALLOW TRUE
                     SOURCE_SUBDIR do-not-add)
# WoWee (MIT): only the format parsers are compiled, none of the renderer.
FetchContent_Declare(wowee GIT_REPOSITORY https://github.com/Kelsidavis/WoWee GIT_TAG master GIT_SHALLOW TRUE
                     SOURCE_SUBDIR do-not-add)
FetchContent_MakeAvailable(glm meshoptimizer rgetc1 wowee)

# StormLib (MIT): read the client's MPQ archives directly, no extraction step.
option(W3D_STORMLIB "Read MPQ archives directly via StormLib" ON)
if(W3D_STORMLIB)
  set(STORM_USE_BUNDLED_LIBRARIES ON CACHE BOOL "" FORCE)
  set(STORM_SKIP_INSTALL ON CACHE BOOL "" FORCE)
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(stormlib GIT_REPOSITORY https://github.com/ladislav-zezula/StormLib GIT_TAG master GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(stormlib)
endif()

file(GLOB MESHOPT_SRC ${meshoptimizer_SOURCE_DIR}/src/*.cpp)

# Converter core as a static library: the tool and the tests link it.
add_library(w3dcore STATIC
  src/pack.cpp src/mpq.cpp src/texture.cpp src/model.cpp src/terrain.cpp src/synthetic.cpp src/wmo.cpp
  src/creature_model.cpp src/creatures.cpp src/items.cpp src/spells.cpp src/progress.cpp src/ui.cpp src/ui_font.cpp src/texts.cpp src/sound.cpp src/dungeon.cpp src/liquid.cpp
  ${MESHOPT_SRC}
  ${rgetc1_SOURCE_DIR}/rg_etc1.cpp
  ${wowee_SOURCE_DIR}/src/pipeline/adt_loader.cpp
  ${wowee_SOURCE_DIR}/src/pipeline/adt_alpha.cpp
  ${wowee_SOURCE_DIR}/src/pipeline/m2_loader.cpp
  ${wowee_SOURCE_DIR}/src/pipeline/blp_loader.cpp
  ${wowee_SOURCE_DIR}/src/pipeline/wmo_loader.cpp
  ${wowee_SOURCE_DIR}/src/pipeline/wdt_loader.cpp
  ${wowee_SOURCE_DIR}/src/core/logger.cpp)
target_include_directories(w3dcore PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/../common
  ${CMAKE_CURRENT_SOURCE_DIR}/extern/minimp3
  ${glm_SOURCE_DIR}
  ${meshoptimizer_SOURCE_DIR}/src
  ${rgetc1_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/extern/stb
  ${wowee_SOURCE_DIR}/include)
target_compile_definitions(w3dcore PUBLIC GLM_ENABLE_EXPERIMENTAL)
find_package(Threads REQUIRED)
target_link_libraries(w3dcore PUBLIC Threads::Threads)
if(W3D_STORMLIB)
  target_link_libraries(w3dcore PUBLIC storm)
  target_include_directories(w3dcore PUBLIC ${stormlib_SOURCE_DIR}/src)
  target_compile_definitions(w3dcore PUBLIC W3D_WITH_STORMLIB)
endif()

add_executable(w3dconv src/main.cpp)
target_link_libraries(w3dconv PRIVATE w3dcore)

# Tests (no game data needed): creature animation/skinning against WoWee's track sampler.
option(W3D_TESTS "Build converter tests" ON)
if(W3D_TESTS AND NOT MINGW)
  add_executable(creature_test tests/creature_test.cpp)
  target_link_libraries(creature_test PRIVATE w3dcore)
  add_executable(uv_range_test tests/uv_range_test.cpp)
  target_link_libraries(uv_range_test PRIVATE w3dcore)
  add_executable(skin_simplify_test tests/skin_simplify_test.cpp)
  target_link_libraries(skin_simplify_test PRIVATE w3dcore)
endif()

# Debug viewer: decodes a pack back to PNGs (textures + top-down terrain).
add_executable(w3dpreview src/preview.cpp ${rgetc1_SOURCE_DIR}/rg_etc1.cpp)
target_include_directories(w3dpreview PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../common ${rgetc1_SOURCE_DIR}
                           ${wowee_SOURCE_DIR}/extern)

# Self-contained .exe files when cross-building for Windows (MinGW / zig).
if(MINGW)
  target_link_options(w3dconv PRIVATE -static)
  target_link_options(w3dpreview PRIVATE -static)
endif()

# The converter window (stage 5.22): Windows only; it starts w3dconv.exe (next to it) step by step.
if(WIN32 OR MINGW)
  add_executable(w3dgui WIN32 gui/w3dgui.cpp)
  target_link_options(w3dgui PRIVATE -static -municode)
  target_link_libraries(w3dgui PRIVATE comctl32 shell32 ole32 user32 gdi32)
endif()
