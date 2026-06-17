import { useMemo } from 'react'
import CodeMirror from '@uiw/react-codemirror'
import {
  sql,
  SQLite,
  schemaCompletionSource,
  keywordCompletionSource,
} from '@codemirror/lang-sql'
import {
  autocompletion,
  type Completion,
  type CompletionContext,
  type CompletionResult,
} from '@codemirror/autocomplete'

// SQL editor on CodeMirror 6 (chosen over Monaco: bundles single-file, no web workers — needed
// for file:// in WKWebView, SPEC §2.1). Autocompletion is wired explicitly so that table names,
// dotted columns, AND bare column names all surface as you type (the default lang-sql source only
// offers columns after a `table.` prefix).

export interface EditorSchema {
  [table: string]: string[] // table -> column names
}

// Offer every column name (unqualified), with its table shown as detail. This is what makes
// `SELECT <col>` complete without having to type the table prefix first.
function bareColumnSource(schema: EditorSchema) {
  return (ctx: CompletionContext): CompletionResult | null => {
    const word = ctx.matchBefore(/[\w]+/)
    if (!word && !ctx.explicit) return null
    if (word && word.from === word.to && !ctx.explicit) return null
    const seen = new Set<string>()
    const options: Completion[] = []
    for (const [table, cols] of Object.entries(schema)) {
      for (const c of cols) {
        if (!c || seen.has(c)) continue
        seen.add(c)
        options.push({ label: c, type: 'property', detail: table })
      }
    }
    if (options.length === 0) return null
    return { from: word ? word.from : ctx.pos, options, validFor: /^[\w]*$/ }
  }
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
    () => [
      sql({ dialect: SQLite, upperCaseKeywords: true }),
      autocompletion({
        activateOnTyping: true,
        override: [
          schemaCompletionSource({ dialect: SQLite, schema: schema ?? {} }),
          bareColumnSource(schema ?? {}),
          keywordCompletionSource(SQLite, true),
        ],
      }),
    ],
    [schema],
  )
  return (
    <CodeMirror
      value={value}
      onChange={onChange}
      theme={dark ? 'dark' : 'light'}
      extensions={extensions}
      basicSetup={{
        lineNumbers: true,
        foldGutter: false,
        highlightActiveLine: true,
        autocompletion: false, // we provide our own autocompletion extension above
      }}
      className="cm-editor-wrap"
      height="140px"
    />
  )
}
