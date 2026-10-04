# Mario Kart Arcade GP 2 (USA, GGPE02)

opengp_game_dol(MKGP2_DOL GGPE02 2969c177219dda71ad36b41575bede7ad4637f96 "Mario Kart Arcade GP 2")
if(NOT MKGP2_DOL)
  return()
endif()

# The recompiled game code, as a library so the launcher and future tools can share it.
add_library(mkgp2_recomp STATIC)
opengp_recompile(mkgp2_recomp
  CONFIG ${CMAKE_CURRENT_SOURCE_DIR}/recomp.toml
  FILE_COUNT 64
  OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated
  DOL ${MKGP2_DOL}
  SYMBOLS ${CMAKE_CURRENT_SOURCE_DIR}/config/symbols.txt)
target_include_directories(mkgp2_recomp PUBLIC ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(mkgp2_recomp PUBLIC ppc_runtime)
opengp_generated_code_options(mkgp2_recomp)

add_executable(opengp-mkgp2 main.cpp patches.cpp ${OPENGP_GAMES_COMMON})
target_link_libraries(opengp-mkgp2 PRIVATE mkgp2_recomp opengp_app opengp_warnings)
target_include_directories(opengp-mkgp2 PRIVATE ${PROJECT_SOURCE_DIR}/games/common)
target_compile_definitions(opengp-mkgp2 PRIVATE
  OPENGP_SYMBOLS="${CMAKE_CURRENT_SOURCE_DIR}/config/symbols.txt")
