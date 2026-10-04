# Mario Kart Arcade GP (USA, GGPE01)

set(MKGP1_DOL_SHA1 0e84df7cac27eab7d6dbb1892933ae3bf762f742)
opengp_game_dol(MKGP1_DOL GGPE01 ${MKGP1_DOL_SHA1} "Mario Kart Arcade GP")
if(NOT MKGP1_DOL)
  return()
endif()

# The recompiled game code, as a library so the launcher and future tools can share it.
add_library(mkgp1_recomp STATIC)
opengp_recompile(mkgp1_recomp
  CONFIG ${CMAKE_CURRENT_SOURCE_DIR}/recomp.toml
  FILE_COUNT 64
  OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated
  DOL ${MKGP1_DOL}
  SYMBOLS ${CMAKE_CURRENT_SOURCE_DIR}/config/symbols.txt)
target_include_directories(mkgp1_recomp PUBLIC ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(mkgp1_recomp PUBLIC ppc_runtime)
opengp_generated_code_options(mkgp1_recomp)

add_executable(opengp-mkgp1 main.cpp patches.cpp ${OPENGP_GAMES_COMMON})
target_link_libraries(opengp-mkgp1 PRIVATE mkgp1_recomp opengp_app opengp_warnings)
target_include_directories(opengp-mkgp1 PRIVATE ${PROJECT_SOURCE_DIR}/games/common)
target_compile_definitions(opengp-mkgp1 PRIVATE
  OPENGP_SYMBOLS="${CMAKE_CURRENT_SOURCE_DIR}/config/symbols.txt"
  OPENGP_DOL_SHA1="${MKGP1_DOL_SHA1}")
