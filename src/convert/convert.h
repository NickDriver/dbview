#ifndef DB_CONVERT_H
#define DB_CONVERT_H
/*
 * dbview — conversion SQL builders (Phase 2, SPEC §4.5).
 *
 * The conversion feature is "a wizard that generates editable SQL": these pure functions
 * build DuckDB SQL strings the UI shows and the user runs (via query.run). Keeping them as
 * string builders makes them trivially testable and keeps execution on the one engine path.
 *
 * All identifiers are double-quoted and all paths single-quoted with internal quotes doubled,
 * so names/paths with spaces or quotes are safe. DuckDB dialect (the conversion engine).
 *
 * Each builder sets *out_sql to a malloc'd string (free with free()) and returns DB_OK,
 * or sets *out_sql = NULL and returns an error.
 */
#include "../support/db_error.h"

/* CSV file -> new table:  CREATE TABLE "t" AS SELECT * FROM read_csv_auto('path'); */
db_err db_sql_import_csv(const char *table, const char *csv_path, char **out_sql) DB_MUST_CHECK;

/* Parquet file -> new table:  CREATE TABLE "t" AS SELECT * FROM read_parquet('path'); */
db_err db_sql_import_parquet(const char *table, const char *parquet_path, char **out_sql) DB_MUST_CHECK;

/* table -> Parquet file:  COPY (SELECT * FROM "t") TO 'path' (FORMAT parquet); */
db_err db_sql_export_parquet(const char *table, const char *out_path, char **out_sql) DB_MUST_CHECK;

/* table -> CSV file:  COPY (SELECT * FROM "t") TO 'path' (HEADER, FORMAT csv); */
db_err db_sql_export_csv(const char *table, const char *out_path, char **out_sql) DB_MUST_CHECK;

/* attach a SQLite file read-only:  ATTACH 'path' AS "alias" (TYPE sqlite, READ_ONLY); */
db_err db_sql_attach_sqlite(const char *sqlite_path, const char *alias, char **out_sql) DB_MUST_CHECK;

/* copy a (optionally schema-qualified) source table into a new local table:
 *   CREATE TABLE "dst" AS SELECT * FROM "schema"."src";   (schema may be NULL) */
db_err db_sql_copy_table(const char *src_schema, const char *src_table,
                         const char *dst_table, char **out_sql) DB_MUST_CHECK;

#endif /* DB_CONVERT_H */
