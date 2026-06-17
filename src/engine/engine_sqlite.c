/*
 * dbview — SQLite backend. Implements the dbe_sqlite_* entry points (dispatched by
 * engine.c) plus the public db_open_sqlite* constructors.
 *
 * SQLite errors are wrapped into a db_err but the raw sqlite message is preserved in the
 * last-error (the SQL workbench shows it verbatim). See SPEC §7.
 */
#include "engine_internal.h"

#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>

static sqlite3 *handle(db_conn *c) { return (sqlite3 *)c->h1; }

static db_err fail_sqlite(sqlite3 *db, db_err code, const char *what) {
  return db_set_error(code, __FILE__, __LINE__, __func__, "%s: %s", what,
                      db ? sqlite3_errmsg(db) : "(no handle)");
}

/* ---- lifecycle ---- */
static db_err open_with_flags(const char *path, int flags, db_conn **out) {
  if (!out) return DB_FAIL(DB_ERR_INVALID_ARG, "out is NULL");
  *out = NULL;

  db_conn *c = dbe_conn_alloc(DB_ENGINE_SQLITE,
                              (path && strcmp(path, ":memory:")) ? path : "");
  if (!c) return DB_FAIL(DB_ERR_OOM, "alloc db_conn");

  sqlite3 *db = NULL;
  int rc = sqlite3_open_v2(path, &db, flags, NULL);
  if (rc != SQLITE_OK) {
    db_err e = fail_sqlite(db, DB_ERR_SQLITE, "open");
    sqlite3_close(db);
    free(c->path);
    free(c);
    return e;
  }
  c->h1 = db;
  *out = c;
  return DB_OK;
}

db_err db_open_sqlite(const char *path, db_conn **out) {
  if (!path || !path[0]) return DB_FAIL(DB_ERR_INVALID_ARG, "path required");
  return open_with_flags(path, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, out);
}

db_err db_open_sqlite_memory(db_conn **out) {
  return open_with_flags(":memory:", SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, out);
}

void dbe_sqlite_close(db_conn *c) {
  if (c->h1) sqlite3_close((sqlite3 *)c->h1);
}

/* Materialize a prepared statement into a db_result (caps rows at DB_RESULT_MAX_ROWS). */
static db_err materialize(sqlite3 *db, sqlite3_stmt *st, db_result **out) {
  *out = NULL;
  int n_cols = sqlite3_column_count(st);
  db_result *r = dbe_result_alloc(n_cols);
  if (!r) return DB_ERR_OOM;

  for (int i = 0; i < n_cols; i++) {
    r->cols[i].name = dbe_strdup(sqlite3_column_name(st, i));
    r->cols[i].type = dbe_strdup(sqlite3_column_decltype(st, i));  /* NULL -> "" */
    if (!r->cols[i].name || !r->cols[i].type) { db_result_free(r); return DB_FAIL(DB_ERR_OOM, "col meta"); }
  }

  size_t cap = 0;
  int rc;
  while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
    if (r->n_rows >= DB_RESULT_MAX_ROWS) { r->truncated = true; break; }
    db_err ge = dbe_result_ensure_row(r, &cap);
    if (ge != DB_OK) { db_result_free(r); return ge; }
    for (int i = 0; i < n_cols; i++) {
      size_t idx = (size_t)r->n_rows * (size_t)n_cols + (size_t)i;
      if (sqlite3_column_type(st, i) == SQLITE_NULL) {
        r->values[idx] = NULL;
      } else {
        r->values[idx] = dbe_strdup((const char *)sqlite3_column_text(st, i));
        if (!r->values[idx]) { r->n_rows++; db_result_free(r); return DB_FAIL(DB_ERR_OOM, "cell"); }
      }
    }
    r->n_rows++;
  }
  if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
    db_err e = fail_sqlite(db, DB_ERR_SQL, "step");
    db_result_free(r);
    return e;
  }
  *out = r;
  return DB_OK;
}

db_err dbe_sqlite_query(db_conn *c, const char *sql, db_result **out) {
  sqlite3 *db = handle(c);
  if (!db) return DB_FAIL(DB_ERR_INVALID_ARG, "connection not open");
  sqlite3_stmt *st = NULL;
  if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
    return fail_sqlite(db, DB_ERR_SQL, "prepare");
  db_err e = materialize(db, st, out);
  sqlite3_finalize(st);
  return e;
}

db_err dbe_sqlite_exec(db_conn *c, const char *sql) {
  sqlite3 *db = handle(c);
  if (!db) return DB_FAIL(DB_ERR_INVALID_ARG, "connection not open");
  char *msg = NULL;
  if (sqlite3_exec(db, sql, NULL, NULL, &msg) != SQLITE_OK) {
    db_err e = db_set_error(DB_ERR_SQL, __FILE__, __LINE__, __func__, "exec: %s",
                            msg ? msg : sqlite3_errmsg(db));
    sqlite3_free(msg);
    return e;
  }
  return DB_OK;
}

/* ---------------------------------------------------------------------------
 * Unit tests (compiled only into the test runner via -DDB_TEST).
 * ------------------------------------------------------------------------- */
#ifdef DB_TEST
#include "engine_test.h"

TEST(sqlite, open_memory_kind_and_path) {
  db_conn *c = NULL;
  ASSERT_OK(db_open_sqlite_memory(&c));
  ASSERT_EQ_INT(db_conn_kind(c), DB_ENGINE_SQLITE);
  ASSERT_STR_EQ(db_conn_path(c), "");
  db_close(c);
}

TEST(sqlite, open_requires_path) {
  db_conn *c = NULL;
  ASSERT_ERR_EQ(db_open_sqlite("", &c), DB_ERR_INVALID_ARG);
  ASSERT_ERR_EQ(db_open_sqlite(NULL, &c), DB_ERR_INVALID_ARG);
}

TEST(sqlite, query_returns_rows_cols_values) {
  db_conn *c = NULL;
  ASSERT_OK(db_open_sqlite_memory(&c));
  ASSERT_OK(db_exec(c, "CREATE TABLE t(id INTEGER, name TEXT);"));
  ASSERT_OK(db_exec(c, "INSERT INTO t VALUES (1,'alice'),(2,'bob'),(3,NULL);"));

  db_result *r = NULL;
  ASSERT_OK(db_query(c, "SELECT id, name FROM t ORDER BY id;", &r));
  ASSERT_COLS(r, 2);
  ASSERT_ROWS(r, 3);
  ASSERT_STR_EQ(r->cols[0].name, "id");
  ASSERT_CELL_EQ(r, 0, 1, "alice");
  ASSERT_CELL_EQ(r, 2, 0, "3");
  ASSERT(db_result_value(r, 2, 1) == NULL);
  ASSERT(db_result_value(r, 99, 0) == NULL);
  ASSERT(r->truncated == false);
  db_result_free(r);
  db_close(c);
}

TEST(sqlite, scalar_helper) {
  db_conn *c = NULL;
  ASSERT_OK(db_open_sqlite_memory(&c));
  ASSERT_OK(db_exec(c, "CREATE TABLE t(x); INSERT INTO t VALUES (10),(20),(30);"));
  ASSERT_SQL_SCALAR(c, "SELECT SUM(x) FROM t;", "60");
  db_close(c);
}

TEST(sqlite, bad_sql_reports_error_with_message) {
  db_conn *c = NULL;
  ASSERT_OK(db_open_sqlite_memory(&c));
  db_result *r = NULL;
  ASSERT_ERR_EQ(db_query(c, "SELCT 1;", &r), DB_ERR_SQL);
  ASSERT(r == NULL);
  ASSERT(strstr(db_last_error()->message, "prepare:") != NULL);
  db_close(c);
}

TEST(sqlite, list_tables_orders_and_excludes_internal) {
  db_conn *c = NULL;
  ASSERT_OK(db_open_sqlite_memory(&c));
  ASSERT_OK(db_exec(c, "CREATE TABLE zebra(a); CREATE TABLE apple(b); CREATE VIEW v AS SELECT 1;"));
  db_result *r = NULL;
  ASSERT_OK(db_list_tables(c, &r));
  ASSERT_ROWS(r, 3);
  ASSERT_CELL_EQ(r, 0, 0, "apple");
  ASSERT_CELL_EQ(r, 1, 0, "v");
  ASSERT_CELL_EQ(r, 2, 0, "zebra");
  ASSERT_CELL_EQ(r, 1, 1, "view");
  db_result_free(r);
  db_close(c);
}

#endif /* DB_TEST */
