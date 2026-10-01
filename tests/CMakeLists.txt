add_executable(test_decoder test_decoder.cpp)
target_link_libraries(test_decoder PRIVATE gekko_isa doctest opengp_warnings)
add_test(NAME decoder COMMAND test_decoder)

add_executable(test_semantics test_semantics.cpp)
target_link_libraries(test_semantics PRIVATE ppc_runtime doctest opengp_warnings)
add_test(NAME semantics COMMAND test_semantics)

# Differential test: fixturegen writes a DOL of test functions, gekkorecomp
# recompiles it, and test_diff compares the recompiled code with the interpreter.
add_executable(fixturegen fixturegen.cpp)
target_link_libraries(fixturegen PRIVATE gekkorecomp_lib gekko_isa opengp_warnings)

set(FIXTURE_DIR ${CMAKE_CURRENT_BINARY_DIR}/fixture)
add_custom_command(
  OUTPUT ${FIXTURE_DIR}/fixture.dol ${FIXTURE_DIR}/fixture_symbols.txt ${FIXTURE_DIR}/fixture.toml
         ${FIXTURE_DIR}/fixture_cases.inc ${FIXTURE_DIR}/fixture_layout.h
  COMMAND fixturegen ${FIXTURE_DIR}
  DEPENDS fixturegen
  COMMENT "Generating the differential-test fixture"
  VERBATIM)

add_executable(test_diff test_diff.cpp
  ${FIXTURE_DIR}/fixture_cases.inc ${FIXTURE_DIR}/fixture_layout.h)
opengp_recompile(test_diff
  CONFIG ${FIXTURE_DIR}/fixture.toml
  FILE_COUNT 4
  OUTPUT_DIR ${FIXTURE_DIR}/generated
  DEPENDS ${FIXTURE_DIR}/fixture.dol ${FIXTURE_DIR}/fixture_symbols.txt)
target_include_directories(test_diff PRIVATE ${FIXTURE_DIR})
target_compile_definitions(test_diff PRIVATE
  FIXTURE_DOL="${FIXTURE_DIR}/fixture.dol"
  FIXTURE_GENERATED="${FIXTURE_DIR}/generated")
target_link_libraries(test_diff PRIVATE ppc_runtime doctest opengp_warnings)
add_test(NAME diff COMMAND test_diff)

add_executable(test_recomp_inputs test_recomp_inputs.cpp)
target_link_libraries(test_recomp_inputs PRIVATE gekkorecomp_lib doctest opengp_warnings)
add_test(NAME recomp_inputs COMMAND test_recomp_inputs)

add_executable(test_input test_input.cpp)
target_link_libraries(test_input PRIVATE opengp_machine doctest opengp_warnings)
add_test(NAME input COMMAND test_input)

add_executable(test_dsp test_dsp.cpp)
target_link_libraries(test_dsp PRIVATE opengp_machine doctest opengp_warnings)
add_test(NAME dsp COMMAND test_dsp)
