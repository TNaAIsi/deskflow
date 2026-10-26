/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once

#include "FrameCodec.h"
#include <QElapsedTimer>
#include <QHash>
#include <QJsonArray>
#include <QObject>
#include <QPointer>
#include <QSslConfiguration>
#include <QTimer>

class QSslSocket;
class QTcpServer;

namespace deskflow::workspace {

class WorkspaceTransport : public QObject
{
  Q_OBJECT
public:
  enum class Channel
  {
    Control,
    Audio,
    Files
  };
  explicit WorkspaceTransport(QObject *parent = nullptr);
  ~WorkspaceTransport() override;
  bool
  start(bool server, const QString &name, quint16 port, const QStringList &hosts, const QString &bindInterface = {});
  // Optional explicit identities also make portable profiles independent of
  // process-global Settings. Paths can only change while stopped.
  bool setIdentityFiles(const QString &certificate, const QString &trustedServers, const QString &trustedClients);
  quint16 listeningPort() const;
  void stop();
  bool send(
      const QString &peer, const QString &type, const QJsonObject &metadata = {}, const QByteArray &payload = {},
      Channel channel = Channel::Control
  );
  QStringList peers() const;
  QJsonArray peerState() const;
  bool channelReady(const QString &peer, Channel channel) const;

Q_SIGNALS:
  void peerConnected(const QString &peer);
  void peerDisconnected(const QString &peer);
  void channelConnected(const QString &peer, Channel channel);
  void channelDisconnected(const QString &peer, Channel channel);
  void
  messageReceived(const QString &peer, const QString &type, const QJsonObject &metadata, const QByteArray &payload);
  void error(const QString &message);

private:
  struct Link
  {
    FrameCodec codec;
    QString peer;
    Channel channel = Channel::Control;
    bool welcomed = false;
    QByteArray fingerprint;
    QElapsedTimer activity;
  };
  struct Peer
  {
    QString token;
    QByteArray fingerprint;
    QPointer<QSslSocket> control;
    QPointer<QSslSocket> audio;
    QPointer<QSslSocket> files;
  };
  bool loadIdentity();
  bool isPinned(QSslSocket *socket) const;
  void acceptDescriptor(qintptr descriptor);
  void prepareSocket(QSslSocket *socket, Channel channel);
  void connectClient(Channel channel);
  void encrypted(QSslSocket *socket);
  void read(QSslSocket *socket);
  void receiveHandshake(QSslSocket *socket, const QJsonObject &message);
  void disconnected(QSslSocket *socket);
  void retryClient();
  void heartbeat();
  QSslSocket *socketFor(const Peer &peer, Channel channel) const;
  void setSocket(Peer &peer, Channel channel, QSslSocket *socket);
  static QString channelName(Channel channel);
  static bool parseChannel(const QString &name, Channel &channel);
  bool writeFrame(QSslSocket *socket, const QJsonObject &metadata, const QByteArray &payload = {});

  QTcpServer *m_listener = nullptr;
  QHash<QSslSocket *, Link> m_links;
  QHash<QString, Peer> m_peers;
  QSslConfiguration m_ssl;
  QTimer m_retry;
  QTimer m_heartbeat;
  QString m_name;
  QString m_certificateFile;
  QString m_trustedServersFile;
  QString m_trustedClientsFile;
  QStringList m_hosts;
  QString m_clientPeer;
  QString m_clientToken;
  QString m_clientHost;
  quint16 m_port = 0;
  qsizetype m_hostIndex = 0;
  bool m_server = false;
  bool m_running = false;
  bool m_connecting = false;
};

} // namespace deskflow::workspace
