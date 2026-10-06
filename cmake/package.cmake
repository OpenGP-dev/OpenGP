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
set(package_libs opengp_app opengp_settings opengp_video opengp_input opengp_audio imgui
                 opengp_machine ppc_runtime gekko_isa SDL3-static)
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
  set(system_libs -ldbghelp -lkernel32 -luser32 -lgdi32 -lwinmm -limm32 -lole32 -loleaut32 -lversion
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
  set(cxx_candidates c++ g++ clang++)
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
  install(FILES ${dir}/game.toml ${dir}/recomp.toml ${dir}/patches.cpp DESTINATION sdk/games/${game})
  install(FILES ${dir}/config/symbols.txt DESTINATION sdk/games/${game}/config)
endforeach()
install(DIRECTORY ${PROJECT_SOURCE_DIR}/games/common/ DESTINATION sdk/games/common)

# Documents, and the folder for the player's images.
install(FILES ${PROJECT_SOURCE_DIR}/README.md ${PROJECT_SOURCE_DIR}/LICENSE ${PROJECT_SOURCE_DIR}/CHANGELOG.md
        DESTINATION .)
install(FILES ${PROJECT_SOURCE_DIR}/docs/playing.md ${PROJECT_SOURCE_DIR}/docs/game-files.md
        DESTINATION docs)
install(FILES ${PROJECT_SOURCE_DIR}/tools/installer/ISO-README.txt DESTINATION ISO RENAME README.txt)
