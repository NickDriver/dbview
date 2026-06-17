/*
 * Integration test: the conversion round-trips that back Phase 2's goals — CSV -> DuckDB
 * table -> Parquet -> re-read, and SQLite -> DuckDB via the scanner -> copy. Built from the
 * convert.c SQL builders + executed on a real DuckDB connection, asserting rows/values survive
 * each hop (the "round-trip conversion tests are first-class" rule, SPEC §8).
 */
#include "../src/convert/convert.h"
#include "../src/engine/engine.h"
#include "../src/engine/engine_test.h"
#include "../src/support/db_test.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static void tmp_path(char *buf, size_t n, const char *suffix) {
  snprintf(buf, n, "%s/dbview_cvt_%d%s",
           getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", (int)getpid(), suffix);
}

/* helper: build SQL with `builder`, run it via db_exec, asserting OK. */
#define RUN_BUILT(call) do { \
    char *_sql = NULL; ASSERT_OK(call); \
    db_err _e = db_exec(c, _sql); \
    if (_e != DB_OK) { db_test_fail(t, __FILE__, __LINE__, "exec failed: %s — %s", _sql, db_last_error()->message); free(_sql); return; } \
    free(_sql); \
  } while (0)

TEST(integration, csv_to_table_to_parquet) {
  char csv[256], parquet[256];
  tmp_path(csv, sizeof csv, ".csv");
  tmp_path(parquet, sizeof parquet, ".parquet");

  FILE *f = fopen(csv, "w");
  ASSERT(f != NULL);
  fputs("id,name,score\n1,alice,9.5\n2,bob,7\n3,carol,8.25\n", f);
  fclose(f);

  db_conn *c = NULL;
  ASSERT_OK(db_open_duckdb_memory(&c));

  RUN_BUILT(db_sql_import_csv("people", csv, &_sql));
  ASSERT_SQL_SCALAR(c, "SELECT count(*) FROM people;", "3");
  ASSERT_SQL_SCALAR(c, "SELECT typeof(score) FROM people LIMIT 1;", "DOUBLE");  /* type inferred */

  RUN_BUILT(db_sql_export_parquet("people", parquet, &_sql));
  /* re-read the parquet to prove rows + values survived the round trip */
  db_result *r = NULL;
  char q[512];
  snprintf(q, sizeof q, "SELECT count(*) AS n, sum(score) AS s FROM read_parquet('%s');", parquet);
  ASSERT_OK(db_query(c, q, &r));
  ASSERT_ROWS(r, 1);
  ASSERT_CELL_EQ(r, 0, 0, "3");
  ASSERT_CELL_EQ(r, 0, 1, "24.75");
  db_result_free(r);

  db_close(c);
  unlink(csv);
  unlink(parquet);
}

TEST(integration, sqlite_attach_and_copy) {
  char sqlitep[256];
  tmp_path(sqlitep, sizeof sqlitep, ".sqlite");
  unlink(sqlitep);

  /* build a source SQLite db using our own engine */
  db_conn *s = NULL;
  ASSERT_OK(db_open_sqlite(sqlitep, &s));
  ASSERT_OK(db_exec(s, "CREATE TABLE city(id INTEGER, name TEXT); INSERT INTO city VALUES (1,'NYC'),(2,'LA');"));
  db_close(s);

  /* in a DuckDB connection: ATTACH the sqlite file, copy its table in, verify */
  db_conn *c = NULL;
  ASSERT_OK(db_open_duckdb_memory(&c));
  RUN_BUILT(db_sql_attach_sqlite(sqlitep, "src", &_sql));
  RUN_BUILT(db_sql_copy_table("src", "city", "city_local", &_sql));
  ASSERT_SQL_SCALAR(c, "SELECT count(*) FROM city_local;", "2");
  ASSERT_SQL_SCALAR(c, "SELECT name FROM city_local ORDER BY id LIMIT 1;", "NYC");

  db_close(c);
  unlink(sqlitep);
}
