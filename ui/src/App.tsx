import { useCallback, useEffect, useMemo, useRef, useState } from 'react'
import { api, DbCallError, type ResultSet } from './bridge'
import { SqlEditor, type EditorSchema } from './SqlEditor'

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
  const [openInput, setOpenInput] = useState('')
  const [tables, setTables] = useState<string[]>([])
  const [sql, setSql] = useState('SELECT 1 AS hello;')
  const [result, setResult] = useState<ResultSet | null>(null)
  const [sort, setSort] = useState<Sort>(null)
  const [error, setError] = useState<string | null>(null)
  const [busy, setBusy] = useState(false)
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

  const refreshTables = useCallback(async () => {
    try {
      const r = await api.tables()
      setTables(r.rows.map((row) => row[0] ?? ''))
    } catch (e) {
      setTables([])
      reportError(e)
    }
  }, [])

  useEffect(() => {
    api
      .current()
      .then((c) => {
        setPath(c.path)
        if (c.path) refreshTables()
      })
      .catch(reportError)
  }, [refreshTables])

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

  async function openDb() {
    if (!openInput.trim()) return
    await withBusy(async () => {
      const r = await api.open(openInput.trim())
      setPath(r.path)
      setResult(null)
      await refreshTables()
    })
  }

  async function newMemory(engine: 'duckdb' | 'sqlite') {
    await withBusy(async () => {
      const r = await api.newMemory(engine)
      setPath(r.path)
      setResult(null)
      await refreshTables()
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
      const text = (q ?? sql).trim()
      if (!text) return
      setBusy(true)
      setError(null)
      try {
        const r = await api.query(text)
        setResult(r)
        setSort(null)
        pushHistory(text, true, r.row_count)
      } catch (e) {
        setResult(null)
        reportError(e)
        pushHistory(text, false)
      } finally {
        setBusy(false)
      }
    },
    [sql],
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

  function toggleSort(col: number) {
    setSort((s) => (s && s.col === col ? (s.dir === 'asc' ? { col, dir: 'desc' } : null) : { col, dir: 'asc' }))
  }

  const schema: EditorSchema = useMemo(
    () => Object.fromEntries(tables.map((t) => [t, [] as string[]])),
    [tables],
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

  return (
    <div className="app">
      <header className="topbar">
        <strong>dbview</strong>
        <span className="path">{path ?? 'no database open'}</span>
        <span className="open-row">
          <input
            value={openInput}
            onChange={(e) => setOpenInput(e.target.value)}
            placeholder="/path/to/file.sqlite or .duckdb"
            onKeyDown={(e) => e.key === 'Enter' && openDb()}
          />
          <button onClick={openDb} disabled={busy}>
            Open
          </button>
          <button onClick={() => newMemory('duckdb')} disabled={busy} title="New in-memory DuckDB">
            ＋ DuckDB
          </button>
          <button onClick={() => newMemory('sqlite')} disabled={busy} title="New in-memory SQLite">
            ＋ SQLite
          </button>
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
          <div className="sidebar-title">Tables ({tables.length})</div>
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
          <SqlEditor value={sql} onChange={setSql} dark={dark} schema={schema} />
          <div className="toolbar">
            <button onClick={() => runQuery()} disabled={busy}>
              Run ▸ <span className="hint">⌘⏎</span>
            </button>
            {result && (
              <span className="muted">
                {result.row_count} row{result.row_count === 1 ? '' : 's'}
                {result.truncated ? ' (truncated)' : ''}
              </span>
            )}
          </div>

          {error && <div className="error">{error}</div>}

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
