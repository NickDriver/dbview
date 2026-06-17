#ifndef DBVIEW_FILE_DIALOG_H
#define DBVIEW_FILE_DIALOG_H
/*
 * dbview — native file dialogs (macOS). Implemented in file_dialog.m over NSOpenPanel /
 * NSSavePanel. Must be called on the main (UI) thread — the webview bind callback is.
 */

/* Show an Open panel for an existing file. Returns a malloc'd UTF-8 path, or NULL if the
 * user cancelled. Caller frees. */
char *dbview_dialog_open(const char *title);

/* Show a Save panel. `default_name` pre-fills the filename (may be NULL). Returns a malloc'd
 * UTF-8 path, or NULL if cancelled. Caller frees. */
char *dbview_dialog_save(const char *title, const char *default_name);

/* Install a minimal app menu (App + Edit). The Edit menu is what makes the standard
 * Cmd+C/V/X/A/Z key-equivalents reach the WKWebView content. Call once after the webview
 * (and thus NSApplication) is created. */
void dbview_install_menu(void);

/* Put UTF-8 text on the system clipboard (reliable over file:// where navigator.clipboard
 * is blocked). */
void dbview_clipboard_set(const char *text);

#endif /* DBVIEW_FILE_DIALOG_H */
