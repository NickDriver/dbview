# dbview — Agent / contributor guide

> Lean operating guide for anyone (human or AI) working in this repo. Detail lives in
> `SPEC.md`; decisions in `DESIGN_QUESTIONNAIRE.md`. Rule first, then reason.

## What this is
Cross-platform desktop tool to **practice SQL**, **convert CSV/SQLite/DuckDB**, and **prep data for
Databricks**. C23 engine + WebKit (`webview/webview`) shell + React/TS UI. DuckDB-centric. macOS
(Apple Silicon) first.

## Architecture in one line
React UI → `window.dbInvoke(method,args)` → C `on_invoke` → `db_api_dispatch` → engine modules.
One JSON dispatch surface for UI today and a future MCP server. All logic in the C engine.

## The standing workflow rule (owner directive)
**Test-after-each-functionality.** A unit of functionality is *done* only when:
1. its code is written,
2. its tests are written (unit inline `#ifdef DB_TEST`, or integration in `tests/`), **and**
3. the suite **runs green** (`ctest` / `test_runner`).

Re-run the suite after each unit to confirm we're still on track. Do **not** batch tests to the end
of a phase. Each phase also ships a **minimal visible UI slice** so progress is observable.

## Testing philosophy (binding)
- Assert **independently-derived** expected values; never copy magic numbers from program output.
- **Never adjust a test to make it pass.** If a test is weak because the code might be wrong, that's
  a real-bug hypothesis — chase the code.
- Negative tests must actually exercise the negative path.
- Periodically ask: "if the code had a bug here, would this test catch it?"

## Error handling (see SPEC §7)
- Runtime failures → return `db_err` + rich last-error (`DB_FAIL` / `DB_TRY`).
- Programmer bugs → `DB_DEBUG_ASSERT` (off under `NDEBUG`).
- Data-integrity → `DB_INVARIANT` (always on, aborts — crash beats corrupt data).
- `DB_MUST_CHECK` everywhere; ignored errors must not compile.
- Wrap engine (SQLite/DuckDB) errors into `db_err` but preserve raw text for the workbench.

## Conventions
- Naming prefix `db_`. Module layout `src/<module>/{module.h,module.c}`, unit tests inline.
- Options-object style for 2+ params. English for codes/DB fields/API; UI strings localizable later.
- `workspaceFetch`-style: UI never calls the engine except through the typed `dbInvoke<T>()` wrapper.
- Build: CMake. `ctest` (ASan+UBSan in Debug), plus `leaks` / `analyze` / `deadcode` targets.

## Build / run (filled in as Phase 0 lands)
- `scripts/fetch_webview.sh` — vendor the webview library once.
- `scripts/fetch_duckdb.sh` — vendor the DuckDB C library once (~109M, gitignored).
- `cmake -B build && cmake --build build` — engine + app.
- `ctest --test-dir build` — run tests.
- `cd ui && npm install && npm run build` — build the UI bundle.
- `DBVIEW_UI_URL=http://localhost:5173 ./build/dbview` — dev against the Vite server.

## Git
New, standalone repo (not related to kompot). Conventional commits. No AI/Claude mentions in
commits, code, or docs. Commit/push only when explicitly asked.
