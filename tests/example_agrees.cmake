# One example, run both ways, and the two answers compared.
#
# An example nobody runs is a comment. These were being read by hand and by
# nothing else — eight files showing what the language is for, none of which
# would have said a word if they stopped working.
#
# `reading.xag` asks stdin for lines, so every one of them is given none:
# an example that waits for input is an example that hangs a test run.
execute_process(
  COMMAND "${XAGC}" run "${SOURCE}"
  INPUT_FILE /dev/null
  RESULT_VARIABLE outcome
  OUTPUT_VARIABLE reading
  ERROR_VARIABLE trouble)
if(NOT outcome EQUAL 0)
  message("${trouble}")
  message(FATAL_ERROR "reading ${SOURCE} did not finish")
endif()

execute_process(
  COMMAND "${XAGC}" fast "${SOURCE}"
  INPUT_FILE /dev/null
  RESULT_VARIABLE outcome
  OUTPUT_VARIABLE running
  ERROR_VARIABLE trouble)
if(NOT outcome EQUAL 0)
  message("${trouble}")
  message(FATAL_ERROR "running ${SOURCE} did not finish")
endif()

if(NOT reading STREQUAL running)
  message("reading it:\n${reading}")
  message("running it:\n${running}")
  message(FATAL_ERROR "the two engines do not agree about ${SOURCE}")
endif()
