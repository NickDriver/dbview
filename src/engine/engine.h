#ifndef DB_ENGINE_H
#define DB_ENGINE_H
/*
 * dbview — database engine layer (Phase 0).
 *
 * A thin backend abstraction over the actual databases. v1 ships a SQLite backend
 * (this file's API is implemented in engine_sqlite.c); DuckDB slots in behind the
 * same `db_conn` / `db_result` types in Phase 1 (SPEC §4.2).
 *
 * Conventions:
 *   - All fallible functions return a `db_err` (+ rich last-error) and are DB_MUST_CHECK.
 *   - Result values are materialized as text (NULL pointer = SQL NULL). This is the unit
 *     the API/UI layer serializes to JSON; it keeps the viewer engine-agnostic.
 *   - Conversions/queries are read into memory with a row cap (truncation is flagged,
 *     never silent — see DB_RESULT_MAX_ROWS).
 */
#include "../support/db_error.h"

#include <stdbool.h>

/* Default cap on rows materialized into a db_result. Truncation sets db_result.truncated. */
#ifndef DB_RESULT_MAX_ROWS
#define DB_RESULT_MAX_ROWS 100000
#endif

typedef enum {
  DB_ENGINE_SQLITE,
  DB_ENGINE_DUCKDB,   /* reserved for Phase 1 */
} db_engine_kind;

typedef struct db_conn db_conn;   /* opaque connection handle */

typedef struct {
  char *name;   /* column name (malloc'd) */
  char *type;   /* declared/engine type name, may be "" if unknown (malloc'd) */
} db_column;

typedef struct {
  int        n_cols;
  int        n_rows;
  db_column *cols;       /* [n_cols] */
  char     **values;     /* [n_rows * n_cols], row-major; NULL entry = SQL NULL */
  bool       truncated;  /* true if more rows existed than DB_RESULT_MAX_ROWS */
} db_result;

/* ---- connection lifecycle ---- */
db_err db_open_sqlite(const char *path, db_conn **out) DB_MUST_CHECK;
db_err db_open_sqlite_memory(db_conn **out) DB_MUST_CHECK;  /* ":memory:", for tests */
db_err db_open_duckdb(const char *path, db_conn **out) DB_MUST_CHECK;
db_err db_open_duckdb_memory(db_conn **out) DB_MUST_CHECK;  /* in-memory, for tests */
void   db_close(db_conn *c);

db_engine_kind db_conn_kind(const db_conn *c);
const char    *db_conn_path(const db_conn *c);  /* "" for in-memory */
bool           db_conn_read_only(const db_conn *c);
bool           db_conn_is_snapshot(const db_conn *c);  /* opened from a temp copy of a locked file */

/* ---- statements ---- */
/* Run a statement that returns rows. *out is set to a malloc'd db_result on success
 * (free with db_result_free) and left NULL on failure. */
db_err db_query(db_conn *c, const char *sql, db_result **out) DB_MUST_CHECK;

/* Run a statement with no result rows (DDL/DML). */
db_err db_exec(db_conn *c, const char *sql) DB_MUST_CHECK;

/* List user tables (name, type) ordered by name. Convenience over sqlite_master. */
db_err db_list_tables(db_conn *c, db_result **out) DB_MUST_CHECK;

/* ---- result access / cleanup ---- */
void        db_result_free(db_result *r);
/* Cell as text; NULL if the cell is SQL NULL or indices are out of range. */
const char *db_result_value(const db_result *r, int row, int col);

#endif /* DB_ENGINE_H */
