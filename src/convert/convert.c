/*
 * dbview — conversion SQL builders. See convert.h and SPEC §4.5.
 */
#include "convert.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Quote `s` with delimiter `q`, doubling any internal `q`. Returns malloc'd string. */
static char *quote_with(const char *s, char q) {
  size_t n = 0;
  for (const char *p = s; *p; p++) n += (*p == q) ? 2u : 1u;
  char *out = malloc(n + 3);  /* two delimiters + NUL */
  if (!out) return NULL;
  size_t j = 0;
  out[j++] = q;
  for (const char *p = s; *p; p++) { if (*p == q) out[j++] = q; out[j++] = *p; }
  out[j++] = q;
  out[j] = '\0';
  return out;
}
static char *qident(const char *s) { return quote_with(s, '"'); }
static char *qlit(const char *s) { return quote_with(s, '\''); }

/* printf into a freshly malloc'd, exactly-sized string. Returns NULL on OOM. */
static char *asprintf_dup(const char *fmt, ...) DB_PRINTF(1, 2);
static char *asprintf_dup(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  if (n < 0) return NULL;
  char *out = malloc((size_t)n + 1);
  if (!out) return NULL;
  va_start(ap, fmt);
  vsnprintf(out, (size_t)n + 1, fmt, ap);
  va_end(ap);
  return out;
}

db_err db_sql_import_csv(const char *table, const char *csv_path, char **out_sql) {
  if (!out_sql) return DB_FAIL(DB_ERR_INVALID_ARG, "out_sql is NULL");
  *out_sql = NULL;
  if (!table || !table[0] || !csv_path || !csv_path[0])
    return DB_FAIL(DB_ERR_INVALID_ARG, "table and csv_path are required");
  char *t = qident(table), *p = qlit(csv_path);
  char *sql = (t && p) ? asprintf_dup("CREATE TABLE %s AS SELECT * FROM read_csv_auto(%s);", t, p) : NULL;
  free(t); free(p);
  if (!sql) return DB_FAIL(DB_ERR_OOM, "build sql");
  *out_sql = sql;
  return DB_OK;
}

db_err db_sql_import_parquet(const char *table, const char *parquet_path, char **out_sql) {
  if (!out_sql) return DB_FAIL(DB_ERR_INVALID_ARG, "out_sql is NULL");
  *out_sql = NULL;
  if (!table || !table[0] || !parquet_path || !parquet_path[0])
    return DB_FAIL(DB_ERR_INVALID_ARG, "table and parquet_path are required");
  char *t = qident(table), *p = qlit(parquet_path);
  char *sql = (t && p) ? asprintf_dup("CREATE TABLE %s AS SELECT * FROM read_parquet(%s);", t, p) : NULL;
  free(t); free(p);
  if (!sql) return DB_FAIL(DB_ERR_OOM, "build sql");
  *out_sql = sql;
  return DB_OK;
}

db_err db_sql_export_parquet(const char *table, const char *out_path, char **out_sql) {
  if (!out_sql) return DB_FAIL(DB_ERR_INVALID_ARG, "out_sql is NULL");
  *out_sql = NULL;
  if (!table || !table[0] || !out_path || !out_path[0])
    return DB_FAIL(DB_ERR_INVALID_ARG, "table and out_path are required");
  char *t = qident(table), *p = qlit(out_path);
  char *sql = (t && p) ? asprintf_dup("COPY (SELECT * FROM %s) TO %s (FORMAT parquet);", t, p) : NULL;
  free(t); free(p);
  if (!sql) return DB_FAIL(DB_ERR_OOM, "build sql");
  *out_sql = sql;
  return DB_OK;
}

db_err db_sql_export_csv(const char *table, const char *out_path, char **out_sql) {
  if (!out_sql) return DB_FAIL(DB_ERR_INVALID_ARG, "out_sql is NULL");
  *out_sql = NULL;
  if (!table || !table[0] || !out_path || !out_path[0])
    return DB_FAIL(DB_ERR_INVALID_ARG, "table and out_path are required");
  char *t = qident(table), *p = qlit(out_path);
  char *sql = (t && p) ? asprintf_dup("COPY (SELECT * FROM %s) TO %s (HEADER, FORMAT csv);", t, p) : NULL;
  free(t); free(p);
  if (!sql) return DB_FAIL(DB_ERR_OOM, "build sql");
  *out_sql = sql;
  return DB_OK;
}

db_err db_sql_attach_sqlite(const char *sqlite_path, const char *alias, char **out_sql) {
  if (!out_sql) return DB_FAIL(DB_ERR_INVALID_ARG, "out_sql is NULL");
  *out_sql = NULL;
  if (!sqlite_path || !sqlite_path[0] || !alias || !alias[0])
    return DB_FAIL(DB_ERR_INVALID_ARG, "sqlite_path and alias are required");
  char *p = qlit(sqlite_path), *a = qident(alias);
  char *sql = (p && a) ? asprintf_dup("ATTACH %s AS %s (TYPE sqlite, READ_ONLY);", p, a) : NULL;
  free(p); free(a);
  if (!sql) return DB_FAIL(DB_ERR_OOM, "build sql");
  *out_sql = sql;
  return DB_OK;
}

db_err db_sql_copy_table(const char *src_schema, const char *src_table,
                         const char *dst_table, char **out_sql) {
  if (!out_sql) return DB_FAIL(DB_ERR_INVALID_ARG, "out_sql is NULL");
  *out_sql = NULL;
  if (!src_table || !src_table[0] || !dst_table || !dst_table[0])
    return DB_FAIL(DB_ERR_INVALID_ARG, "src_table and dst_table are required");

  char *dst = qident(dst_table);
  char *st = qident(src_table);
  char *ss = (src_schema && src_schema[0]) ? qident(src_schema) : NULL;
  char *sql = NULL;
  if (dst && st && (!src_schema || !src_schema[0] || ss)) {
    sql = (src_schema && src_schema[0])
              ? asprintf_dup("CREATE TABLE %s AS SELECT * FROM %s.%s;", dst, ss, st)
              : asprintf_dup("CREATE TABLE %s AS SELECT * FROM %s;", dst, st);
  }
  free(dst); free(st); free(ss);
  if (!sql) return DB_FAIL(DB_ERR_OOM, "build sql");
  *out_sql = sql;
  return DB_OK;
}

/* ---------------------------------------------------------------------------
 * Unit tests (compiled only into the test runner via -DDB_TEST).
 * ------------------------------------------------------------------------- */
#ifdef DB_TEST
#include "../support/db_test.h"

TEST(convert, import_csv_sql) {
  char *sql = NULL;
  ASSERT_OK(db_sql_import_csv("people", "/tmp/data.csv", &sql));
  ASSERT_STR_EQ(sql, "CREATE TABLE \"people\" AS SELECT * FROM read_csv_auto('/tmp/data.csv');");
  free(sql);
}

TEST(convert, import_csv_quotes_special_chars) {
  char *sql = NULL;
  /* a table name with a quote and a path with a space + apostrophe must be escaped */
  ASSERT_OK(db_sql_import_csv("we\"ird", "/a b/it's.csv", &sql));
  ASSERT_STR_EQ(sql, "CREATE TABLE \"we\"\"ird\" AS SELECT * FROM read_csv_auto('/a b/it''s.csv');");
  free(sql);
}

TEST(convert, import_parquet_sql) {
  char *sql = NULL;
  ASSERT_OK(db_sql_import_parquet("t", "/tmp/x.parquet", &sql));
  ASSERT_STR_EQ(sql, "CREATE TABLE \"t\" AS SELECT * FROM read_parquet('/tmp/x.parquet');");
  free(sql);
}

TEST(convert, export_parquet_and_csv_sql) {
  char *sql = NULL;
  ASSERT_OK(db_sql_export_parquet("t", "/out/x.parquet", &sql));
  ASSERT_STR_EQ(sql, "COPY (SELECT * FROM \"t\") TO '/out/x.parquet' (FORMAT parquet);");
  free(sql);
  ASSERT_OK(db_sql_export_csv("t", "/out/x.csv", &sql));
  ASSERT_STR_EQ(sql, "COPY (SELECT * FROM \"t\") TO '/out/x.csv' (HEADER, FORMAT csv);");
  free(sql);
}

TEST(convert, attach_sqlite_sql) {
  char *sql = NULL;
  ASSERT_OK(db_sql_attach_sqlite("/db/app.sqlite", "src", &sql));
  ASSERT_STR_EQ(sql, "ATTACH '/db/app.sqlite' AS \"src\" (TYPE sqlite, READ_ONLY);");
  free(sql);
}

TEST(convert, copy_table_sql) {
  char *sql = NULL;
  ASSERT_OK(db_sql_copy_table(NULL, "t", "t2", &sql));
  ASSERT_STR_EQ(sql, "CREATE TABLE \"t2\" AS SELECT * FROM \"t\";");
  free(sql);
  ASSERT_OK(db_sql_copy_table("src", "people", "people", &sql));
  ASSERT_STR_EQ(sql, "CREATE TABLE \"people\" AS SELECT * FROM \"src\".\"people\";");
  free(sql);
}

TEST(convert, rejects_missing_args) {
  char *sql = (char *)"sentinel";
  ASSERT_ERR_EQ(db_sql_import_csv("", "/x.csv", &sql), DB_ERR_INVALID_ARG);
  ASSERT(sql == NULL);
  ASSERT_ERR_EQ(db_sql_export_parquet("t", "", &sql), DB_ERR_INVALID_ARG);
  ASSERT_ERR_EQ(db_sql_copy_table(NULL, "", "dst", &sql), DB_ERR_INVALID_ARG);
}

#endif /* DB_TEST */
