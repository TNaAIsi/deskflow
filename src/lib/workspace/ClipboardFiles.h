/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once
#include <QStringList>

namespace deskflow::workspace {
struct ClipboardFiles
{
  quint64 revision = 0;
  QStringList paths;
  bool available = false;
};
// Native revision polling works even when the Qt application is inactive.
quint64 nativeClipboardRevision();
ClipboardFiles nativeClipboardFiles();
} // namespace deskflow::workspace
