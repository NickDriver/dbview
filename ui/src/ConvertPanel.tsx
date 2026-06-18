import { useState } from 'react'
import { api, DbCallError } from './bridge'

// Attach/Copy panel: database-to-database operations that the Import/Export buttons don't
// cover. Generates editable SQL into the editor for review + run. Runs on DuckDB.

type Op = 'attach_sqlite' | 'copy_table'

const OPS: { id: Op; label: string }[] = [
  { id: 'attach_sqlite', label: 'Attach SQLite file' },
  { id: 'copy_table', label: 'Copy table → new table' },
]

export function ConvertPanel({
  isDuckDB,
  onGenerated,
  onClose,
  onNeedDuckDB,
}: {
  isDuckDB: boolean
  onGenerated: (sql: string) => void
  onClose: () => void
  onNeedDuckDB: () => void
}) {
  const [op, setOp] = useState<Op>('attach_sqlite')
  const [err, setErr] = useState<string | null>(null)

  const [path, setPath] = useState('')
  const [alias, setAlias] = useState('src')
  const [srcSchema, setSrcSchema] = useState('')
  const [srcTable, setSrcTable] = useState('')
  const [dstTable, setDstTable] = useState('')

  async function browseOpen(set: (v: string) => void) {
    try {
      const r = await api.pickOpen()
      if (r.path) set(r.path)
    } catch (e) {
      setErr(e instanceof DbCallError ? `${e.code}: ${e.message}` : String(e))
    }
  }

  async function generate() {
    setErr(null)
    try {
      const res =
        op === 'attach_sqlite'
          ? await api.convert.attachSqlite(path, alias)
          : await api.convert.copyTable(srcSchema, srcTable, dstTable)
      onGenerated(res.sql)
    } catch (e) {
      setErr(e instanceof DbCallError ? `${e.code}: ${e.message}` : String(e))
    }
  }

  return (
    <div className="convert">
      <div className="convert-head">
        <strong>Attach / Copy</strong>
        <select value={op} onChange={(e) => setOp(e.target.value as Op)}>
          {OPS.map((o) => (
            <option key={o.id} value={o.id}>
              {o.label}
            </option>
          ))}
        </select>
        <button className="link" onClick={onClose}>
          close
        </button>
      </div>

      {!isDuckDB && (
        <div className="convert-note">
          These run on a DuckDB connection.{' '}
          <button className="link" onClick={onNeedDuckDB}>
            Start in-memory DuckDB
          </button>
        </div>
      )}

      <div className="convert-fields">
        {op === 'attach_sqlite' && (
          <>
            <label>SQLite path<span className="field-row"><input value={path} onChange={(e) => setPath(e.target.value)} placeholder="/path/app.sqlite" /><button type="button" className="browse" onClick={() => browseOpen(setPath)} title="Browse…">…</button></span></label>
            <label>Alias<input value={alias} onChange={(e) => setAlias(e.target.value)} placeholder="src" /></label>
          </>
        )}
        {op === 'copy_table' && (
          <>
            <label>Source schema<input value={srcSchema} onChange={(e) => setSrcSchema(e.target.value)} placeholder="(optional, e.g. attached alias)" /></label>
            <label>Source table<input value={srcTable} onChange={(e) => setSrcTable(e.target.value)} placeholder="table" /></label>
            <label>New table<input value={dstTable} onChange={(e) => setDstTable(e.target.value)} placeholder="copy" /></label>
          </>
        )}
      </div>

      {err && <div className="error">{err}</div>}

      <div className="convert-actions">
        <button onClick={generate}>Generate SQL → editor</button>
        <span className="muted">Review the SQL, then Run ▸</span>
      </div>
    </div>
  )
}
