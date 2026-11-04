# The installer package (OPENGP_PACKAGE=ON): `cmake --install` lays it out
# (see tools/installer/installer.h and docs/packaging.md). It holds the
# runtime as static libraries, its headers, the games' own files and the
# installer, which builds each game on the player's machine. Nothing derived
# from a game is in it.

# The compiler the Windows installer downloads on first use. The runtime in
# the package must be built with the same one (see CMakePresets.json).
set(OPENGP_LLVM_MINGW_VERSION 20260922)
set(OPENGP_LLVM_MINGW_DIR llvm-mingw-${OPENGP_LLVM_MINGW_VERSION}-ucrt-x86_64)
set(OPENGP_LLVM_MINGW_URL
    https://github.com/mstorsjo/llvm-mingw/releases/download/${OPENGP_LLVM_MINGW_VERSION}/${OPENGP_LLVM_MINGW_DIR}.zip)
set(OPENGP_LLVM_MINGW_SHA256 e3ad77d117a4bea19a7a3b333341824d79a5a371004a10e25b8504e7b3047666)

# The runtime's libraries, in link order.
set(package_libs opengp_app opengp_settings opengp_discord opengp_video opengp_input opengp_audio opengp_webcam imgui
                 opengp_machine ppc_runtime gekko_isa opengp_utf8 SDL3-static)
install(TARGETS ${package_libs} ARCHIVE DESTINATION sdk/lib)
install(TARGETS opengp_installer RUNTIME DESTINATION .)

# Compiling and linking a game: the flags the runtime was built with that
# matter for the generated code (see opengp_guest_flags), and what SDL3 and
# the runtime need from the system.
set(cxx_flags -std=c++20 -O2 -g0 -ffp-contract=off -fwrapv -fno-strict-aliasing)
if(WIN32)
  set(toolchain_section "
[toolchain]
url = \"${OPENGP_LLVM_MINGW_URL}\"
sha256 = \"${OPENGP_LLVM_MINGW_SHA256}\"
dir = \"${OPENGP_LLVM_MINGW_DIR}\"
cxx = \"bin/x86_64-w64-mingw32-clang++.exe\"
# Only the x86-64 Windows target is needed.
exclude = [\"*/aarch64-w64-mingw32/*\", \"*/armv7-w64-mingw32/*\", \"*/i686-w64-mingw32/*\", \"*/python/*\"]
")
  list(APPEND cxx_flags -D_CRT_SECURE_NO_WARNINGS)
  set(cxx_candidates)
  set(link_flags -static)
  set(system_libs -ldbghelp -lkernel32 -luser32 -lgdi32 -lwinmm -limm32 -lole32 -loleaut32 -lversion -lws2_32 -liphlpapi
                  -luuid -ladvapi32 -lsetupapi -lshell32 -ldinput8)
  set(executable_suffix .exe)
  set(link_group true)
elseif(APPLE)
  # Apple's Clang, from the Xcode Command Line Tools; Apple Silicon only.
  set(toolchain_section "")
  set(cxx_candidates clang++ c++)
  set(apple_flags -arch arm64 -mmacosx-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET})
  list(APPEND cxx_flags ${apple_flags})
  set(link_flags ${apple_flags})
  set(system_libs)
  foreach(framework Cocoa Carbon CoreFoundation Foundation CoreVideo CoreMedia IOKit ForceFeedback CoreAudio
                    AudioToolbox AVFoundation GameController CoreHaptics Metal QuartzCore UniformTypeIdentifiers)
    list(APPEND system_libs -framework ${framework})
  endforeach()
  list(APPEND system_libs -liconv)
  set(executable_suffix "")
  set(link_group false)  # Apple's linker finds symbols in any library order
else()
  set(toolchain_section "")
  # The usual names, then versioned ones (Ubuntu 22.04's g++ is GCC 11).
  set(cxx_candidates c++ g++ clang++ g++-20 g++-19 g++-18 g++-17 g++-16 g++-15 g++-14 g++-13 g++-12
                     clang++-25 clang++-24 clang++-23 clang++-22 clang++-21 clang++-20 clang++-19 clang++-18
                     clang++-17 clang++-16)
  set(link_flags -pthread)
  set(system_libs -ldl -lm -lrt)
  set(executable_suffix "")
  set(link_group true)
endif()

function(toml_array out_var)
  set(items)
  foreach(item ${ARGN})
    list(APPEND items "\"${item}\"")
  endforeach()
  list(JOIN items ", " joined)
  set(${out_var} "[${joined}]" PARENT_SCOPE)
endfunction()

set(lib_paths)
foreach(lib ${package_libs})
  list(APPEND lib_paths "lib/$<TARGET_LINKER_FILE_NAME:${lib}>")
endforeach()
toml_array(libs_toml ${lib_paths})
toml_array(cxx_toml ${cxx_candidates})
toml_array(cxx_flags_toml ${cxx_flags})
toml_array(link_flags_toml ${link_flags})
toml_array(system_libs_toml ${system_libs})
toml_array(games_toml mkgp2 mkgp1)  # the main game first
include(${PROJECT_SOURCE_DIR}/cmake/game_sources.cmake)
toml_array(common_sources_toml ${OPENGP_GAME_COMMON_SOURCES})
toml_array(own_sources_toml ${OPENGP_GAME_OWN_SOURCES})

file(GENERATE OUTPUT ${CMAKE_BINARY_DIR}/package/package.toml CONTENT "\
# OpenGP ${PROJECT_VERSION}: how the installer builds the games here.
version = \"${PROJECT_VERSION}\"
games = ${games_toml}
${toolchain_section}
[build]
cxx = ${cxx_toml}
cxx_flags = ${cxx_flags_toml}
link_flags = ${link_flags_toml}
libs = ${libs_toml}
system_libs = ${system_libs_toml}
executable_suffix = \"${executable_suffix}\"
link_group = ${link_group}
# The hand-written sources of each game: these in sdk/games/common/, then
# these in the game's own folder.
common_sources = ${common_sources_toml}
game_sources = ${own_sources_toml}
")
install(FILES ${CMAKE_BINARY_DIR}/package/package.toml DESTINATION sdk)

# Headers: everything the generated code and the games' sources include.
install(DIRECTORY ${PROJECT_SOURCE_DIR}/runtime/ DESTINATION sdk/include
        FILES_MATCHING PATTERN "*.h" PATTERN "*.inc")
install(DIRECTORY ${PROJECT_SOURCE_DIR}/common/gekko/include/ DESTINATION sdk/include
        FILES_MATCHING PATTERN "*.h" PATTERN "*.inc")

# The games' own files (addresses, names and hand-written code only).
foreach(game mkgp2 mkgp1)
  set(dir ${PROJECT_SOURCE_DIR}/games/${game})
  set(own)
  foreach(source ${OPENGP_GAME_OWN_SOURCES})
    list(APPEND own ${dir}/${source})
  endforeach()
  install(FILES ${dir}/game.toml ${dir}/recomp.toml ${own} DESTINATION sdk/games/${game})
  install(FILES ${dir}/config/symbols.txt DESTINATION sdk/games/${game}/config)
endforeach()
install(DIRECTORY ${PROJECT_SOURCE_DIR}/games/common/ DESTINATION sdk/games/common)

# Documents, and the folder for the player's images.
install(FILES ${PROJECT_SOURCE_DIR}/README.md ${PROJECT_SOURCE_DIR}/LICENSE ${PROJECT_SOURCE_DIR}/CHANGELOG.md
              ${PROJECT_SOURCE_DIR}/THIRD-PARTY-NOTICES.md
        DESTINATION .)

# The licences of the third-party code the package and the games it builds
# contain (THIRD-PARTY-NOTICES.md lists them all), from the sources the
# package was built with.
include(FetchContent)
FetchContent_GetProperties(imgui)
FetchContent_GetProperties(SDL3)
install(FILES ${imgui_SOURCE_DIR}/LICENSE.txt DESTINATION licenses RENAME imgui-LICENSE.txt)
install(FILES ${sdl3_SOURCE_DIR}/LICENSE.txt DESTINATION licenses RENAME SDL3-LICENSE.txt)
install(FILES ${sdl3_SOURCE_DIR}/src/hidapi/LICENSE-bsd.txt DESTINATION licenses RENAME SDL3-hidapi-LICENSE.txt)
install(FILES ${sdl3_SOURCE_DIR}/src/video/yuv2rgb/LICENSE DESTINATION licenses RENAME SDL3-yuv2rgb-LICENSE.txt)
install(FILES ${PROJECT_SOURCE_DIR}/thirdparty/tomlplusplus/LICENSE DESTINATION licenses RENAME tomlplusplus-LICENSE.txt)
install(FILES ${PROJECT_SOURCE_DIR}/thirdparty/gc-fonts/LICENSE DESTINATION licenses RENAME gc-fonts-LICENSE.txt)
install(FILES ${PROJECT_SOURCE_DIR}/thirdparty/gc-fonts/README.md DESTINATION licenses RENAME gc-fonts-README.md)
# The menu's typeface (the launcher's too: its install puts the same text in
# the same place).
install(FILES ${PROJECT_SOURCE_DIR}/thirdparty/fonts/mplus-rounded-1c/OFL.txt DESTINATION licenses RENAME MPLUSRounded1c-OFL.txt)
# The launcher the package workflow adds (OpenGP-dev/opengp-launcher) links Qt,
# under the LGPL. Its install puts the same text in the same place. With it,
# the notices of the code Qt contains (tools/qt_notices.py makes them).
install(FILES ${PROJECT_SOURCE_DIR}/thirdparty/qt/LGPL-3.0.txt DESTINATION licenses RENAME Qt-LGPL-3.0.txt)
install(FILES ${PROJECT_SOURCE_DIR}/thirdparty/qt/third-party.txt DESTINATION licenses RENAME Qt-third-party.txt)
# All of them: the player's pages link to the others.
install(DIRECTORY ${PROJECT_SOURCE_DIR}/docs/ DESTINATION docs FILES_MATCHING PATTERN "*.md")
install(FILES ${PROJECT_SOURCE_DIR}/tools/installer/ISO-README.txt DESTINATION ISO RENAME README.txt)
