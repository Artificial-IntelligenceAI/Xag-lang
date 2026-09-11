# A mistake in a library is reported in the library's file, at the library's
# line. It was reported against the program's file at the same offset, which
# pointed at whatever happened to be there.
execute_process(
  COMMAND "${XAGC}" check "${SOURCE}"
  RESULT_VARIABLE outcome
  OUTPUT_VARIABLE out
  ERROR_VARIABLE err)
if(outcome EQUAL 0)
  message(FATAL_ERROR "the mistake in the library was not reported at all")
endif()
string(FIND "${err}" "mistaken/lib.xag, line: 6" where)
if(where EQUAL -1)
  message("${err}")
  message(FATAL_ERROR "the mistake was not reported in mistaken/lib.xag at line 6")
endif()
string(FIND "${err}" "var.str 's' = ['n']" shown)
if(shown EQUAL -1)
  message("${err}")
  message(FATAL_ERROR "the line shown is not the library's line")
endif()
