/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once
#include <QStringList>

#include <optional>

namespace deskflow::workspace {
struct ClipboardFiles
{
  quint64 revision = 0;
  QStringList paths;
  QString generation;
  bool available = false;
};
// Native revision polling works even when the Qt application is inactive.
quint64 nativeClipboardRevision();
ClipboardFiles nativeClipboardFiles();
// Publishes native file formats and their transfer marker on the GUI thread.
// An expected revision preserves a newer local copy when preparation was slow.
// Returns false on revision mismatch, unsupported platforms, or native failure.
bool publishNativeClipboardFiles(
    const QStringList &paths, const QString &generation, std::optional<quint64> expectedRevision = std::nullopt
);
} // namespace deskflow::workspace
