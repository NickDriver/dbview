import { useCallback, useEffect, useMemo, useRef, useState } from 'react'
import { format as formatSql } from 'sql-formatter'
import { api, DbCallError, type ResultSet } from './bridge'
import { SqlEditor, type EditorSchema } from './SqlEditor'
import { ConvertPanel } from './ConvertPanel'

type Theme = 'light' | 'dark' | 'system'
type Sort = { col: number; dir: 'asc' | 'desc' } | null
interface HistItem { sql: string; ts: number; ok: boolean; rows?: number }

const HISTORY_MAX = 50

function load<T>(key: string, fallback: T): T {
  try {
    const v = localStorage.getItem(key)
    if (v != null) return JSON.parse(v) as T
  } catch {
    /* localStorage may be unavailable over file:// */
  }
  return fallback
}
function save(key: string, value: unknown) {
  try {
    localStorage.setItem(key, JSON.stringify(value))
  } catch {
    /* best-effort */
  }
}

// Resolve whether the editor should render dark, honoring `system`.
function useEffectiveDark(theme: Theme): boolean {
  const [dark, setDark] = useState(false)
  useEffect(() => {
    const mq = window.matchMedia('(prefers-color-scheme: dark)')
    const compute = () => setDark(theme === 'dark' || (theme === 'system' && mq.matches))
    compute()
    mq.addEventListener('change', compute)
    return () => mq.removeEventListener('change', compute)
  }, [theme])
  return dark
}

export function App() {
  const [path, setPath] = useState<string | null>(null)
  const [readOnly, setReadOnly] = useState(false)
  const [snapshot, setSnapshot] = useState(false)
  const [engine, setEngine] = useState<'duckdb' | 'sqlite' | null>(null)
  const [tables, setTables] = useState<string[]>([])
  const [columns, setColumns] = useState<EditorSchema>({})
  const [sql, setSql] = useState<string>(() => load('dbview-sql', 'SELECT 1 AS hello;'))
  const [lastSql, setLastSql] = useState('') // the SQL that produced the current result
  const [result, setResult] = useState<ResultSet | null>(null)
  const [sort, setSort] = useState<Sort>(null)
  const [error, setError] = useState<string | null>(null)
  const [notice, setNotice] = useState<string | null>(null)
  const [busy, setBusy] = useState(false)
  const [showConvert, setShowConvert] = useState(false)
  const [openMenu, setOpenMenu] = useState<'import' | 'export' | 'newdb' | null>(null)
  const [autoFormat, setAutoFormat] = useState<boolean>(() => load('dbview-autoformat', false))
  const [theme, setTheme] = useState<Theme>(() => load<Theme>('dbview-theme', 'system'))
  const [history, setHistory] = useState<HistItem[]>(() => load<HistItem[]>('dbview-history', []))

  const dark = useEffectiveDark(theme)

  useEffect(() => {
    document.documentElement.dataset.theme = theme
    save('dbview-theme', theme)
  }, [theme])

  useEffect(() => {
    save('dbview-history', history)
  }, [history])

  useEffect(() => {
    save('dbview-sql', sql)
  }, [sql])

  useEffect(() => {
    save('dbview-autoformat', autoFormat)
  }, [autoFormat])

  const refreshSchema = useCallback(async () => {
    try {
      const t = await api.tables()
      setTables(t.rows.map((row) => row[0] ?? ''))
      // table -> [columns], for editor autocompletion
      const cr = await api.columns()
      const map: EditorSchema = {}
      for (const row of cr.rows) {
        const tbl = row[0]
        if (tbl == null) continue
        ;(map[tbl] ??= []).push(row[1] ?? '')
      }
      setColumns(map)
    } catch (e) {
      setTables([])
      setColumns({})
      reportError(e)
    }
  }, [])

  function applyConn(c: {
    path: string | null
    read_only?: boolean
    snapshot?: boolean
    engine?: 'duckdb' | 'sqlite'
  }) {
    setPath(c.path)
    setReadOnly(!!c.read_only)
    setSnapshot(!!c.snapshot)
    setEngine(c.engine ?? null)
    if (c.path) save('dbview-last-path', c.path) // remember for next launch
  }

  useEffect(() => {
    api
      .current()
      .then(async (c) => {
        if (c.path) {
          applyConn(c)
          await refreshSchema()
          return
        }
        // launched without a DB: try to reopen the last one (best-effort)
        const last = load<string>('dbview-last-path', '')
        if (last) {
          try {
            applyConn(await api.open(last))
            await refreshSchema()
            return
          } catch {
            /* file moved/locked — fall back to the launcher */
          }
        }
        applyConn(c)
      })
      .catch(reportError)
  }, [refreshSchema])

  function reportError(e: unknown) {
    if (e instanceof DbCallError) setError(`${e.code}: ${e.message}`)
    else setError(String(e))
  }

  function pushHistory(text: string, ok: boolean, rows?: number) {
    setHistory((h) => {
      if (h[0]?.sql === text && h[0]?.ok === ok) return h // skip immediate dupes
      return [{ sql: text, ts: Date.now(), ok, rows }, ...h].slice(0, HISTORY_MAX)
    })
  }

  async function openPath(p: string, engine?: 'duckdb' | 'sqlite') {
    await withBusy(async () => {
      applyConn(await api.open(p, engine))
      setResult(null)
      await refreshSchema()
    })
  }

  // Open: native file picker -> open the chosen database.
  async function openViaDialog() {
    try {
      const r = await api.pickOpen()
      if (r.path) await openPath(r.path)
    } catch (e) {
      reportError(e)
    }
  }

  // New DB: pick a format, then a Save location -> create & open a real on-disk database.
  async function newDatabase(eng: 'duckdb' | 'sqlite') {
    setOpenMenu(null)
    try {
      const ext = eng === 'duckdb' ? 'duckdb' : 'sqlite'
      const r = await api.pickSave(`database.${ext}`)
      if (!r.path) return
      const path = r.path.toLowerCase().endsWith(`.${ext}`) ? r.path : `${r.path}.${ext}`
      await openPath(path, eng)
      setNotice(`Created ${eng} database → ${path}`)
    } catch (e) {
      reportError(e)
    }
  }

  // Quick in-memory DuckDB — used by the Attach/Copy panel's nudge.
  async function newMemory(kind: 'duckdb' | 'sqlite') {
    await withBusy(async () => {
      applyConn(await api.newMemory(kind))
      setResult(null)
      await refreshSchema()
    })
  }

  async function withBusy(fn: () => Promise<void>) {
    setBusy(true)
    setError(null)
    try {
      await fn()
    } catch (e) {
      reportError(e)
    } finally {
      setBusy(false)
    }
  }

  const runQuery = useCallback(
    async (q?: string) => {
      let text = (q ?? sql).trim()
      if (!text) return
      // optional tidy-on-run
      if (autoFormat) {
        try {
          text = formatSql(text, {
            language: engine === 'duckdb' ? 'postgresql' : 'sqlite',
            keywordCase: 'upper',
          }).trim()
          setSql(text)
        } catch {
          /* keep original text if it can't be formatted */
        }
      }
      setBusy(true)
      setError(null)
      setNotice(null)
      try {
        const r = await api.query(text)
        setResult(r)
        setLastSql(text)
        setSort(null)
        pushHistory(text, true, r.row_count)
        // DDL changes the schema -> refresh the Tables sidebar + editor autocompletion.
        if (/\b(create|drop|alter|attach|detach|rename)\b/i.test(text)) void refreshSchema()
      } catch (e) {
        setResult(null)
        reportError(e)
        pushHistory(text, false)
      } finally {
        setBusy(false)
      }
    },
    [sql, autoFormat, engine, refreshSchema],
  )

  // Global ⌘⏎ / Ctrl+⏎ to run regardless of focus.
  const runRef = useRef(runQuery)
  runRef.current = runQuery
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if ((e.metaKey || e.ctrlKey) && (e.key === 'Enter' || e.keyCode === 13)) {
        e.preventDefault()
        void runRef.current()
      }
    }
    window.addEventListener('keydown', onKey, true)
    return () => window.removeEventListener('keydown', onKey, true)
  }, [])

  function previewTable(name: string) {
    const q = `SELECT * FROM "${name}" LIMIT 100;`
    setSql(q)
    void runQuery(q)
  }

  const formatQuery = useCallback(() => {
    try {
      setSql((s) =>
        formatSql(s, {
          language: engine === 'duckdb' ? 'postgresql' : 'sqlite',
          keywordCase: 'upper',
        }),
      )
    } catch {
      /* leave SQL unchanged if it can't be parsed for formatting */
    }
  }, [engine])

  async function copyQuery() {
    try {
      await api.clipboardWrite(sql)
    } catch {
      try {
        await navigator.clipboard?.writeText(sql)
      } catch {
        /* clipboard unavailable */
      }
    }
  }

  // ⌘⇧F to format
  const formatRef = useRef(formatQuery)
  formatRef.current = formatQuery
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if ((e.metaKey || e.ctrlKey) && e.shiftKey && (e.key === 'f' || e.key === 'F')) {
        e.preventDefault()
        formatRef.current()
      }
    }
    window.addEventListener('keydown', onKey, true)
    return () => window.removeEventListener('keydown', onKey, true)
  }, [])

  function toggleSort(col: number) {
    setSort((s) => (s && s.col === col ? (s.dir === 'asc' ? { col, dir: 'desc' } : null) : { col, dir: 'asc' }))
  }

  // Editor schema: every table is a key (so table names complete even before columns load),
  // overlaid with the fetched column lists for column-name completion.
  const schema: EditorSchema = useMemo(
    () => ({ ...Object.fromEntries(tables.map((t) => [t, [] as string[]])), ...columns }),
    [tables, columns],
  )

  // Client-side sort of the current result. NULLs always sort last; numeric when both parse.
  const sortedRows = useMemo(() => {
    if (!result) return []
    if (!sort) return result.rows
    const { col, dir } = sort
    const mul = dir === 'asc' ? 1 : -1
    return [...result.rows].sort((a, b) => {
      const x = a[col]
      const y = b[col]
      if (x === null && y === null) return 0
      if (x === null) return 1
      if (y === null) return -1
      const nx = Number(x)
      const ny = Number(y)
      if (x !== '' && y !== '' && !Number.isNaN(nx) && !Number.isNaN(ny)) return (nx - ny) * mul
      return x.localeCompare(y) * mul
    })
  }, [result, sort])

  // Serialize the current (sorted) result. quote=true → RFC-style CSV; quote=false → TSV
  // (tabs/newlines flattened to spaces) for clean paste into spreadsheets.
  function serializeResult(delim: string, quote: boolean): string {
    if (!result) return ''
    const esc = (v: string | null) => {
      const s = v ?? ''
      if (!quote) return s.replace(/[\t\n\r]+/g, ' ')
      return /[",\n\r]/.test(s) ? '"' + s.replace(/"/g, '""') + '"' : s
    }
    const header = result.columns.map((c) => esc(c.name)).join(delim)
    const body = sortedRows.map((row) => row.map(esc).join(delim))
    return [header, ...body].join('\n')
  }

  async function copyResult() {
    if (!result) return
    const tsv = serializeResult('\t', false)
    try {
      await api.clipboardWrite(tsv)
    } catch {
      try {
        await navigator.clipboard?.writeText(tsv)
      } catch {
        /* clipboard unavailable */
      }
    }
    setNotice(`Copied ${result.row_count} row${result.row_count === 1 ? '' : 's'} (TSV)`)
  }

  async function exportResultCsv() {
    if (!result) return
    try {
      const r = await api.pickSave('result.csv')
      if (!r.path) return
      await api.writeFile(r.path, serializeResult(',', true))
      setNotice(`Saved CSV → ${r.path}`)
    } catch (e) {
      reportError(e)
    }
  }

  async function exportResultParquet() {
    if (!result || !lastSql) return
    try {
      const r = await api.pickSave('result.parquet')
      if (!r.path) return
      const inner = lastSql.replace(/;\s*$/, '')
      const escaped = r.path.replace(/'/g, "''")
      await api.query(`COPY (${inner}) TO '${escaped}' (FORMAT parquet);`)
      setNotice(`Saved Parquet → ${r.path}`)
    } catch (e) {
      reportError(e)
    }
  }

  // ---- Import: pick a file, auto-create a table named from the file, preview it ----
  function tableNameFromPath(p: string): string {
    const file = p.split('/').pop() || 'import'
    const base = file.replace(/\.[^.]+$/, '').replace(/[^A-Za-z0-9_]/g, '_')
    const safe = /^[A-Za-z_]/.test(base) ? base : `t_${base}`
    if (!tables.includes(safe)) return safe
    let i = 2
    while (tables.includes(`${safe}_${i}`)) i++
    return `${safe}_${i}`
  }

  async function importFile(fmt: 'csv' | 'parquet') {
    setOpenMenu(null)
    if (engine !== 'duckdb') {
      setError('Import needs a DuckDB connection — use “New DB ▾ → DuckDB” (or open a .duckdb file) first.')
      return
    }
    setError(null)
    setNotice(null)
    try {
      const r = await api.pickOpen()
      if (!r.path) return
      const tbl = tableNameFromPath(r.path)
      const built =
        fmt === 'csv'
          ? await api.convert.importCsv(tbl, r.path)
          : await api.convert.importParquet(tbl, r.path)
      await api.query(built.sql) // run the generated CREATE TABLE
      await refreshSchema()
      const preview = `SELECT * FROM "${tbl}" LIMIT 100;`
      setSql(preview)
      await runQuery(preview)
      setNotice(`Imported ${fmt.toUpperCase()} → table "${tbl}"`)
    } catch (e) {
      reportError(e)
    }
  }

  function exportResult(fmt: 'csv' | 'parquet') {
    setOpenMenu(null)
    if (fmt === 'csv') void exportResultCsv()
    else void exportResultParquet()
  }

  return (
    <div className="app">
      <header className="topbar">
        <strong>dbview</strong>
        <span className="path">{path ?? 'no database open'}</span>
        {engine && <span className="badge">{engine}</span>}
        {readOnly && !snapshot && (
          <span className="badge ro" title="opened read-only (file is in use or on read-only media)">read-only</span>
        )}
        {snapshot && (
          <span className="badge ro" title="the file was locked by another process; viewing a point-in-time snapshot copy">
            snapshot
          </span>
        )}
        <span className="open-row">
          <button onClick={openViaDialog} disabled={busy} title="Open a database file">
            Open…
          </button>
          <span className="dropdown">
            <button onClick={() => setOpenMenu((m) => (m === 'newdb' ? null : 'newdb'))} disabled={busy} title="Create a new database">
              New DB ▾
            </button>
            {openMenu === 'newdb' && (
              <span className="menu menu-right">
                <button onClick={() => newDatabase('duckdb')}>DuckDB…</button>
                <button onClick={() => newDatabase('sqlite')}>SQLite…</button>
              </span>
            )}
          </span>
          <select
            className="theme-select"
            value={theme}
            onChange={(e) => setTheme(e.target.value as Theme)}
            title="Theme"
            aria-label="Theme"
          >
            <option value="light">☀︎ Light</option>
            <option value="dark">☾ Dark</option>
            <option value="system">⚙︎ System</option>
          </select>
        </span>
      </header>

      <div className="body">
        <aside className="sidebar">
          <div className="sidebar-title hist-head">
            Tables ({tables.length})
            <button className="link" onClick={() => refreshSchema()} title="Refresh tables">
              ↻
            </button>
          </div>
          {tables.length === 0 && <div className="muted">— none —</div>}
          <ul>
            {tables.map((t) => (
              <li key={t}>
                <button className="link" onClick={() => previewTable(t)}>
                  {t}
                </button>
              </li>
            ))}
          </ul>

          {history.length > 0 && (
            <>
              <div className="sidebar-title hist-head">
                History
                <button className="link clear" onClick={() => setHistory([])} title="Clear history">
                  clear
                </button>
              </div>
              <ul className="history">
                {history.map((h, i) => (
                  <li key={i}>
                    <button
                      className={`link hist ${h.ok ? '' : 'hist-err'}`}
                      title={h.sql}
                      onClick={() => setSql(h.sql)}
                    >
                      {h.ok ? '' : '⚠ '}
                      {h.sql.replace(/\s+/g, ' ').slice(0, 40)}
                    </button>
                  </li>
                ))}
              </ul>
            </>
          )}
        </aside>

        <main className="main">
          {showConvert && (
            <ConvertPanel
              isDuckDB={engine === 'duckdb'}
              onClose={() => setShowConvert(false)}
              onNeedDuckDB={() => newMemory('duckdb')}
              onGenerated={(generated) => {
                setSql(generated)
                setShowConvert(false)
              }}
            />
          )}
          <SqlEditor
            value={sql}
            onChange={setSql}
            dark={dark}
            schema={schema}
            onFormat={formatQuery}
            onCopy={copyQuery}
            onClear={() => setSql('')}
          />
          <div className="toolbar">
            <button onClick={() => runQuery()} disabled={busy}>
              Run ▸ <span className="hint">⌘⏎</span>
            </button>

            <span className="dropdown">
              <button onClick={() => setOpenMenu((m) => (m === 'import' ? null : 'import'))} disabled={busy} title="Import a file as a new table">
                Import ▾
              </button>
              {openMenu === 'import' && (
                <span className="menu">
                  <button onClick={() => importFile('csv')}>CSV…</button>
                  <button onClick={() => importFile('parquet')}>Parquet…</button>
                </span>
              )}
            </span>

            <span className="dropdown">
              <button
                onClick={() => setOpenMenu((m) => (m === 'export' ? null : 'export'))}
                disabled={busy || !result || result.row_count === 0}
                title="Export the current result to a file"
              >
                Export ▾
              </button>
              {openMenu === 'export' && (
                <span className="menu">
                  <button onClick={() => exportResult('csv')}>CSV…</button>
                  {engine === 'duckdb' && <button onClick={() => exportResult('parquet')}>Parquet…</button>}
                </span>
              )}
            </span>

            <button onClick={() => setShowConvert((v) => !v)} title="Attach a SQLite database or copy a table">
              Attach/Copy ▾
            </button>
            <label className="autofmt" title="Format the query each time you run it">
              <input type="checkbox" checked={autoFormat} onChange={(e) => setAutoFormat(e.target.checked)} />
              Auto-format
            </label>
            {result && (
              <span className="muted">
                {result.row_count} row{result.row_count === 1 ? '' : 's'}
                {result.truncated ? ' (truncated)' : ''}
              </span>
            )}
            {result && result.row_count > 0 && (
              <span className="result-actions">
                <button onClick={copyResult} title="Copy result as TSV (paste into a spreadsheet)">Copy</button>
              </span>
            )}
          </div>
          {openMenu && <div className="menu-backdrop" onClick={() => setOpenMenu(null)} />}

          {error && <div className="error">{error}</div>}
          {notice && <div className="notice">{notice}</div>}

          {result && (
            <div className="grid-wrap">
              <table className="grid">
                <thead>
                  <tr>
                    {result.columns.map((c, ci) => (
                      <th key={c.name} onClick={() => toggleSort(ci)} className="sortable">
                        {c.name}
                        {c.type ? <span className="coltype"> {c.type}</span> : null}
                        {sort && sort.col === ci ? (
                          <span className="sort-ind">{sort.dir === 'asc' ? ' ▲' : ' ▼'}</span>
                        ) : null}
                      </th>
                    ))}
                  </tr>
                </thead>
                <tbody>
                  {sortedRows.map((row, i) => (
                    <tr key={i}>
                      {row.map((cell, j) => (
                        <td key={j} className={cell === null ? 'null' : ''}>
                          {cell === null ? 'NULL' : cell}
                        </td>
                      ))}
                    </tr>
                  ))}
                </tbody>
              </table>
            </div>
          )}
        </main>
      </div>
    </div>
  )
}
