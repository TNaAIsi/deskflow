/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "WorkspaceTransport.h"
#include "common/Settings.h"
#include "net/FingerprintDatabase.h"

#include <QFile>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSslCertificate>
#include <QSslKey>
#include <QSslSocket>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUuid>
#include <functional>

namespace deskflow::workspace {
namespace {
class Listener final : public QTcpServer
{
public:
  using QTcpServer::QTcpServer;
  std::function<void(qintptr)> accepted;

protected:
  void incomingConnection(qintptr descriptor) override
  {
    accepted(descriptor);
  }
};
} // namespace

WorkspaceTransport::WorkspaceTransport(QObject *parent) : QObject(parent)
{
  m_retry.setInterval(2500);
  connect(&m_retry, &QTimer::timeout, this, &WorkspaceTransport::retryClient);
  m_heartbeat.setInterval(5000);
  connect(&m_heartbeat, &QTimer::timeout, this, &WorkspaceTransport::heartbeat);
}

WorkspaceTransport::~WorkspaceTransport()
{
  stop();
}

bool WorkspaceTransport::loadIdentity()
{
  QFile file(
      m_certificateFile.isEmpty() ? Settings::value(Settings::Security::Certificate).toString() : m_certificateFile
  );
  if (!file.open(QIODevice::ReadOnly))
    return false;
  const auto pem = file.readAll();
  const auto certificates = QSslCertificate::fromData(pem);
  QSslKey key(pem, QSsl::Rsa, QSsl::Pem, QSsl::PrivateKey);
  if (certificates.isEmpty() || key.isNull())
    return false;
  m_ssl = QSslConfiguration::defaultConfiguration();
  m_ssl.setProtocol(QSsl::TlsV1_2OrLater);
  m_ssl.setPeerVerifyMode(QSslSocket::VerifyPeer);
  m_ssl.setLocalCertificateChain(certificates);
  m_ssl.setPrivateKey(key);
  return QSslSocket::supportsSsl();
}

bool WorkspaceTransport::isPinned(QSslSocket *socket) const
{
  if (socket->peerCertificate().isNull())
    return false;
  FingerprintDatabase database;
  const auto path = m_server ? m_trustedClientsFile : m_trustedServersFile;
  database.read(path.isEmpty() ? (m_server ? Settings::tlsTrustedClientsDb() : Settings::tlsTrustedServersDb()) : path);
  const Fingerprint fingerprint{
      QCryptographicHash::Sha256, socket->peerCertificate().digest(QCryptographicHash::Sha256)
  };
  return database.isTrusted(fingerprint);
}

bool WorkspaceTransport::start(
    bool server, const QString &name, quint16 port, const QStringList &hosts, const QString &bindInterface
)
{
  stop();
  m_server = server;
  m_name = name;
  m_port = port;
  m_hosts = hosts;
  if (!loadIdentity()) {
    Q_EMIT error(tr("Workspace needs a valid Deskflow TLS certificate and SSL backend."));
    return false;
  }
  m_running = true;
  if (server) {
    auto *listener = new Listener(this);
    m_listener = listener;
    listener->accepted = [this](qintptr descriptor) { acceptDescriptor(descriptor); };
    QHostAddress bindAddress = QHostAddress::Any;
    if (!bindInterface.isEmpty() && !bindAddress.setAddress(bindInterface)) {
      Q_EMIT error(tr("Workspace bind address must be an IP address."));
      stop();
      return false;
    }
    if (!listener->listen(bindAddress, port)) {
      Q_EMIT error(tr("Workspace port %1: %2").arg(port).arg(listener->errorString()));
      stop();
      return false;
    }
  } else {
    m_retry.start();
    retryClient();
  }
  m_heartbeat.start();
  return true;
}

bool WorkspaceTransport::setIdentityFiles(
    const QString &certificate, const QString &trustedServers, const QString &trustedClients
)
{
  if (m_running) {
    return false;
  }
  m_certificateFile = certificate;
  m_trustedServersFile = trustedServers;
  m_trustedClientsFile = trustedClients;
  return true;
}

quint16 WorkspaceTransport::listeningPort() const
{
  return m_listener != nullptr ? m_listener->serverPort() : 0;
}

void WorkspaceTransport::stop()
{
  m_running = false;
  m_retry.stop();
  m_heartbeat.stop();
  if (m_listener) {
    m_listener->close();
    m_listener->deleteLater();
    m_listener = nullptr;
  }
  const auto sockets = m_links.keys();
  for (auto *socket : sockets) {
    disconnect(socket, nullptr, this, nullptr);
    socket->abort();
    socket->deleteLater();
  }
  const auto names = m_peers.keys();
  m_links.clear();
  m_peers.clear();
  m_clientPeer.clear();
  m_clientToken.clear();
  m_connecting = false;
  for (const auto &name : names)
    Q_EMIT peerDisconnected(name);
}

QString WorkspaceTransport::channelName(Channel channel)
{
  switch (channel) {
  case Channel::Audio:
    return QStringLiteral("audio");
  case Channel::Files:
    return QStringLiteral("files");
  default:
    return QStringLiteral("control");
  }
}

bool WorkspaceTransport::parseChannel(const QString &name, Channel &channel)
{
  if (name == "control")
    channel = Channel::Control;
  else if (name == "audio")
    channel = Channel::Audio;
  else if (name == "files")
    channel = Channel::Files;
  else
    return false;
  return true;
}

QSslSocket *WorkspaceTransport::socketFor(const Peer &peer, Channel channel) const
{
  switch (channel) {
  case Channel::Audio:
    return peer.audio;
  case Channel::Files:
    return peer.files;
  default:
    return peer.control;
  }
}

void WorkspaceTransport::setSocket(Peer &peer, Channel channel, QSslSocket *socket)
{
  switch (channel) {
  case Channel::Audio:
    peer.audio = socket;
    break;
  case Channel::Files:
    peer.files = socket;
    break;
  default:
    peer.control = socket;
    break;
  }
}

void WorkspaceTransport::prepareSocket(QSslSocket *socket, Channel channel)
{
  socket->setSslConfiguration(m_ssl);
  socket->setReadBufferSize(FrameCodec::MaxFrame + 8);
  socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
  socket->setSocketOption(QAbstractSocket::KeepAliveOption, 1);
  Link link;
  link.channel = channel;
  link.activity.start();
  m_links.insert(socket, std::move(link));
  connect(socket, &QSslSocket::encrypted, this, [this, socket] { encrypted(socket); });
  connect(socket, &QSslSocket::readyRead, this, [this, socket] { read(socket); });
  connect(socket, &QSslSocket::disconnected, this, [this, socket] { disconnected(socket); });
  connect(socket, &QSslSocket::errorOccurred, this, [this, socket](QAbstractSocket::SocketError) {
    if (!m_server && m_links.contains(socket) && m_links[socket].channel == Channel::Control) {
      m_connecting = false;
      Q_EMIT error(tr("Workspace connection failed: %1").arg(socket->errorString()));
    }
    socket->abort();
    if (m_links.contains(socket))
      disconnected(socket);
  });
  connect(socket, &QSslSocket::sslErrors, this, [this, socket](const QList<QSslError> &errors) {
    if (!isPinned(socket))
      return;
    for (const auto &error : errors) {
      const auto code = error.error();
      if (code != QSslError::SelfSignedCertificate && code != QSslError::SelfSignedCertificateInChain &&
          code != QSslError::CertificateUntrusted && code != QSslError::HostNameMismatch)
        return;
    }
    // Existing Deskflow certificate fingerprints are the identity authority.
    // Other certificate errors (expiry, missing cert, etc.) remain fatal.
    socket->ignoreSslErrors(errors);
  });
  QTimer::singleShot(10000, socket, [this, socket] {
    if (m_links.contains(socket) && !m_links[socket].welcomed) {
      socket->abort();
      if (m_links.contains(socket))
        disconnected(socket);
    }
  });
}

void WorkspaceTransport::acceptDescriptor(qintptr descriptor)
{
  // Bound unauthenticated handshakes as well as established auxiliary links.
  if (m_links.size() >= 96) {
    QTcpSocket rejected;
    if (rejected.setSocketDescriptor(descriptor)) {
      rejected.abort();
    }
    return;
  }
  auto *socket = new QSslSocket(this);
  prepareSocket(socket, Channel::Control);
  if (!socket->setSocketDescriptor(descriptor)) {
    disconnected(socket);
    return;
  }
  socket->startServerEncryption();
}

void WorkspaceTransport::retryClient()
{
  if (!m_running || m_server || m_hosts.isEmpty())
    return;
  if (m_clientPeer.isEmpty()) {
    if (!m_connecting)
      connectClient(Channel::Control);
    return;
  }
  const auto peer = m_peers.value(m_clientPeer);
  for (const auto channel : {Channel::Audio, Channel::Files}) {
    bool connecting = false;
    for (auto it = m_links.cbegin(); it != m_links.cend(); ++it)
      connecting |= it->channel == channel;
    if (!socketFor(peer, channel) && !connecting)
      connectClient(channel);
  }
}

void WorkspaceTransport::connectClient(Channel channel)
{
  auto *socket = new QSslSocket(this);
  prepareSocket(socket, channel);
  QString host = m_clientHost;
  quint16 port = m_port;
  if (channel == Channel::Control) {
    m_connecting = true;
    host = m_hosts.at(m_hostIndex++ % m_hosts.size()).trimmed();
    // Core host fields accept hostname:port and [IPv6]:port. Workspace
    // follows that server's input port plus one.
    if (host.startsWith('[')) {
      const auto end = host.indexOf(']');
      if (end > 0) {
        bool ok = false;
        const auto custom = host.mid(end + 2).toUShort(&ok);
        if (ok && custom < 65535)
          port = custom + 1;
        host = host.mid(1, end - 1);
      }
    } else if (host.count(':') == 1) {
      bool ok = false;
      const auto custom = host.section(':', 1).toUShort(&ok);
      if (ok && custom < 65535)
        port = custom + 1;
      host = host.section(':', 0, 0);
    }
    m_clientHost = host;
  } else if (m_peers.contains(m_clientPeer)) {
    // Match the actual accepted control endpoint, including a custom port.
    auto *control = m_peers[m_clientPeer].control.data();
    if (control) {
      host = control->peerAddress().toString();
      port = control->peerPort();
    }
  }
  socket->connectToHostEncrypted(host, port);
}

void WorkspaceTransport::encrypted(QSslSocket *socket)
{
  if (!m_links.contains(socket))
    return;
  if (!isPinned(socket)) {
    if (!m_server) {
      Q_EMIT error(tr("Workspace peer certificate is not trusted by Deskflow."));
    }
    socket->abort();
    return;
  }
  auto &link = m_links[socket];
  link.activity.restart();
  link.fingerprint = socket->peerCertificate().digest(QCryptographicHash::Sha256);
  if (!m_server) {
    writeFrame(
        socket, {{"type", "$hello"},
                 {"version", 1},
                 {"device", m_name},
                 {"channel", channelName(link.channel)},
                 {"token", m_clientToken}}
    );
  }
}

bool WorkspaceTransport::writeFrame(QSslSocket *socket, const QJsonObject &metadata, const QByteArray &payload)
{
  const auto frame = FrameCodec::encode(metadata, payload);
  return !frame.isEmpty() && socket->write(frame) == frame.size();
}

void WorkspaceTransport::read(QSslSocket *socket)
{
  if (!m_links.contains(socket) || !socket->isEncrypted())
    return;
  m_links[socket].activity.restart();
  QList<WorkspaceFrame> frames;
  if (!m_links[socket].codec.append(socket->readAll(), frames)) {
    socket->abort();
    return;
  }
  for (const auto &frame : frames) {
    if (!m_links.contains(socket))
      return;
    const auto link = m_links.value(socket);
    if (!link.welcomed) {
      if (!frame.payload.isEmpty()) {
        socket->abort();
        return;
      }
      receiveHandshake(socket, frame.metadata);
      continue;
    }
    const auto type = frame.metadata.value("type").toString();
    const bool appropriate =
        (link.channel == Channel::Audio && type == "audio") ||
        (link.channel == Channel::Files && type.startsWith("files.")) ||
        (link.channel == Channel::Control &&
         (type == "displays" || type == "layout" || type == "identify" || type == "ping" || type == "pong"));
    if (!appropriate) {
      socket->abort();
      return;
    }
    if (type == "ping") {
      writeFrame(socket, {{"type", "pong"}});
      continue;
    }
    if (type == "pong")
      continue;
    Q_EMIT messageReceived(link.peer, type, frame.metadata, frame.payload);
  }
}

void WorkspaceTransport::receiveHandshake(QSslSocket *socket, const QJsonObject &message)
{
  const auto type = message.value("type").toString();
  const auto name = message.value("device").toString();
  Channel channel;
  if (message.value("version").toInt() != 1 || name.isEmpty() || name.size() > 255 || name == m_name ||
      !parseChannel(message.value("channel").toString(), channel)) {
    socket->abort();
    return;
  }
  auto &link = m_links[socket];
  if (m_server && type == "$hello") {
    if (channel == Channel::Control) {
      if (m_peers.contains(name)) {
        // A duplicate identity may not replace a live session.
        socket->abort();
        return;
      }
      Peer peer;
      peer.token = QUuid::createUuid().toString(QUuid::Id128);
      peer.fingerprint = link.fingerprint;
      peer.control = socket;
      m_peers.insert(name, peer);
    } else {
      const auto peer = m_peers.value(name);
      if (peer.token.isEmpty() || peer.token != message.value("token").toString() ||
          peer.fingerprint != link.fingerprint || socketFor(peer, channel)) {
        socket->abort();
        return;
      }
      setSocket(m_peers[name], channel, socket);
    }
    link.peer = name;
    link.channel = channel;
    link.welcomed = true;
    writeFrame(
        socket, {{"type", "$welcome"},
                 {"version", 1},
                 {"device", m_name},
                 {"channel", channelName(channel)},
                 {"token", m_peers[name].token}}
    );
  } else if (!m_server && type == "$welcome" && channel == link.channel) {
    if (channel == Channel::Control) {
      const auto token = message.value("token").toString();
      if (token.size() != 32) {
        socket->abort();
        return;
      }
      m_clientPeer = name;
      m_clientToken = token;
      m_connecting = false;
      Peer peer;
      peer.token = token;
      peer.fingerprint = link.fingerprint;
      peer.control = socket;
      m_peers.insert(name, peer);
    } else {
      if (name != m_clientPeer || m_peers.value(name).fingerprint != link.fingerprint ||
          message.value("token").toString() != m_clientToken) {
        socket->abort();
        return;
      }
      setSocket(m_peers[name], channel, socket);
    }
    link.peer = name;
    link.welcomed = true;
  } else {
    socket->abort();
    return;
  }
  if (channel == Channel::Control) {
    Q_EMIT peerConnected(name);
    if (!m_server)
      QTimer::singleShot(0, this, &WorkspaceTransport::retryClient);
  }
  Q_EMIT channelConnected(name, channel);
}

void WorkspaceTransport::disconnected(QSslSocket *socket)
{
  if (!m_links.contains(socket))
    return;
  const auto link = m_links.take(socket);
  if (!m_server && link.channel == Channel::Control)
    m_connecting = false;
  if (!link.peer.isEmpty() && m_peers.contains(link.peer)) {
    if (link.channel == Channel::Control) {
      const auto peer = m_peers.take(link.peer);
      if (!m_server) {
        m_clientPeer.clear();
        m_clientToken.clear();
      }
      for (auto *other : {peer.audio.data(), peer.files.data()})
        if (other)
          other->abort();
      Q_EMIT peerDisconnected(link.peer);
    } else {
      setSocket(m_peers[link.peer], link.channel, nullptr);
    }
  }
  socket->deleteLater();
}

bool WorkspaceTransport::send(
    const QString &peer, const QString &type, const QJsonObject &metadata, const QByteArray &payload, Channel channel
)
{
  const auto it = m_peers.constFind(peer);
  if (it == m_peers.cend())
    return false;
  auto *socket = socketFor(*it, channel);
  if (!socket || !m_links.contains(socket) || !m_links[socket].welcomed)
    return false;
  const qint64 limit = channel == Channel::Audio ? 24 * 1024 : channel == Channel::Files ? 512 * 1024 : 256 * 1024;
  auto object = metadata;
  object.insert("type", type);
  const auto frame = FrameCodec::encode(object, payload);
  if (frame.isEmpty())
    return false;
  const auto pending = socket->bytesToWrite() + socket->encryptedBytesToWrite();
  // A large manifest is still bounded by FrameCodec::MaxFrame. Allow one on
  // an empty file socket, otherwise enforce the complete queued-frame limit.
  const bool standaloneManifest = channel == Channel::Files && pending == 0;
  if (!standaloneManifest && pending + frame.size() > limit)
    return false;
  return socket->write(frame) == frame.size();
}

QStringList WorkspaceTransport::peers() const
{
  return m_peers.keys();
}

bool WorkspaceTransport::channelReady(const QString &peer, Channel channel) const
{
  auto *socket = socketFor(m_peers.value(peer), channel);
  return socket && m_links.contains(socket) && m_links.value(socket).welcomed;
}

QJsonArray WorkspaceTransport::peerState() const
{
  QJsonArray result;
  for (auto it = m_peers.cbegin(); it != m_peers.cend(); ++it)
    result.append(
        QJsonObject{
            {"name", it.key()},
            {"audio", channelReady(it.key(), Channel::Audio)},
            {"files", channelReady(it.key(), Channel::Files)}
        }
    );
  return result;
}

void WorkspaceTransport::heartbeat()
{
  for (const auto &name : peers()) {
    auto *socket = socketFor(m_peers.value(name), Channel::Control);
    if (socket != nullptr && m_links.contains(socket) && m_links.value(socket).activity.elapsed() > 20000) {
      socket->abort();
    } else {
      send(name, "ping");
    }
  }
}

} // namespace deskflow::workspace
