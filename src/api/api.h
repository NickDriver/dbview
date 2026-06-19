#ifndef DB_API_H
#define DB_API_H
/*
 * dbview — JSON command API (Phase 0, SPEC §6).
 *
 * One dispatch surface used by BOTH the WebView UI bridge today and a future MCP tool
 * layer: a method name + JSON args -> engine ops -> JSON result. This is the single
 * contract the UI (and later an AI) speak.
 *
 * Methods (Phase 0):
 *   "schema.tables"        args: {}                -> result set of (name, type)
 *   "schema.table_detail"  args: {"table": "..."}  -> {row_count, columns, indexes}
 *   "query.run"            args: {"sql": "..."}    -> result set
 *
 * `app.*` methods (open file, recent files) are handled at the shell level in main.c.
 */
#include "../engine/engine.h"

/* Dispatch `method` with `args_json` (may be NULL/empty for no args).
 * *result_json is always set to a malloc'd JSON string (free with free()):
 *   success -> the result object;  failure -> {"error":{"code":...,"message":...}}.
 * Returns the underlying db_err as well. */
db_err db_api_dispatch(db_conn *c, const char *method, const char *args_json,
                       char **result_json) DB_MUST_CHECK;

#endif /* DB_API_H */
