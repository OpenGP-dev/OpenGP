# The installer: builds the games on the player's machine (see installer.h).
# A library, which the launcher (OpenGP-dev/opengp-launcher) uses too, and the
# command-line program, opengp-install. In a package, OpenGP is the launcher.
add_library(opengp_installer_lib STATIC installer.cpp process.cpp sha256.cpp)
# Included as "installer/installer.h" (this folder's process.h must not hide
# the system's <process.h>).
target_include_directories(opengp_installer_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(opengp_installer_lib
  PUBLIC opengp_settings
  PRIVATE gekkorecomp_lib tomlplusplus opengp_warnings)

add_executable(opengp_installer main.cpp)
target_link_libraries(opengp_installer PRIVATE opengp_installer_lib opengp_warnings)
set_target_properties(opengp_installer PROPERTIES OUTPUT_NAME opengp-install)
