# dbview — Roadmap

Status of the build and the backlog. ✅ done · ▶ in progress · ☐ planned.

## Shipped

### Phase 0 — skeleton ✅
- CMake + C23 build, ASan+UBSan, `db_error` (X-macro codes, `DB_TRY`/`DB_FAIL`, invariants),
  auto-registering `db_test` harness.
- `webview/webview` shell + React/TS UI + `dbInvoke` ↔ `db_api_dispatch` bridge.

### Phase 1 — query workbench ✅
- SQLite + DuckDB behind one `db_conn`/`db_result` interface.
- Open files read-write → read-only → **snapshot copy** fallback (locked files still open);
  engine / read-only / snapshot badges.
- CodeMirror SQL editor: highlight, **table + column autocomplete**, format (`⌘⇧F`), copy, clear.
- Sortable result grid; query history; `⌘⏎` to run.

### Phase 2 — conversion ✅
- **Import ▾** CSV/Parquet → new table; **Export ▾** result → CSV/Parquet (native dialogs).
- **Attach/Copy** panel: ATTACH a SQLite file, copy a table (DuckDB scanner).
- **Convert to DuckDB/SQLite…** — turn a whole open DB into a new file of the other engine
  (copy each base table + recreate views; skips SQLite-internal `sqlite_*` tables) and switch
  to it; works both directions and round-trips cleanly.
- Conversions are generated DuckDB SQL; export auto-creates the destination folder.

### Cross-cutting shipped
- Native **Open…** dialog; **New DB ▾** creates an on-disk SQLite/DuckDB file (Save dialog).
- Native **Edit menu** (⌘C/V/X/A/Z) + clipboard; light/dark/system theme.
- Persistence: editor SQL, preferences, last-opened DB reopen on launch.
- 38 engine tests green under ASan+UBSan.

## Next (backlog)

### Section B — persistence & connection UX
- ☐ **Recent files / launcher** — MRU list of opened databases.
- ☐ **Save in-memory DB to a file** — promote a scratch DuckDB to disk.
- ☐ **Read-only/snapshot banner** with one-click "open a writable copy" / "reopen live".

### Section C — conversion & Databricks
- ☐ **CSV import options** — delimiter/header/type overrides ("adjust types").
- ☐ **Databricks Volume upload** — push exported Parquet via REST (host + token). *(Phase 3)*

### Section D — exploration depth
- ✅ **Table detail / schema view** — sidebar tables carry a `T`/`V` mark (table vs view);
  clicking one previews its rows and exposes a **Schema** toggle in the result toolbar that
  flips the pane to columns/types/nullable/PK + indexes + row count (`schema.table_detail`).
- ☐ ~~Cancel a long-running query; run-only-selected-text~~ — dropped (not needed).
- ☐ ~~Export a specific table from the sidebar~~ — deferred (too early).

### Section E — bigger / later
- ☐ **Cell editing & write UI** (INSERT/UPDATE/DELETE).
- ☐ **Packaging** — macOS `.app` (icon, Info.plist, bundled dylibs via `@loader_path`), `.dmg`,
  notarize. *(Phase 4)*
- ☐ **Server connections** (Postgres/MySQL/Databricks SQL) via the reserved connection abstraction.
- ☐ **MCP server** exposing `db_api_dispatch`.
- ☐ **Release hardening** — DuckDB static link, SQLite amalgamation, Windows test-registration shim.

### Possible UX refinements
- ☐ Auto-format-on-Run already shipped as a toggle; consider per-statement formatting.
- ☐ Optional Vitest/RTL for the UI once it stabilizes (Q11 deferred).
