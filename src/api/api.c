/*
 * dbview — JSON command API. Turns method + JSON args into engine calls and JSON results.
 * The same surface backs the WebView bridge and (later) an MCP server. See SPEC §6.
 */
#include "api.h"

#include "../convert/convert.h"
#include "../vendor/cjson/cJSON.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* Create the parent directory chain for a file path (like `mkdir -p $(dirname path)`), so
 * exports to a not-yet-existing folder succeed (DuckDB won't create directories itself). */
static db_err ensure_parent_dir(const char *file_path) {
  char buf[2048];
  if (snprintf(buf, sizeof buf, "%s", file_path) >= (int)sizeof buf)
    return DB_FAIL(DB_ERR_INVALID_ARG, "path too long");
  char *slash = strrchr(buf, '/');
  if (!slash || slash == buf) return DB_OK;  /* no parent dir component */
  *slash = '\0';
  for (char *p = buf + 1; *p; p++) {
    if (*p != '/') continue;
    *p = '\0';
    if (mkdir(buf, 0755) != 0 && errno != EEXIST)
      return DB_FAIL(DB_ERR_IO, "mkdir %s: %s", buf, strerror(errno));
    *p = '/';
  }
  if (mkdir(buf, 0755) != 0 && errno != EEXIST)
    return DB_FAIL(DB_ERR_IO, "mkdir %s: %s", buf, strerror(errno));
  return DB_OK;
}

/* ---- small JSON helpers ---- */
static char *json_take(cJSON *o) {
  char *s = cJSON_PrintUnformatted(o);
  cJSON_Delete(o);
  return s ? s : strdup("{\"error\":{\"code\":\"DB_ERR_OOM\",\"message\":\"print failed\"}}");
}

static char *json_error(db_err code, const char *message) {
  cJSON *env = cJSON_CreateObject();
  cJSON *err = cJSON_AddObjectToObject(env, "error");
  cJSON_AddStringToObject(err, "code", db_err_name(code));
  cJSON_AddStringToObject(err, "message",
                          message && message[0] ? message : db_err_default_msg(code));
  return json_take(env);
}

static const char *sget(const cJSON *a, const char *key) {
  const cJSON *v = cJSON_GetObjectItemCaseSensitive(a, key);
  return cJSON_IsString(v) ? v->valuestring : NULL;
}

/* Serialize a db_result to {columns:[{name,type}], rows:[[...]], row_count, truncated}. */
static char *result_to_json(const db_result *r) {
  cJSON *o = cJSON_CreateObject();

  cJSON *cols = cJSON_AddArrayToObject(o, "columns");
  for (int i = 0; i < r->n_cols; i++) {
    cJSON *col = cJSON_CreateObject();
    cJSON_AddStringToObject(col, "name", r->cols[i].name);
    cJSON_AddStringToObject(col, "type", r->cols[i].type);
    cJSON_AddItemToArray(cols, col);
  }

  cJSON *rows = cJSON_AddArrayToObject(o, "rows");
  for (int row = 0; row < r->n_rows; row++) {
    cJSON *jr = cJSON_CreateArray();
    for (int col = 0; col < r->n_cols; col++) {
      const char *v = db_result_value(r, row, col);
      cJSON_AddItemToArray(jr, v ? cJSON_CreateString(v) : cJSON_CreateNull());
    }
    cJSON_AddItemToArray(rows, jr);
  }

  cJSON_AddNumberToObject(o, "row_count", r->n_rows);
  cJSON_AddBoolToObject(o, "truncated", r->truncated);
  return json_take(o);
}

/* Recreate the source's views in dst (whole-DB conversion copies only base tables). `dk` must
 * have src+dst attached and the default catalog set to dst (USE dst) so unqualified table
 * references resolve there. Best-effort: a view whose DDL the target rejects (dialect quirk)
 * is appended to `failed` rather than aborting. Views are taken in creation order so a view
 * built on another view recreates after its dependency.
 *
 * For a SQLite source we read the ORIGINAL sqlite_master DDL — NOT duckdb_views() — because the
 * SQLite scanner can hand back a passthrough definition referencing the attachment
 * (sqlite_query('src', …)) that dangles once src is detached. The raw SQLite DDL uses bare
 * table names. For a DuckDB source, duckdb_views() gives a clean native definition. */
static void recreate_views(const char *src_path, int src_sqlite, db_conn *dk, cJSON *failed) {
  if (db_exec(dk, "USE dst;") != DB_OK) return;
  if (src_sqlite) {
    db_conn *sc = NULL;
    if (db_open_sqlite(src_path, &sc) != DB_OK) return;
    db_result *r = NULL;
    if (db_query(sc,
          "SELECT name, sql FROM sqlite_master "
          "WHERE type='view' AND name NOT LIKE 'sqlite_%' ORDER BY rowid;", &r) == DB_OK) {
      for (int i = 0; i < r->n_rows; i++) {
        const char *name = db_result_value(r, i, 0);
        const char *sql = db_result_value(r, i, 1);
        if (sql && sql[0] && db_exec(dk, sql) == DB_OK) continue;
        if (name) cJSON_AddItemToArray(failed, cJSON_CreateString(name));
      }
    }
    db_result_free(r);
    db_close(sc);
  } else {
    db_result *r = NULL;
    if (db_query(dk,
          "SELECT view_name, sql FROM duckdb_views() "
          "WHERE database_name='src' AND internal=false;", &r) == DB_OK) {
      for (int i = 0; i < r->n_rows; i++) {
        const char *name = db_result_value(r, i, 0);
        const char *sql = db_result_value(r, i, 1);
        if (sql && sql[0] && db_exec(dk, sql) == DB_OK) continue;
        if (name) cJSON_AddItemToArray(failed, cJSON_CreateString(name));
      }
    }
    db_result_free(r);
  }
}

/* Run a query and produce either a result JSON or an error envelope. */
static db_err run_query(db_conn *c, const char *sql, char **out) {
  db_result *r = NULL;
  db_err e = db_query(c, sql, &r);
  if (e != DB_OK) { *out = json_error(e, db_last_error()->message); return e; }
  *out = result_to_json(r);
  db_result_free(r);
  return DB_OK;
}

db_err db_api_dispatch(db_conn *c, const char *method, const char *args_json,
                       char **result_json) {
  if (!result_json) return DB_FAIL(DB_ERR_INVALID_ARG, "result_json is NULL");
  *result_json = NULL;

  if (!method) { *result_json = json_error(DB_ERR_INVALID_ARG, "method is NULL"); return DB_ERR_INVALID_ARG; }
  if (!c) { *result_json = json_error(DB_ERR_INVALID_ARG, "no connection open"); return DB_ERR_INVALID_ARG; }

  cJSON *args = (args_json && args_json[0]) ? cJSON_Parse(args_json) : cJSON_CreateObject();
  db_err e;

  if (!strcmp(method, "schema.tables")) {
    db_result *r = NULL;
    e = db_list_tables(c, &r);
    if (e != DB_OK) *result_json = json_error(e, db_last_error()->message);
    else { *result_json = result_to_json(r); db_result_free(r); }

  } else if (!strcmp(method, "schema.columns")) {
    db_result *r = NULL;
    e = db_list_columns(c, &r);
    if (e != DB_OK) *result_json = json_error(e, db_last_error()->message);
    else { *result_json = result_to_json(r); db_result_free(r); }

  } else if (!strcmp(method, "schema.table_detail")) {
    /* One table's structure: columns/types/nullable/pk, indexes, and row count.
     * Returns {row_count, columns:<ResultSet>, indexes:<ResultSet>}. */
    const char *table = sget(args, "table");
    if (!table || !table[0]) {
      *result_json = json_error(DB_ERR_INVALID_ARG, "table is required");
      e = DB_ERR_INVALID_ARG;
    } else {
      db_result *cols = NULL, *idx = NULL;
      long long rows = 0;
      e = db_table_columns(c, table, &cols);
      if (e == DB_OK) e = db_table_indexes(c, table, &idx);
      if (e == DB_OK) e = db_table_row_count(c, table, &rows);
      if (e != DB_OK) {
        *result_json = json_error(e, db_last_error()->message);
      } else {
        char *cols_json = result_to_json(cols);
        char *idx_json = result_to_json(idx);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "row_count", (double)rows);
        cJSON_AddItemToObject(o, "columns", cJSON_Parse(cols_json));
        cJSON_AddItemToObject(o, "indexes", cJSON_Parse(idx_json));
        *result_json = json_take(o);
        free(cols_json);
        free(idx_json);
      }
      db_result_free(cols);
      db_result_free(idx);
    }

  } else if (!strcmp(method, "query.run")) {
    const char *sql = sget(args, "sql");
    if (!sql || !sql[0]) { *result_json = json_error(DB_ERR_INVALID_ARG, "sql is required"); e = DB_ERR_INVALID_ARG; }
    else e = run_query(c, sql, result_json);

  } else if (!strcmp(method, "convert.database")) {
    /* Convert a whole database to a new file, either direction (SQLite<->DuckDB). Unlike the
     * other convert.* methods (which only build SQL), this performs the work on its own
     * throwaway in-memory DuckDB: ATTACH both, copy each base table, then recreate views.
     * args: {src, src_engine, dst, dst_engine} with engine in {sqlite,duckdb}.
     *
     * We copy tables one by one (CREATE TABLE … AS SELECT *) rather than COPY FROM DATABASE so
     * we can skip SQLite-internal tables (sqlite_sequence, sqlite_stat*, …). COPY FROM DATABASE
     * copies them verbatim, which both pollutes a DuckDB target and makes the round-trip back
     * to SQLite fail ("object name reserved for internal use: sqlite_sequence"). */
    const char *src = sget(args, "src");
    const char *dst = sget(args, "dst");
    const char *se = sget(args, "src_engine");
    const char *de = sget(args, "dst_engine");
    if (!src || !src[0] || !dst || !dst[0] || !se || !de) {
      *result_json = json_error(DB_ERR_INVALID_ARG, "src, dst, src_engine, dst_engine are required");
      e = DB_ERR_INVALID_ARG;
    } else {
      int src_sqlite = !strcmp(se, "sqlite");
      int dst_sqlite = !strcmp(de, "sqlite");
      db_conn *dk = NULL;
      char *s_attach = NULL, *d_attach = NULL;
      e = ensure_parent_dir(dst);
      if (e == DB_OK) e = db_open_duckdb_memory(&dk);
      if (e == DB_OK) e = db_sql_attach_db(src, "src", src_sqlite, 1, &s_attach);  /* read-only */
      if (e == DB_OK) e = db_exec(dk, s_attach);
      if (e == DB_OK) e = db_sql_attach_db(dst, "dst", dst_sqlite, 0, &d_attach);
      if (e == DB_OK) e = db_exec(dk, d_attach);
      free(s_attach); free(d_attach);
      /* copy base tables (skip sqlite_* internal tables), default catalog = dst */
      db_result *tabs = NULL;
      if (e == DB_OK)
        e = db_query(dk,
          "SELECT table_name FROM information_schema.tables "
          "WHERE table_catalog='src' AND table_type='BASE TABLE' "
          "AND table_name NOT LIKE 'sqlite_%' ORDER BY table_name;", &tabs);
      if (e == DB_OK) e = db_exec(dk, "USE dst;");
      for (int i = 0; e == DB_OK && i < tabs->n_rows; i++) {
        char *cp = NULL;
        const char *t = db_result_value(tabs, i, 0);
        e = db_sql_copy_table("src", t, t, &cp);  /* CREATE TABLE "t" AS SELECT * FROM "src"."t" */
        if (e == DB_OK) e = db_exec(dk, cp);
        free(cp);
      }
      db_result_free(tabs);
      if (e != DB_OK) {
        *result_json = json_error(e, db_last_error()->message);
      } else {
        cJSON *failed = cJSON_CreateArray();
        recreate_views(src, src_sqlite, dk, failed);  /* best-effort; tables already copied */
        cJSON *o = cJSON_CreateObject();
        cJSON_AddBoolToObject(o, "ok", 1);
        cJSON_AddStringToObject(o, "dst", dst);
        cJSON_AddItemToObject(o, "views_failed", failed);
        *result_json = json_take(o);
      }
      db_close(dk);
    }

  } else if (!strncmp(method, "convert.", 8)) {
    /* convert.* build (don't run) DuckDB SQL the UI shows in the editor for review + run. */
    char *sql = NULL;
    const char *m = method + 8;
    const char *xpath = sget(args, "path");
    if (!strcmp(m, "import_csv"))
      e = db_sql_import_csv(sget(args, "table"), xpath, &sql);
    else if (!strcmp(m, "import_parquet"))
      e = db_sql_import_parquet(sget(args, "table"), xpath, &sql);
    else if (!strcmp(m, "export_parquet")) {
      e = (xpath && xpath[0]) ? ensure_parent_dir(xpath) : DB_OK;   /* create dest folder */
      if (e == DB_OK) e = db_sql_export_parquet(sget(args, "table"), xpath, &sql);
    } else if (!strcmp(m, "export_csv")) {
      e = (xpath && xpath[0]) ? ensure_parent_dir(xpath) : DB_OK;
      if (e == DB_OK) e = db_sql_export_csv(sget(args, "table"), xpath, &sql);
    }
    else if (!strcmp(m, "attach_sqlite"))
      e = db_sql_attach_sqlite(sget(args, "path"), sget(args, "alias"), &sql);
    else if (!strcmp(m, "copy_table"))
      e = db_sql_copy_table(sget(args, "src_schema"), sget(args, "src"), sget(args, "dst"), &sql);
    else { *result_json = json_error(DB_ERR_UNSUPPORTED, "unknown convert method"); e = DB_ERR_UNSUPPORTED; }

    if (!*result_json) {
      if (e != DB_OK) *result_json = json_error(e, db_last_error()->message);
      else {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "sql", sql);
        *result_json = json_take(o);
      }
    }
    free(sql);

  } else {
    *result_json = json_error(DB_ERR_UNSUPPORTED, "unknown method");
    e = DB_ERR_UNSUPPORTED;
  }

  cJSON_Delete(args);
  if (!*result_json) *result_json = json_error(DB_ERR_INTERNAL, "no result produced");
  return e;
}

/* ---------------------------------------------------------------------------
 * Unit tests (compiled only into the test runner via -DDB_TEST).
 * ------------------------------------------------------------------------- */
#ifdef DB_TEST
#include "../support/db_test.h"
#include <unistd.h>

/* helper: dispatch and parse the result JSON; caller cJSON_Delete()s. */
static cJSON *dispatch_json(db_conn *c, const char *m, const char *args, db_err *e) {
  char *out = NULL;
  *e = db_api_dispatch(c, m, args, &out);
  cJSON *j = cJSON_Parse(out);
  free(out);
  return j;
}

TEST(api, query_run_serializes_result) {
  db_conn *c = NULL;
  ASSERT_OK(db_open_sqlite_memory(&c));
  ASSERT_OK(db_exec(c, "CREATE TABLE t(id,name);"));
  ASSERT_OK(db_exec(c, "INSERT INTO t VALUES (1,'x'),(2,NULL);"));

  db_err e;
  cJSON *j = dispatch_json(c, "query.run", "{\"sql\":\"SELECT id,name FROM t ORDER BY id;\"}", &e);
  ASSERT_ERR_EQ(e, DB_OK);
  ASSERT_EQ_INT(cJSON_GetObjectItem(j, "row_count")->valueint, 2);

  cJSON *cols = cJSON_GetObjectItem(j, "columns");
  ASSERT_EQ_INT(cJSON_GetArraySize(cols), 2);
  ASSERT_STR_EQ(cJSON_GetObjectItem(cJSON_GetArrayItem(cols, 0), "name")->valuestring, "id");

  cJSON *rows = cJSON_GetObjectItem(j, "rows");
  cJSON *row0 = cJSON_GetArrayItem(rows, 0);
  ASSERT_STR_EQ(cJSON_GetArrayItem(row0, 0)->valuestring, "1");
  ASSERT_STR_EQ(cJSON_GetArrayItem(row0, 1)->valuestring, "x");
  cJSON *row1 = cJSON_GetArrayItem(rows, 1);
  ASSERT(cJSON_IsNull(cJSON_GetArrayItem(row1, 1)));   /* SQL NULL -> JSON null */
  cJSON_Delete(j);
  db_close(c);
}

TEST(api, schema_tables) {
  db_conn *c = NULL;
  ASSERT_OK(db_open_sqlite_memory(&c));
  ASSERT_OK(db_exec(c, "CREATE TABLE alpha(a); CREATE TABLE beta(b);"));
  db_err e;
  cJSON *j = dispatch_json(c, "schema.tables", "{}", &e);
  ASSERT_ERR_EQ(e, DB_OK);
  ASSERT_EQ_INT(cJSON_GetObjectItem(j, "row_count")->valueint, 2);
  cJSON_Delete(j);
  db_close(c);
}

TEST(api, schema_table_detail) {
  db_conn *c = NULL;
  ASSERT_OK(db_open_sqlite_memory(&c));
  ASSERT_OK(db_exec(c, "CREATE TABLE t(id INTEGER PRIMARY KEY, name TEXT NOT NULL);"));
  ASSERT_OK(db_exec(c, "CREATE INDEX ix_name ON t(name);"));
  ASSERT_OK(db_exec(c, "INSERT INTO t VALUES (1,'a'),(2,'b'),(3,'c');"));

  db_err e;
  cJSON *j = dispatch_json(c, "schema.table_detail", "{\"table\":\"t\"}", &e);
  ASSERT_ERR_EQ(e, DB_OK);
  ASSERT_EQ_INT(cJSON_GetObjectItem(j, "row_count")->valueint, 3);

  cJSON *cols = cJSON_GetObjectItem(cJSON_GetObjectItem(j, "columns"), "rows");
  ASSERT_EQ_INT(cJSON_GetArraySize(cols), 2);
  /* row 0: (name="id", type="INTEGER", notnull, pk=1) */
  ASSERT_STR_EQ(cJSON_GetArrayItem(cJSON_GetArrayItem(cols, 0), 0)->valuestring, "id");
  ASSERT_STR_EQ(cJSON_GetArrayItem(cJSON_GetArrayItem(cols, 0), 3)->valuestring, "1");  /* pk */

  cJSON *idx = cJSON_GetObjectItem(cJSON_GetObjectItem(j, "indexes"), "rows");
  ASSERT_EQ_INT(cJSON_GetArraySize(idx), 1);
  ASSERT_STR_EQ(cJSON_GetArrayItem(cJSON_GetArrayItem(idx, 0), 0)->valuestring, "ix_name");
  cJSON_Delete(j);

  /* missing table arg -> error envelope */
  j = dispatch_json(c, "schema.table_detail", "{}", &e);
  ASSERT_ERR_EQ(e, DB_ERR_INVALID_ARG);
  ASSERT(cJSON_GetObjectItem(j, "error") != NULL);
  cJSON_Delete(j);
  db_close(c);
}

TEST(api, schema_table_detail_duckdb) {
  db_conn *c = NULL;
  ASSERT_OK(db_open_duckdb_memory(&c));
  ASSERT_OK(db_exec(c, "CREATE TABLE t(id INTEGER, name VARCHAR);"));
  ASSERT_OK(db_exec(c, "CREATE INDEX ix_name ON t(name);"));
  ASSERT_OK(db_exec(c, "INSERT INTO t VALUES (1,'a'),(2,'b');"));

  db_err e;
  cJSON *j = dispatch_json(c, "schema.table_detail", "{\"table\":\"t\"}", &e);
  ASSERT_ERR_EQ(e, DB_OK);
  ASSERT_EQ_INT(cJSON_GetObjectItem(j, "row_count")->valueint, 2);
  cJSON *cols = cJSON_GetObjectItem(cJSON_GetObjectItem(j, "columns"), "rows");
  ASSERT_EQ_INT(cJSON_GetArraySize(cols), 2);
  cJSON *idx = cJSON_GetObjectItem(cJSON_GetObjectItem(j, "indexes"), "rows");
  ASSERT_EQ_INT(cJSON_GetArraySize(idx), 1);
  ASSERT_STR_EQ(cJSON_GetArrayItem(cJSON_GetArrayItem(idx, 0), 0)->valuestring, "ix_name");
  cJSON_Delete(j);
  db_close(c);
}

TEST(api, convert_sqlite_to_duckdb_end_to_end) {
  /* Build a small SQLite file with a table AND a view, convert to DuckDB, then open the
   * result and confirm both the table rows and the view came across (views need recreating). */
  const char *tmp = getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp";
  char src[320], dst[320];
  snprintf(src, sizeof src, "%s/dbview_conv_%d.sqlite", tmp, (int)getpid());
  snprintf(dst, sizeof dst, "%s/dbview_conv_%d.duckdb", tmp, (int)getpid());
  unlink(src);
  unlink(dst);

  db_conn *s = NULL;
  ASSERT_OK(db_open_sqlite(src, &s));
  ASSERT_OK(db_exec(s, "CREATE TABLE people(id INTEGER, name TEXT);"));
  ASSERT_OK(db_exec(s, "INSERT INTO people VALUES (1,'ada'),(2,'bob'),(3,'cy');"));
  ASSERT_OK(db_exec(s,
    "CREATE VIEW grown AS SELECT name FROM people "
    "WHERE id IN (SELECT id FROM people WHERE id > 1);"));
  db_close(s);

  /* dispatch is connection-agnostic for this method; pass any open connection. */
  db_conn *any = NULL;
  ASSERT_OK(db_open_duckdb_memory(&any));
  char args[768];
  snprintf(args, sizeof args,
           "{\"src\":\"%s\",\"src_engine\":\"sqlite\",\"dst\":\"%s\",\"dst_engine\":\"duckdb\"}",
           src, dst);
  db_err e;
  cJSON *j = dispatch_json(any, "convert.database", args, &e);
  ASSERT_ERR_EQ(e, DB_OK);
  ASSERT(cJSON_IsTrue(cJSON_GetObjectItem(j, "ok")));
  ASSERT_EQ_INT(cJSON_GetArraySize(cJSON_GetObjectItem(j, "views_failed")), 0);
  cJSON_Delete(j);
  db_close(any);

  /* Open the converted DuckDB file and verify the table data + the view. */
  db_conn *d = NULL;
  ASSERT_OK(db_open_duckdb(dst, &d));
  db_result *r = NULL;
  ASSERT_OK(db_query(d, "SELECT count(*) FROM people;", &r));
  ASSERT_STR_EQ(db_result_value(r, 0, 0), "3");
  db_result_free(r);
  ASSERT_OK(db_query(d, "SELECT count(*) FROM grown;", &r));  /* view recreated + queryable */
  ASSERT_STR_EQ(db_result_value(r, 0, 0), "2");
  db_result_free(r);
  /* the recreated definition must NOT reference the (now-detached) scanner attachment */
  ASSERT_OK(db_query(d, "SELECT sql FROM duckdb_views() WHERE view_name='grown';", &r));
  ASSERT(strstr(db_result_value(r, 0, 0), "sqlite_query") == NULL);
  ASSERT(strstr(db_result_value(r, 0, 0), "src") == NULL);
  db_result_free(r);
  db_close(d);

  unlink(src);
  unlink(dst);
}

TEST(api, convert_skips_sqlite_internal_tables_roundtrip) {
  /* AUTOINCREMENT creates the internal sqlite_sequence table. It must NOT be carried into the
   * DuckDB copy, and the round-trip back to SQLite must succeed (sqlite_sequence is a reserved
   * name there — copying it verbatim aborts the whole conversion). */
  const char *tmp = getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp";
  char sq[320], dk[320], sq2[320];
  snprintf(sq, sizeof sq, "%s/dbview_seq_%d.sqlite", tmp, (int)getpid());
  snprintf(dk, sizeof dk, "%s/dbview_seq_%d.duckdb", tmp, (int)getpid());
  snprintf(sq2, sizeof sq2, "%s/dbview_seq2_%d.sqlite", tmp, (int)getpid());
  unlink(sq); unlink(dk); unlink(sq2);

  db_conn *s = NULL;
  ASSERT_OK(db_open_sqlite(sq, &s));
  ASSERT_OK(db_exec(s, "CREATE TABLE items(id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT);"));
  ASSERT_OK(db_exec(s, "INSERT INTO items(name) VALUES ('a'),('b'),('c');"));
  db_close(s);

  db_conn *any = NULL;
  ASSERT_OK(db_open_duckdb_memory(&any));
  char a[800];
  db_err e;
  snprintf(a, sizeof a,
           "{\"src\":\"%s\",\"src_engine\":\"sqlite\",\"dst\":\"%s\",\"dst_engine\":\"duckdb\"}", sq, dk);
  cJSON *j = dispatch_json(any, "convert.database", a, &e);
  ASSERT_ERR_EQ(e, DB_OK);
  cJSON_Delete(j);

  /* the DuckDB copy must have items but NOT sqlite_sequence */
  db_conn *d = NULL;
  ASSERT_OK(db_open_duckdb(dk, &d));
  db_result *r = NULL;
  ASSERT_OK(db_query(d,
    "SELECT count(*) FROM information_schema.tables "
    "WHERE table_schema='main' AND table_name='sqlite_sequence';", &r));
  ASSERT_STR_EQ(db_result_value(r, 0, 0), "0");
  db_result_free(r);
  db_close(d);

  /* round-trip DuckDB -> SQLite must succeed and carry the data */
  snprintf(a, sizeof a,
           "{\"src\":\"%s\",\"src_engine\":\"duckdb\",\"dst\":\"%s\",\"dst_engine\":\"sqlite\"}", dk, sq2);
  j = dispatch_json(any, "convert.database", a, &e);
  ASSERT_ERR_EQ(e, DB_OK);
  cJSON_Delete(j);
  db_close(any);

  db_conn *x = NULL;
  ASSERT_OK(db_open_sqlite(sq2, &x));
  ASSERT_OK(db_query(x, "SELECT count(*) FROM items;", &r));
  ASSERT_STR_EQ(db_result_value(r, 0, 0), "3");
  db_result_free(r);
  db_close(x);

  unlink(sq); unlink(dk); unlink(sq2);
}

TEST(api, convert_duckdb_to_sqlite_end_to_end) {
  /* Reverse direction: DuckDB (table + view) -> SQLite. COPY FROM DATABASE copies views too
   * here, so the missing-view pass must be a no-op (no "already exists" error). */
  const char *tmp = getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp";
  char src[320], dst[320];
  snprintf(src, sizeof src, "%s/dbview_rconv_%d.duckdb", tmp, (int)getpid());
  snprintf(dst, sizeof dst, "%s/dbview_rconv_%d.sqlite", tmp, (int)getpid());
  unlink(src);
  unlink(dst);

  db_conn *s = NULL;
  ASSERT_OK(db_open_duckdb(src, &s));
  ASSERT_OK(db_exec(s, "CREATE TABLE people(id INTEGER, name VARCHAR);"));
  ASSERT_OK(db_exec(s, "INSERT INTO people VALUES (1,'ada'),(2,'bob'),(3,'cy');"));
  ASSERT_OK(db_exec(s, "CREATE VIEW grown AS SELECT name FROM people WHERE id > 1;"));
  db_close(s);

  db_conn *any = NULL;
  ASSERT_OK(db_open_duckdb_memory(&any));
  char args[768];
  snprintf(args, sizeof args,
           "{\"src\":\"%s\",\"src_engine\":\"duckdb\",\"dst\":\"%s\",\"dst_engine\":\"sqlite\"}",
           src, dst);
  db_err e;
  cJSON *j = dispatch_json(any, "convert.database", args, &e);
  ASSERT_ERR_EQ(e, DB_OK);
  ASSERT_EQ_INT(cJSON_GetArraySize(cJSON_GetObjectItem(j, "views_failed")), 0);
  cJSON_Delete(j);
  db_close(any);

  db_conn *d = NULL;
  ASSERT_OK(db_open_sqlite(dst, &d));
  db_result *r = NULL;
  ASSERT_OK(db_query(d, "SELECT count(*) FROM people;", &r));
  ASSERT_STR_EQ(db_result_value(r, 0, 0), "3");
  db_result_free(r);
  ASSERT_OK(db_query(d, "SELECT count(*) FROM grown;", &r));
  ASSERT_STR_EQ(db_result_value(r, 0, 0), "2");
  db_result_free(r);
  db_close(d);

  unlink(src);
  unlink(dst);
}

TEST(api, bad_sql_returns_error_envelope) {
  db_conn *c = NULL;
  ASSERT_OK(db_open_sqlite_memory(&c));
  db_err e;
  cJSON *j = dispatch_json(c, "query.run", "{\"sql\":\"SELCT 1\"}", &e);
  ASSERT_ERR_EQ(e, DB_ERR_SQL);
  cJSON *err = cJSON_GetObjectItem(j, "error");
  ASSERT(err != NULL);
  ASSERT_STR_EQ(cJSON_GetObjectItem(err, "code")->valuestring, "DB_ERR_SQL");
  cJSON_Delete(j);
  db_close(c);
}

TEST(api, convert_returns_sql) {
  db_conn *c = NULL;
  ASSERT_OK(db_open_duckdb_memory(&c));
  db_err e;
  cJSON *j = dispatch_json(c, "convert.import_csv", "{\"table\":\"people\",\"path\":\"/tmp/d.csv\"}", &e);
  ASSERT_ERR_EQ(e, DB_OK);
  ASSERT_STR_EQ(cJSON_GetObjectItem(j, "sql")->valuestring,
                "CREATE TABLE \"people\" AS SELECT * FROM read_csv_auto('/tmp/d.csv');");
  cJSON_Delete(j);

  j = dispatch_json(c, "convert.import_csv", "{\"table\":\"\",\"path\":\"/tmp/d.csv\"}", &e);
  ASSERT_ERR_EQ(e, DB_ERR_INVALID_ARG);
  ASSERT(cJSON_GetObjectItem(j, "error") != NULL);
  cJSON_Delete(j);
  db_close(c);
}

TEST(api, export_creates_parent_dir) {
  db_conn *c = NULL;
  ASSERT_OK(db_open_duckdb_memory(&c));

  char dir[256], file[320];
  snprintf(dir, sizeof dir, "%s/dbview_mkdir_%d/nested",
           getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", (int)getpid());
  snprintf(file, sizeof file, "%s/out.parquet", dir);
  /* ensure it does not exist yet */
  ASSERT(access(dir, F_OK) != 0);

  char args[512];
  snprintf(args, sizeof args, "{\"table\":\"t\",\"path\":\"%s\"}", file);
  db_err e;
  cJSON *j = dispatch_json(c, "convert.export_parquet", args, &e);
  ASSERT_ERR_EQ(e, DB_OK);
  ASSERT(cJSON_GetObjectItem(j, "sql") != NULL);
  ASSERT(access(dir, F_OK) == 0);   /* parent directory was created */
  cJSON_Delete(j);

  rmdir(dir);
  db_close(c);
}

TEST(api, unknown_method) {
  db_conn *c = NULL;
  ASSERT_OK(db_open_sqlite_memory(&c));
  db_err e;
  cJSON *j = dispatch_json(c, "nope.nope", "{}", &e);
  ASSERT_ERR_EQ(e, DB_ERR_UNSUPPORTED);
  ASSERT_STR_EQ(cJSON_GetObjectItem(cJSON_GetObjectItem(j, "error"), "code")->valuestring,
                "DB_ERR_UNSUPPORTED");
  cJSON_Delete(j);
  db_close(c);
}

#endif /* DB_TEST */
