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
option(W3D_CASC "Read the modern client (WoW Forever) through CascLib" ON)
if(W3D_CASC)
  enable_language(C)
endif()
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
  src/creature_model.cpp src/creatures.cpp src/items.cpp src/spells.cpp src/spelltext.cpp src/progress.cpp src/ui.cpp src/ui_font.cpp src/texts.cpp src/sound.cpp src/dungeon.cpp src/liquid.cpp src/all.cpp src/crashguard.cpp src/glue.cpp src/spellfx.cpp src/skybox.cpp src/farlod.cpp src/farobj.cpp src/m2_fix.cpp src/modern_adt.cpp src/forever_data.cpp src/casc_source.cpp
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
if(W3D_CASC)
  set(CASC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../net/forever/extern/casclib/src)
  file(GLOB CASC_SRC ${CASC_DIR}/common/*.cpp ${CASC_DIR}/hashes/*.cpp ${CASC_DIR}/overwatch/apm.cpp ${CASC_DIR}/overwatch/cmf.cpp
       ${CASC_DIR}/overwatch/aes.cpp ${CASC_DIR}/Casc*.cpp)
  set(CASC_C ${CASC_DIR}/jenkins/lookup3.c ${CASC_DIR}/zlib/adler32.c ${CASC_DIR}/zlib/crc32.c ${CASC_DIR}/zlib/inffast.c
      ${CASC_DIR}/zlib/inflate.c ${CASC_DIR}/zlib/inftrees.c ${CASC_DIR}/zlib/zutil.c)
  add_library(w3dcasc STATIC ${CASC_SRC} ${CASC_C})
  target_include_directories(w3dcasc PUBLIC ${CASC_DIR})
  target_compile_definitions(w3dcasc PUBLIC CASCLIB_NO_AUTO_LINK_LIBRARY CASCLIB_NODEBUG)
  target_compile_options(w3dcasc PRIVATE -w)
  target_link_libraries(w3dcore PUBLIC w3dcasc)
  if(WIN32 OR MINGW)
    target_link_libraries(w3dcasc PUBLIC ws2_32 wininet bcrypt)
  endif()
  target_compile_definitions(w3dcore PUBLIC W3D_WITH_CASC)
endif()
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
  add_executable(pool_test tests/pool_test.cpp)
  target_link_libraries(pool_test PRIVATE w3dcore)
  add_executable(uv_range_test tests/uv_range_test.cpp)
  target_link_libraries(uv_range_test PRIVATE w3dcore)
  add_executable(uv_anim_test tests/uv_anim_test.cpp)
  target_link_libraries(uv_anim_test PRIVATE w3dcore)
  add_executable(pose_doodad_test tests/pose_doodad_test.cpp)   # 6.45: doodads in their Stand pose
  target_link_libraries(pose_doodad_test PRIVATE w3dcore)
  add_executable(m2_ranges_test tests/m2_ranges_test.cpp)   # 6.43: vanilla key ranges read inclusive (gen_m2_ranges.py)
  target_link_libraries(m2_ranges_test PRIVATE w3dcore)
  add_executable(wmo_hole_test tests/wmo_hole_test.cpp)
  target_link_libraries(wmo_hole_test PRIVATE w3dcore)
  add_executable(wmo_lod_test tests/wmo_lod_test.cpp)   # 6.47
  target_link_libraries(wmo_lod_test PRIVATE w3dcore)
  add_executable(skin_simplify_test tests/skin_simplify_test.cpp)
  target_link_libraries(skin_simplify_test PRIVATE w3dcore)
  add_executable(lod_thin_test tests/lod_thin_test.cpp)
  target_link_libraries(lod_thin_test PRIVATE w3dcore)
  add_executable(card_lod_test tests/card_lod_test.cpp)   # 6.84
  target_link_libraries(card_lod_test PRIVATE w3dcore)
  add_executable(far_shell_test tests/far_shell_test.cpp)
  target_link_libraries(far_shell_test PRIVATE w3dcore)
  add_executable(spell_text_test tests/spell_text_test.cpp)
  target_link_libraries(spell_text_test PRIVATE w3dcore)
  add_executable(modern_adt_test tests/modern_adt_test.cpp)   # 6.86: WoW Forever split ADT
  target_link_libraries(modern_adt_test PRIVATE w3dcore)
endif()

# Debug viewer: decodes a pack back to PNGs (textures + top-down terrain).
add_executable(w3dpreview src/preview.cpp ${rgetc1_SOURCE_DIR}/rg_etc1.cpp)
target_include_directories(w3dpreview PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../common ${rgetc1_SOURCE_DIR}
                           ${wowee_SOURCE_DIR}/extern)
# 6.53: what is seen from one spot vs what the client draws (occlusion measurement)
add_executable(w3docc src/occ.cpp ${rgetc1_SOURCE_DIR}/rg_etc1.cpp ${CMAKE_CURRENT_SOURCE_DIR}/../proto3ds/source/wmo_portals.cpp)   # (6.57: the horizon's colours from the textures)
target_include_directories(w3docc PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../common ${wowee_SOURCE_DIR}/extern ${rgetc1_SOURCE_DIR}
                           ${CMAKE_CURRENT_SOURCE_DIR}/../proto3ds/source)

# Self-contained .exe files when cross-building for Windows (MinGW / zig).
if(MINGW)
  target_link_options(w3dconv PRIVATE -static)
  target_link_options(w3dpreview PRIVATE -static)
  target_link_options(w3docc PRIVATE -static)
endif()

# The converter window (stage 5.22): Windows only; it starts w3dconv.exe (next to it) step by step.
if(WIN32 OR MINGW)
  add_executable(w3dgui WIN32 gui/w3dgui.cpp)
  if(MINGW)
    target_link_options(w3dgui PRIVATE -static -municode)   # the release .exe: no DLLs, wWinMain
  endif()
  target_link_libraries(w3dgui PRIVATE comctl32 shell32 ole32 user32 gdi32)
endif()
