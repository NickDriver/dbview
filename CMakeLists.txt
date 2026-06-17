cmake_minimum_required(VERSION 3.21)
project(dbview C)

# ---- standard: C23 ----
set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---- options ----
option(DBVIEW_SANITIZE "Build with ASan+UBSan (default ON in Debug)" OFF)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  set(DBVIEW_SANITIZE ON)
endif()

# ---- warnings: high-signal, errors. Conversion warnings stay visible but advisory ----
set(DBVIEW_WARN
  -Wall -Wextra -Wpedantic -Werror
  -Wshadow -Wstrict-prototypes -Wmissing-prototypes
  -Wmissing-variable-declarations
  -Wconversion -Wsign-conversion
  -Wno-error=conversion -Wno-error=sign-conversion)

set(DBVIEW_SAN_FLAGS
  -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)

# ---- dependencies ----
find_package(SQLite3 REQUIRED)

# ---- engine sources: all of src/, minus the test runner and (future) app entry ----
file(GLOB_RECURSE DBVIEW_ENGINE_SRC CONFIGURE_DEPENDS src/*.c)
list(FILTER DBVIEW_ENGINE_SRC EXCLUDE REGEX "src/test/")
list(FILTER DBVIEW_ENGINE_SRC EXCLUDE REGEX "src/app/")

# ---- integration tests live in tests/ (Rust-style) ----
file(GLOB DBVIEW_INTEGRATION_SRC CONFIGURE_DEPENDS tests/*.c)

# ---------------------------------------------------------------------------
# test_runner: engine (compiled with -DDB_TEST so inline unit tests register)
#              + integration tests + the runner. Sanitized.
# ---------------------------------------------------------------------------
add_executable(test_runner
  ${DBVIEW_ENGINE_SRC}
  ${DBVIEW_INTEGRATION_SRC}
  src/test/db_test.c
  src/test/test_main.c)
target_include_directories(test_runner PRIVATE src)
target_compile_definitions(test_runner PRIVATE DB_TEST)
target_compile_options(test_runner PRIVATE ${DBVIEW_WARN})
target_link_libraries(test_runner PRIVATE SQLite::SQLite3)
if(DBVIEW_SANITIZE)
  target_compile_options(test_runner PRIVATE ${DBVIEW_SAN_FLAGS})
  target_link_options(test_runner PRIVATE ${DBVIEW_SAN_FLAGS})
endif()

enable_testing()
add_test(NAME unit COMMAND test_runner)

# ---------------------------------------------------------------------------
# dbview_engine: optimized static lib (no DB_TEST, NDEBUG). The app and other
# consumers link this. Built on demand (not part of `all` test flow noise).
# ---------------------------------------------------------------------------
add_library(dbview_engine STATIC ${DBVIEW_ENGINE_SRC})
target_include_directories(dbview_engine PUBLIC src)
target_compile_options(dbview_engine PRIVATE ${DBVIEW_WARN})
target_link_libraries(dbview_engine PUBLIC SQLite::SQLite3)

# ---------------------------------------------------------------------------
# Tooling targets (parity with the money_books Makefile).
#   make leaks    — run tests under Apple's `leaks` (no ASan; it replaces malloc)
#   make analyze  — clang static analyzer over engine sources
# ---------------------------------------------------------------------------
add_custom_target(leaks
  COMMAND ${CMAKE_COMMAND} -E env MallocStackLogging=1
          leaks --atExit -- $<TARGET_FILE:leak_runner>
  DEPENDS leak_runner
  USES_TERMINAL)
add_executable(leak_runner EXCLUDE_FROM_ALL
  ${DBVIEW_ENGINE_SRC} ${DBVIEW_INTEGRATION_SRC}
  src/test/db_test.c src/test/test_main.c)
target_include_directories(leak_runner PRIVATE src)
target_compile_definitions(leak_runner PRIVATE DB_TEST)
target_compile_options(leak_runner PRIVATE ${DBVIEW_WARN} -O1 -g)
target_link_libraries(leak_runner PRIVATE SQLite::SQLite3)

add_custom_target(analyze
  COMMAND clang --analyze -DDB_TEST -Isrc -Xclang -analyzer-output=text
          ${DBVIEW_ENGINE_SRC}
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  USES_TERMINAL
  COMMENT "Running clang static analyzer over engine sources")
