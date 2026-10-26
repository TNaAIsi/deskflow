/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025-2026 Synergy App Ltd
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "IpcServer.h"

#include "base/Log.h"
#include "common/VersionInfo.h"

#include <QLocalServer>
#include <QLocalSocket>

#if defined(Q_OS_WIN)
#include "platform/MSWindowsHandle.h"

#include <Wtsapi32.h>
#include <vector>
#elif defined(Q_OS_MACOS) || defined(Q_OS_LINUX)
#include <sys/types.h>
#include <unistd.h>
#if defined(Q_OS_LINUX)
#include <sys/socket.h>
#endif
#endif

namespace deskflow::core::ipc {

#if defined(Q_OS_WIN)
namespace {

PSID tokenUserSid(HANDLE token, std::vector<BYTE> &buffer)
{
  DWORD size = 0;
  if (GetTokenInformation(token, TokenUser, nullptr, 0, &size) || GetLastError() != ERROR_INSUFFICIENT_BUFFER ||
      size < sizeof(TOKEN_USER)) {
    return nullptr;
  }

  buffer.resize(size);
  if (!GetTokenInformation(token, TokenUser, buffer.data(), size, &size)) {
    return nullptr;
  }

  const auto user = reinterpret_cast<TOKEN_USER *>(buffer.data());
  return IsValidSid(user->User.Sid) ? user->User.Sid : nullptr;
}

} // namespace
#endif

IpcServer::IpcServer(QObject *parent, const QString &serverName, const QString &typeName)
    : QObject(parent),
      m_server{new QLocalServer(this)}, // NOSONAR - Qt memory
      m_serverName(serverName),
      m_typeName(typeName.toUtf8())
{
  // do nothing
}

IpcServer::~IpcServer()
{
  m_server->close();
}

void IpcServer::listen()
{
#if defined(Q_OS_WIN)
  // The service starts the core as SYSTEM in the desktop session. Its GUI must
  // still be able to connect; workspace commands verify the peer separately.
  m_server->setSocketOptions(QLocalServer::WorldAccessOption);
#else
  m_server->setSocketOptions(m_typeName == "core" ? QLocalServer::UserAccessOption : QLocalServer::WorldAccessOption);
#endif

  connect(m_server, &QLocalServer::newConnection, this, &IpcServer::handleNewConnection);
  QLocalServer::removeServer(m_serverName);
  if (m_server->listen(m_serverName)) {
    LOG_DEBUG("%s ipc server listening on: %s", m_typeName.constData(), m_serverName.toUtf8().constData());
  } else {
    LOG_ERR(
        "%s ipc server failed to listen on %s: %s", m_typeName.constData(), m_serverName.toUtf8().constData(),
        qPrintable(m_server->errorString())
    );
  }
}

bool IpcServer::authorizedWorkspaceClient(QLocalSocket *clientSocket) const
{
  if (!clientSocket || clientSocket->state() != QLocalSocket::ConnectedState) {
    return false;
  }

#if defined(Q_OS_WIN)
  // QLocalSocket's native descriptor on Windows is its named-pipe HANDLE.
  // This handle belongs to Qt and must not be closed here.
  const auto pipe = reinterpret_cast<HANDLE>(clientSocket->socketDescriptor());
  ULONG peerProcessId = 0;
  if (pipe == nullptr || pipe == INVALID_HANDLE_VALUE || !GetNamedPipeClientProcessId(pipe, &peerProcessId) ||
      peerProcessId == 0) {
    return false;
  }

  MSWindowsHandle peerProcess(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, peerProcessId));
  if (peerProcess.get() == nullptr) {
    return false;
  }

  HANDLE peerTokenHandle = nullptr;
  if (!OpenProcessToken(peerProcess.get(), TOKEN_QUERY, &peerTokenHandle)) {
    return false;
  }
  MSWindowsHandle peerToken(peerTokenHandle);

  DWORD coreSession = 0;
  DWORD peerSession = 0;
  DWORD size = 0;
  if (!ProcessIdToSessionId(GetCurrentProcessId(), &coreSession) ||
      !GetTokenInformation(peerToken.get(), TokenSessionId, &peerSession, sizeof(peerSession), &size) ||
      size != sizeof(peerSession) || peerSession != coreSession) {
    return false;
  }

  HANDLE coreTokenHandle = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &coreTokenHandle)) {
    return false;
  }
  MSWindowsHandle coreToken(coreTokenHandle);

  std::vector<BYTE> peerUserBuffer;
  std::vector<BYTE> coreUserBuffer;
  const auto peerSid = tokenUserSid(peerToken.get(), peerUserBuffer);
  const auto coreSid = tokenUserSid(coreToken.get(), coreUserBuffer);
  if (!peerSid || !coreSid) {
    return false;
  }

  if (!IsWellKnownSid(coreSid, WinLocalSystemSid)) {
    return EqualSid(peerSid, coreSid) != FALSE;
  }

  // SYSTEM is not the GUI's identity. Resolve the logged-on user of this
  // particular core session on every command, including after a user switch.
  HANDLE interactiveTokenHandle = nullptr;
  if (!WTSQueryUserToken(coreSession, &interactiveTokenHandle)) {
    return false;
  }
  MSWindowsHandle interactiveToken(interactiveTokenHandle);
  std::vector<BYTE> interactiveUserBuffer;
  const auto interactiveSid = tokenUserSid(interactiveToken.get(), interactiveUserBuffer);
  return interactiveSid && EqualSid(peerSid, interactiveSid) != FALSE;
#elif defined(Q_OS_MACOS)
  // macOS does not enforce Unix-domain socket file permissions.
  uid_t peerUser = 0;
  gid_t peerGroup = 0;
  return getpeereid(static_cast<int>(clientSocket->socketDescriptor()), &peerUser, &peerGroup) == 0 &&
         peerUser == geteuid();
#elif defined(Q_OS_LINUX)
  ucred credentials{};
  socklen_t size = sizeof(credentials);
  return getsockopt(static_cast<int>(clientSocket->socketDescriptor()), SOL_SOCKET, SO_PEERCRED, &credentials, &size) ==
             0 &&
         size == sizeof(credentials) && credentials.uid == geteuid();
#else
  // Other platforms use the access restrictions configured by listen().
  return true;
#endif
}

void IpcServer::handleNewConnection()
{
  QLocalSocket *clientSocket = m_server->nextPendingConnection();
  if (!clientSocket) {
    LOG_ERR("%s ipc server failed to get new connection", m_typeName.constData());
    return;
  }

  LOG_DEBUG("%s ipc server got new connection", m_typeName.constData());
  m_clients.insert(clientSocket);

  connect(clientSocket, &QLocalSocket::readyRead, this, &IpcServer::handleReadyRead);
  connect(clientSocket, &QLocalSocket::disconnected, this, &IpcServer::handleDisconnected);
  connect(clientSocket, &QLocalSocket::errorOccurred, this, &IpcServer::handleErrorOccurred);
}

void IpcServer::handleReadyRead()
{
  const auto clientSocket = qobject_cast<QLocalSocket *>(sender());
  LOG_VERBOSE("%s ipc server ready to read data", m_typeName.constData());

  auto &data = m_readBuffers[clientSocket];
  data += clientSocket->readAll();
  if (data.size() > 2 * 1024 * 1024) {
    clientSocket->abort();
    return;
  }
  if (data.isEmpty()) {
    LOG_WARN("%s ipc server got empty message", m_typeName.constData());
    return;
  }

  // each message is delimited by a newline to keep the protocol super simple.
  while (data.contains('\n')) {
    const auto index = data.indexOf('\n');
    QByteArray messageData = data.left(index);
    data.remove(0, index + 1);
    QString message = QString::fromUtf8(messageData);
    processMessage(clientSocket, message);
  }
}

void IpcServer::handleDisconnected()
{
  const auto clientSocket = qobject_cast<QLocalSocket *>(sender());
  LOG_DEBUG("%s ipc server client disconnected", m_typeName.constData());
  m_clients.remove(clientSocket);
  m_readBuffers.remove(clientSocket);
  clientSocket->deleteLater();
}

void IpcServer::handleErrorOccurred()
{
  const auto clientSocket = qobject_cast<QLocalSocket *>(sender());
  if (clientSocket->error() == QLocalSocket::PeerClosedError) {
    LOG_DEBUG("%s ipc server client closed connection", m_typeName.constData());
  } else {
    LOG_ERR("%s ipc server client error: %s", m_typeName.constData(), clientSocket->errorString().toUtf8().constData());
  }

  m_clients.remove(clientSocket);
  m_readBuffers.remove(clientSocket);
  clientSocket->deleteLater();
}

void IpcServer::processMessage(QLocalSocket *clientSocket, const QString &message)
{
  LOG_VERBOSE("%s ipc server got message: %s", m_typeName.constData(), message.toUtf8().constData());
  const auto parts = message.split('=');
  if (parts.isEmpty()) {
    LOG_ERR("%s ipc server got invalid message: %s", m_typeName.constData(), message.toUtf8().constData());
    writeToClientSocket(clientSocket, QStringLiteral("error"));
    return;
  }

  if (const auto &command = parts.at(0); command == QStringLiteral("hello")) {
    if (parts.size() < 2) {
      LOG_ERR("%s ipc client hello missing version", m_typeName.constData());
      writeToClientSocket(clientSocket, "error=missing version");
      clientSocket->flush();
      clientSocket->disconnectFromServer();
      return;
    }

    const auto versionId = QStringLiteral("%1+%2").arg(kVersion, kVersionGitSha);
    const auto clientVersion = parts.at(1);
    LOG_DEBUG("%s ipc server got hello message (version: %s)", m_typeName.constData(), versionId.toUtf8().constData());

    if (clientVersion != versionId) {
      LOG_WARN(
          "%s ipc client version mismatch (client: %s, server: %s)", m_typeName.constData(),
          clientVersion.toUtf8().constData(), versionId.toUtf8().constData()
      );
      writeToClientSocket(clientSocket, QStringLiteral("versionMismatch=%1").arg(versionId));
      clientSocket->flush();
      return;
    }

    LOG_DEBUG("%s ipc server sending hello back", m_typeName.constData());
    writeToClientSocket(clientSocket, QStringLiteral("hello=%1").arg(versionId));

    // Replay messages that were queued before any clients connected.
    LOG_VERBOSE("ipc server replaying %d pending messages", m_pendingMessages.size());
    for (const auto &pending : std::as_const(m_pendingMessages)) {
      LOG_VERBOSE("%s ipc server replaying: %s", m_typeName.constData(), pending.toUtf8().constData());
      writeToClientSocket(clientSocket, pending);
    }
    m_pendingMessages.clear();
  } else if (command == QStringLiteral("noop")) {
    LOG_DEBUG("%s ipc server got noop message", m_typeName.constData());
    writeToClientSocket(clientSocket, QStringLiteral("ok"));
  } else {
    processCommand(clientSocket, command, parts);
  }

  clientSocket->flush();
}

void IpcServer::broadcastCommand(const QString &command, const QString &args)
{
  const auto message = args.isEmpty() ? command : QStringLiteral("%1=%2").arg(command, args);

  if (m_clients.isEmpty()) {
    LOG_VERBOSE(
        "%s ipc server has no clients, message queued: %s", m_typeName.constData(), message.toUtf8().constData()
    );
    // A GUI may be closed for hours while transfers continue. Retain the last
    // snapshot per command rather than accumulating every progress update.
    const auto prefix = command + '=';
    for (qsizetype i = m_pendingMessages.size(); i > 0; --i)
      if (m_pendingMessages.at(i - 1).startsWith(prefix) || m_pendingMessages.at(i - 1) == command)
        m_pendingMessages.removeAt(i - 1);
    if (m_pendingMessages.size() >= 64)
      m_pendingMessages.removeFirst();
    m_pendingMessages.append(message);
    return;
  }

  LOG_VERBOSE(
      "%s ipc server broadcasting message to %d clients: %s", m_typeName.constData(), m_clients.size(),
      message.toUtf8().constData()
  );
  for (auto *client : std::as_const(m_clients)) {
    writeToClientSocket(client, message);
    client->flush();
  }
}

void IpcServer::writeToClientSocket(QLocalSocket *&clientSocket, const QString &message) const
{
  QByteArray messageData = message.toUtf8() + '\n';
  qint64 bytesWritten = clientSocket->write(messageData);
  if (bytesWritten != messageData.size()) {
    LOG_ERR("%s ipc server failed to write full message to client socket", m_typeName.constData());
  } else {
    LOG_VERBOSE(
        "%s ipc server wrote message to client socket: %s", m_typeName.constData(), message.toUtf8().constData()
    );
  }
}

} // namespace deskflow::core::ipc
