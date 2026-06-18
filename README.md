# dbview

A cross-platform desktop tool to **practice SQL**, **convert data between CSV / SQLite / DuckDB /
Parquet**, and **prepare data for Databricks**.

Native WebKit shell (`webview/webview`) + React/TypeScript UI over a **C23** engine. DuckDB-centric.
macOS (Apple Silicon) first → Linux → Windows.

## Status

Working app — **Phases 0–2 complete** (skeleton, query workbench, conversion). See [`docs/`](docs/):

- [`docs/SPEC.md`](docs/SPEC.md) — specification (architecture, modules, contracts, phasing)
- [`docs/ROADMAP.md`](docs/ROADMAP.md) — what's done and what's next
- [`docs/AGENT.md`](docs/AGENT.md) — contributor / agent operating guide
- [`docs/DESIGN_QUESTIONNAIRE.md`](docs/DESIGN_QUESTIONNAIRE.md) — design decisions

## Features

- **Engines:** SQLite + DuckDB behind one interface. Files open read-write, fall back to
  read-only, then to a temp **snapshot** copy if the original is locked (so a DB in use elsewhere
  still opens). Badges show engine / read-only / snapshot.
- **Query workbench:** CodeMirror SQL editor with syntax highlighting, table + column
  autocompletion, format (`⌘⇧F`), copy, clear; run with `⌘⏎`; sortable result grid; query history.
- **Open / New DB:** native Open dialog; **New DB** creates a real on-disk SQLite/DuckDB file via a
  Save dialog.
- **Import / Export:** one-click **Import ▾** (CSV/Parquet → new table) and **Export ▾**
  (result → CSV/Parquet), all via native file dialogs. **Attach/Copy** panel for SQLite⇄DuckDB.
- **Databricks prep:** Parquet export (`COPY … (FORMAT parquet)`); destination folders auto-created.
- **Native niceties:** Edit menu (⌘C/V/X/A/Z), clipboard, light/dark/system theme; editor SQL,
  preferences, and last-opened DB persist across launches.

## Architecture

```
React/TS UI → window.dbInvoke(method, args) → C on_invoke → db_api_dispatch → engine modules
```

One JSON dispatch surface serves the UI today and a future MCP server. All logic lives in the C
engine; the UI is a thin view. Conversions are DuckDB SQL the wizard generates/runs.

## Build (macOS)

```bash
# one-time: vendor native deps (not committed)
./scripts/fetch_webview.sh      # WKWebView wrapper
./scripts/fetch_duckdb.sh       # prebuilt libduckdb (~109 MB)

# configure + build
cmake -S . -B build
cmake --build build --target dbview     # builds the React UI, then the app

# run
./build/dbview [path/to/file.sqlite|.duckdb]
```

Requires CMake ≥ 3.21, clang with C23, Node/npm (for the UI), and the macOS SDK
(WebKit/Cocoa, system SQLite). DuckDB links as a dylib in dev (see `docs/SPEC.md` §9).

## Test

```bash
cmake --build build --target test_runner
ctest --test-dir build --output-on-failure   # or: ./build/test_runner
```

Auto-registering C harness; unit tests live inline (`#ifdef DB_TEST`), integration tests in
`tests/`. Debug builds run under ASan+UBSan.

## License

TBD.
