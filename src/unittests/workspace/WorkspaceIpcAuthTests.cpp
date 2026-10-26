/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "arch/Arch.h"
#include "base/Log.h"
#include "deskflow/ipc/IpcServer.h"

#include <QLocalSocket>
#include <QTest>
#include <QUuid>

namespace {

QString uniqueServerName()
{
  // macOS includes its long per-user temporary directory in the Unix socket
  // path, whose total length is limited to roughly 104 bytes.
  return QStringLiteral("dfwa-%1").arg(QUuid::createUuid().toString(QUuid::Id128).left(12));
}

class AuthProbeIpcServer final : public deskflow::core::ipc::IpcServer
{
public:
  explicit AuthProbeIpcServer(const QString &name) : IpcServer(nullptr, name, QStringLiteral("core"))
  {
  }

  using IpcServer::authorizedWorkspaceClient;

private:
  void processCommand(QLocalSocket *socket, const QString &command, const QStringList &) override
  {
    if (command == QStringLiteral("probe")) {
      writeToClientSocket(
          socket, authorizedWorkspaceClient(socket) ? QStringLiteral("authorized") : QStringLiteral("denied")
      );
    }
  }
};

} // namespace

class WorkspaceIpcAuthTests : public QObject
{
  Q_OBJECT

private Q_SLOTS:
  void initTestCase();
  void sameUserLocalClientAuthorized();
  void unattachedSocketRejected();

private:
  Arch m_arch;
  Log m_log;
};

void WorkspaceIpcAuthTests::initTestCase()
{
  m_arch.init();
  m_log.setFilter(LogLevel::Level::Warning);
}

void WorkspaceIpcAuthTests::sameUserLocalClientAuthorized()
{
  const auto name = uniqueServerName();
  AuthProbeIpcServer server(name);
  server.listen();

  QLocalSocket client;
  client.connectToServer(name);
  QTRY_COMPARE_WITH_TIMEOUT(client.state(), QLocalSocket::ConnectedState, 3000);
  QCOMPARE(client.write("probe\n"), qint64(6));
  client.flush();

  // The server authorizes its actual accepted socket. On Windows this proves
  // Qt's native descriptor can identify the client through the named-pipe API.
  QTRY_VERIFY_WITH_TIMEOUT(client.canReadLine(), 3000);
  QCOMPARE(client.readLine(), QByteArray("authorized\n"));

  client.disconnectFromServer();
  QTRY_COMPARE_WITH_TIMEOUT(client.state(), QLocalSocket::UnconnectedState, 3000);
  QVERIFY(!server.authorizedWorkspaceClient(&client));
}

void WorkspaceIpcAuthTests::unattachedSocketRejected()
{
  AuthProbeIpcServer server(uniqueServerName());
  QLocalSocket unattached;
  QVERIFY(!server.authorizedWorkspaceClient(&unattached));
  QVERIFY(!server.authorizedWorkspaceClient(nullptr));
}

QTEST_GUILESS_MAIN(WorkspaceIpcAuthTests)
#include "WorkspaceIpcAuthTests.moc"
