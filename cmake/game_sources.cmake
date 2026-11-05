# The hand-written sources compiled into each game's executable, with the
# game's own patches.cpp and status.cpp: one list for the source build
# (games/CMakeLists.txt) and the installer package (cmake/package.cmake, which
# writes it into package.toml for the installer). Names in games/common/.
set(OPENGP_GAME_COMMON_SOURCES main.cpp arcade.cpp cabinet.cpp camera_use.cpp game_log.cpp link_pace.cpp menu_back.cpp sdk_hle.cpp)
set(OPENGP_GAME_OWN_SOURCES patches.cpp status.cpp)
