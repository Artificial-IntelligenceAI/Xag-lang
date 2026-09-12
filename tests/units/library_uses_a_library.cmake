# A program names `outer`; `outer` uses `inner`. The program's manifest has to
# list every library reached, so the compiler writes `inner` in, says so on
# standard error, and the program runs. On a copy, because the fixture must
# stay as it is for the next run.
file(REMOVE_RECURSE "${SCRATCH}")
file(MAKE_DIRECTORY "${SCRATCH}/chain")
file(COPY "${FIXTURES}/chain/main.xag" "${FIXTURES}/chain/Xag-Config.toml"
     DESTINATION "${SCRATCH}/chain")
file(COPY "${FIXTURES}/outer.xaglib" "${FIXTURES}/inner.xaglib" DESTINATION "${SCRATCH}")

execute_process(
  COMMAND "${XAGC}" run "${SCRATCH}/chain/main.xag"
  RESULT_VARIABLE outcome
  OUTPUT_VARIABLE said
  ERROR_VARIABLE told)
if(NOT outcome EQUAL 0)
  message("${told}")
  message(FATAL_ERROR "the program did not run")
endif()
if(NOT said STREQUAL "41\n")
  message(FATAL_ERROR "the program said `${said}`, and 41 was expected")
endif()
string(FIND "${told}" "was added to" added)
if(added EQUAL -1)
  message("${told}")
  message(FATAL_ERROR "the compiler did not say it added `inner` to the manifest")
endif()
file(READ "${SCRATCH}/chain/Xag-Config.toml" manifest)
string(FIND "${manifest}" "inner.xaglib" listed)
if(listed EQUAL -1)
  message("${manifest}")
  message(FATAL_ERROR "`inner.xaglib` was not written into the manifest")
endif()

# A second run has nothing to add and says nothing.
execute_process(
  COMMAND "${XAGC}" run "${SCRATCH}/chain/main.xag"
  RESULT_VARIABLE outcome
  OUTPUT_VARIABLE said
  ERROR_VARIABLE told)
if(NOT outcome EQUAL 0 OR NOT told STREQUAL "")
  message("${told}")
  message(FATAL_ERROR "the second run should have been quiet")
endif()
