/*
 * Integration test: create an on-disk DuckDB file, populate it, reopen it fresh, and assert
 * tables + data persist — the file-open path the app uses for *.duckdb files.
 */
#include "../src/engine/engine.h"
#include "../src/engine/engine_test.h"
#include "../src/support/db_test.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

TEST(integration, duckdb_file_open_and_list) {
  char path[256];
  snprintf(path, sizeof path, "%s/dbview_it_%d.duckdb",
           getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", (int)getpid());
  unlink(path);

  db_conn *w = NULL;
  ASSERT_OK(db_open_duckdb(path, &w));
  ASSERT_STR_EQ(db_conn_path(w), path);
  ASSERT_OK(db_exec(w, "CREATE TABLE metrics(day DATE, value DOUBLE);"));
  ASSERT_OK(db_exec(w, "INSERT INTO metrics VALUES ('2026-01-01', 1.5), ('2026-01-02', 2.5);"));
  db_close(w);

  db_conn *c = NULL;
  ASSERT_OK(db_open_duckdb(path, &c));
  db_result *tables = NULL;
  ASSERT_OK(db_list_tables(c, &tables));
  ASSERT_ROWS(tables, 1);
  ASSERT_CELL_EQ(tables, 0, 0, "metrics");
  db_result_free(tables);

  ASSERT_SQL_SCALAR(c, "SELECT COUNT(*) FROM metrics;", "2");
  ASSERT_SQL_SCALAR(c, "SELECT SUM(value) FROM metrics;", "4.0");
  db_close(c);
  unlink(path);
}
