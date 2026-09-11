# One program, run both ways, and each answer compared against what was
# expected — `expected.txt` beside the source. For the cases where agreeing is
# not enough: two engines that both ask a side they were told not to ask agree
# with each other and are both wrong.
#
# All three engines: `xagc run`, `xagc fast`, and the program `xagc build`
# leaves beside the source.
get_filename_component(beside "${SOURCE}" DIRECTORY)
get_filename_component(stem "${SOURCE}" NAME_WE)
file(READ "${beside}/expected.txt" expected)

execute_process(
  COMMAND "${XAGC}" build "${SOURCE}"
  RESULT_VARIABLE outcome
  OUTPUT_VARIABLE ignored
  ERROR_VARIABLE trouble)
if(NOT outcome EQUAL 0)
  message("${trouble}")
  message(FATAL_ERROR "`xagc build` on ${SOURCE} did not finish")
endif()

foreach(way run fast built)
  if(way STREQUAL built)
    set(command "${beside}/${stem}")
  else()
    set(command "${XAGC}" ${way} "${SOURCE}")
  endif()
  execute_process(
    COMMAND ${command}
    INPUT_FILE /dev/null
    RESULT_VARIABLE outcome
    OUTPUT_VARIABLE said
    ERROR_VARIABLE trouble)
  if(NOT outcome EQUAL 0)
    message("${trouble}")
    message(FATAL_ERROR "`${way}` on ${SOURCE} did not finish")
  endif()
  if(NOT said STREQUAL expected)
    message("expected:\n${expected}")
    message("`${way}` said:\n${said}")
    message(FATAL_ERROR "`${way}` did not answer what ${SOURCE} expects")
  endif()
endforeach()
