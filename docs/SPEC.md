# dbview — Specification (v0.1, draft)

> A cross-platform desktop tool to **practice SQL**, **convert data between CSV / SQLite /
> DuckDB**, and **prepare data for Databricks**. Native WebKit shell + React/TS UI over a C23
> engine. macOS (Apple Silicon) first → Linux → Windows.
>
> Status: Phases 0–2 implemented (see §10 and `ROADMAP.md`). Decisions trace back to
> `DESIGN_QUESTIONNAIRE.md`. Dual-audience (human + AI agent): rule first, then reason.

---

## 1. Goals & non-goals

### Goals (the "why")
1. **Practice SQL** — a real query workbench over SQLite and DuckDB.
2. **Convert data** easily between CSV ⇄ SQLite ⇄ DuckDB.
3. **Prepare data for Databricks** — export typed Parquet (later: push to a Databricks Volume).

### Non-goals (v1)
- No server connections yet (Postgres/MySQL/Databricks SQL) — but the connection abstraction is
  designed so they slot in later (§4.4).
- No in-app AI assistant yet — but the dispatch surface is MCP-ready (§6.3).
- No spreadsheet-style cell editing yet — v1 is run-any-SQL (§5.5 editing scope).
- No dark mode concerns inherited from other projects; this is a fresh codebase.

---

## 2. Architecture overview

```
┌─────────────────────────────────────────────┐
│  React + TypeScript UI  (Vite → single HTML) │   ui/
│  dbInvoke<T>(method, args) ──────────────┐   │
└──────────────────────────────────────────┼───┘
                       window.dbInvoke      │  JSON: ["method","argsJson"]
┌──────────────────────────────────────────▼───┐
│  Native shell (webview/webview, WKWebView)    │   src/app/main.c
│  on_invoke → db_api_dispatch  (+ worker pool) │
└──────────────────────────────────────────┬───┘
                       method + jsonArgs    │  JSON result / {error:{code,message}}
┌──────────────────────────────────────────▼───┐
│  C23 engine  (one dispatch surface)           │   src/api/
│  ┌─────────┬──────────┬─────────┬──────────┐  │
│  │ conn    │ query    │ convert │ export   │  │   src/<module>/
│  ├─────────┴──────────┴─────────┴──────────┤  │
│  │ engines: sqlite | duckdb (central)       │  │   src/engine/
│  ├──────────────────────────────────────────┤ │
│  │ support: db_error, db_test, db_json, …    │  │   src/support/
│  └──────────────────────────────────────────┘ │
└────────────────────────────────────────────────┘
```

**Key principle (from money_books):** one JSON **dispatch surface** (`db_api_dispatch`) is the
single contract spoken by the UI bridge today and a future MCP server. The UI is a thin view; all
logic lives in the C engine.

### 2.1 The bridge
- JS calls `window.dbInvoke(method, argsJson)`; the shell's `on_invoke` parses `["method","args"]`,
  dispatches, returns a JSON string (result object, or `{"error":{"code","message"}}`).
- **Slow calls run off the UI thread** (a worker thread + `webview_dispatch` back to main), so the
  window stays responsive during long imports/queries. (money_books does this for `agent.send`; we
  do it for `query.run`, `convert.run`, `export.run`.)
- **Dev vs bundled UI:** `DBVIEW_UI_URL=http://localhost:5173` loads the Vite dev server; unset
  loads `file://…/ui/dist/index.html`. WKWebView over `file://` blocks sibling module files, so the
  UI is inlined into one `index.html` via `vite-plugin-singlefile`.

---

## 3. Engine topology — DuckDB-centric

DuckDB is the always-on query/conversion engine because it natively:
- reads CSV (with type inference),
- attaches & scans SQLite files (`sqlite_scanner` / `ATTACH … (TYPE sqlite)`),
- writes Parquet (`COPY … TO 'x.parquet' (FORMAT parquet)`), the Databricks on-ramp.

`libsqlite3` is used for **native SQLite browsing/editing** (and as a fallback read path). This
collapses all three goals onto one SQL surface and minimizes glue code.

| Concern | Engine |
|---|---|
| Run SQL in the workbench | DuckDB (default), SQLite (when targeting a native .sqlite session) |
| Read CSV | DuckDB `read_csv_auto` |
| Read SQLite tables | DuckDB `ATTACH` scanner, or libsqlite3 directly |
| Convert / `CREATE TABLE AS` | DuckDB |
| Export Parquet/CSV | DuckDB `COPY` |
| Native SQLite write/edit | libsqlite3 |

---

## 4. Modules (C engine)

Layout mirrors money_books (`src/<module>/{module.h,module.c}`; unit tests inline under
`#ifdef DB_TEST`). Naming prefix: `db_`.

### 4.1 `support/` — foundation
- `db_error.{h,c}` — error model (§7). X-macro registry, last-error, `DB_TRY`/`DB_FAIL`,
  invariants, located logging.
- `db_test.h` + `src/test/` — auto-registering test harness (§8).
- `db_json.{h,c}` — cJSON wrapper + helpers (`json_take`, `sget`, error-envelope builder).
- `db_id.{h,c}` — stable ids for connections/queries if needed.

### 4.2 `engine/` — database backends
- `engine.h` — a thin backend interface: `open`, `close`, `exec`, `query` → `db_result`,
  `last_error`. Two implementations: `engine_sqlite.c`, `engine_duckdb.c`.
- `db_result` — column metadata (name, type) + row buffer + accessors; the unit returned to the
  API/UI as JSON `{columns:[…], rows:[[…]]}` with truncation/paging metadata.

### 4.3 `query/` — SQL workbench
- Run one or many statements on the active connection; return results + timing + affected rows.
- Capture raw engine error text (for display) and map to a `db_err` code.
- Query **history** (auto-saved per connection).

### 4.4 `conn/` — connection & workspace model
- A **connection abstraction** with a `kind`: `file_sqlite`, `file_duckdb`, `csv_dir`, and a
  reserved `network_*` (Postgres/MySQL/Databricks SQL) for later — implemented only for file kinds
  in v1, but the seam exists now.
- **Recent-files registry** (JSON on disk, money_books' `registry` pattern): MRU list of opened
  sources; reopen most-recent on launch; launcher screen when none.

### 4.5 `convert/` — CSV ⇄ SQLite ⇄ DuckDB
- Source (CSV file / SQLite table / DuckDB table) → optional type adjustments → destination (new
  table or file).
- **Wizard generates editable SQL** (`CREATE TABLE … AS SELECT …` / `COPY`), shown to the user and
  runnable — teaches SQL while converting (goal #1 + #2).
- **All-or-nothing invariant:** a failed conversion leaves no partial target (transactional / temp-
  then-rename).

### 4.6 `export/` — Databricks prep
- Parquet export via DuckDB `COPY` with type controls.
- Reserved: `databricks_volume` push (REST upload, host+token) — a later phase.

### 4.7 `api/` — dispatch surface
- `db_api_dispatch(conn, method, argsJson, &resultJson)` — single entry; `app.*` shell methods
  (connection registry) handled in `main.c`, everything else in the engine.

---

## 5. UI (React + TypeScript)

- **Stack:** Vite + React 18 + TypeScript; `vite-plugin-singlefile` for the inlined bundle.
- **Bridge wrapper:** `dbInvoke<T>(method, args): Promise<T>` — recognizes the error envelope and
  throws a typed `DbError { code, message }`.
- **v1 screens:**
  - **Launcher** — recent sources + "open file" + "import CSV".
  - **Workbench** — Monaco SQL editor · result grid (sort/paginate/copy) · schema sidebar
    (click-to-insert) · query history · export-from-grid.
  - **Convert wizard** — source → types → destination, with the generated SQL preview.
- **Visible-slice rule (Q11):** every phase ships a minimal but real UI for the feature it adds, so
  progress is observable, even though automated tests are engine-only in v1.

---

## 6. Cross-cutting contracts

### 6.1 Result JSON shape
```json
{ "columns": [{"name":"id","type":"BIGINT"}],
  "rows": [[1],[2]],
  "row_count": 2, "truncated": false, "elapsed_ms": 3 }
```

### 6.2 Error envelope
```json
{ "error": { "code": "DB_ERR_SQL", "message": "Parser Error: syntax error at or near \"selct\"" } }
```

### 6.3 MCP-readiness
The dispatch surface is method+JSON in/out with no UI assumptions, so an MCP server (stdio) can be
added later that reuses `db_api_dispatch` verbatim — no engine changes. No AI ships in v1.

---

## 7. Error handling  (adopted from money_books, renamed `db_`)

**Principle: AI-first diagnostics — every failure is greppable, located, and explained.**

- **X-macro registry** `DB_ERR_LIST(X)` → `db_err` enum + `db_err_name()` + `db_err_default_msg()`.
  Proposed codes: `DB_OK`, `DB_ERR_INVALID_ARG`, `DB_ERR_NOT_FOUND`, `DB_ERR_IO`, `DB_ERR_SQLITE`,
  `DB_ERR_DUCKDB`, `DB_ERR_SQL`, `DB_ERR_PARSE`, `DB_ERR_TYPE`, `DB_ERR_UNSUPPORTED`,
  `DB_ERR_CONFLICT`, `DB_ERR_OOM`, `DB_ERR_INTERNAL`.
- **Return codes + thread-local last-error** `{code, message, file, line, func, trace}`.
- **`DB_TRY(expr)`** propagates like `?` and appends a breadcrumb; **`DB_FAIL(code,…)`** sets+returns.
- **`DB_MUST_CHECK`** (`warn_unused_result`) on every fallible function — ignored errors won't compile.
- **Three tiers:** runtime → `db_err`; programmer bug → `DB_DEBUG_ASSERT` (off under `NDEBUG`);
  data-integrity → `DB_INVARIANT` (always on, aborts — "crash > corrupt data").
- **Engine errors are wrapped, not leaked:** `sqlite3_errmsg` / `duckdb_result_error` captured into
  `message`, mapped to a code; the **raw text is preserved** for the workbench (users need it).
- **Located leveled logging** `DB_LOGD/I/W/E`.

---

## 8. Testing  (adopted from money_books, renamed `db_`)

- **Auto-registering harness:** `TEST(suite,name){…}` self-registers (constructor); one
  `test_runner`; `--filter/--json/--list`; signal handler names the crashing test; prints a
  copy-paste re-run command per failure.
- **Unit tests inline** under `#ifdef DB_TEST` in each `.c`; **integration tests in `tests/`** drive
  the public API across modules.
- **Domain assertions:** `ASSERT_OK`, `ASSERT_ERR(expr,code)`, `ASSERT_STR_EQ`, `ASSERT_EQ_INT`,
  plus dbview's `ASSERT_ROWS(result,n)`, `ASSERT_CELL_EQ(result,r,c,"…")`,
  `ASSERT_SQL_SCALAR(conn,"SELECT …",expected)`.
- **In-memory DBs** (`:memory:` / DuckDB in-memory) for speed & isolation.
- **Round-trip conversion tests are first-class:** CSV→SQLite→DuckDB→Parquet→back asserts rows,
  types, values survive each hop.
- **Sanitizers + tooling as CMake/CTest targets:** `ctest` (ASan+UBSan in Debug), `leaks`,
  `analyze`, `deadcode`.
- **Philosophy (binding):** assert independently-derived values, never magic numbers from output;
  **never adjust a test to make it pass** — a weak test is a real-bug hypothesis; negative tests must
  exercise the negative path; periodically audit "would this test catch a bug here?"
- **Standing rule:** *test-after-each-functionality* — a unit is done only when its tests are written
  and green; the suite re-runs after each unit (per-phase definition of done).

---

## 9. Build & toolchain (CMake)

- **C23** engine (`-std=c2x`/`c23`), clang on macOS. High-signal warnings as errors
  (`-Wall -Wextra -Wpedantic -Werror -Wshadow -Wstrict-prototypes …`), conversion warnings advisory.
- **Targets:** `dbview` (app), `dbview_engine` (static lib), `test_runner` (via CTest), `leaks`,
  `analyze`, `deadcode`, `ui` (npm build).
- **Deps:** `libsqlite3` (system now, vendor amalgamation before release), `libduckdb`
  (dylib in dev, static for distributables — `DBVIEW_DUCKDB_STATIC` toggle), `webview/webview`
  (vendored via `scripts/fetch_webview.sh`), `cJSON` (vendored).
- **Sanitizers** behind `DBVIEW_SANITIZE` (default ON in Debug).
- **macOS link:** `-framework WebKit -framework Cocoa` (+ DuckDB/SQLite libs).

### Cross-platform caveats (design now, solve in the OS phase)
- `__attribute__((constructor))` test auto-registration and the `SIG*` crash handler are
  clang/gcc + POSIX. Windows/MSVC will need a linker-section registration shim + SEH.
- `webview/webview` abstracts WKWebView / WebKitGTK / WebView2; packaging differs per OS.

---

## 10. Phasing  (definition of done = feature + green tests + visible UI slice)

| Phase | Deliverable | Tests added |
|---|---|---|
| **0 — skeleton** ✅ | CMake + C23 engine, `db_error`/`db_test`, webview, React/TS shell, `dbInvoke` bridge; open a SQLite file & list tables. | error/test harness; open+list tables. |
| **1 — workbench** ✅ | SQLite + DuckDB engines; CodeMirror editor (table+column autocomplete) + sortable result grid + schema sidebar + query history. | query run; result JSON shape; error mapping; list tables/columns; read-only/snapshot fallback. |
| **2 — convert** ✅ | CSV/Parquet import; Parquet/CSV export; SQLite⇄DuckDB attach/copy; conversions are generated DuckDB SQL. | round-trips CSV→table→Parquet and SQLite ATTACH→copy; SQL-builder quoting; export mkdir. |
| **3 — Databricks prep** ▶ | Parquet export done ✅; remaining: type controls, push to a Databricks Volume (REST). | Parquet round-trip (export→re-read). |
| **4 — polish/packaging** | macOS `.app`; (later) MCP server, server connections, cell editing. | per-feature as added. |

**Beyond the phase table (also shipped):** native Open/Save dialogs; **New DB** creates an on-disk
SQLite/DuckDB file; unified **Import ▾ / Export ▾** buttons; native Edit menu (⌘C/V/X/A/Z) +
clipboard; SQL formatting; light/dark/system theme; persistence of editor SQL, prefs, and
last-opened DB. Engine test count: **38 green** under ASan+UBSan. Editor is **CodeMirror 6**, not
Monaco (Monaco's worker/CDN model breaks single-file `file://` loading; CodeMirror bundles cleanly).

---

## 11. Open items to revisit
- DuckDB acquisition: **decided** — vendor the prebuilt `libduckdb` (dylib) via
  `scripts/fetch_duckdb.sh`, pinned to v1.5.4; static link is a release task.
- SQLite amalgamation vendoring before first distributable (currently system `libsqlite3`).
- Linux/Windows packaging formats (AppImage/.deb, MSI/portable) — decide at Phase 4.
- Windows: `__attribute__((constructor))` test registration + `SIG*` crash handler need a
  linker-section/SEH shim (clang/gcc + POSIX today).
- Whether to add Vitest/RTL once the UI stabilizes (Q11 deferred).
