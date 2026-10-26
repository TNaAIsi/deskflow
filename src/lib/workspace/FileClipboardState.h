/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once
#include <atomic>

namespace deskflow::workspace {
// The native input loop and Qt clipboard coordinator live on different
// threads. A file clipboard has no representation in the legacy protocol;
// an empty legacy update must not clear it during a pointer transition.
inline std::atomic_bool g_fileClipboardActive = false;
inline std::atomic_bool g_fileClipboardEnabled = false;
inline bool preserveFileClipboard(bool hasLegacyContents)
{
  return g_fileClipboardActive.load(std::memory_order_acquire) && !hasLegacyContents;
}
} // namespace deskflow::workspace
