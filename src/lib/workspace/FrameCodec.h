/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QList>

namespace deskflow::workspace {

struct WorkspaceFrame
{
  QJsonObject metadata;
  QByteArray payload;
};

// Fixed limits apply before allocation or JSON parsing. TCP fragmentation and
// coalescing are independent of application message boundaries.
class FrameCodec
{
public:
  static constexpr qsizetype MaxMetadata = 2 * 1024 * 1024;
  static constexpr qsizetype MaxPayload = 256 * 1024;
  static constexpr qsizetype MaxFrame = MaxMetadata + MaxPayload + 4;
  static QByteArray encode(const QJsonObject &metadata, const QByteArray &payload = {});
  bool append(const QByteArray &bytes, QList<WorkspaceFrame> &frames);
  bool failed() const
  {
    return m_failed;
  }
  void clear()
  {
    m_buffer.clear();
    m_failed = false;
  }

private:
  QByteArray m_buffer;
  bool m_failed = false;
};

} // namespace deskflow::workspace
