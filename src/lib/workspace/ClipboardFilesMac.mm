/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "ClipboardFiles.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QThread>

#import <AppKit/AppKit.h>

namespace deskflow::workspace {

namespace {

NSString *const kGenerationType = @"org.deskflow.file-transfer-generation";
constexpr qsizetype kMaximumGenerationBytes = 256;

// The old native file-list type is intentionally supported as well as modern
// file-URL items. Plain public.url or public.utf8-plain-text is not a file copy.
NSString *const kLegacyFilenamesType = @"NSFilenamesPboardType";

} // namespace

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
    const auto types = pasteboard.types;
    if ([types containsObject:NSPasteboardTypeFileURL]) {
      NSArray<NSURL *> *urls = [pasteboard readObjectsForClasses:@[ [NSURL class] ]
                                                         options:@{
                                                           NSPasteboardURLReadingFileURLsOnlyKey : @YES
                                                         }];
      if (urls.count <= 4096) {
        for (NSURL *url in urls) {
          if (url.isFileURL && url.path)
            result.paths.append(QString::fromUtf8(url.path.UTF8String));
        }
      }
    } else if ([types containsObject:kLegacyFilenamesType]) {
      id filenames = [pasteboard propertyListForType:kLegacyFilenamesType];
      if ([filenames isKindOfClass:[NSArray class]] && [filenames count] <= 4096) {
        for (id filename in filenames) {
          if ([filename isKindOfClass:[NSString class]] && [filename isAbsolutePath])
            result.paths.append(QString::fromUtf8([filename UTF8String]));
        }
      }
    }
    if (!result.paths.isEmpty()) {
      NSData *marker = [pasteboard dataForType:kGenerationType];
      if (marker.length > 0 && marker.length <= NSUInteger(kMaximumGenerationBytes))
        result.generation = QString::fromUtf8(static_cast<const char *>(marker.bytes), qsizetype(marker.length));
    }
    if (quint64(pasteboard.changeCount) != result.revision)
      result.available = false;
    return result;
  }
}

bool publishNativeClipboardFiles(
    const QStringList &paths, const QString &generation, std::optional<quint64> expectedRevision
)
{
  @autoreleasepool {
    auto *application = QCoreApplication::instance();
    const auto marker = generation.toUtf8();
    if (!application || QThread::currentThread() != application->thread() || paths.isEmpty() || paths.size() > 4096 ||
        marker.isEmpty() || marker.size() >= kMaximumGenerationBytes || generation.contains(QChar::Null)) {
      return false;
    }

    NSData *generationData = [NSData dataWithBytes:marker.constData() length:NSUInteger(marker.size())];
    NSMutableArray<NSPasteboardItem *> *items = [NSMutableArray arrayWithCapacity:NSUInteger(paths.size())];
    for (const auto &path : paths) {
      if (!QDir::isAbsolutePath(path) || path.contains(QChar::Null))
        return false;
      NSString *nativePath = [NSString stringWithCharacters:reinterpret_cast<const unichar *>(path.utf16())
                                                     length:NSUInteger(path.size())];
      NSURL *url = [NSURL fileURLWithPath:nativePath isDirectory:QFileInfo(path).isDir()];
      if (url == nil)
        return false;
      NSPasteboardItem *item = [[NSPasteboardItem alloc] init];
      const bool configured = [item setString:url.absoluteString forType:NSPasteboardTypeFileURL] &&
                              (items.count != 0 || [item setData:generationData forType:kGenerationType]);
      if (configured)
        [items addObject:item];
      [item release];
      if (!configured)
        return false;
    }
    NSPasteboard *pasteboard = [NSPasteboard generalPasteboard];
    if (expectedRevision && *expectedRevision != quint64(pasteboard.changeCount))
      return false;
    const auto claimedRevision = [pasteboard clearContents];
    // File URLs and their generation marker are part of the same native write.
    // No observer can see our file selection before its marker is supplied.
    if (![pasteboard writeObjects:items]) {
      // Another owner may have copied while the pasteboard write was running.
      // Only clean up a failed write while our empty claim is still current.
      if (pasteboard.changeCount == claimedRevision)
        [pasteboard clearContents];
      return false;
    }
    return true;
  }
}

} // namespace deskflow::workspace
