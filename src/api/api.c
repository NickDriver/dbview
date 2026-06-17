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

  } else if (!strcmp(method, "query.run")) {
    const char *sql = sget(args, "sql");
    if (!sql || !sql[0]) { *result_json = json_error(DB_ERR_INVALID_ARG, "sql is required"); e = DB_ERR_INVALID_ARG; }
    else e = run_query(c, sql, result_json);

  } else if (!strncmp(method, "convert.", 8)) {
    /* convert.* build (don't run) DuckDB SQL the UI shows in the editor for review + run. */
    char *sql = NULL;
    const char *m = method + 8;
    const char *xpath = sget(args, "path");
    if (!strcmp(m, "import_csv"))
      e = db_sql_import_csv(sget(args, "table"), xpath, &sql);
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
