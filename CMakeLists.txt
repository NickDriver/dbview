cmake_minimum_required(VERSION 3.21)
project(dbview C CXX)   # CXX is needed only for the vendored webview (the app target)

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

# DuckDB: vendored prebuilt C library (run scripts/fetch_duckdb.sh). Dev links the shared
# lib; a static build is a release concern (SPEC §9).
set(DUCKDB_DIR ${CMAKE_SOURCE_DIR}/src/vendor/duckdb)
if(NOT EXISTS ${DUCKDB_DIR}/duckdb.h)
  message(FATAL_ERROR "DuckDB not vendored — run scripts/fetch_duckdb.sh")
endif()
if(APPLE)
  set(DUCKDB_LIB ${DUCKDB_DIR}/libduckdb.dylib)
else()
  set(DUCKDB_LIB ${DUCKDB_DIR}/libduckdb.so)
endif()

# Apply DuckDB include path, link, and runtime rpath to a target that compiles the engine.
function(dbview_use_duckdb tgt)
  target_include_directories(${tgt} PRIVATE ${DUCKDB_DIR})
  target_link_libraries(${tgt} PRIVATE ${DUCKDB_LIB})
  set_property(TARGET ${tgt} APPEND PROPERTY BUILD_RPATH ${DUCKDB_DIR})
endfunction()

# ---- engine sources: all of src/, minus the test runner, app entry, and vendor ----
file(GLOB_RECURSE DBVIEW_ENGINE_SRC CONFIGURE_DEPENDS src/*.c)
list(FILTER DBVIEW_ENGINE_SRC EXCLUDE REGEX "src/test/")
list(FILTER DBVIEW_ENGINE_SRC EXCLUDE REGEX "src/app/")
list(FILTER DBVIEW_ENGINE_SRC EXCLUDE REGEX "src/vendor/")

# ---- integration tests live in tests/ (Rust-style) ----
file(GLOB DBVIEW_INTEGRATION_SRC CONFIGURE_DEPENDS tests/*.c)

# ---- vendored third-party C (compiled without our strict warnings) ----
add_library(dbview_vendor STATIC src/vendor/cjson/cJSON.c)
target_include_directories(dbview_vendor PUBLIC src/vendor)
target_compile_options(dbview_vendor PRIVATE -w)

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
target_link_libraries(test_runner PRIVATE SQLite::SQLite3 dbview_vendor)
if(DBVIEW_SANITIZE)
  target_compile_options(test_runner PRIVATE ${DBVIEW_SAN_FLAGS})
  target_link_options(test_runner PRIVATE ${DBVIEW_SAN_FLAGS})
endif()
dbview_use_duckdb(test_runner)

enable_testing()
add_test(NAME unit COMMAND test_runner)

# ---------------------------------------------------------------------------
# dbview_engine: optimized static lib (no DB_TEST, NDEBUG). The app and other
# consumers link this. Built on demand (not part of `all` test flow noise).
# ---------------------------------------------------------------------------
add_library(dbview_engine STATIC ${DBVIEW_ENGINE_SRC})
target_include_directories(dbview_engine PUBLIC src)
target_compile_options(dbview_engine PRIVATE ${DBVIEW_WARN})
target_link_libraries(dbview_engine PUBLIC SQLite::SQLite3 dbview_vendor)
dbview_use_duckdb(dbview_engine)

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
target_link_libraries(leak_runner PRIVATE SQLite::SQLite3 dbview_vendor)
dbview_use_duckdb(leak_runner)

add_custom_target(analyze
  COMMAND clang --analyze -DDB_TEST -Isrc -Xclang -analyzer-output=text
          ${DBVIEW_ENGINE_SRC}
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  USES_TERMINAL
  COMMENT "Running clang static analyzer over engine sources")

# ---------------------------------------------------------------------------
# Native app: WebView shell (webview/webview) + React UI.
#   Vendor webview once with scripts/fetch_webview.sh, then:
#     cmake --build build --target dbview   (builds the UI, then the app)
#     ./build/dbview [path/to/file.sqlite]
# ---------------------------------------------------------------------------
set(WV_DIR ${CMAKE_SOURCE_DIR}/src/vendor/webview)

# Build the React UI bundle (ui/dist/index.html, inlined single file).
add_custom_target(ui
  COMMAND npm install --silent && npm run build
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/ui
  USES_TERMINAL
  COMMENT "Building React UI bundle")

if(EXISTS ${WV_DIR}/core/src/webview.cc)
  # webview is C++; compile it as its own lib with relaxed warnings.
  add_library(webview_lib STATIC ${WV_DIR}/core/src/webview.cc)
  target_include_directories(webview_lib PUBLIC ${WV_DIR}/core/include)
  target_compile_features(webview_lib PRIVATE cxx_std_17)
  target_compile_definitions(webview_lib PUBLIC WEBVIEW_STATIC)
  target_compile_options(webview_lib PRIVATE -w)

  add_executable(dbview EXCLUDE_FROM_ALL src/app/main.c ${DBVIEW_ENGINE_SRC})
  target_include_directories(dbview PRIVATE src)
  target_compile_options(dbview PRIVATE ${DBVIEW_WARN})
  target_link_libraries(dbview PRIVATE SQLite::SQLite3 dbview_vendor webview_lib)
  set_target_properties(dbview PROPERTIES LINKER_LANGUAGE CXX)  # link the C++ runtime for webview
  dbview_use_duckdb(dbview)
  if(APPLE)
    target_link_libraries(dbview PRIVATE "-framework WebKit" "-framework Cocoa")
    # native file dialogs (Objective-C, ARC) in their own lib with relaxed warnings
    enable_language(OBJC)
    add_library(dbview_macos STATIC src/app/file_dialog.m)
    target_include_directories(dbview_macos PRIVATE src/app)
    target_compile_options(dbview_macos PRIVATE -fobjc-arc -w)
    target_compile_definitions(dbview PRIVATE DBVIEW_MACOS)
    target_link_libraries(dbview PRIVATE dbview_macos "-framework AppKit")
  endif()
  add_dependencies(dbview ui)
else()
  message(STATUS "webview not vendored — run scripts/fetch_webview.sh to enable the 'dbview' app target")
endif()
