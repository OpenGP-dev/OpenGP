# The installer: builds the games on the player's machine (see installer.h).
add_executable(opengp_installer main.cpp installer.cpp process.cpp sha256.cpp)
target_link_libraries(opengp_installer PRIVATE gekkorecomp_lib opengp_settings tomlplusplus opengp_warnings)
set_target_properties(opengp_installer PROPERTIES OUTPUT_NAME OpenGP)
