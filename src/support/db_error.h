#ifndef DB_ERROR_H
#define DB_ERROR_H
/*
 * dbview — error handling & diagnostics (Phase 0).
 *
 * AI-first design goal: every failure is greppable, located, and explained, so an
 * LLM (or human) reading the output can diagnose without a debugger.
 *
 *   - Expected/runtime failures  -> return a `db_err` code (+ rich last-error).
 *   - Programmer errors          -> DB_DEBUG_ASSERT (compiled out in release).
 *   - Data-integrity violations  -> DB_INVARIANT (ALWAYS on; crash > corrupt data).
 *   - Ignored error codes        -> compile error (DB_MUST_CHECK).
 *
 * Adopted wholesale from the money_books convention; see docs/SPEC.md §7.
 */
#include <stdbool.h>
#include <stddef.h>

/* ---- compiler attributes ---- */
#if defined(__GNUC__) || defined(__clang__)
#  define DB_MUST_CHECK   __attribute__((warn_unused_result))
#  define DB_UNUSED       __attribute__((unused))
#  define DB_PRINTF(f, a) __attribute__((format(printf, f, a)))
#  define DB_NORETURN     __attribute__((noreturn))
#  define DB_LIKELY(x)    __builtin_expect(!!(x), 1)
#  define DB_UNLIKELY(x)  __builtin_expect(!!(x), 0)
#else
#  define DB_MUST_CHECK
#  define DB_UNUSED
#  define DB_PRINTF(f, a)
#  define DB_NORETURN
#  define DB_LIKELY(x)   (x)
#  define DB_UNLIKELY(x) (x)
#endif

/* ---- error codes: single source of truth via X-macro ----
 * Add a code here and db_err_name()/db_err_default_msg() update automatically. */
#define DB_ERR_LIST(X) \
  X(DB_OK,               "ok") \
  X(DB_ERR_INVALID_ARG,  "invalid argument") \
  X(DB_ERR_NOT_FOUND,    "not found") \
  X(DB_ERR_EXISTS,       "already exists") \
  X(DB_ERR_IO,           "I/O error") \
  X(DB_ERR_SQLITE,       "sqlite error") \
  X(DB_ERR_DUCKDB,       "duckdb error") \
  X(DB_ERR_SQL,          "sql error") \
  X(DB_ERR_PARSE,        "parse error") \
  X(DB_ERR_TYPE,         "type error") \
  X(DB_ERR_CONFLICT,     "conflict") \
  X(DB_ERR_UNSUPPORTED,  "unsupported operation") \
  X(DB_ERR_OOM,          "out of memory") \
  X(DB_ERR_INTERNAL,     "internal error")

typedef enum {
#define DB_ENUM(name, msg) name,
  DB_ERR_LIST(DB_ENUM)
#undef DB_ENUM
  DB_ERR__COUNT
} db_err;

const char *db_err_name(db_err e);        /* e.g. "DB_ERR_NOT_FOUND" */
const char *db_err_default_msg(db_err e); /* e.g. "not found"        */

/* ---- last-error context (thread-local) ---- */
typedef struct {
  db_err      code;
  char        message[256];
  const char *file;
  int         line;
  const char *func;
  char        trace[512];   /* breadcrumb chain accumulated by DB_TRY */
} db_error_ctx;

const db_error_ctx *db_last_error(void);
void                db_clear_error(void);

/* set + return the error; prefer the DB_FAIL macro below */
db_err db_set_error(db_err code, const char *file, int line,
                    const char *func, const char *fmt, ...) DB_PRINTF(5, 6);
void   db_error_add_trace(const char *file, int line, const char *func,
                          const char *expr);

/* DB_FAIL(code)  or  DB_FAIL(code, "ctx %d", x) */
#define DB_FAIL(code, ...) \
  db_set_error((code), __FILE__, __LINE__, __func__, "" __VA_ARGS__)

/* propagate like Rust's `?`, appending a breadcrumb to the trace */
#define DB_TRY(expr) \
  do { db_err _db_e = (expr); \
       if (DB_UNLIKELY(_db_e != DB_OK)) { \
         db_error_add_trace(__FILE__, __LINE__, __func__, #expr); \
         return _db_e; } } while (0)

/* ---- invariants vs debug asserts ---- */
DB_NORETURN void db_invariant_fail(const char *expr, const char *file, int line,
                                   const char *func, const char *fmt, ...)
    DB_PRINTF(5, 6);

/* ALWAYS on (even with NDEBUG): integrity guards. Aborts with a full diagnostic. */
#define DB_INVARIANT(cond, ...) \
  do { if (DB_UNLIKELY(!(cond))) \
         db_invariant_fail(#cond, __FILE__, __LINE__, __func__, "" __VA_ARGS__); \
  } while (0)

/* Compiled out under NDEBUG: ordinary bug-catching. */
#ifdef NDEBUG
#  define DB_DEBUG_ASSERT(cond, ...) ((void)0)
#else
#  define DB_DEBUG_ASSERT(cond, ...) DB_INVARIANT((cond), "" __VA_ARGS__)
#endif

/* ---- minimal leveled logging (stderr, located) ---- */
typedef enum { DB_LOG_DEBUG, DB_LOG_INFO, DB_LOG_WARN, DB_LOG_ERROR } db_log_level;
void db_log(db_log_level lvl, const char *file, int line, const char *fmt, ...)
    DB_PRINTF(4, 5);
#define DB_LOGD(...) db_log(DB_LOG_DEBUG, __FILE__, __LINE__, __VA_ARGS__)
#define DB_LOGI(...) db_log(DB_LOG_INFO,  __FILE__, __LINE__, __VA_ARGS__)
#define DB_LOGW(...) db_log(DB_LOG_WARN,  __FILE__, __LINE__, __VA_ARGS__)
#define DB_LOGE(...) db_log(DB_LOG_ERROR, __FILE__, __LINE__, __VA_ARGS__)

#endif /* DB_ERROR_H */
