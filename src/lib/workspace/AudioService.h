/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>

#include <memory>

namespace deskflow::workspace {

class AudioService : public QObject
{
  Q_OBJECT

public:
  explicit AudioService(QObject *parent = nullptr);
  ~AudioService() override;
  static QJsonArray outputDevices();
  void configure(const QJsonObject &configuration);
  void startCapture();
  void stopCapture();
  void stopPlayback();
  void peerDisconnected(const QString &peer);
  void receiveAudio(const QString &peer, const QJsonObject &metadata, const QByteArray &payload);

Q_SIGNALS:
  void outbound(const QString &type, const QJsonObject &metadata, const QByteArray &payload);
  void statusChanged(const QString &message);
  void error(const QString &message);

private:
  bool startPlayback();
  void capturedAudioAvailable();
  struct Private;
  std::unique_ptr<Private> m_private;
};

} // namespace deskflow::workspace
