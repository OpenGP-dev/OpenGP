# Each game is built only when its executable has been extracted into orig/
# (see docs/game-files.md). Nothing from the games is ever committed.

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
  if(NOT OPENGP_GENERATED_DEBUG_INFO)
    set_property(TARGET ${target} PROPERTY MSVC_DEBUG_INFORMATION_FORMAT "")
    if(NOT CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
      target_compile_options(${target} PRIVATE -g0)
    endif()
  endif()
endfunction()

add_subdirectory(mkgp2)
