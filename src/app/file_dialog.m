/*
 * dbview — native file dialogs for macOS (NSOpenPanel / NSSavePanel).
 * Compiled with ARC into the dbview_macos lib; only linked into the app target.
 */
#import <Cocoa/Cocoa.h>
#include "file_dialog.h"

#include <stdlib.h>
#include <string.h>

static char *dup_path(NSURL *url) {
  if (!url) return NULL;
  const char *u = url.path.UTF8String;
  if (!u) return NULL;
  size_t n = strlen(u) + 1;
  char *p = malloc(n);
  if (p) memcpy(p, u, n);
  return p;
}

char *dbview_dialog_open(const char *title) {
  @autoreleasepool {
    [NSApp activateIgnoringOtherApps:YES];
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    panel.canChooseFiles = YES;
    panel.canChooseDirectories = NO;
    panel.allowsMultipleSelection = NO;
    if (title) panel.message = [NSString stringWithUTF8String:title];
    if ([panel runModal] != NSModalResponseOK) return NULL;
    return dup_path(panel.URLs.firstObject);
  }
}

char *dbview_dialog_save(const char *title, const char *default_name) {
  @autoreleasepool {
    [NSApp activateIgnoringOtherApps:YES];
    NSSavePanel *panel = [NSSavePanel savePanel];
    panel.canCreateDirectories = YES;
    if (title) panel.message = [NSString stringWithUTF8String:title];
    if (default_name) panel.nameFieldStringValue = [NSString stringWithUTF8String:default_name];
    if ([panel runModal] != NSModalResponseOK) return NULL;
    return dup_path(panel.URL);
  }
}
