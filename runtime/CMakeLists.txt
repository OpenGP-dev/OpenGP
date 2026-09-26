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
  hw/dsp.cpp
  hw/exi.cpp
  hw/hardware.cpp
  hw/si.cpp
  hw/vi.cpp
  machine.cpp
  os/boot.cpp
  os/printf.cpp
  os/threads.cpp
  triforce/baseboard.cpp
  triforce/io_ports.cpp
  triforce/jvsio.cpp
  triforce/magcard.cpp
  triforce/mkgp.cpp
)
target_include_directories(opengp_machine PUBLIC .)
find_package(Threads REQUIRED)
target_link_libraries(opengp_machine
  PUBLIC ppc_runtime Threads::Threads
  PRIVATE opengp_warnings)
if(WIN32)
  target_link_libraries(opengp_machine PRIVATE winmm)
endif()
