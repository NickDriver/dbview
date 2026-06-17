/*
 * dbview — DuckDB backend. Implements the dbe_duckdb_* entry points plus the public
 * db_open_duckdb* constructors, over the DuckDB C API (vendored, SPEC §3/§9).
 *
 * Values are materialized as text via duckdb_value_varchar, matching db_result's text model.
 * DuckDB error text is preserved verbatim in the last-error for the workbench.
 */
#include "engine_internal.h"

#include "duckdb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static duckdb_database  db_of(db_conn *c)  { return (duckdb_database)c->h1; }
static duckdb_connection con_of(db_conn *c) { return (duckdb_connection)c->h2; }

/* Map the DuckDB column type enum to a short display name (best-effort; "" if unknown). */
static const char *type_name(duckdb_type t) {
  switch (t) {
    case DUCKDB_TYPE_BOOLEAN: return "BOOLEAN";
    case DUCKDB_TYPE_TINYINT: return "TINYINT";
    case DUCKDB_TYPE_SMALLINT: return "SMALLINT";
    case DUCKDB_TYPE_INTEGER: return "INTEGER";
    case DUCKDB_TYPE_BIGINT: return "BIGINT";
    case DUCKDB_TYPE_HUGEINT: return "HUGEINT";
    case DUCKDB_TYPE_UTINYINT: return "UTINYINT";
    case DUCKDB_TYPE_USMALLINT: return "USMALLINT";
    case DUCKDB_TYPE_UINTEGER: return "UINTEGER";
    case DUCKDB_TYPE_UBIGINT: return "UBIGINT";
    case DUCKDB_TYPE_FLOAT: return "FLOAT";
    case DUCKDB_TYPE_DOUBLE: return "DOUBLE";
    case DUCKDB_TYPE_DECIMAL: return "DECIMAL";
    case DUCKDB_TYPE_VARCHAR: return "VARCHAR";
    case DUCKDB_TYPE_BLOB: return "BLOB";
    case DUCKDB_TYPE_DATE: return "DATE";
    case DUCKDB_TYPE_TIME: return "TIME";
    case DUCKDB_TYPE_TIMESTAMP: return "TIMESTAMP";
    case DUCKDB_TYPE_TIMESTAMP_TZ: return "TIMESTAMPTZ";
    case DUCKDB_TYPE_INTERVAL: return "INTERVAL";
    case DUCKDB_TYPE_UUID: return "UUID";
    case DUCKDB_TYPE_LIST: return "LIST";
    case DUCKDB_TYPE_STRUCT: return "STRUCT";
    case DUCKDB_TYPE_MAP: return "MAP";
    case DUCKDB_TYPE_ARRAY: return "ARRAY";
    case DUCKDB_TYPE_ENUM: return "ENUM";
    default: return "";
  }
}

/* Open a DuckDB database with an explicit access mode, capturing the verbatim error. */
static db_err duck_open(const char *path, bool read_only, duckdb_database *out_db) {
  duckdb_config cfg = NULL;
  if (duckdb_create_config(&cfg) != DuckDBSuccess)
    return DB_FAIL(DB_ERR_DUCKDB, "create_config failed");
  duckdb_set_config(cfg, "access_mode", read_only ? "READ_ONLY" : "READ_WRITE");
  char *err = NULL;
  duckdb_state st = duckdb_open_ext(path, out_db, cfg, &err);
  duckdb_destroy_config(&cfg);
  if (st != DuckDBSuccess) {
    db_err e = db_set_error(DB_ERR_DUCKDB, __FILE__, __LINE__, __func__, "open: %s",
                            err ? err : "(no message)");
    duckdb_free(err);
    return e;
  }
  return DB_OK;
}

/* ---- lifecycle ---- */
static db_err open_duckdb(const char *path, db_conn **out) {
  if (!out) return DB_FAIL(DB_ERR_INVALID_ARG, "out is NULL");
  *out = NULL;

  db_conn *c = dbe_conn_alloc(DB_ENGINE_DUCKDB, path ? path : "");
  if (!c) return DB_FAIL(DB_ERR_OOM, "alloc db_conn");

  duckdb_database database = NULL;
  duckdb_connection connection = NULL;
  const char *p = (path && path[0]) ? path : NULL;  /* NULL/"" => in-memory */

  if (!p) {
    /* in-memory is always read-write */
    if (duck_open(NULL, false, &database) != DB_OK) { free(c->path); free(c); return DB_ERR_DUCKDB; }
  } else {
    /* A viewer opens files read-write when it can, falls back to read-only (read-only media),
     * and finally to a temp snapshot copy when the original is exclusively locked by another
     * process (DuckDB blocks even read-only opens then). */
    db_err e = duck_open(p, false, &database);
    if (e != DB_OK) {
      e = duck_open(p, true, &database);     /* read-only retry */
      if (e == DB_OK) c->read_only = true;
    }
    if (e != DB_OK) {
      char *tmp = NULL;
      if (dbe_snapshot_copy(p, &tmp) != DB_OK) { free(c->path); free(c); return e; }  /* keep lock error */
      db_err se = duck_open(tmp, true, &database);
      if (se != DB_OK) { remove(tmp); free(tmp); free(c->path); free(c); return se; }
      c->read_only = true;
      c->snapshot = true;
      c->temp_copy = tmp;
    }
  }

  if (duckdb_connect(database, &connection) != DuckDBSuccess) {
    duckdb_close(&database);
    free(c->path); free(c);
    return DB_FAIL(DB_ERR_DUCKDB, "connect failed");
  }
  c->h1 = database;
  c->h2 = connection;
  *out = c;
  return DB_OK;
}

db_err db_open_duckdb(const char *path, db_conn **out) {
  if (!path || !path[0]) return DB_FAIL(DB_ERR_INVALID_ARG, "path required");
  return open_duckdb(path, out);
}

db_err db_open_duckdb_memory(db_conn **out) {
  return open_duckdb(NULL, out);
}

void dbe_duckdb_close(db_conn *c) {
  if (c->h2) { duckdb_connection con = con_of(c); duckdb_disconnect(&con); }
  if (c->h1) { duckdb_database db = db_of(c); duckdb_close(&db); }
}

/* Materialize a duckdb_result (text values) into a db_result. */
static db_err materialize(duckdb_result *res, db_result **out) {
  *out = NULL;
  idx_t n_cols = duckdb_column_count(res);
  idx_t n_rows = duckdb_row_count(res);

  db_result *r = dbe_result_alloc((int)n_cols);
  if (!r) return DB_ERR_OOM;

  for (idx_t i = 0; i < n_cols; i++) {
    r->cols[i].name = dbe_strdup(duckdb_column_name(res, i));
    r->cols[i].type = dbe_strdup(type_name(duckdb_column_type(res, i)));
    if (!r->cols[i].name || !r->cols[i].type) { db_result_free(r); return DB_FAIL(DB_ERR_OOM, "col meta"); }
  }

  size_t cap = 0;
  for (idx_t row = 0; row < n_rows; row++) {
    if (r->n_rows >= DB_RESULT_MAX_ROWS) { r->truncated = true; break; }
    db_err ge = dbe_result_ensure_row(r, &cap);
    if (ge != DB_OK) { db_result_free(r); return ge; }
    for (idx_t col = 0; col < n_cols; col++) {
      size_t idx = (size_t)r->n_rows * (size_t)n_cols + (size_t)col;
      if (duckdb_value_is_null(res, col, row)) {
        r->values[idx] = NULL;
      } else {
        char *v = duckdb_value_varchar(res, col, row);   /* duckdb-owned: copy then free */
        r->values[idx] = dbe_strdup(v ? v : "");
        duckdb_free(v);
        if (!r->values[idx]) { r->n_rows++; db_result_free(r); return DB_FAIL(DB_ERR_OOM, "cell"); }
      }
    }
    r->n_rows++;
  }
  *out = r;
  return DB_OK;
}

db_err dbe_duckdb_query(db_conn *c, const char *sql, db_result **out) {
  duckdb_connection con = con_of(c);
  if (!con) return DB_FAIL(DB_ERR_INVALID_ARG, "connection not open");
  duckdb_result res;
  if (duckdb_query(con, sql, &res) != DuckDBSuccess) {
    db_err e = db_set_error(DB_ERR_SQL, __FILE__, __LINE__, __func__, "query: %s",
                            duckdb_result_error(&res));
    duckdb_destroy_result(&res);
    return e;
  }
  db_err e = materialize(&res, out);
  duckdb_destroy_result(&res);
  return e;
}

db_err dbe_duckdb_exec(db_conn *c, const char *sql) {
  duckdb_connection con = con_of(c);
  if (!con) return DB_FAIL(DB_ERR_INVALID_ARG, "connection not open");
  duckdb_result res;
  if (duckdb_query(con, sql, &res) != DuckDBSuccess) {
    db_err e = db_set_error(DB_ERR_SQL, __FILE__, __LINE__, __func__, "exec: %s",
                            duckdb_result_error(&res));
    duckdb_destroy_result(&res);
    return e;
  }
  duckdb_destroy_result(&res);
  return DB_OK;
}

/* ---------------------------------------------------------------------------
 * Unit tests (compiled only into the test runner via -DDB_TEST).
 * ------------------------------------------------------------------------- */
#ifdef DB_TEST
#include "engine_test.h"
#include <sys/stat.h>  /* chmod */
#include <unistd.h>    /* unlink */

TEST(duckdb, open_memory_kind) {
  db_conn *c = NULL;
  ASSERT_OK(db_open_duckdb_memory(&c));
  ASSERT_EQ_INT(db_conn_kind(c), DB_ENGINE_DUCKDB);
  ASSERT_STR_EQ(db_conn_path(c), "");
  ASSERT(db_conn_read_only(c) == false);
  db_close(c);
}

/* Regression: a DuckDB file that can't be opened read-write (read-only on disk, or held
 * by another process) must still open by falling back to read-only, with rows readable.
 * This is the bug where an in-use .duckdb file showed "no records". (Cross-process locking
 * is verified manually; here we force the condition with a read-only file on disk.) */
TEST(duckdb, opens_read_only_when_not_writable) {
  char path[256];
  snprintf(path, sizeof path, "%s/dbview_ro_%d.duckdb",
           getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", (int)getpid());
  unlink(path);

  db_conn *w = NULL;
  ASSERT_OK(db_open_duckdb(path, &w));
  ASSERT(db_conn_read_only(w) == false);
  ASSERT_OK(db_exec(w, "CREATE TABLE t(x INTEGER); INSERT INTO t VALUES (1),(2),(3);"));
  db_close(w);

  chmod(path, 0444);   /* read-only on disk -> RW open must fail and fall back */
  db_conn *v = NULL;
  ASSERT_OK(db_open_duckdb(path, &v));
  ASSERT(db_conn_read_only(v) == true);
  ASSERT_SQL_SCALAR(v, "SELECT COUNT(*) FROM t;", "3");
  db_close(v);

  chmod(path, 0644);
  unlink(path);
}

/* The snapshot mechanism the locked-file fallback relies on: a copy is a valid, readable DB. */
TEST(duckdb, snapshot_copy_is_readable) {
  char path[256];
  snprintf(path, sizeof path, "%s/dbview_snapsrc_%d.duckdb",
           getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", (int)getpid());
  unlink(path);
  db_conn *w = NULL;
  ASSERT_OK(db_open_duckdb(path, &w));
  ASSERT_OK(db_exec(w, "CREATE TABLE t(x INTEGER); INSERT INTO t VALUES (1),(2),(3);"));
  db_close(w);

  char *snap = NULL;
  ASSERT_OK(dbe_snapshot_copy(path, &snap));
  ASSERT(snap != NULL && strstr(snap, "dbview_snap_") != NULL);

  db_conn *v = NULL;
  ASSERT_OK(db_open_duckdb(snap, &v));
  ASSERT_SQL_SCALAR(v, "SELECT COUNT(*) FROM t;", "3");
  db_close(v);

  remove(snap);
  free(snap);
  unlink(path);
}

TEST(duckdb, query_rows_cols_types_nulls) {
  db_conn *c = NULL;
  ASSERT_OK(db_open_duckdb_memory(&c));
  ASSERT_OK(db_exec(c, "CREATE TABLE t(id INTEGER, name VARCHAR);"));
  ASSERT_OK(db_exec(c, "INSERT INTO t VALUES (1,'alice'),(2,'bob'),(3,NULL);"));

  db_result *r = NULL;
  ASSERT_OK(db_query(c, "SELECT id, name FROM t ORDER BY id;", &r));
  ASSERT_COLS(r, 2);
  ASSERT_ROWS(r, 3);
  ASSERT_STR_EQ(r->cols[0].name, "id");
  ASSERT_STR_EQ(r->cols[0].type, "INTEGER");
  ASSERT_STR_EQ(r->cols[1].type, "VARCHAR");
  ASSERT_CELL_EQ(r, 0, 0, "1");
  ASSERT_CELL_EQ(r, 0, 1, "alice");
  ASSERT(db_result_value(r, 2, 1) == NULL);   /* SQL NULL -> NULL pointer */
  db_result_free(r);
  db_close(c);
}

TEST(duckdb, scalar_and_analytics) {
  db_conn *c = NULL;
  ASSERT_OK(db_open_duckdb_memory(&c));
  ASSERT_OK(db_exec(c, "CREATE TABLE t(x INTEGER); INSERT INTO t VALUES (10),(20),(30);"));
  ASSERT_SQL_SCALAR(c, "SELECT SUM(x) FROM t;", "60");
  ASSERT_SQL_SCALAR(c, "SELECT COUNT(*) FROM t;", "3");
  /* a DuckDB-ism that SQLite lacks: generate_series */
  ASSERT_SQL_SCALAR(c, "SELECT COUNT(*) FROM generate_series(1, 100);", "100");
  db_close(c);
}

TEST(duckdb, bad_sql_reports_error) {
  db_conn *c = NULL;
  ASSERT_OK(db_open_duckdb_memory(&c));
  db_result *r = NULL;
  ASSERT_ERR_EQ(db_query(c, "SELCT 1;", &r), DB_ERR_SQL);
  ASSERT(r == NULL);
  ASSERT(db_last_error()->message[0] != '\0');
  db_close(c);
}

TEST(duckdb, list_columns) {
  db_conn *c = NULL;
  ASSERT_OK(db_open_duckdb_memory(&c));
  ASSERT_OK(db_exec(c, "CREATE TABLE t(id INTEGER, name VARCHAR); CREATE TABLE u(x INTEGER);"));
  db_result *r = NULL;
  ASSERT_OK(db_list_columns(c, &r));
  ASSERT_ROWS(r, 3);
  ASSERT_CELL_EQ(r, 0, 0, "t"); ASSERT_CELL_EQ(r, 0, 1, "id");
  ASSERT_CELL_EQ(r, 1, 0, "t"); ASSERT_CELL_EQ(r, 1, 1, "name");
  ASSERT_CELL_EQ(r, 2, 0, "u"); ASSERT_CELL_EQ(r, 2, 1, "x");
  db_result_free(r);
  db_close(c);
}

TEST(duckdb, list_tables) {
  db_conn *c = NULL;
  ASSERT_OK(db_open_duckdb_memory(&c));
  ASSERT_OK(db_exec(c, "CREATE TABLE apple(a INTEGER); CREATE TABLE zebra(b INTEGER);"));
  ASSERT_OK(db_exec(c, "CREATE VIEW v AS SELECT 1;"));
  db_result *r = NULL;
  ASSERT_OK(db_list_tables(c, &r));
  ASSERT_ROWS(r, 3);
  ASSERT_CELL_EQ(r, 0, 0, "apple");
  ASSERT_CELL_EQ(r, 1, 0, "v");
  ASSERT_CELL_EQ(r, 1, 1, "view");
  ASSERT_CELL_EQ(r, 2, 0, "zebra");
  db_result_free(r);
  db_close(c);
}

#endif /* DB_TEST */
