# Each game is built only when its executable has been extracted into orig/
# (see docs/game-files.md). Nothing from the games is ever committed.
#
# A game is a directory with:
#   game.toml          id, title, the SHA-1 of the supported main.dol, executable name
#   recomp.toml        the recompiler's settings, patches and hooks
#   config/symbols.txt the decomp-toolkit symbol list
#   patches.cpp        hand-written replacements for recompiled functions
# The launcher's main() and the code the games share are in common/. The
# installer (tools/installer) builds a game from the same files.

# opengp_game_dol(<out-var> <game-id> <expected-sha1> <display-name>)
#
# Sets <out-var> to the path of orig/<game-id>/sys/main.dol if it exists and
# has the expected SHA-1, or to "" (with a status message) otherwise.
function(opengp_game_dol out_var game_id expected_sha1 display_name)
  set(dol "${PROJECT_SOURCE_DIR}/orig/${game_id}/sys/main.dol")
  if(NOT EXISTS "${dol}")
    message(STATUS "${display_name}: orig/${game_id}/sys/main.dol not found, skipping "
                   "(see docs/game-files.md)")
    set(${out_var} "" PARENT_SCOPE)
    return()
  endif()
  file(SHA1 "${dol}" actual)
  if(NOT actual STREQUAL expected_sha1)
    message(WARNING "${display_name}: orig/${game_id}/sys/main.dol has SHA-1 ${actual}, expected "
                    "${expected_sha1}. Only that revision is supported; skipping.")
    set(${out_var} "" PARENT_SCOPE)
    return()
  endif()
  message(STATUS "${display_name}: found orig/${game_id}/sys/main.dol")
  set(${out_var} "${dol}" PARENT_SCOPE)
endfunction()

# Compile options for recompiled game code: no debug info by default (tens of
# MB of generated C++), and no warnings (the code is machine-generated and
# already checked by the test suite).
function(opengp_generated_code_options target)
  if(OPENGP_TRACE_CALLS)
    target_compile_definitions(${target} PUBLIC PPC_TRACE_CALLS)
  endif()
  if(NOT OPENGP_GENERATED_DEBUG_INFO)
    set_property(TARGET ${target} PROPERTY MSVC_DEBUG_INFORMATION_FORMAT "")
    if(NOT CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
      target_compile_options(${target} PRIVATE -g0)
    endif()
  endif()
endfunction()

# opengp_toml_string(<out-var> <toml text> <key>): a top-level `key = "value"`.
function(opengp_toml_string out_var text key)
  string(REGEX MATCH "(^|\n)[ \t]*${key}[ \t]*=[ \t]*\"([^\"]*)\"" _ "${text}")
  set(${out_var} "${CMAKE_MATCH_2}" PARENT_SCOPE)
endfunction()

# Hand-written code the games share, compiled into each game's executable
# against that game's generated header.
set(OPENGP_GAMES_COMMON
  ${CMAKE_CURRENT_SOURCE_DIR}/common/main.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/common/arcade.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/common/attract_sound.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/common/camera_use.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/common/game_log.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/common/sdk_hle.cpp)

# opengp_game(<dir>): builds the game in <dir> if its main.dol is present.
function(opengp_game dir)
  set(src ${CMAKE_CURRENT_SOURCE_DIR}/${dir})
  set(bin ${CMAKE_CURRENT_BINARY_DIR}/${dir})
  file(READ ${src}/game.toml manifest)
  opengp_toml_string(id "${manifest}" id)
  opengp_toml_string(title "${manifest}" title)
  opengp_toml_string(dol_sha1 "${manifest}" dol_sha1)
  opengp_toml_string(executable "${manifest}" executable)
  file(READ ${src}/recomp.toml recomp)
  string(REGEX MATCH "file_count[ \t]*=[ \t]*([0-9]+)" _ "${recomp}")
  set(file_count ${CMAKE_MATCH_1})

  opengp_game_dol(dol ${id} ${dol_sha1} "${title}")
  if(NOT dol)
    return()
  endif()

  # The recompiled game code, as a library so the launcher and tools can share it.
  add_library(${dir}_recomp STATIC)
  opengp_recompile(${dir}_recomp
    CONFIG ${src}/recomp.toml
    FILE_COUNT ${file_count}
    OUTPUT_DIR ${bin}/generated
    DOL ${dol}
    SYMBOLS ${src}/config/symbols.txt)
  target_include_directories(${dir}_recomp PUBLIC ${bin}/generated)
  target_link_libraries(${dir}_recomp PUBLIC ppc_runtime)
  opengp_generated_code_options(${dir}_recomp)

  add_executable(${executable} ${src}/patches.cpp ${OPENGP_GAMES_COMMON})
  target_link_libraries(${executable} PRIVATE ${dir}_recomp opengp_app opengp_warnings)
  target_include_directories(${executable} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/common)
  target_compile_definitions(${executable} PRIVATE
    OPENGP_GAME_ID="${id}"
    OPENGP_GAME_TITLE="${title}"
    OPENGP_DOL_SHA1="${dol_sha1}"
    OPENGP_SYMBOLS="${src}/config/symbols.txt")
  set_target_properties(${executable} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${bin})
endfunction()

opengp_game(mkgp2)  # Mario Kart Arcade GP 2 (USA, GGPE02): the main game
opengp_game(mkgp1)  # Mario Kart Arcade GP (USA, GGPE01)
