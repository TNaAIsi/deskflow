/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once
#include <atomic>

namespace deskflow::workspace {
// An unsupported file copy looks empty to the original text/image protocol.
// Keep its ownership notifications from cancelling the independent file job,
// including while the file bytes are still being downloaded.
inline std::atomic_bool g_fileClipboardActive = false;
inline std::atomic_bool g_fileClipboardEnabled = false;
inline bool preserveFileClipboard(bool hasLegacyContents)
{
  return (g_fileClipboardEnabled.load(std::memory_order_acquire) ||
          g_fileClipboardActive.load(std::memory_order_acquire)) &&
         !hasLegacyContents;
}
} // namespace deskflow::workspace
