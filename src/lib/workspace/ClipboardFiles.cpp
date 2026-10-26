/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "ClipboardFiles.h"
#include <string>
#ifndef Q_OS_MACOS
#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#include <shellapi.h>
#include <windows.h>
#endif

namespace deskflow::workspace {
quint64 nativeClipboardRevision()
{
#ifdef Q_OS_WIN
  return GetClipboardSequenceNumber();
#else
  return 0;
#endif
}

ClipboardFiles nativeClipboardFiles()
{
  ClipboardFiles result;
#ifdef Q_OS_WIN
  result.revision = GetClipboardSequenceNumber();
  if (!OpenClipboard(nullptr))
    return result;
  result.available = true;
  if (const auto drop = static_cast<HDROP>(GetClipboardData(CF_HDROP))) {
    const auto count = DragQueryFileW(drop, 0xffffffffu, nullptr, 0);
    if (count <= 4096) {
      for (UINT index = 0; index < count; ++index) {
        const auto length = DragQueryFileW(drop, index, nullptr, 0);
        if (length > 32767)
          continue;
        std::wstring path(length + 1, L'\0');
        const auto copied = DragQueryFileW(drop, index, path.data(), UINT(path.size()));
        result.paths.append(QString::fromWCharArray(path.data(), int(copied)));
      }
    }
  }
  CloseClipboard();
  if (result.revision != GetClipboardSequenceNumber())
    result.available = false;
#endif
  return result;
}
} // namespace deskflow::workspace
#endif
