/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "FrameCodec.h"
#include <QJsonDocument>
#include <QtEndian>

namespace deskflow::workspace {

QByteArray FrameCodec::encode(const QJsonObject &metadata, const QByteArray &payload)
{
  const QByteArray json = QJsonDocument(metadata).toJson(QJsonDocument::Compact);
  if (json.size() > MaxMetadata || payload.size() > MaxPayload)
    return {};
  QByteArray encoded(8, Qt::Uninitialized);
  qToBigEndian<quint32>(static_cast<quint32>(4 + json.size() + payload.size()), encoded.data());
  qToBigEndian<quint32>(static_cast<quint32>(json.size()), encoded.data() + 4);
  encoded += json;
  encoded += payload;
  return encoded;
}

bool FrameCodec::append(const QByteArray &bytes, QList<WorkspaceFrame> &frames)
{
  if (m_failed)
    return false;
  m_buffer += bytes;
  while (m_buffer.size() >= 4) {
    const quint32 size = qFromBigEndian<quint32>(m_buffer.constData());
    if (size < 6 || size > MaxFrame) {
      m_failed = true;
      break;
    }
    if (m_buffer.size() < 8)
      break;
    const quint32 jsonSize = qFromBigEndian<quint32>(m_buffer.constData() + 4);
    if (jsonSize < 2 || jsonSize > MaxMetadata || jsonSize > size - 4 || size - 4 - jsonSize > MaxPayload) {
      m_failed = true;
      break;
    }
    if (m_buffer.size() < static_cast<qsizetype>(size) + 4)
      break;
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(m_buffer.mid(8, jsonSize), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
      m_failed = true;
      break;
    }
    frames.append({document.object(), m_buffer.mid(8 + jsonSize, size - 4 - jsonSize)});
    m_buffer.remove(0, static_cast<qsizetype>(size) + 4);
  }
  if (m_buffer.size() > MaxFrame + 4)
    m_failed = true;
  if (m_failed)
    m_buffer.clear();
  return !m_failed;
}

} // namespace deskflow::workspace
