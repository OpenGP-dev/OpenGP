add_library(gekko_isa STATIC
  src/isa.cpp
  src/disasm.cpp
)
target_include_directories(gekko_isa PUBLIC include)
target_link_libraries(gekko_isa PRIVATE opengp_warnings)
