import { useState } from 'react'
import { api, DbCallError } from './bridge'

// Conversion wizard (Phase 2): pick an operation, fill fields, "Generate SQL" -> the SQL is
// loaded into the editor for review and running. All conversions run on a DuckDB connection.

type Op = 'import_csv' | 'export_parquet' | 'export_csv' | 'attach_sqlite' | 'copy_table'

const OPS: { id: Op; label: string }[] = [
  { id: 'import_csv', label: 'Import CSV → table' },
  { id: 'export_parquet', label: 'Export table → Parquet' },
  { id: 'export_csv', label: 'Export table → CSV' },
  { id: 'attach_sqlite', label: 'Attach SQLite file' },
  { id: 'copy_table', label: 'Copy table → new table' },
]

export function ConvertPanel({
  tables,
  isDuckDB,
  onGenerated,
  onClose,
  onNeedDuckDB,
}: {
  tables: string[]
  isDuckDB: boolean
  onGenerated: (sql: string) => void
  onClose: () => void
  onNeedDuckDB: () => void
}) {
  const [op, setOp] = useState<Op>('import_csv')
  const [err, setErr] = useState<string | null>(null)

  // shared fields
  const [path, setPath] = useState('')
  const [table, setTable] = useState(tables[0] ?? '')
  const [alias, setAlias] = useState('src')
  const [srcSchema, setSrcSchema] = useState('')
  const [srcTable, setSrcTable] = useState('')
  const [dstTable, setDstTable] = useState('')

  // COPY TO writes a single FILE, so if the user gives a directory (trailing slash) append
  // "<table>.<ext>" for convenience.
  function filePath(p: string, ext: string) {
    return p.endsWith('/') ? `${p}${table || 'export'}.${ext}` : p
  }

  async function browse(target: 'open' | 'save', set: (v: string) => void, defaultName?: string) {
    try {
      const r = target === 'open' ? await api.pickOpen() : await api.pickSave(defaultName)
      if (r.path) set(r.path)
    } catch (e) {
      setErr(e instanceof DbCallError ? `${e.code}: ${e.message}` : String(e))
    }
  }
  const browseBtn = (onClick: () => void) => (
    <button type="button" className="browse" onClick={onClick} title="Browse…">
      …
    </button>
  )

  async function generate() {
    setErr(null)
    try {
      let res: { sql: string }
      switch (op) {
        case 'import_csv':
          res = await api.convert.importCsv(table, path)
          break
        case 'export_parquet':
          res = await api.convert.exportParquet(table, filePath(path, 'parquet'))
          break
        case 'export_csv':
          res = await api.convert.exportCsv(table, filePath(path, 'csv'))
          break
        case 'attach_sqlite':
          res = await api.convert.attachSqlite(path, alias)
          break
        case 'copy_table':
          res = await api.convert.copyTable(srcSchema, srcTable, dstTable)
          break
      }
      onGenerated(res.sql)
    } catch (e) {
      setErr(e instanceof DbCallError ? `${e.code}: ${e.message}` : String(e))
    }
  }

  const tableSelect = (value: string, set: (v: string) => void, placeholder: string) =>
    tables.length > 0 ? (
      <select value={value} onChange={(e) => set(e.target.value)}>
        {tables.map((t) => (
          <option key={t} value={t}>
            {t}
          </option>
        ))}
      </select>
    ) : (
      <input value={value} onChange={(e) => set(e.target.value)} placeholder={placeholder} />
    )

  return (
    <div className="convert">
      <div className="convert-head">
        <strong>Convert</strong>
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
          Conversions run on a DuckDB connection.{' '}
          <button className="link" onClick={onNeedDuckDB}>
            Start in-memory DuckDB
          </button>
        </div>
      )}

      <div className="convert-fields">
        {op === 'import_csv' && (
          <>
            <label>CSV path<span className="field-row"><input value={path} onChange={(e) => setPath(e.target.value)} placeholder="/path/to/data.csv" />{browseBtn(() => browse('open', setPath))}</span></label>
            <label>New table<input value={table} onChange={(e) => setTable(e.target.value)} placeholder="my_table" /></label>
          </>
        )}
        {(op === 'export_parquet' || op === 'export_csv') && (
          <>
            <label>Source table{tableSelect(table, setTable, 'table')}</label>
            <label>Output path<span className="field-row"><input value={path} onChange={(e) => setPath(e.target.value)} placeholder={op === 'export_parquet' ? '/path/out.parquet' : '/path/out.csv'} />{browseBtn(() => browse('save', setPath, `${table || 'export'}.${op === 'export_parquet' ? 'parquet' : 'csv'}`))}</span></label>
          </>
        )}
        {op === 'attach_sqlite' && (
          <>
            <label>SQLite path<span className="field-row"><input value={path} onChange={(e) => setPath(e.target.value)} placeholder="/path/app.sqlite" />{browseBtn(() => browse('open', setPath))}</span></label>
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
