# Run the cases through the reference and fail on any disagreement. A script,
# because ctest has no pipe of its own and the two halves have to meet somewhere.
execute_process(
  COMMAND "${CASES}" 20000 1
  COMMAND "${PYTHON}" "${CHECKER}"
  RESULT_VARIABLE outcome
  OUTPUT_VARIABLE said
  ERROR_VARIABLE trouble)
message("${said}")
if(NOT trouble STREQUAL "")
  message("${trouble}")
endif()
if(NOT outcome EQUAL 0)
  message(FATAL_ERROR "decimal and libmpdec do not agree")
endif()
