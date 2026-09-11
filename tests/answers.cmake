# One program, run both ways, and each answer compared against what was
# expected — `expected.txt` beside the source. For the cases where agreeing is
# not enough: two engines that both ask a side they were told not to ask agree
# with each other and are both wrong.
file(READ "${SOURCE}" ignored)
get_filename_component(beside "${SOURCE}" DIRECTORY)
file(READ "${beside}/expected.txt" expected)
foreach(way run fast)
  execute_process(
    COMMAND "${XAGC}" ${way} "${SOURCE}"
    INPUT_FILE /dev/null
    RESULT_VARIABLE outcome
    OUTPUT_VARIABLE said
    ERROR_VARIABLE trouble)
  if(NOT outcome EQUAL 0)
    message("${trouble}")
    message(FATAL_ERROR "`xagc ${way}` on ${SOURCE} did not finish")
  endif()
  if(NOT said STREQUAL expected)
    message("expected:\n${expected}")
    message("`xagc ${way}` said:\n${said}")
    message(FATAL_ERROR "`xagc ${way}` did not answer what ${SOURCE} expects")
  endif()
endforeach()
