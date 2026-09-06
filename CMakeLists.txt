cmake_minimum_required(VERSION 3.20)

project(xag
  VERSION 0.0.0
  DESCRIPTION "The Xag programming language"
  LANGUAGES C CXX)  # C is unused, but LLVM's CMake config runs C probes

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Homebrew keeps LLVM keg-only, so it is not on the default search path.
# Give CMake a hint rather than making every developer pass -DCMAKE_PREFIX_PATH.
if(APPLE AND NOT LLVM_DIR)
  find_program(XAG_BREW brew)
  if(XAG_BREW)
    execute_process(COMMAND ${XAG_BREW} --prefix llvm
      OUTPUT_VARIABLE XAG_LLVM_PREFIX
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET)
    if(XAG_LLVM_PREFIX)
      list(APPEND CMAKE_PREFIX_PATH "${XAG_LLVM_PREFIX}")
    endif()
  endif()
endif()

# LLVM's config package insists on an exact version match, so ask for any
# LLVM and enforce our own floor afterwards.
find_package(LLVM REQUIRED CONFIG)
if(LLVM_VERSION_MAJOR LESS 23)
  message(FATAL_ERROR
    "Xag needs LLVM 23 or newer; found ${LLVM_PACKAGE_VERSION} at ${LLVM_DIR}")
endif()
message(STATUS "Xag: LLVM ${LLVM_PACKAGE_VERSION} from ${LLVM_DIR}")

# LLVM_DEFINITIONS arrives as one space-separated string; CMake needs a list.
separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND "${LLVM_DEFINITIONS}")

# 'core' already pulls Support in transitively; naming both makes the linker
# warn about a duplicate archive.
# `passes` pulls core in, and each target pulls its own pieces.
llvm_map_components_to_libnames(LLVM_LIBS passes ${LLVM_TARGETS_TO_BUILD})
list(REMOVE_DUPLICATES LLVM_LIBS)

# Which decimal the runtime carries. `software` is IEEE 754 decimal written out
# in C++ and runs anywhere; `hardware` is IBM's decimal floating-point unit,
# which only two architectures have. The choice is made here rather than at each
# call, so that neither costs anything over the other.
set(XAG_DECIMAL "software" CACHE STRING "software or hardware")
set_property(CACHE XAG_DECIMAL PROPERTY STRINGS software hardware)

# z/Architecture has the unit from z9, POWER from POWER6. Nothing else has one,
# and no amount of asking makes it appear.
set(XAG_HAS_DECIMAL_UNIT OFF)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(s390x|ppc64|ppc64le|powerpc64|powerpc64le)$")
  set(XAG_HAS_DECIMAL_UNIT ON)
endif()

if(XAG_DECIMAL STREQUAL "hardware" AND NOT XAG_HAS_DECIMAL_UNIT)
  message(FATAL_ERROR
    "XAG_DECIMAL=hardware, and ${CMAKE_SYSTEM_PROCESSOR} has no decimal unit.\n"
    "IBM's decimal floating point is on z/Architecture (s390x) from z9 and on "
    "POWER (ppc64, ppc64le) from POWER6. Every other machine works a decimal "
    "out in software, which is what XAG_DECIMAL=software asks for.")
endif()

# The one place a value is made, joined, counted, printed and freed. Every engine
# calls it, so none of them can disagree about what those mean.
add_library(xagrt STATIC runtime/xag_runtime.cpp runtime/xag_bin128.cpp runtime/xag_deci.cpp)
target_include_directories(xagrt PUBLIC runtime)
# The runtime is not a development artefact: a copy of it is linked into every
# program `xagc build` writes, so it is compiled for delivery whatever the build
# type says. Without this it inherited `Debug` and shipped unoptimised, which is
# invisible in a program doing arithmetic the backend emits inline and about
# five times over in one leaning on `deci` or `bin128`.
target_compile_options(xagrt PRIVATE -fno-exceptions -Wall -Wextra -O2)
if(XAG_DECIMAL STREQUAL "hardware")
  target_compile_definitions(xagrt PRIVATE XAG_DECIMAL_HARDWARE)
endif()

# The front end knows nothing about LLVM, so it builds and tests without it.
add_library(xagfront STATIC
  src/Source.cpp
  src/Diagnostic.cpp
  src/Lexer.cpp
  src/Parser.cpp
  src/AstPrinter.cpp
  src/Check.cpp
  src/Own.cpp
  src/MirBuild.cpp
  src/MirPrinter.cpp
  src/MirElaborate.cpp
  src/Interpret.cpp
  src/Regions.cpp
  src/Fast.cpp)
target_include_directories(xagfront PUBLIC include)
target_link_libraries(xagfront PUBLIC xagrt)
target_compile_options(xagfront PRIVATE -fno-exceptions -Wall -Wextra)

# The native backend is the only thing that links LLVM.
add_library(xagnative STATIC src/Native.cpp)
target_link_libraries(xagnative PUBLIC xagfront PRIVATE ${LLVM_LIBS})
target_include_directories(xagnative SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(xagnative PRIVATE ${LLVM_DEFINITIONS_LIST})
target_compile_options(xagnative PRIVATE -fno-exceptions -Wall -Wextra)

add_executable(xagc src/main.cpp)
target_link_libraries(xagc PRIVATE xagnative)
target_compile_definitions(xagc PRIVATE XAG_RUNTIME_LIB="${CMAKE_BINARY_DIR}/libxagrt.a")

# SYSTEM so LLVM's own headers never emit warnings into our build output.
target_include_directories(xagc SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(xagc PRIVATE ${LLVM_DEFINITIONS_LIST})
target_link_libraries(xagc PRIVATE ${LLVM_LIBS})

# LLVM is built without exceptions; match it so our objects agree with its headers.
target_compile_options(xagc PRIVATE -fno-exceptions -Wall -Wextra)

enable_testing()

foreach(suite lexer parser check own mir runtime interpret native regions fast)
  add_executable(${suite}_tests tests/${suite}_tests.cpp)
  if(${suite} STREQUAL native)
    target_link_libraries(${suite}_tests PRIVATE xagnative xagrt)
  else()
    target_link_libraries(${suite}_tests PRIVATE xagfront xagrt)
  endif()
  target_include_directories(${suite}_tests PRIVATE tests)
  target_compile_options(${suite}_tests PRIVATE -fno-exceptions -Wall -Wextra)
  add_test(NAME ${suite} COMMAND ${suite}_tests)
endforeach()

# Decimal, against something that shares no code with it. Three engines calling
# one runtime cannot catch a mistake inside that runtime — no vote between them
# ever will — so the answers are written down and an implementation with no
# shared ancestry is asked to disagree. Python's `decimal` is libmpdec, written
# from the same IBM specification and from nothing here. It is a test-only
# reference and never goes near the language.
add_executable(decimal_cases tests/decimal_cases.cpp)
target_link_libraries(decimal_cases PRIVATE xagrt)
target_compile_options(decimal_cases PRIVATE -fno-exceptions -Wall -Wextra)

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_FOUND)
  add_test(NAME decimal_against_libmpdec
           COMMAND ${CMAKE_COMMAND}
                   -DCASES=$<TARGET_FILE:decimal_cases>
                   -DCHECKER=${CMAKE_CURRENT_SOURCE_DIR}/tests/decimal_reference.py
                   -DPYTHON=${Python3_EXECUTABLE}
                   -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/decimal_against.cmake)
else()
  message(STATUS "no python3 found, so decimal is not checked against libmpdec")
endif()

# The decimal unit itself, on a machine that has one. Nothing here has: this
# cross-compiles a program with no operating system under it and hands it to
# QEMU, which emulates POWER's decimal instructions. Only registered when the
# three things it needs are present, because most machines have none of them.
# Apple's clang has no PowerPC target; the LLVM this compiler is built against
# does, and it is already found above.
find_program(XAG_QEMU_PPC64 qemu-system-ppc64)
find_program(XAG_LLD ld.lld HINTS ${LLVM_TOOLS_BINARY_DIR}
             /opt/homebrew/opt/lld/bin /usr/local/opt/lld/bin)
find_program(XAG_CROSS_CLANG clang HINTS ${LLVM_TOOLS_BINARY_DIR} NO_DEFAULT_PATH)
if(XAG_QEMU_PPC64 AND XAG_LLD AND XAG_CROSS_CLANG AND
   LLVM_TARGETS_TO_BUILD MATCHES "PowerPC")
  add_executable(power_decimal_compare tests/power/compare.cpp)
  target_link_libraries(power_decimal_compare PRIVATE xagrt)
  target_compile_options(power_decimal_compare PRIVATE -Wall -Wextra)

  add_test(NAME power_decimal_unit
           COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/power/run.sh)
  set_tests_properties(power_decimal_unit PROPERTIES
    ENVIRONMENT "CLANG=${XAG_CROSS_CLANG};LLD=${XAG_LLD};QEMU=${XAG_QEMU_PPC64};PATH=${LLVM_TOOLS_BINARY_DIR}:$ENV{PATH};COMPARE=$<TARGET_FILE:power_decimal_compare>"
    TIMEOUT 180)
else()
  message(STATUS "no qemu-system-ppc64, ld.lld or PowerPC target, "
                 "so the decimal unit is not asked")
endif()

# The decimal knob, asked of the compiler itself rather than of a library: what
# a build has is a fact about the build, and the only way to ask it is to run it.
add_test(NAME decimal_software
         COMMAND xagc --decimal=software run
                 ${CMAKE_CURRENT_SOURCE_DIR}/examples/counting.xag)

add_test(NAME decimal_hardware_is_refused
         COMMAND xagc --decimal=hardware run
                 ${CMAKE_CURRENT_SOURCE_DIR}/examples/counting.xag)
set_tests_properties(decimal_hardware_is_refused PROPERTIES
  WILL_FAIL TRUE)

# And it says why, rather than only saying no.
add_test(NAME decimal_hardware_says_why
         COMMAND xagc --decimal=hardware run
                 ${CMAKE_CURRENT_SOURCE_DIR}/examples/counting.xag)
set_tests_properties(decimal_hardware_says_why PROPERTIES
  PASS_REGULAR_EXPRESSION "this build has software decimal")
