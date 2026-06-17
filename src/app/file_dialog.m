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

void dbview_install_menu(void) {
  @autoreleasepool {
    NSMenu *mainMenu = [[NSMenu alloc] init];

    /* App menu (Quit) */
    NSMenuItem *appItem = [[NSMenuItem alloc] init];
    [mainMenu addItem:appItem];
    NSMenu *appMenu = [[NSMenu alloc] init];
    NSString *appName = [[NSProcessInfo processInfo] processName];
    [appMenu addItemWithTitle:[@"Quit " stringByAppendingString:appName]
                       action:@selector(terminate:)
                keyEquivalent:@"q"];
    appItem.submenu = appMenu;

    /* Edit menu — its standard selectors are what enable Cmd+C/V/X/A/Z in the web view */
    NSMenuItem *editItem = [[NSMenuItem alloc] init];
    [mainMenu addItem:editItem];
    NSMenu *editMenu = [[NSMenu alloc] initWithTitle:@"Edit"];
    [editMenu addItemWithTitle:@"Undo" action:@selector(undo:) keyEquivalent:@"z"];
    NSMenuItem *redo = [editMenu addItemWithTitle:@"Redo" action:@selector(redo:) keyEquivalent:@"z"];
    redo.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagShift;
    [editMenu addItem:[NSMenuItem separatorItem]];
    [editMenu addItemWithTitle:@"Cut" action:@selector(cut:) keyEquivalent:@"x"];
    [editMenu addItemWithTitle:@"Copy" action:@selector(copy:) keyEquivalent:@"c"];
    [editMenu addItemWithTitle:@"Paste" action:@selector(paste:) keyEquivalent:@"v"];
    [editMenu addItemWithTitle:@"Select All" action:@selector(selectAll:) keyEquivalent:@"a"];
    editItem.submenu = editMenu;

    [NSApp setMainMenu:mainMenu];
  }
}

void dbview_clipboard_set(const char *text) {
  if (!text) return;
  @autoreleasepool {
    NSPasteboard *pb = [NSPasteboard generalPasteboard];
    [pb clearContents];
    [pb setString:[NSString stringWithUTF8String:text] forType:NSPasteboardTypeString];
  }
}
