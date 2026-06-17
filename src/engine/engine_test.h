#ifndef DB_ENGINE_TEST_H
#define DB_ENGINE_TEST_H
/*
 * dbview — result-oriented test assertions (compiled only under DB_TEST).
 *
 * Kept separate from db_test.h so the support layer stays engine-agnostic: only
 * engine tests pull in db_result knowledge.
 */
#include "engine.h"
#include "../support/db_test.h"

#include <stdio.h>
#include <string.h>

#define ASSERT_COLS(res, n) ASSERT_EQ_INT((res)->n_cols, (n))
#define ASSERT_ROWS(res, n) ASSERT_EQ_INT((res)->n_rows, (n))

#define ASSERT_CELL_EQ(res, r, c, want) \
  do { const char *_v = db_result_value((res), (r), (c)); \
       if (strcmp(_v ? _v : "(null)", (want)) != 0) { db_test_fail(t, __FILE__, __LINE__, \
           "ASSERT_CELL_EQ(%s[%d][%d]): \"%s\" != \"%s\"", #res, (r), (c), \
           _v ? _v : "(null)", (want)); return; } } while (0)

/* Run a query expected to yield exactly one cell; compare its text. Frees the result
 * before any early return so failures don't leak under ASan. */
#define ASSERT_SQL_SCALAR(conn, sql, want) \
  do { db_result *_r = NULL; \
       ASSERT_OK(db_query((conn), (sql), &_r)); \
       int _rows = _r->n_rows, _cols = _r->n_cols, _ok = (_rows == 1 && _cols == 1); \
       char _buf[256]; _buf[0] = '\0'; \
       if (_ok) { const char *_v = db_result_value(_r, 0, 0); \
                  snprintf(_buf, sizeof _buf, "%s", _v ? _v : "(null)"); } \
       db_result_free(_r); \
       if (!_ok) { db_test_fail(t, __FILE__, __LINE__, \
           "ASSERT_SQL_SCALAR(%s): expected 1x1, got %dx%d", #sql, _rows, _cols); return; } \
       if (strcmp(_buf, (want)) != 0) { db_test_fail(t, __FILE__, __LINE__, \
           "ASSERT_SQL_SCALAR(%s): \"%s\" != \"%s\"", #sql, _buf, (want)); return; } \
  } while (0)

#endif /* DB_ENGINE_TEST_H */
