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
export const api = {
  current: () => dbInvoke<{ path: string | null }>('app.current'),
  open: (path: string) => dbInvoke<{ path: string }>('app.open', { path }),
  tables: () => dbInvoke<ResultSet>('schema.tables'),
  query: (sql: string) => dbInvoke<ResultSet>('query.run', { sql }),
}
