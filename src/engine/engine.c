/*
 * dbview — generic engine layer. Owns the db_conn/db_result lifecycle and dispatches
 * statements to the active backend (SQLite or DuckDB). Backend-specific materialization
 * lives in engine_sqlite.c / engine_duckdb.c. See SPEC §4.2.
 */
#include "engine_internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---- shared helpers ---- */
char *dbe_strdup(const char *s) {
  if (!s) s = "";
  size_t n = strlen(s) + 1;
  char *p = malloc(n);
  if (p) memcpy(p, s, n);
  return p;
}

db_conn *dbe_conn_alloc(db_engine_kind kind, const char *path) {
  db_conn *c = calloc(1, sizeof *c);
  if (!c) return NULL;
  c->kind = kind;
  c->path = dbe_strdup(path ? path : "");
  if (!c->path) { free(c); return NULL; }
  return c;
}

db_result *dbe_result_alloc(int n_cols) {
  db_result *r = calloc(1, sizeof *r);
  if (!r) { (void)DB_FAIL(DB_ERR_OOM, "alloc db_result"); return NULL; }
  r->n_cols = n_cols;
  if (n_cols > 0) {
    r->cols = calloc((size_t)n_cols, sizeof *r->cols);
    if (!r->cols) { free(r); (void)DB_FAIL(DB_ERR_OOM, "alloc cols"); return NULL; }
  }
  return r;
}

db_err dbe_snapshot_copy(const char *src, char **out_tmp) {
  *out_tmp = NULL;
  const char *tmpdir = getenv("TMPDIR");
  if (!tmpdir || !tmpdir[0]) tmpdir = "/tmp";
  const char *ext = strrchr(src, '.');
  if (!ext) ext = "";

  static int counter = 0;   /* + pid keeps the name unique without Date/random */
  char path[1280];
  snprintf(path, sizeof path, "%s/dbview_snap_%d_%d%s", tmpdir, (int)getpid(), ++counter, ext);

  FILE *in = fopen(src, "rb");
  if (!in) return DB_FAIL(DB_ERR_IO, "snapshot: open source: %s", strerror(errno));
  FILE *out = fopen(path, "wb");
  if (!out) { fclose(in); return DB_FAIL(DB_ERR_IO, "snapshot: create temp: %s", strerror(errno)); }

  char buf[65536];
  size_t k;
  db_err e = DB_OK;
  while ((k = fread(buf, 1, sizeof buf, in)) > 0) {
    if (fwrite(buf, 1, k, out) != k) { e = DB_FAIL(DB_ERR_IO, "snapshot: write failed"); break; }
  }
  if (!e && ferror(in)) e = DB_FAIL(DB_ERR_IO, "snapshot: read failed");
  fclose(in);
  fclose(out);
  if (e != DB_OK) { remove(path); return e; }

  *out_tmp = dbe_strdup(path);
  if (!*out_tmp) { remove(path); return DB_FAIL(DB_ERR_OOM, "snapshot path"); }
  return DB_OK;
}

/* Ensure r->values has room for one more row; grows the backing array geometrically. */
db_err dbe_result_ensure_row(db_result *r, size_t *cap) {
  if ((size_t)(r->n_rows + 1) * (size_t)r->n_cols <= *cap) return DB_OK;
  size_t newcap = *cap ? *cap * 2 : (size_t)(r->n_cols > 0 ? r->n_cols : 1) * 64;
  char **nv = realloc(r->values, newcap * sizeof *nv);
  if (!nv) return DB_FAIL(DB_ERR_OOM, "grow values");
  r->values = nv;
  *cap = newcap;
  return DB_OK;
}

/* ---- result access / cleanup ---- */
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

/* ---- connection accessors ---- */
db_engine_kind db_conn_kind(const db_conn *c) { return c->kind; }
const char    *db_conn_path(const db_conn *c) { return c->path; }
bool           db_conn_read_only(const db_conn *c) { return c->read_only; }
bool           db_conn_is_snapshot(const db_conn *c) { return c->snapshot; }

void db_close(db_conn *c) {
  if (!c) return;
  switch (c->kind) {
    case DB_ENGINE_SQLITE: dbe_sqlite_close(c); break;
    case DB_ENGINE_DUCKDB: dbe_duckdb_close(c); break;
  }
  if (c->temp_copy) { remove(c->temp_copy); free(c->temp_copy); }
  free(c->path);
  free(c);
}

db_err db_query(db_conn *c, const char *sql, db_result **out) {
  if (!c) return DB_FAIL(DB_ERR_INVALID_ARG, "connection not open");
  if (!sql) return DB_FAIL(DB_ERR_INVALID_ARG, "sql is NULL");
  if (!out) return DB_FAIL(DB_ERR_INVALID_ARG, "out is NULL");
  *out = NULL;
  switch (c->kind) {
    case DB_ENGINE_SQLITE: return dbe_sqlite_query(c, sql, out);
    case DB_ENGINE_DUCKDB: return dbe_duckdb_query(c, sql, out);
  }
  return DB_FAIL(DB_ERR_INTERNAL, "unknown engine kind");
}

db_err db_exec(db_conn *c, const char *sql) {
  if (!c) return DB_FAIL(DB_ERR_INVALID_ARG, "connection not open");
  if (!sql) return DB_FAIL(DB_ERR_INVALID_ARG, "sql is NULL");
  switch (c->kind) {
    case DB_ENGINE_SQLITE: return dbe_sqlite_exec(c, sql);
    case DB_ENGINE_DUCKDB: return dbe_duckdb_exec(c, sql);
  }
  return DB_FAIL(DB_ERR_INTERNAL, "unknown engine kind");
}

db_err db_list_tables(db_conn *c, db_result **out) {
  if (!c) return DB_FAIL(DB_ERR_INVALID_ARG, "connection not open");
  switch (c->kind) {
    case DB_ENGINE_SQLITE:
      return db_query(c,
        "SELECT name, type FROM sqlite_master "
        "WHERE type IN ('table','view') AND name NOT LIKE 'sqlite_%' "
        "ORDER BY name;", out);
    case DB_ENGINE_DUCKDB:
      return db_query(c,
        "SELECT table_name AS name, "
        "       CASE WHEN table_type = 'VIEW' THEN 'view' ELSE 'table' END AS type "
        "FROM information_schema.tables "
        "WHERE table_schema = 'main' "
        "ORDER BY table_name;", out);
  }
  return DB_FAIL(DB_ERR_INTERNAL, "unknown engine kind");
}

db_err db_list_columns(db_conn *c, db_result **out) {
  if (!c) return DB_FAIL(DB_ERR_INVALID_ARG, "connection not open");
  switch (c->kind) {
    case DB_ENGINE_SQLITE:
      /* table-valued pragma_table_info joined per table (SQLite >= 3.16) */
      return db_query(c,
        "SELECT m.name AS table_name, p.name AS column_name "
        "FROM sqlite_master m JOIN pragma_table_info(m.name) p "
        "WHERE m.type IN ('table','view') AND m.name NOT LIKE 'sqlite_%' "
        "ORDER BY m.name, p.cid;", out);
    case DB_ENGINE_DUCKDB:
      return db_query(c,
        "SELECT table_name, column_name "
        "FROM information_schema.columns "
        "WHERE table_schema = 'main' "
        "ORDER BY table_name, ordinal_position;", out);
  }
  return DB_FAIL(DB_ERR_INTERNAL, "unknown engine kind");
}
