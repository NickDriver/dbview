# dbview — Design Questionnaire

> Purpose: lock the design decisions before we write the SPEC and start prototyping.
> Format: each open question lists options + a **Recommendation**. Answer inline (✅) or
> override. Decisions already made are in Part A.
>
> Audience: human (you) + AI agent. State the rule, then the reason.

---

## Part A — Decisions already locked

| Topic | Decision |
|---|---|
| **Language** | C23 (engine). TypeScript + React (UI). |
| **Platform order** | macOS (Apple Silicon) first → Linux → Windows. |
| **Interface** | Native desktop GUI via **WebKit/WebView** — `webview/webview` C wrapper hosting a React UI (pattern proven in `money_books`). |
| **DB access** | Official C libraries: `libsqlite3` + `libduckdb`. No custom on-disk parsers. |
| **Goal** | A polished, distributable **product**. |
| **Build** | **CMake** (cross-platform, packaging, IDE integration). |
| **Project path** | `~/work/dbview` (fresh git repo). |

### Core use cases (the "why")
1. **Practice SQL queries** — a real query workbench.
2. **Convert data** between CSV ⇄ SQLite ⇄ DuckDB easily.
3. **Prepare data for Databricks** integration.

### Architecture borrowed from `money_books`
- Modular C engine under `src/<module>/`, one JSON **dispatch surface** (`method + jsonArgs → jsonResult`).
- React UI built with Vite + `vite-plugin-singlefile` → one inlined `index.html` loaded over `file://` (WKWebView blocks sibling module files over `file://`).
- UI ↔ engine bridge = `window.dbInvoke(method, args)` → C `on_invoke` → dispatch. Slow calls run off the UI thread (worker + `webview_dispatch` back to main).
- cJSON for JSON marshalling. Sanitizers in debug, `leaks` on arm64.

---

## Part B — Open design questions

### 1. Engine topology — is DuckDB the center of gravity?
**Why it matters:** it shapes every other module. DuckDB natively reads CSV, can `ATTACH`
and scan SQLite databases, and writes Parquet — which is the Databricks on-ramp. SQLite's own
C API is mostly needed for *native* SQLite editing/browsing.

- **(a) DuckDB-centric (recommended).** DuckDB is the always-on query engine. It reads CSV,
  attaches SQLite files (`sqlite_scanner`), and exports Parquet/CSV. SQLite's `libsqlite3` is
  used only for native SQLite browse/edit. One SQL surface for the workbench.
- **(b) Dual peers.** SQLite and DuckDB are equal engines; the user picks per-connection. More
  symmetric, but two query paths to build and maintain.
- **(c) SQLite-first.** Ship SQLite browsing now, add DuckDB later.

> **Recommendation: (a).** It collapses all three goals (SQL practice, conversion, Databricks)
> onto one engine and minimizes glue code.

**Answer:** _____

---

### 2. DuckDB packaging / linking
**Why it matters:** `libduckdb` is large (~50 MB+) and ships as C++ with a C API; affects binary
size, build, and cross-platform packaging.

- **(a) Bundle statically.** Biggest binary, zero runtime deps, simplest UX. Good for a product.
- **(b) Bundle the shared lib** (`libduckdb.dylib/.so/.dll`) next to the app. Smaller link, easy
  to update DuckDB independently.
- **(c) Load dynamically at runtime (optional).** App runs SQLite-only if DuckDB is absent.

> **Recommendation: (b) for dev, (a) for release.** Ship the dylib during prototyping (fast
> iteration), switch to static for distributable builds. CMake handles both via a toggle.

**Answer:** _____

---

### 3. Databricks "prep" — what exactly do we output?
**Why it matters:** defines the export module and the data-typing work.

- **(a) Parquet files (recommended baseline).** DuckDB `COPY ... TO 'x.parquet'`. Databricks
  ingests Parquet directly; preserves types; columnar. Lowest-friction.
- **(b) Delta Lake tables.** Native Databricks format. DuckDB has a `delta` reader; *writing*
  Delta from C is heavier (needs delta-rs/extension). More work, more native.
- **(c) Unity Catalog / Volumes upload.** Push files to a Databricks workspace via REST API
  (requires host + token, network). A later phase.
- **(d) Clean typed CSV only.** Simplest, but loses types; weakest fit for Databricks.

> **Recommendation: (a) now, (c) later.** Parquet export covers 90% of the on-ramp; add a
> "Push to Databricks Volume" action once the local pipeline is solid.

**Answer:** _____

---

### 4. Conversion UX — wizard or SQL-driven?
**Why it matters:** defines the most-used screen.

- **(a) Guided wizard.** Pick source (CSV/SQLite table/DuckDB table) → preview & adjust column
  types → pick destination (table or file) → run. Approachable.
- **(b) SQL-driven.** Expose DuckDB `COPY`/`CREATE TABLE AS` directly; power-user, less UI.
- **(c) Both — wizard generates the SQL, which is shown and editable (recommended).** Teaches SQL
  (goal #1) while staying easy. The wizard is a SQL generator with a preview.

> **Recommendation: (c).** Directly serves "practice SQL" + "convert easily" together.

**Answer:** _____

---

### 5. Editing scope — read-only or read/write?
**Why it matters:** safety, complexity, and the data-grid component.

- **(a) Read + query only.** Browse tables, run SELECTs, export. Safest first prototype.
- **(b) Read + query + DDL/DML via SQL.** Run any statement; no cell-level editing UI.
- **(c) Full editing.** Inline cell edits, add/drop columns, etc. Most product work.

> **Recommendation: start (b), grow to (c).** Run-any-SQL is cheap and unblocks real work;
> spreadsheet-style cell editing is a later polish phase.

**Answer:** _____

---

### 6. Connection / workspace model
**Why it matters:** mirrors money_books' "book registry" (recent files); defines app state.

- **(a) Recent-files registry (recommended).** Track recently opened SQLite/DuckDB files + CSV
  folders; reopen MRU on launch; a launcher screen when none. Direct analog of money_books.
- **(b) Workspace projects.** Group multiple files/queries/exports under a named project. Richer,
  more to build.
- **(c) Single-file-at-a-time.** Open one DB, no history. Minimal.

> **Recommendation: (a).** Reuse the proven registry pattern; revisit (b) if multi-source
> workflows demand it.

**Answer:** _____

---

### 7. Query workbench features (for "practice SQL")
**Why it matters:** this is goal #1; pick the v1 feature set.

Check what's in scope for the first prototype (multi-select):
- [ ] Multi-statement editor with syntax highlighting (CodeMirror/Monaco)
- [ ] Result grid with sort, paginate, copy
- [ ] Query history (auto-saved)
- [ ] Saved/named queries (snippets)
- [ ] `EXPLAIN` / query plan view
- [ ] Schema sidebar (tables, columns, types) with click-to-insert
- [ ] Export result set (CSV/Parquet) from the grid
- [ ] Inline SQL error highlighting

> **Recommendation v1:** editor (Monaco) + result grid + schema sidebar + query history +
> export-from-grid. Defer EXPLAIN view, saved snippets to v2.

**Answer:** _____

---

### 8. AI / MCP layer — in or out for v1?
**Why it matters:** money_books exposes the *same* dispatch surface to an MCP server + an in-app
agent. For a SQL-practice tool, a "natural-language → SQL" assistant is a strong fit, but it adds
LLM plumbing.

- **(a) Design the dispatch surface to be MCP-ready, but ship no AI in v1 (recommended).** Keep
  the contract clean so an MCP server / agent can be added later with zero engine changes.
- **(b) Include an in-app NL→SQL assistant in v1.** Higher value for SQL practice, more work +
  API-key/secret handling.
- **(c) No AI consideration at all.**

> **Recommendation: (a).** Same low-cost insurance money_books took; revisit (b) after the core
> workbench lands.

**Answer:** _____

---

### 9. Server connections (stated as a later phase) — reserve the abstraction now?
**Why it matters:** you said "later we will add capabilities to connect to DB servers."

- **(a) Define a `connection` abstraction now** (file-backed vs network-backed) so Postgres/MySQL
  /Databricks SQL can slot in later, but implement only file connections in v1. *(recommended)*
- **(b) Hard-code file DBs now; refactor when servers arrive.**

> **Recommendation: (a).** Cheap to design the seam now; expensive to retrofit.

**Answer:** _____

---

### 10. Distribution / packaging targets
**Why it matters:** "product" implies shippable artifacts; informs CMake packaging early.

- macOS: signed/notarized `.app` bundle + `.dmg`?
- Linux: AppImage / `.deb` / Flatpak?
- Windows: MSI / portable `.exe`?

> **Recommendation:** target a macOS `.app` first (unsigned dev build → notarized later), keep
> CMake packaging hooks generic. Decide Linux/Windows formats when we get there.

**Answer:** _____

---

## Part C — Proposed phasing (for confirmation)

1. **Phase 0** — skeleton: CMake + C23 engine stub, `webview` vendored, React/TS UI shell, the
   `dbInvoke` bridge, open a SQLite file and list tables. *(proves the stack end-to-end)*
2. **Phase 1** — query workbench: DuckDB engine, editor + result grid + schema sidebar + history.
3. **Phase 2** — conversion: CSV import, SQLite⇄DuckDB, wizard-generates-SQL.
4. **Phase 3** — Databricks prep: Parquet export, typing controls.
5. **Phase 4** — polish + packaging; (optional) MCP/AI; (later) server connections.

**Phasing OK?** _____

---

## Part D — Testing & error handling (adopted from money_books)

money_books has a deliberate, mature approach here. **Recommendation: adopt it almost verbatim,
renaming the `mb_` prefix → `db_`.** Below is what we inherit, the dbview-specific deltas, and a
few decisions to confirm.

### D.1 Error handling — what we inherit (`src/support/db_error.h/.c`)
The guiding principle is **AI-first diagnostics: every failure is greppable, located, and
explained**, so an LLM or human can diagnose without a debugger.

- **X-macro error registry.** A single `DB_ERR_LIST(X)` table generates the `db_err` enum,
  `db_err_name()` ("DB_ERR_NOT_FOUND"), and `db_err_default_msg()`. Add a code in one place.
- **Return codes + rich last-error.** Runtime/expected failures return a `db_err`; a thread-local
  `db_error_ctx` carries `{code, message, file, line, func, trace}`.
- **`?`-style propagation.** `DB_TRY(expr)` returns on error *and appends a breadcrumb* to the
  trace (so you get a `file:line func [expr] <- …` chain). `DB_FAIL(code, "fmt", …)` sets+returns.
- **Ignored errors are a compile error.** `DB_MUST_CHECK` (`warn_unused_result`) on every
  fallible function.
- **Three failure tiers, on purpose:**
  | Tier | Mechanism | When |
  |---|---|---|
  | Expected/runtime | return `db_err` + last-error | bad SQL, file not found, type mismatch, I/O |
  | Programmer bug | `DB_DEBUG_ASSERT` (compiled out under `NDEBUG`) | precondition a caller must meet |
  | Data-integrity violation | `DB_INVARIANT` (**always on**, aborts) | "crash > corrupt data" guards |
- **Portability shims** (`DB_PRINTF`, `DB_NORETURN`, `DB_LIKELY`…) so non-clang/gcc still compile.
- **Located leveled logging** — `DB_LOGD/I/W/E(…)` print `[LEVEL] file:line: …` to stderr.
- **One JSON error envelope** at the bridge: failures serialize to
  `{"error":{"code":"DB_ERR_…","message":"…"}}` — the same contract the UI and a future MCP layer
  both consume.

### D.2 Error handling — dbview deltas
- **Error codes tuned for a data tool.** Proposed `DB_ERR_LIST`: `DB_OK`, `DB_ERR_INVALID_ARG`,
  `DB_ERR_NOT_FOUND`, `DB_ERR_IO`, `DB_ERR_SQLITE`, `DB_ERR_DUCKDB`, `DB_ERR_SQL` (query rejected
  by engine), `DB_ERR_PARSE` (CSV/type inference), `DB_ERR_TYPE` (conversion/coercion),
  `DB_ERR_UNSUPPORTED`, `DB_ERR_CONFLICT`, `DB_ERR_OOM`, `DB_ERR_INTERNAL`.
- **Wrap engine errors, don't leak them.** SQLite (`sqlite3_errmsg`) and DuckDB
  (`duckdb_result_error`) messages get captured into the last-error `message`, mapped to a
  `db_err` code. The raw engine string is preserved for the SQL workbench (users need to see it).
- **Invariants for our domain:** e.g. a conversion is **all-or-nothing** (no half-written target
  table/file on failure); result column count matches the declared schema; an open connection
  handle is non-NULL before dispatch.
- **Typed bridge on the TS side.** A thin `dbInvoke<T>(method, args)` wrapper that recognizes the
  `{error:{code,message}}` envelope and throws a typed `DbError { code, message }`, so the React
  UI never string-matches error text.

### D.3 Testing — what we inherit (`src/support/db_test.h`, `src/test/`, `tests/`)
- **Auto-registering, zero-boilerplate harness.** `TEST(suite, name){ … }` self-registers via a
  `__attribute__((constructor))` — no manual test list to maintain. One `test_runner` binary.
- **Unit tests live next to the code**, inside `#ifdef DB_TEST … #endif` at the bottom of each
  `.c`. **Integration tests live in `tests/`** (Rust-style) and drive the public API across
  modules.
- **AI-first runner.** Assertions print `file:line` + the **actual** values; a signal handler
  names the crashing test (SIGSEGV/SIGABRT/SIGBUS); supports `--filter`, `--json`, `--list`; on
  failure it prints a copy-paste command to re-run that one test.
- **Assertion vocabulary.** `EXPECT_*` (record, continue) vs `ASSERT_*` (record, stop), plus
  domain helpers: `ASSERT_OK(expr)`, `ASSERT_ERR(expr, code)`, `ASSERT_EQ_INT`, `ASSERT_STR_EQ`.
- **Fast & isolated.** Tests run against in-memory databases (SQLite `:memory:`, DuckDB
  in-memory) — no temp files, no shared state.
- **Sanitizers + tooling as build targets:** ASan+UBSan in debug/test; a separate no-ASan
  `leaks` run (Apple `leaks` is weak under ASan on arm64); clang static analyzer; a coverage-based
  dead-code report (0-hit functions = candidates).
- **Testing *philosophy* (from `TEST_AUDIT.md`) — this is the important part:**
  - Assert **independently-derived** expected values, never magic numbers copied from output.
  - **Never "adjust the test to make it pass."** If a test looks weak because the *code* might be
    wrong, that's a real-bug hypothesis to chase in the code.
  - **Negative tests must actually exercise the negative path** (a "rejects bad input" test that
    only ever passes good input is a defect).
  - Periodically audit the suite asking "if the code had a bug here, would this test catch it?"

### D.4 Testing — dbview deltas
- **Domain assertions for a data tool:** add `ASSERT_ROWS(result, n)`, `ASSERT_CELL_EQ(result,
  row, col, "…")`, and `ASSERT_SQL_SCALAR(conn, "SELECT …", expected)` so query/result correctness
  reads cleanly.
- **Round-trip conversion tests are first-class:** CSV→SQLite→DuckDB→Parquet→back, asserting row
  counts, types, and values survive every hop (directly guards goals #2/#3).
- **CMake, not Make.** Replicate money_books' targets as CMake/CTest: `ctest` (= test), plus
  custom targets `leaks`, `analyze`, `deadcode`. Sanitizers behind a `DBVIEW_SANITIZE=ON` option
  (default ON for Debug).
- **Cross-platform caveats to note now (Mac-first, but design for later):**
  - `__attribute__((constructor))` auto-registration and the `SIG*` crash handler are clang/gcc +
    POSIX. The Windows/MSVC phase will need a registration shim (linker section) and SEH — flag in
    the SPEC, don't solve yet.
- **UI testing decision (see Q11 below).**

### Decisions to confirm

**Q11 — Do we test the React/TS UI in v1, or engine-only?**
- (a) **Engine-only C tests for v1** (recommended). money_books shipped this way; the engine holds
  all logic, the UI is a thin view. Add Vitest/RTL later.
- (b) **Add Vitest + React Testing Library now** for the UI bridge + key components. More upfront.

> **Recommendation: (a).** Keep logic in the C engine where the strong harness already lives;
> introduce UI tests once the UI stabilizes.

**Answer:** _____

**Q12 — Confirm we adopt the money_books error + test conventions wholesale (renamed `db_`)?**
- (a) **Yes, adopt D.1–D.4 as the project standard** (recommended).
- (b) Adopt with changes (note them).

**Answer:** _____

---

## Quick-answer block (fill this and I'll write the SPEC)

```
1 engine topology:        a  (DuckDB-centric)
2 duckdb packaging:       b dev / a release  (dylib while prototyping, static for distributables)
3 databricks output:      a now, c later  (Parquet export now; Volume push later)
4 conversion UX:          c  (wizard generates editable SQL)
5 editing scope:          b -> c  (run-any-SQL first, cell editing later)
6 workspace model:        a  (recent-files registry)
7 workbench v1 features:  Monaco editor + result grid + schema sidebar + query history + export-from-grid
8 AI / MCP:               a  (MCP-ready dispatch surface, no AI in v1)
9 connection abstraction: a  (define the file/network seam now)
10 packaging:             macOS .app first (unsigned dev -> notarized later); Linux/Windows TBD
11 UI testing v1:         engine-only automated tests; ship a simple visible UI each phase
12 adopt error+test conv: a  (yes, adopt D.1–D.4 wholesale, renamed db_)
phasing:                  ok
project name:             keep "dbview" (provisional)
```

### Standing workflow rule (owner directive, 2026-06-17)
**Test-after-each-functionality.** Each unit of functionality is "done" only when its tests are
written **and run green**. Tests are added incrementally as features land (not batched at the end),
and the suite is re-run after each unit to confirm we're still on track. This is the per-phase
definition-of-done gate.
