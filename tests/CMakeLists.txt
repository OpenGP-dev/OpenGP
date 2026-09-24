add_executable(test_decoder test_decoder.cpp)
target_link_libraries(test_decoder PRIVATE gekko_isa doctest opengp_warnings)
add_test(NAME decoder COMMAND test_decoder)
