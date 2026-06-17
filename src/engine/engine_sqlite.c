/*
 * dbview — SQLite backend (Phase 0). Implements engine.h over libsqlite3.
 *
 * Errors from SQLite are wrapped into a db_err but the raw sqlite message is preserved
 * in the last-error (the SQL workbench needs to show it verbatim). See SPEC §7.
 */
#include "engine.h"

#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>

struct db_conn {
  db_engine_kind kind;
  sqlite3       *db;
  char          *path;   /* "" for in-memory */
};

/* ---- small helpers ---- */
static char *dup_cstr(const char *s) {
  if (!s) s = "";
  size_t n = strlen(s) + 1;
  char *p = malloc(n);
  if (p) memcpy(p, s, n);
  return p;
}

static db_err fail_sqlite(sqlite3 *db, db_err code, const char *what) {
  /* preserve the verbatim engine message for the workbench */
  return db_set_error(code, __FILE__, __LINE__, __func__, "%s: %s", what,
                      db ? sqlite3_errmsg(db) : "(no handle)");
}

/* ---- lifecycle ---- */
static db_err open_with_flags(const char *path, int flags, db_conn **out) {
  if (!out) return DB_FAIL(DB_ERR_INVALID_ARG, "out is NULL");
  *out = NULL;

  db_conn *c = calloc(1, sizeof *c);
  if (!c) return DB_FAIL(DB_ERR_OOM, "alloc db_conn");
  c->kind = DB_ENGINE_SQLITE;
  c->path = dup_cstr(path && strcmp(path, ":memory:") ? path : "");
  if (!c->path) { free(c); return DB_FAIL(DB_ERR_OOM, "alloc path"); }

  int rc = sqlite3_open_v2(path, &c->db, flags, NULL);
  if (rc != SQLITE_OK) {
    db_err e = fail_sqlite(c->db, DB_ERR_SQLITE, "open");
    sqlite3_close(c->db);
    free(c->path);
    free(c);
    return e;
  }
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

void db_close(db_conn *c) {
  if (!c) return;
  if (c->db) sqlite3_close(c->db);
  free(c->path);
  free(c);
}

db_engine_kind db_conn_kind(const db_conn *c) { return c->kind; }
const char    *db_conn_path(const db_conn *c) { return c->path; }

/* ---- result building ---- */
void db_result_free(db_result *r) {
  if (!r) return;
  if (r->cols) {
    for (int i = 0; i < r->n_cols; i++) { free(r->cols[i].name); free(r->cols[i].type); }
    free(r->cols);
  }
  if (r->values) {
    for (int i = 0; i < r->n_rows * r->n_cols; i++) free(r->values[i]);
    free(r->values);
  }
  free(r);
}

const char *db_result_value(const db_result *r, int row, int col) {
  if (!r || row < 0 || col < 0 || row >= r->n_rows || col >= r->n_cols) return NULL;
  return r->values[(size_t)row * (size_t)r->n_cols + (size_t)col];
}

/* Materialize a prepared statement into a db_result (caps rows at DB_RESULT_MAX_ROWS). */
static db_err materialize(sqlite3 *db, sqlite3_stmt *st, db_result **out) {
  *out = NULL;
  db_result *r = calloc(1, sizeof *r);
  if (!r) return DB_FAIL(DB_ERR_OOM, "alloc db_result");

  r->n_cols = sqlite3_column_count(st);
  if (r->n_cols > 0) {
    r->cols = calloc((size_t)r->n_cols, sizeof *r->cols);
    if (!r->cols) { db_result_free(r); return DB_FAIL(DB_ERR_OOM, "alloc cols"); }
    for (int i = 0; i < r->n_cols; i++) {
      r->cols[i].name = dup_cstr(sqlite3_column_name(st, i));
      r->cols[i].type = dup_cstr(sqlite3_column_decltype(st, i));  /* may be NULL -> "" */
      if (!r->cols[i].name || !r->cols[i].type) { db_result_free(r); return DB_FAIL(DB_ERR_OOM, "alloc col meta"); }
    }
  }

  /* collect rows into a growable array of value pointers */
  size_t cap = 0;
  int rc;
  while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
    if (r->n_rows >= DB_RESULT_MAX_ROWS) { r->truncated = true; break; }
    if ((size_t)(r->n_rows + 1) * (size_t)r->n_cols > cap) {
      size_t newcap = cap ? cap * 2 : (size_t)(r->n_cols > 0 ? r->n_cols : 1) * 64;
      char **nv = realloc(r->values, newcap * sizeof *nv);
      if (!nv) { db_result_free(r); return DB_FAIL(DB_ERR_OOM, "grow values"); }
      r->values = nv;
      cap = newcap;
    }
    for (int i = 0; i < r->n_cols; i++) {
      size_t idx = (size_t)r->n_rows * (size_t)r->n_cols + (size_t)i;
      if (sqlite3_column_type(st, i) == SQLITE_NULL) {
        r->values[idx] = NULL;
      } else {
        const unsigned char *txt = sqlite3_column_text(st, i);
        r->values[idx] = dup_cstr((const char *)txt);
        if (!r->values[idx]) { r->n_rows++; db_result_free(r); return DB_FAIL(DB_ERR_OOM, "dup cell"); }
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

db_err db_query(db_conn *c, const char *sql, db_result **out) {
  if (!c || !c->db) return DB_FAIL(DB_ERR_INVALID_ARG, "connection not open");
  if (!sql) return DB_FAIL(DB_ERR_INVALID_ARG, "sql is NULL");
  if (!out) return DB_FAIL(DB_ERR_INVALID_ARG, "out is NULL");
  *out = NULL;

  sqlite3_stmt *st = NULL;
  if (sqlite3_prepare_v2(c->db, sql, -1, &st, NULL) != SQLITE_OK)
    return fail_sqlite(c->db, DB_ERR_SQL, "prepare");

  db_err e = materialize(c->db, st, out);
  sqlite3_finalize(st);
  return e;
}

db_err db_exec(db_conn *c, const char *sql) {
  if (!c || !c->db) return DB_FAIL(DB_ERR_INVALID_ARG, "connection not open");
  if (!sql) return DB_FAIL(DB_ERR_INVALID_ARG, "sql is NULL");
  char *msg = NULL;
  if (sqlite3_exec(c->db, sql, NULL, NULL, &msg) != SQLITE_OK) {
    db_err e = db_set_error(DB_ERR_SQL, __FILE__, __LINE__, __func__, "exec: %s",
                            msg ? msg : sqlite3_errmsg(c->db));
    sqlite3_free(msg);
    return e;
  }
  return DB_OK;
}

db_err db_list_tables(db_conn *c, db_result **out) {
  return db_query(c,
    "SELECT name, type FROM sqlite_master "
    "WHERE type IN ('table','view') AND name NOT LIKE 'sqlite_%' "
    "ORDER BY name;", out);
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
  ASSERT_STR_EQ(r->cols[1].name, "name");
  ASSERT_CELL_EQ(r, 0, 0, "1");
  ASSERT_CELL_EQ(r, 0, 1, "alice");
  ASSERT_CELL_EQ(r, 2, 0, "3");
  ASSERT(db_result_value(r, 2, 1) == NULL);          /* SQL NULL -> NULL pointer */
  ASSERT(db_result_value(r, 99, 0) == NULL);         /* out of range -> NULL */
  ASSERT(r->truncated == false);
  db_result_free(r);
  db_close(c);
}

TEST(sqlite, scalar_helper) {
  db_conn *c = NULL;
  ASSERT_OK(db_open_sqlite_memory(&c));
  ASSERT_OK(db_exec(c, "CREATE TABLE t(x);"));
  ASSERT_OK(db_exec(c, "INSERT INTO t VALUES (10),(20),(30);"));
  ASSERT_SQL_SCALAR(c, "SELECT SUM(x) FROM t;", "60");
  ASSERT_SQL_SCALAR(c, "SELECT COUNT(*) FROM t;", "3");
  db_close(c);
}

TEST(sqlite, bad_sql_reports_error_with_message) {
  db_conn *c = NULL;
  ASSERT_OK(db_open_sqlite_memory(&c));
  db_result *r = NULL;
  ASSERT_ERR_EQ(db_query(c, "SELCT 1;", &r), DB_ERR_SQL);
  ASSERT(r == NULL);                                  /* nothing leaked on error */
  ASSERT(strstr(db_last_error()->message, "prepare:") != NULL);  /* verbatim engine msg kept */
  db_close(c);
}

TEST(sqlite, list_tables_orders_and_excludes_internal) {
  db_conn *c = NULL;
  ASSERT_OK(db_open_sqlite_memory(&c));
  ASSERT_OK(db_exec(c, "CREATE TABLE zebra(a);"));
  ASSERT_OK(db_exec(c, "CREATE TABLE apple(b);"));
  ASSERT_OK(db_exec(c, "CREATE VIEW v AS SELECT 1;"));

  db_result *r = NULL;
  ASSERT_OK(db_list_tables(c, &r));
  ASSERT_ROWS(r, 3);
  ASSERT_CELL_EQ(r, 0, 0, "apple");   /* alphabetical */
  ASSERT_CELL_EQ(r, 1, 0, "v");
  ASSERT_CELL_EQ(r, 2, 0, "zebra");
  ASSERT_CELL_EQ(r, 1, 1, "view");
  db_result_free(r);
  db_close(c);
}

#endif /* DB_TEST */
