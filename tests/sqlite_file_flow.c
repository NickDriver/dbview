/*
 * Integration test: drive the engine the way the app will — create an on-disk SQLite
 * file, populate it, reopen it fresh, and assert tables + data are visible. This is the
 * Phase 0 #4 acceptance: "open a SQLite file & list tables".
 */
#include "../src/engine/engine.h"
#include "../src/engine/engine_test.h"
#include "../src/support/db_test.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

TEST(integration, sqlite_file_open_and_list) {
  /* a unique temp path in the OS temp dir */
  char path[256];
  snprintf(path, sizeof path, "%s/dbview_it_%d.sqlite",
           getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", (int)getpid());
  unlink(path);

  /* write */
  db_conn *w = NULL;
  ASSERT_OK(db_open_sqlite(path, &w));
  ASSERT_STR_EQ(db_conn_path(w), path);
  ASSERT_OK(db_exec(w, "CREATE TABLE customers(id INTEGER PRIMARY KEY, name TEXT);"));
  ASSERT_OK(db_exec(w, "CREATE TABLE orders(id INTEGER PRIMARY KEY, total REAL);"));
  ASSERT_OK(db_exec(w, "INSERT INTO customers(name) VALUES ('Ada'),('Grace');"));
  db_close(w);

  /* reopen fresh and verify persistence + listing */
  db_conn *c = NULL;
  ASSERT_OK(db_open_sqlite(path, &c));

  db_result *tables = NULL;
  ASSERT_OK(db_list_tables(c, &tables));
  ASSERT_ROWS(tables, 2);
  ASSERT_CELL_EQ(tables, 0, 0, "customers");
  ASSERT_CELL_EQ(tables, 1, 0, "orders");
  db_result_free(tables);

  ASSERT_SQL_SCALAR(c, "SELECT COUNT(*) FROM customers;", "2");
  ASSERT_SQL_SCALAR(c, "SELECT name FROM customers ORDER BY id LIMIT 1;", "Ada");

  db_close(c);
  unlink(path);
}
