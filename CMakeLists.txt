cmake_minimum_required(VERSION 3.20)

project(message_db_sqlite VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(MSGDB_STRICT_WARNINGS "Build with strict C warnings as errors" ON)
option(MSGDB_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(MSGDB_BUILD_FUZZERS "Build libFuzzer harnesses" OFF)

find_package(SQLite3 REQUIRED)
find_package(Threads REQUIRED)

add_library(msgdb_core STATIC
  extension/msgdb_core.c
)
set_target_properties(msgdb_core PROPERTIES
  POSITION_INDEPENDENT_CODE ON
)
target_include_directories(msgdb_core PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/extension"
)

add_library(message_db MODULE
  extension/message_db.c
)
target_link_libraries(message_db PRIVATE msgdb_core SQLite::SQLite3)
target_include_directories(message_db PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/extension"
)
set_target_properties(message_db PROPERTIES
  PREFIX ""
  OUTPUT_NAME "message_db"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/extension"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/extension"
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/extension"
  WINDOWS_EXPORT_ALL_SYMBOLS ON
)

add_library(msgdb_extension_static STATIC
  extension/message_db.c
)
target_compile_definitions(msgdb_extension_static PRIVATE MSGDB_STATIC=1)
target_link_libraries(msgdb_extension_static PRIVATE msgdb_core SQLite::SQLite3)
target_include_directories(msgdb_extension_static PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/extension"
)

add_executable(msgdb_core_tests
  test/c/test_core.c
)
target_link_libraries(msgdb_core_tests PRIVATE msgdb_core)

add_executable(msgdb_sql_tests
  test/c/test_sql.c
)
target_link_libraries(msgdb_sql_tests PRIVATE msgdb_extension_static SQLite::SQLite3 Threads::Threads)
target_include_directories(msgdb_sql_tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/extension"
)
if(NOT WIN32)
  target_compile_definitions(msgdb_sql_tests PRIVATE _POSIX_C_SOURCE=200809L)
endif()

function(msgdb_apply_warnings target)
  if(NOT MSGDB_STRICT_WARNINGS)
    return()
  endif()

  if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options("${target}" PRIVATE
      -Wall
      -Wextra
      -Wpedantic
      -Wconversion
      -Wsign-conversion
      -Wshadow
      -Wstrict-prototypes
      -Wmissing-prototypes
      -Wformat=2
      -Werror
    )
  endif()
endfunction()

function(msgdb_apply_sanitizers target)
  if(NOT MSGDB_ENABLE_SANITIZERS)
    return()
  endif()

  if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options("${target}" PRIVATE
      -fsanitize=address,undefined
      -fno-omit-frame-pointer
    )
    target_link_options("${target}" PRIVATE
      -fsanitize=address,undefined
    )
  endif()
endfunction()

foreach(target msgdb_core message_db msgdb_extension_static msgdb_core_tests msgdb_sql_tests)
  msgdb_apply_warnings("${target}")
  msgdb_apply_sanitizers("${target}")
endforeach()

if(MSGDB_BUILD_FUZZERS)
  include(CheckCSourceCompiles)

  set(CMAKE_REQUIRED_FLAGS "-fsanitize=fuzzer,address,undefined")
  check_c_source_compiles(
    "int LLVMFuzzerTestOneInput(const unsigned char *data, unsigned long size) { (void)data; (void)size; return 0; }"
    MSGDB_HAS_LIBFUZZER
  )
  unset(CMAKE_REQUIRED_FLAGS)

  function(msgdb_add_fuzzer target source)
    add_executable("${target}" "${source}")
    target_include_directories("${target}" PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/extension"
      "${CMAKE_CURRENT_SOURCE_DIR}/fuzz"
    )

    if(MSGDB_HAS_LIBFUZZER)
      target_compile_options("${target}" PRIVATE -fsanitize=fuzzer,address,undefined)
      target_link_options("${target}" PRIVATE -fsanitize=fuzzer,address,undefined)
    else()
      target_compile_definitions("${target}" PRIVATE MSGDB_STANDALONE_FUZZER=1)
      if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options("${target}" PRIVATE -fsanitize=address,undefined)
        target_link_options("${target}" PRIVATE -fsanitize=address,undefined)
      endif()
    endif()

    msgdb_apply_warnings("${target}")
  endfunction()

  msgdb_add_fuzzer(fuzz_strings fuzz/fuzz_strings.c)
  target_link_libraries(fuzz_strings PRIVATE msgdb_core)

  msgdb_add_fuzzer(fuzz_sql_model fuzz/fuzz_sql_model.c)
  target_link_libraries(fuzz_sql_model PRIVATE
    msgdb_extension_static
    msgdb_core
    SQLite::SQLite3
  )

  msgdb_add_fuzzer(fuzz_vtab_args fuzz/fuzz_vtab_args.c)
  target_link_libraries(fuzz_vtab_args PRIVATE
    msgdb_extension_static
    msgdb_core
    SQLite::SQLite3
  )
endif()

include(CTest)
if(BUILD_TESTING)
  add_test(NAME c_core COMMAND msgdb_core_tests)
  add_test(
    NAME sql_integration
    COMMAND msgdb_sql_tests "${CMAKE_CURRENT_SOURCE_DIR}"
  )
endif()
