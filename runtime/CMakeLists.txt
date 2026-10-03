# The CPU core: needed by everything that runs guest code (tests included).
add_library(ppc_runtime STATIC
  ppc/dispatch.cpp
  ppc/fp.cpp
  ppc/hooks.cpp
  ppc/interpreter.cpp
  ppc/memory.cpp
  ppc/module.cpp
  ppc/ops.cpp
  ppc/trace.cpp
)
target_include_directories(ppc_runtime PUBLIC .)
target_link_libraries(ppc_runtime
  PUBLIC gekko_isa opengp_guest_flags
  PRIVATE opengp_warnings)

# The emulated machine around the CPU: hardware, OS support, boot, disc.
add_library(opengp_machine STATIC
  audio/wav.cpp
  disc/disc.cpp
  dsp/accelerator.cpp
  dsp/ax.cpp
  gx/decoder.cpp
  gx/frontend.cpp
  gx/gpu.cpp
  gx/texture.cpp
  gx/transform.cpp
  gx/vertex_loader.cpp
  hw/ai.cpp
  hw/di.cpp
  hw/dsp.cpp
  hw/exi.cpp
  hw/hardware.cpp
  hw/si.cpp
  hw/vi.cpp
  input/script.cpp
  machine.cpp
  os/atomic_file.cpp
  os/boot.cpp
  os/crash.cpp
  os/printf.cpp
  os/profiler.cpp
  os/threads.cpp
  triforce/baseboard.cpp
  triforce/card_folder.cpp
  triforce/io_ports.cpp
  triforce/jvsio.cpp
  triforce/mediaboard.cpp
  triforce/magcard.cpp
  triforce/mkgp.cpp
)
target_include_directories(opengp_machine PUBLIC .)

# The boot ROM's fonts (free replacements; see thirdparty/gc-fonts), embedded.
set(font_dir ${PROJECT_SOURCE_DIR}/thirdparty/gc-fonts)
foreach(font western japanese)
  set(out ${CMAKE_CURRENT_BINARY_DIR}/ipl_font_${font}.cpp)
  add_custom_command(
    OUTPUT ${out}
    COMMAND ${CMAKE_COMMAND} -DINPUT=${font_dir}/font_${font}.bin -DOUTPUT=${out} -DNAME=kIplFont_${font}
            -P ${PROJECT_SOURCE_DIR}/cmake/embed.cmake
    DEPENDS ${font_dir}/font_${font}.bin ${PROJECT_SOURCE_DIR}/cmake/embed.cmake
    VERBATIM)
  target_sources(opengp_machine PRIVATE ${out})
endforeach()
find_package(Threads REQUIRED)
target_link_libraries(opengp_machine
  PUBLIC ppc_runtime Threads::Threads
  PRIVATE opengp_warnings)
if(WIN32)
  target_link_libraries(opengp_machine PRIVATE winmm dbghelp)
endif()

if(OPENGP_BUILD_VIDEO)
  add_subdirectory(video)

  # The player's input devices (SDL3, fetched by video/).
  add_library(opengp_input STATIC input/host.cpp)
  target_link_libraries(opengp_input
    PUBLIC opengp_machine SDL3::SDL3
    PRIVATE tomlplusplus opengp_warnings)

  # Sound output (SDL3).
  add_library(opengp_audio STATIC audio/host.cpp)
  target_link_libraries(opengp_audio
    PUBLIC opengp_machine SDL3::SDL3
    PRIVATE opengp_warnings)

  # Dear ImGui, for the menu: the pinned release, built with its SDL3 and
  # SDL GPU backends.
  include(FetchContent)
  FetchContent_Declare(imgui
    URL https://github.com/ocornut/imgui/archive/refs/tags/v1.92.9b.tar.gz
    URL_HASH SHA256=21d8a0a565e85dce943e375db00812c2f3f0ab21f3f0f7964e364a63422d7f99
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(imgui)
  add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlgpu3.cpp)
  target_include_directories(imgui SYSTEM PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
  target_link_libraries(imgui PUBLIC SDL3::SDL3)
endif()

# The player's settings (user/settings.toml).
add_library(opengp_settings STATIC app/settings.cpp)
target_link_libraries(opengp_settings
  PUBLIC opengp_machine
  PRIVATE tomlplusplus opengp_warnings)

# The launcher the games share: settings, window, sound, controls, menu.
add_library(opengp_app STATIC app/launcher.cpp)
target_link_libraries(opengp_app
  PUBLIC opengp_machine opengp_settings
  PRIVATE opengp_warnings)
target_compile_definitions(opengp_app PRIVATE OPENGP_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
if(OPENGP_BUILD_VIDEO)
  target_sources(opengp_app PRIVATE app/menu.cpp)
  target_link_libraries(opengp_app PUBLIC opengp_video opengp_input opengp_audio imgui)
  target_compile_definitions(opengp_app PRIVATE OPENGP_VIDEO)
endif()
