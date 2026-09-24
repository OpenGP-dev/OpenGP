add_library(ppc_runtime STATIC
  ppc/dispatch.cpp
  ppc/fp.cpp
  ppc/hooks.cpp
  ppc/interpreter.cpp
  ppc/memory.cpp
  ppc/module.cpp
  ppc/ops.cpp
)
target_include_directories(ppc_runtime PUBLIC .)
target_link_libraries(ppc_runtime
  PUBLIC gekko_isa opengp_guest_flags
  PRIVATE opengp_warnings)
