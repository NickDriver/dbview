/*
 * dbview — native shell (Phase 0).
 *
 * Creates a system WebView window (via webview/webview), binds `dbInvoke` so the React UI
 * can call the C engine, and loads the bundled front-end. The bridge dispatches to the SAME
 * db_api_dispatch surface a future MCP layer will use (SPEC §6).
 *
 *   - `app.*` methods (open a database, report the current one) are handled here at the shell
 *     level because they swap the active connection.
 *   - everything else dispatches to the open connection.
 *
 * Built only by the `app` CMake target (needs the webview header + WebKit/Cocoa).
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "webview/webview.h"
#include "vendor/cjson/cJSON.h"
#include "engine/engine.h"
#include "api/api.h"
#ifdef DBVIEW_MACOS
#include "file_dialog.h"
#endif

struct app_ctx {
  webview_t w;
  db_conn  *conn;          /* current connection, or NULL on the launcher */
  char      path[1024];     /* path of the current database ("" if none) */
};

/* ---- small JSON helpers ---- */
static char *json_take(cJSON *o) {
  char *s = cJSON_PrintUnformatted(o);
  cJSON_Delete(o);
  return s ? s : strdup("{\"error\":{\"code\":\"DB_ERR_OOM\",\"message\":\"print failed\"}}");
}
static char *json_err(db_err code, const char *msg) {
  cJSON *env = cJSON_CreateObject();
  cJSON *err = cJSON_AddObjectToObject(env, "error");
  cJSON_AddStringToObject(err, "code", db_err_name(code));
  cJSON_AddStringToObject(err, "message", msg && msg[0] ? msg : db_err_default_msg(code));
  return json_take(env);
}
static const char *sget(const cJSON *a, const char *key) {
  const cJSON *v = cJSON_GetObjectItemCaseSensitive(a, key);
  return cJSON_IsString(v) ? v->valuestring : NULL;
}

static const char *engine_name(db_conn *c) {
  return db_conn_kind(c) == DB_ENGINE_DUCKDB ? "duckdb" : "sqlite";
}

/* {path, read_only, engine} for the current connection, or {path:null} when none. */
static char *current_json(struct app_ctx *c) {
  cJSON *o = cJSON_CreateObject();
  if (c->conn) {
    cJSON_AddStringToObject(o, "path", c->path);
    cJSON_AddBoolToObject(o, "read_only", db_conn_read_only(c->conn));
    cJSON_AddBoolToObject(o, "snapshot", db_conn_is_snapshot(c->conn));
    cJSON_AddStringToObject(o, "engine", engine_name(c->conn));
  } else {
    cJSON_AddNullToObject(o, "path");
  }
  return json_take(o);
}

/* Pick the engine from the file extension (.duckdb/.ddb -> DuckDB, else SQLite). */
static bool has_suffix(const char *s, const char *suf) {
  size_t ls = strlen(s), lf = strlen(suf);
  return ls >= lf && strcmp(s + ls - lf, suf) == 0;
}

/* Open `path`, replace the active connection, update title. `engine` ("duckdb"/"sqlite")
 * forces the backend; when NULL/empty it's inferred from the file extension. */
static db_err open_db(struct app_ctx *c, const char *path, const char *engine) {
  bool duck = (engine && engine[0])
                  ? !strcmp(engine, "duckdb")
                  : (has_suffix(path, ".duckdb") || has_suffix(path, ".ddb"));
  db_conn *nc = NULL;
  db_err e = duck ? db_open_duckdb(path, &nc) : db_open_sqlite(path, &nc);
  if (e != DB_OK) return e;
  if (c->conn) db_close(c->conn);
  c->conn = nc;
  snprintf(c->path, sizeof c->path, "%s", path);
  if (c->w) {
    char title[1152];
    snprintf(title, sizeof title, "%s — dbview", path);
    webview_set_title(c->w, title);
  }
  return DB_OK;
}

/* Open a fresh in-memory database (DuckDB or SQLite), replace the active connection. */
static db_err open_memory(struct app_ctx *c, const char *engine) {
  bool duck = engine && !strcmp(engine, "duckdb");
  db_conn *nc = NULL;
  db_err e = duck ? db_open_duckdb_memory(&nc) : db_open_sqlite_memory(&nc);
  if (e != DB_OK) return e;
  if (c->conn) db_close(c->conn);
  c->conn = nc;
  snprintf(c->path, sizeof c->path, "(in-memory \xC2\xB7 %s)", duck ? "duckdb" : "sqlite");
  if (c->w) {
    char title[1152];
    snprintf(title, sizeof title, "%s \xE2\x80\x94 dbview", c->path);
    webview_set_title(c->w, title);
  }
  return DB_OK;
}

/* Shell-level `app.*` methods. Always returns a malloc'd JSON string. */
static char *shell_dispatch(struct app_ctx *c, const char *method, const char *args_json) {
  cJSON *a = (args_json && args_json[0]) ? cJSON_Parse(args_json) : cJSON_CreateObject();
  char *out = NULL;

  if (!strcmp(method, "app.current")) {
    out = current_json(c);

  } else if (!strcmp(method, "app.open")) {
    const char *path = sget(a, "path");
    if (!path || !path[0]) out = json_err(DB_ERR_INVALID_ARG, "path required");
    else {
      db_err e = open_db(c, path, sget(a, "engine"));
      out = (e != DB_OK) ? json_err(e, db_last_error()->message) : current_json(c);
    }

  } else if (!strcmp(method, "app.new_memory")) {
    db_err e = open_memory(c, sget(a, "engine"));
    out = (e != DB_OK) ? json_err(e, db_last_error()->message) : current_json(c);

  } else if (!strcmp(method, "app.write_file")) {
    const char *path = sget(a, "path");
    const char *text = sget(a, "text");
    if (!path || !path[0]) {
      out = json_err(DB_ERR_INVALID_ARG, "path required");
    } else {
      FILE *f = fopen(path, "wb");
      if (!f) {
        out = json_err(DB_ERR_IO, strerror(errno));
      } else {
        size_t len = text ? strlen(text) : 0;
        size_t w = fwrite(text ? text : "", 1, len, f);
        fclose(f);
        if (w != len) out = json_err(DB_ERR_IO, "short write");
        else { cJSON *o = cJSON_CreateObject(); cJSON_AddBoolToObject(o, "ok", 1); out = json_take(o); }
      }
    }

  } else if (!strcmp(method, "app.clipboard_write")) {
#ifdef DBVIEW_MACOS
    const char *text = sget(a, "text");
    dbview_clipboard_set(text ? text : "");
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", 1);
    out = json_take(o);
#else
    out = json_err(DB_ERR_UNSUPPORTED, "clipboard write is macOS-only for now");
#endif

  } else if (!strcmp(method, "app.pick_open") || !strcmp(method, "app.pick_save")) {
#ifdef DBVIEW_MACOS
    char *picked = !strcmp(method, "app.pick_open")
                       ? dbview_dialog_open("Open a SQLite or DuckDB database")
                       : dbview_dialog_save("Choose where to save", sget(a, "default_name"));
    cJSON *o = cJSON_CreateObject();
    if (picked) cJSON_AddStringToObject(o, "path", picked);
    else cJSON_AddNullToObject(o, "path");   /* null = user cancelled */
    free(picked);
    out = json_take(o);
#else
    out = json_err(DB_ERR_UNSUPPORTED, "native file dialogs are macOS-only for now");
#endif

  } else {
    out = json_err(DB_ERR_UNSUPPORTED, "unknown app method");
  }

  cJSON_Delete(a);
  return out ? out : json_err(DB_ERR_INTERNAL, "no result");
}

/* JS calls window.dbInvoke(method, argsJson). webview passes req as ["method","argsJson"]. */
static void on_invoke(const char *id, const char *req, void *arg) {
  struct app_ctx *c = arg;

  cJSON *params = cJSON_Parse(req);
  const char *method = "";
  const char *args = "{}";
  if (cJSON_IsArray(params)) {
    cJSON *m = cJSON_GetArrayItem(params, 0);
    cJSON *a = cJSON_GetArrayItem(params, 1);
    if (cJSON_IsString(m)) method = m->valuestring;
    if (cJSON_IsString(a)) args = a->valuestring;
  }

  char *result = NULL;
  if (!strncmp(method, "app.", 4)) {
    result = shell_dispatch(c, method, args);
  } else if (!c->conn) {
    result = json_err(DB_ERR_INVALID_ARG, "no database is open");
  } else {
    (void)db_api_dispatch(c->conn, method, args, &result);
  }

  webview_return(c->w, id, 0, result ? result : "{\"error\":{\"code\":\"DB_ERR_INTERNAL\",\"message\":\"no result\"}}");
  free(result);
  cJSON_Delete(params);
}

int main(int argc, char **argv) {
  static struct app_ctx c;

  webview_t w = webview_create(1, NULL);   /* debug=1 enables the Web Inspector */
  c.w = w;
  webview_set_title(w, "dbview");
  webview_set_size(w, 1100, 760, WEBVIEW_HINT_NONE);
  webview_bind(w, "dbInvoke", on_invoke, &c);
#ifdef DBVIEW_MACOS
  dbview_install_menu();   /* enables Cmd+C/V/X/A/Z in the editor */
#endif

  if (argc > 1) {
    if (open_db(&c, argv[1], NULL) != DB_OK)
      fprintf(stderr, "failed to open '%s': %s\n", argv[1], db_last_error()->message);
  }

  /* Load the built UI; override with DBVIEW_UI_URL (e.g. http://localhost:5173) during dev. */
  const char *url = getenv("DBVIEW_UI_URL");
  char filebuf[1024];
  if (!url) {
    char cwd[768];
    if (getcwd(cwd, sizeof cwd)) {
      snprintf(filebuf, sizeof filebuf, "file://%s/ui/dist/index.html", cwd);
      url = filebuf;
    } else {
      url = "file://ui/dist/index.html";
    }
  }
  webview_navigate(w, url);

  webview_run(w);
  webview_destroy(w);
  if (c.conn) db_close(c.conn);
  return 0;
}
