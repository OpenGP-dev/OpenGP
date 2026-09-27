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
  disc/disc.cpp
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
  machine.cpp
  os/boot.cpp
  os/crash.cpp
  os/printf.cpp
  os/threads.cpp
  triforce/baseboard.cpp
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
  target_link_libraries(opengp_machine PRIVATE winmm)
endif()

if(OPENGP_BUILD_VIDEO)
  add_subdirectory(video)
endif()
