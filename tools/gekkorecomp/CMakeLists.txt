add_library(gekkorecomp_lib STATIC
  lib/analysis.cpp
  lib/codegen.cpp
  lib/config.cpp
  lib/dol.cpp
  lib/symbols.cpp
)
target_include_directories(gekkorecomp_lib PUBLIC lib)
target_link_libraries(gekkorecomp_lib PUBLIC gekko_isa PRIVATE tomlplusplus opengp_warnings)

add_executable(gekkorecomp main.cpp)
target_link_libraries(gekkorecomp PRIVATE gekkorecomp_lib opengp_warnings)

# opengp_recompile(<target> CONFIG <recomp.toml> FILE_COUNT <n> OUTPUT_DIR <dir>
#                  [DOL <path>] [SYMBOLS <path>] [DEPENDS ...])
#
# Runs gekkorecomp at build time and adds the generated sources to <target>.
# FILE_COUNT must match [output] file_count in the config.
function(opengp_recompile target)
  cmake_parse_arguments(ARG "" "CONFIG;FILE_COUNT;OUTPUT_DIR;DOL;SYMBOLS" "DEPENDS" ${ARGN})
  set(outputs
    ${ARG_OUTPUT_DIR}/ppc_recomp_shared.h
    ${ARG_OUTPUT_DIR}/ppc_func_mapping.cpp)
  math(EXPR last "${ARG_FILE_COUNT} - 1")
  foreach(i RANGE 0 ${last})
    list(APPEND outputs ${ARG_OUTPUT_DIR}/ppc_recomp.${i}.cpp)
  endforeach()
  set(extra_args)
  set(extra_deps)
  if(ARG_DOL)
    list(APPEND extra_args --dol ${ARG_DOL})
    list(APPEND extra_deps ${ARG_DOL})
  endif()
  if(ARG_SYMBOLS)
    list(APPEND extra_args --symbols ${ARG_SYMBOLS})
    list(APPEND extra_deps ${ARG_SYMBOLS})
  endif()
  add_custom_command(
    OUTPUT ${outputs}
    COMMAND gekkorecomp ${ARG_CONFIG} --out ${ARG_OUTPUT_DIR} ${extra_args}
    DEPENDS gekkorecomp ${ARG_CONFIG} ${extra_deps} ${ARG_DEPENDS}
    COMMENT "Recompiling ${ARG_CONFIG}"
    VERBATIM)
  target_sources(${target} PRIVATE ${outputs})
  target_include_directories(${target} PRIVATE ${ARG_OUTPUT_DIR})
endfunction()
