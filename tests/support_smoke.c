/*
 * Integration tests live in a separate folder (Rust-style) and exercise the
 * public API across modules the way a real flow would. For Phase 0 the only public
 * surface is the support layer; as the engine grows these will open in-memory
 * databases, run queries, and assert result sets.
 */
#include "../src/support/db_error.h"
#include "../src/support/db_test.h"

#include <string.h>

/* Simulate a small "operation" that validates input and reports a located error,
 * exactly as engine ops will: return a db_err, set the thread-local last-error. */
static db_err open_pretend(const char *path) {
  if (!path || !path[0]) return DB_FAIL(DB_ERR_INVALID_ARG, "path required");
  if (strcmp(path, "missing.db") == 0) return DB_FAIL(DB_ERR_NOT_FOUND, "no such file: %s", path);
  return DB_OK;
}

TEST(integration, error_contract_round_trip) {
  db_clear_error();
  ASSERT_OK(open_pretend("ok.db"));

  ASSERT_ERR_EQ(open_pretend(""), DB_ERR_INVALID_ARG);
  ASSERT_STR_EQ(db_last_error()->message, "path required");

  ASSERT_ERR_EQ(open_pretend("missing.db"), DB_ERR_NOT_FOUND);
  ASSERT_STR_EQ(db_last_error()->message, "no such file: missing.db");
  ASSERT_STR_EQ(db_err_name(db_last_error()->code), "DB_ERR_NOT_FOUND");
}
