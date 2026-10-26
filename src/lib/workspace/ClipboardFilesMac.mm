/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "ClipboardFiles.h"
#import <AppKit/AppKit.h>

namespace deskflow::workspace {
quint64 nativeClipboardRevision()
{
  @autoreleasepool {
    return quint64([[NSPasteboard generalPasteboard] changeCount]);
  }
}

ClipboardFiles nativeClipboardFiles()
{
  @autoreleasepool {
    ClipboardFiles result;
    NSPasteboard *pasteboard = [NSPasteboard generalPasteboard];
    result.revision = quint64(pasteboard.changeCount);
    result.available = true;
    NSArray<NSURL *> *urls = [pasteboard readObjectsForClasses:@[ [NSURL class] ]
                                                       options:@{
                                                         NSPasteboardURLReadingFileURLsOnlyKey : @YES
                                                       }];
    for (NSURL *url in urls) {
      if (url.isFileURL && url.path)
        result.paths.append(QString::fromUtf8(url.path.UTF8String));
    }
    if (quint64(pasteboard.changeCount) != result.revision)
      result.available = false;
    return result;
  }
}
} // namespace deskflow::workspace
