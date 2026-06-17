# dbview

A cross-platform desktop tool to **practice SQL**, **convert data between CSV / SQLite / DuckDB**,
and **prepare data for Databricks**.

Native WebKit shell (`webview/webview`) + React/TypeScript UI over a **C23** engine. DuckDB-centric.
macOS (Apple Silicon) first → Linux → Windows.

## Status

Early development — **Phase 0** (project skeleton). See [`docs/`](docs/):

- [`docs/SPEC.md`](docs/SPEC.md) — specification (architecture, modules, contracts, phasing)
- [`docs/AGENT.md`](docs/AGENT.md) — contributor / agent operating guide
- [`docs/DESIGN_QUESTIONNAIRE.md`](docs/DESIGN_QUESTIONNAIRE.md) — design decisions

## Goals

1. Practice SQL queries in a real workbench over SQLite and DuckDB.
2. Convert data easily between CSV ⇄ SQLite ⇄ DuckDB.
3. Prepare typed data (Parquet) for Databricks integration.

## Architecture

```
React/TS UI → window.dbInvoke(method, args) → C on_invoke → db_api_dispatch → engine modules
```

One JSON dispatch surface serves the UI today and a future MCP server. All logic lives in the C
engine; the UI is a thin view.

## Build

CMake + clang (toolchain instructions land with the Phase 0 skeleton).
