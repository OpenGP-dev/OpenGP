add_executable(test_decoder test_decoder.cpp)
target_link_libraries(test_decoder PRIVATE gekko_isa doctest opengp_warnings)
add_test(NAME decoder COMMAND test_decoder)

add_executable(test_semantics test_semantics.cpp)
target_link_libraries(test_semantics PRIVATE ppc_runtime doctest opengp_warnings)
add_test(NAME semantics COMMAND test_semantics)
