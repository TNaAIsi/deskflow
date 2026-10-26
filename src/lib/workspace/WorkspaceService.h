/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once

#include "WorkspaceTransport.h"
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QTimer>

namespace deskflow::workspace {
class AudioService;
class FileTransferService;

class WorkspaceService : public QObject
{
  Q_OBJECT
public:
  explicit WorkspaceService(bool server, QObject *parent = nullptr);
  void start();
  QJsonObject state() const;
  void command(const QJsonObject &command);
  static QJsonObject defaultOptions();

Q_SIGNALS:
  void stateChanged(const QJsonObject &state);

private:
  void applyOptions();
  void applyLayout(const QJsonObject &layout, bool broadcast);
  void message(const QString &peer, const QString &type, const QJsonObject &metadata, const QByteArray &payload);
  void update();
  void reportError(const QString &error);
  bool sendFiles(const QString &peer, const QString &type, const QJsonObject &meta, const QByteArray &data);

  WorkspaceTransport m_transport;
  AudioService *m_audio;
  FileTransferService *m_files;
  QTimer m_update;
  QTimer m_offerRetry;
  QJsonObject m_pendingOffers;
  QJsonObject m_options;
  QJsonObject m_transfers;
  QJsonObject m_offers;
  QStringList m_errors;
  QString m_audioStatus;
  QString m_name;
  bool m_server;
  bool m_started = false;
  quint16 m_port = 0;
};
} // namespace deskflow::workspace
