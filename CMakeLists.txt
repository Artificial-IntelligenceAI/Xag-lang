cmake_minimum_required(VERSION 3.20)

project(safetybolt
  VERSION 0.0.0
  DESCRIPTION "The SafetyBolt programming language"
  LANGUAGES C CXX)  # C is unused, but LLVM's CMake config runs C probes

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Homebrew keeps LLVM keg-only, so it is not on the default search path.
# Give CMake a hint rather than making every developer pass -DCMAKE_PREFIX_PATH.
if(APPLE AND NOT LLVM_DIR)
  find_program(SB_BREW brew)
  if(SB_BREW)
    execute_process(COMMAND ${SB_BREW} --prefix llvm
      OUTPUT_VARIABLE SB_LLVM_PREFIX
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET)
    if(SB_LLVM_PREFIX)
      list(APPEND CMAKE_PREFIX_PATH "${SB_LLVM_PREFIX}")
    endif()
  endif()
endif()

# LLVM's config package insists on an exact version match, so ask for any
# LLVM and enforce our own floor afterwards.
find_package(LLVM REQUIRED CONFIG)
if(LLVM_VERSION_MAJOR LESS 23)
  message(FATAL_ERROR
    "SafetyBolt needs LLVM 23 or newer; found ${LLVM_PACKAGE_VERSION} at ${LLVM_DIR}")
endif()
message(STATUS "SafetyBolt: LLVM ${LLVM_PACKAGE_VERSION} from ${LLVM_DIR}")

# LLVM_DEFINITIONS arrives as one space-separated string; CMake needs a list.
separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND "${LLVM_DEFINITIONS}")

# 'core' already pulls Support in transitively; naming both makes the linker
# warn about a duplicate archive.
llvm_map_components_to_libnames(LLVM_LIBS core)

add_executable(sbc src/main.cpp)

# SYSTEM so LLVM's own headers never emit warnings into our build output.
target_include_directories(sbc SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(sbc PRIVATE ${LLVM_DEFINITIONS_LIST})
target_link_libraries(sbc PRIVATE ${LLVM_LIBS})

# LLVM is built without exceptions; match it so our objects agree with its headers.
target_compile_options(sbc PRIVATE -fno-exceptions -Wall -Wextra)
