#ifndef DB_TEST_H
#define DB_TEST_H
/*
 * dbview — minimal, auto-registering unit-test harness (Phase 0).
 *
 * Rust-style ergonomics in C:
 *   - Unit tests live next to the code, inside `#ifdef DB_TEST ... #endif`.
 *   - Each TEST(...) self-registers via a constructor — no manual list to keep.
 *   - One `test_runner` binary discovers & runs everything.
 *
 * AI-first: assertions print file:line + the ACTUAL values, the runner names the
 * crashing test on a signal, supports --filter / --json / --list, and prints a
 * copy-paste command to re-run any single failure.
 *
 * NOTE (cross-platform): __attribute__((constructor)) auto-registration is clang/gcc.
 * The Windows/MSVC phase will need a linker-section registration shim (SPEC §9).
 */
#include "db_error.h"
#include <string.h>

typedef struct db_test db_test;
typedef void (*db_test_fn)(db_test *t);

struct db_test {
  const char *suite;
  const char *name;
  const char *file;
  int         line;
  db_test_fn  fn;
  int         failures;  /* set during a run */
  db_test    *next;
};

void db_test_register(db_test *t);
void db_test_fail(db_test *t, const char *file, int line, const char *fmt, ...)
    DB_PRINTF(4, 5);
int  db_test_main(int argc, char **argv);

/* Define a test. Body receives `db_test *t` (used implicitly by the macros). */
#define TEST(suite_, name_) \
  static void dbt_fn_##suite_##_##name_(db_test *t); \
  static db_test dbt_node_##suite_##_##name_ = { \
      #suite_, #name_, __FILE__, __LINE__, dbt_fn_##suite_##_##name_, 0, NULL }; \
  __attribute__((constructor)) \
  static void dbt_ctor_##suite_##_##name_(void) { \
    db_test_register(&dbt_node_##suite_##_##name_); } \
  static void dbt_fn_##suite_##_##name_(db_test *t)

/* ---- assertions ----
 * EXPECT_* record a failure but continue; ASSERT_* record and stop the test. */
#define EXPECT(cond) \
  do { if (!(cond)) db_test_fail(t, __FILE__, __LINE__, "EXPECT failed: %s", #cond); } while (0)

#define ASSERT(cond) \
  do { if (!(cond)) { db_test_fail(t, __FILE__, __LINE__, "ASSERT failed: %s", #cond); return; } } while (0)

#define FAILF(...) \
  do { db_test_fail(t, __FILE__, __LINE__, __VA_ARGS__); return; } while (0)

#define ASSERT_TRUE(a)  ASSERT(a)
#define ASSERT_FALSE(a) ASSERT(!(a))

#define ASSERT_EQ_INT(a, b) \
  do { long long _a = (long long)(a), _b = (long long)(b); \
       if (_a != _b) { db_test_fail(t, __FILE__, __LINE__, \
           "ASSERT_EQ_INT(%s, %s): %lld != %lld", #a, #b, _a, _b); return; } } while (0)

#define ASSERT_STR_EQ(a, b) \
  do { const char *_a = (a), *_b = (b); \
       if (strcmp(_a ? _a : "", _b ? _b : "") != 0) { db_test_fail(t, __FILE__, __LINE__, \
           "ASSERT_STR_EQ(%s, %s): \"%s\" != \"%s\"", #a, #b, \
           _a ? _a : "(null)", _b ? _b : "(null)"); return; } } while (0)

/* expect DB_OK; on failure print the error name + last-error message */
#define ASSERT_OK(expr) \
  do { db_err _e = (expr); \
       if (_e != DB_OK) { db_test_fail(t, __FILE__, __LINE__, \
           "ASSERT_OK(%s): got %s — %s", #expr, db_err_name(_e), db_last_error()->message); \
           return; } } while (0)

/* expect a specific error code */
#define ASSERT_ERR_EQ(expr, want) \
  do { db_err _e = (expr); \
       if (_e != (want)) { db_test_fail(t, __FILE__, __LINE__, \
           "ASSERT_ERR_EQ(%s): expected %s, got %s", #expr, db_err_name(want), db_err_name(_e)); \
           return; } } while (0)

#endif /* DB_TEST_H */
