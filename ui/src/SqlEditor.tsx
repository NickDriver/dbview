import { useMemo } from 'react'
import CodeMirror from '@uiw/react-codemirror'
import { sql, SQLite } from '@codemirror/lang-sql'

// SQL editor built on CodeMirror 6. Chosen over Monaco because it bundles into a single
// inlined file with no web workers — required for loading over file:// in WKWebView (SPEC §2.1).
// Schema is passed for autocompletion of table/column names.

export interface EditorSchema {
  [table: string]: string[] // table -> column names
}

export function SqlEditor({
  value,
  onChange,
  dark,
  schema,
}: {
  value: string
  onChange: (v: string) => void
  dark: boolean
  schema?: EditorSchema
}) {
  const extensions = useMemo(
    () => [sql({ dialect: SQLite, schema, upperCaseKeywords: true })],
    [schema],
  )
  return (
    <CodeMirror
      value={value}
      onChange={onChange}
      theme={dark ? 'dark' : 'light'}
      extensions={extensions}
      basicSetup={{ lineNumbers: true, foldGutter: false, highlightActiveLine: true }}
      className="cm-editor-wrap"
      height="140px"
    />
  )
}
