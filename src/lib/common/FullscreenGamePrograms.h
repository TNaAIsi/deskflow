/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once

#include <QDir>
#include <QStringList>

namespace deskflow {
inline constexpr int kMaximumFullscreenGamePrograms = 64;

// Normalize lexically: the elevated input core must not resolve arbitrary
// paths on disk (especially UNC paths) merely to compare a foreground image.
inline QString cleanFullscreenGameProgramPath(QString path)
{
  path = path.trimmed();
  if (path.isEmpty() || path.size() > 32767)
    return {};
  for (const auto character : path)
    if (character.unicode() < 0x20)
      return {};
  path.replace('\\', '/');
  if (path.startsWith("//?/UNC/", Qt::CaseInsensitive))
    path = "//" + path.mid(8);
  else if (path.startsWith("//?/"))
    path.remove(0, 4);
  const bool drivePath = path.size() >= 3 && path.at(0).isLetter() && path.at(1) == ':' && path.at(2) == '/';
  if ((!drivePath && !path.startsWith('/')) || !path.endsWith(".exe", Qt::CaseInsensitive))
    return {};
  const bool unc = path.startsWith("//");
  path = QDir::cleanPath(path);
  if (unc && !path.startsWith("//"))
    path.prepend('/');
  return path;
}

inline QStringList cleanFullscreenGameProgramList(const QStringList &paths)
{
  QStringList result;
  for (const auto &path : paths) {
    const auto clean = cleanFullscreenGameProgramPath(path);
    if (clean.isEmpty() || result.contains(clean, Qt::CaseInsensitive))
      continue;
    result.append(clean);
    if (result.size() == kMaximumFullscreenGamePrograms)
      break;
  }
  return result;
}
} // namespace deskflow
