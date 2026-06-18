// Typed wrapper around the native bridge. The C shell binds window.dbInvoke(method, argsJson)
// and resolves the promise with the parsed JSON result. We recognize the error envelope and
// throw a typed DbError so the UI never string-matches error text. (SPEC §5, §6.2)

export interface DbError {
  code: string
  message: string
}

export interface Column {
  name: string
  type: string
}

export interface ResultSet {
  columns: Column[]
  rows: (string | null)[][]
  row_count: number
  truncated: boolean
}

declare global {
  interface Window {
    dbInvoke?: (method: string, argsJson: string) => Promise<unknown>
  }
}

export class DbCallError extends Error {
  code: string
  constructor(err: DbError) {
    super(err.message)
    this.name = 'DbCallError'
    this.code = err.code
  }
}

function isErrorEnvelope(v: unknown): v is { error: DbError } {
  return typeof v === 'object' && v !== null && 'error' in v
}

export async function dbInvoke<T = unknown>(
  method: string,
  args: Record<string, unknown> = {},
): Promise<T> {
  if (!window.dbInvoke) {
    throw new DbCallError({ code: 'DB_ERR_INTERNAL', message: 'native bridge unavailable (run inside dbview)' })
  }
  const res = await window.dbInvoke(method, JSON.stringify(args))
  if (isErrorEnvelope(res)) throw new DbCallError(res.error)
  return res as T
}

// Convenience helpers for the Phase 0 methods.
export interface ConnInfo {
  path: string | null
  read_only?: boolean
  snapshot?: boolean
  engine?: 'duckdb' | 'sqlite'
}

export const api = {
  current: () => dbInvoke<ConnInfo>('app.current'),
  open: (path: string) => dbInvoke<ConnInfo>('app.open', { path }),
  newMemory: (engine: 'duckdb' | 'sqlite') => dbInvoke<ConnInfo>('app.new_memory', { engine }),
  pickOpen: () => dbInvoke<{ path: string | null }>('app.pick_open'),
  pickSave: (defaultName?: string) =>
    dbInvoke<{ path: string | null }>('app.pick_save', { default_name: defaultName ?? '' }),
  clipboardWrite: (text: string) => dbInvoke<{ ok: boolean }>('app.clipboard_write', { text }),
  writeFile: (path: string, text: string) => dbInvoke<{ ok: boolean }>('app.write_file', { path, text }),
  tables: () => dbInvoke<ResultSet>('schema.tables'),
  columns: () => dbInvoke<ResultSet>('schema.columns'),
  query: (sql: string) => dbInvoke<ResultSet>('query.run', { sql }),
  convert: {
    importCsv: (table: string, path: string) =>
      dbInvoke<{ sql: string }>('convert.import_csv', { table, path }),
    importParquet: (table: string, path: string) =>
      dbInvoke<{ sql: string }>('convert.import_parquet', { table, path }),
    exportParquet: (table: string, path: string) =>
      dbInvoke<{ sql: string }>('convert.export_parquet', { table, path }),
    exportCsv: (table: string, path: string) =>
      dbInvoke<{ sql: string }>('convert.export_csv', { table, path }),
    attachSqlite: (path: string, alias: string) =>
      dbInvoke<{ sql: string }>('convert.attach_sqlite', { path, alias }),
    copyTable: (src_schema: string, src: string, dst: string) =>
      dbInvoke<{ sql: string }>('convert.copy_table', { src_schema, src, dst }),
  },
}
