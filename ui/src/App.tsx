import { useCallback, useEffect, useRef, useState } from 'react'
import { api, DbCallError, type ResultSet } from './bridge'

type Theme = 'light' | 'dark' | 'system'

function loadTheme(): Theme {
  try {
    const t = localStorage.getItem('dbview-theme')
    if (t === 'light' || t === 'dark' || t === 'system') return t
  } catch {
    /* localStorage may be unavailable over file:// — fall back to system */
  }
  return 'system'
}

export function App() {
  const [path, setPath] = useState<string | null>(null)
  const [openInput, setOpenInput] = useState('')
  const [tables, setTables] = useState<string[]>([])
  const [sql, setSql] = useState('SELECT 1 AS hello;')
  const [result, setResult] = useState<ResultSet | null>(null)
  const [error, setError] = useState<string | null>(null)
  const [busy, setBusy] = useState(false)
  const [theme, setTheme] = useState<Theme>(loadTheme)

  // Apply + persist the theme. `system` defers to prefers-color-scheme via CSS.
  useEffect(() => {
    document.documentElement.dataset.theme = theme
    try {
      localStorage.setItem('dbview-theme', theme)
    } catch {
      /* best-effort persistence */
    }
  }, [theme])

  const refreshTables = useCallback(async () => {
    try {
      const r = await api.tables()
      setTables(r.rows.map((row) => row[0] ?? ''))
    } catch (e) {
      setTables([])
      reportError(e)
    }
  }, [])

  // On launch, learn whether a database is already open (passed via argv).
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

  async function openDb() {
    if (!openInput.trim()) return
    setBusy(true)
    setError(null)
    try {
      const r = await api.open(openInput.trim())
      setPath(r.path)
      setResult(null)
      await refreshTables()
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
        setResult(await api.query(text))
      } catch (e) {
        setResult(null)
        reportError(e)
      } finally {
        setBusy(false)
      }
    },
    [sql],
  )

  // Global ⌘⏎ / Ctrl+⏎ to run, regardless of which element has focus. A textarea-only
  // handler breaks the moment focus moves to a button (e.g. after a mouse click on Run).
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

  return (
    <div className="app">
      <header className="topbar">
        <strong>dbview</strong>
        <span className="path">{path ?? 'no database open'}</span>
        <span className="open-row">
          <input
            value={openInput}
            onChange={(e) => setOpenInput(e.target.value)}
            placeholder="/path/to/file.sqlite"
            onKeyDown={(e) => e.key === 'Enter' && openDb()}
          />
          <button onClick={openDb} disabled={busy}>
            Open
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
        </aside>

        <main className="main">
          <textarea
            className="editor"
            value={sql}
            spellCheck={false}
            onChange={(e) => setSql(e.target.value)}
          />
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
                    {result.columns.map((c) => (
                      <th key={c.name}>
                        {c.name}
                        {c.type ? <span className="coltype"> {c.type}</span> : null}
                      </th>
                    ))}
                  </tr>
                </thead>
                <tbody>
                  {result.rows.map((row, i) => (
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
