# Runs tests/crash_probe for each kind of crash and checks that the process
# ends with exit code 3 and leaves a crash report that names the crash and has
# the callback's part (which sees os::crashing()).
#
#   cmake -DPROBE=<crash_probe> -DDIR=<scratch folder> -P check_crash.cmake

file(REMOVE_RECURSE "${DIR}")
file(MAKE_DIRECTORY "${DIR}")
foreach(how fatal null abort overflow)
  set(report "${DIR}/crash-${how}.txt")
  execute_process(COMMAND "${PROBE}" "${report}" ${how} RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err
                  TIMEOUT 60)
  if(NOT code STREQUAL "3")
    message(FATAL_ERROR "${how}: exit code ${code}, expected 3\n${err}")
  endif()
  if(NOT EXISTS "${report}")
    message(FATAL_ERROR "${how}: no crash report\n${err}")
  endif()
  file(READ "${report}" text)
  if(NOT text MATCHES "\\[crash\\] crash_probe" OR NOT text MATCHES "probe callback ran, crashing")
    message(FATAL_ERROR "${how}: the report is incomplete:\n${text}")
  endif()
  message(STATUS "${how}: report written")
endforeach()
