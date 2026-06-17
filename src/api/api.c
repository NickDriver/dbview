/*
 * dbview — JSON command API. Turns method + JSON args into engine calls and JSON results.
 * The same surface backs the WebView bridge and (later) an MCP server. See SPEC §6.
 */
#include "api.h"

#include "../vendor/cjson/cJSON.h"

#include <stdlib.h>
#include <string.h>

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
