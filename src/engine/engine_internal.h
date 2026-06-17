#ifndef DB_ENGINE_INTERNAL_H
#define DB_ENGINE_INTERNAL_H
/*
 * dbview — engine internals shared between the generic dispatch (engine.c) and the
 * per-backend implementations (engine_sqlite.c, engine_duckdb.c).
 *
 * Backend handles are stored as void* so this header pulls in neither sqlite3.h nor
 * duckdb.h; each backend casts h1/h2 to its own types.
 */
#include "engine.h"

#include <stddef.h>

struct db_conn {
  db_engine_kind kind;
  char          *path;   /* "" for in-memory */
  void          *h1;     /* SQLITE: sqlite3*       DUCKDB: duckdb_database  */
  void          *h2;     /* DUCKDB: duckdb_connection (unused for SQLITE)   */
};

/* ---- shared helpers (engine.c) ---- */
char      *dbe_strdup(const char *s);                 /* dup; "" for NULL; NULL on OOM */
db_conn   *dbe_conn_alloc(db_engine_kind kind, const char *path);  /* NULL on OOM */
db_result *dbe_result_alloc(int n_cols);              /* r + zeroed cols[]; NULL on OOM */
db_err     dbe_result_ensure_row(db_result *r, size_t *cap);  /* room for one more row */

/* ---- backend entry points (dispatched by engine.c on db_conn.kind) ---- */
db_err dbe_sqlite_query(db_conn *c, const char *sql, db_result **out) DB_MUST_CHECK;
db_err dbe_sqlite_exec(db_conn *c, const char *sql) DB_MUST_CHECK;
void   dbe_sqlite_close(db_conn *c);

db_err dbe_duckdb_query(db_conn *c, const char *sql, db_result **out) DB_MUST_CHECK;
db_err dbe_duckdb_exec(db_conn *c, const char *sql) DB_MUST_CHECK;
void   dbe_duckdb_close(db_conn *c);

#endif /* DB_ENGINE_INTERNAL_H */
