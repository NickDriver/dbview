#include "db_error.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static _Thread_local db_error_ctx g_err;

const char *db_err_name(db_err e) {
  switch (e) {
#define DB_NAME(name, msg) case name: return #name;
    DB_ERR_LIST(DB_NAME)
#undef DB_NAME
    default: return "DB_ERR_UNKNOWN";
  }
}

const char *db_err_default_msg(db_err e) {
  switch (e) {
#define DB_MSG(name, msg) case name: return msg;
    DB_ERR_LIST(DB_MSG)
#undef DB_MSG
    default: return "unknown error";
  }
}

const db_error_ctx *db_last_error(void) { return &g_err; }

void db_clear_error(void) {
  memset(&g_err, 0, sizeof g_err);
  g_err.code = DB_OK;
}

db_err db_set_error(db_err code, const char *file, int line, const char *func,
                    const char *fmt, ...) {
  g_err.code = code;
  g_err.file = file;
  g_err.line = line;
  g_err.func = func;
  g_err.trace[0] = '\0';
  if (fmt && fmt[0]) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_err.message, sizeof g_err.message, fmt, ap);
    va_end(ap);
  } else {
    snprintf(g_err.message, sizeof g_err.message, "%s", db_err_default_msg(code));
  }
  return code;
}

void db_error_add_trace(const char *file, int line, const char *func,
                        const char *expr) {
  size_t n = strlen(g_err.trace);
  if (n >= sizeof g_err.trace - 1) return;
  snprintf(g_err.trace + n, sizeof g_err.trace - n, "%s%s:%d %s [%s]",
           n ? " <- " : "", file, line, func, expr);
}

void db_invariant_fail(const char *expr, const char *file, int line,
                       const char *func, const char *fmt, ...) {
  fprintf(stderr, "\n*** DB_INVARIANT VIOLATED ***\n  at %s:%d in %s()\n  cond: %s\n",
          file, line, func, expr);
  if (fmt && fmt[0]) {
    fputs("  note: ", stderr);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
  }
  fflush(stderr);
  abort();
}

void db_log(db_log_level lvl, const char *file, int line, const char *fmt, ...) {
  static const char *tag[] = {"DEBUG", "INFO", "WARN", "ERROR"};
  fprintf(stderr, "[%-5s] %s:%d: ", tag[lvl], file, line);
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
}

/* ---------------------------------------------------------------------------
 * Unit tests (compiled only into the test runner via -DDB_TEST).
 * ------------------------------------------------------------------------- */
#ifdef DB_TEST
#include "db_test.h"

TEST(error, name_and_default_msg) {
  ASSERT_STR_EQ(db_err_name(DB_OK), "DB_OK");
  ASSERT_STR_EQ(db_err_name(DB_ERR_NOT_FOUND), "DB_ERR_NOT_FOUND");
  ASSERT_STR_EQ(db_err_name(DB_ERR_DUCKDB), "DB_ERR_DUCKDB");
  ASSERT_STR_EQ(db_err_default_msg(DB_ERR_NOT_FOUND), "not found");
  /* every code in the X-macro maps to a non-empty, non-"unknown" name+msg */
  for (int e = 0; e < DB_ERR__COUNT; e++) {
    ASSERT(strcmp(db_err_name((db_err)e), "DB_ERR_UNKNOWN") != 0);
    ASSERT(db_err_default_msg((db_err)e)[0] != '\0');
  }
}

static db_err fail_with_ctx(void) {
  return DB_FAIL(DB_ERR_PARSE, "bad token at %d", 7);
}

TEST(error, fail_sets_last_error) {
  db_clear_error();
  db_err e = fail_with_ctx();
  ASSERT_ERR_EQ(e, DB_ERR_PARSE);
  ASSERT_EQ_INT(db_last_error()->code, DB_ERR_PARSE);
  ASSERT_STR_EQ(db_last_error()->message, "bad token at 7");
  ASSERT(db_last_error()->line > 0);
}

TEST(error, default_message_when_no_fmt) {
  db_clear_error();
  (void)DB_FAIL(DB_ERR_NOT_FOUND);
  ASSERT_STR_EQ(db_last_error()->message, "not found");
}

/* a two-level call chain: DB_TRY should propagate the code and build a trace */
static db_err inner(void) { return DB_FAIL(DB_ERR_IO, "disk gone"); }
static db_err outer(void) { DB_TRY(inner()); return DB_OK; }

TEST(error, try_propagates_and_traces) {
  db_clear_error();
  ASSERT_ERR_EQ(outer(), DB_ERR_IO);
  ASSERT_STR_EQ(db_last_error()->message, "disk gone");
  ASSERT(strstr(db_last_error()->trace, "inner()") != NULL);
}

#endif /* DB_TEST */
