/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "arch/Arch.h"
#include "base/Log.h"
#include "deskflow/ipc/CoreIpcServer.h"
#include "deskflow/ipc/IpcServer.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QSignalSpy>
#include <QTest>
#include <QUuid>

namespace {

QString uniqueServerName()
{
  // macOS includes its long per-user temporary directory in the Unix socket
  // path, whose total length is limited to roughly 104 bytes.
  return QStringLiteral("dfwa-%1").arg(QUuid::createUuid().toString(QUuid::Id128).left(12));
}

QByteArray runtimeTopologyPayload()
{
  const QJsonObject topology{{"version", 1}, {"enabled", true}, {"displays", QJsonArray{}}};
  const QJsonObject command{{"action", "runtimeTopology"}, {"topology", topology}};
  return QJsonDocument(command)
      .toJson(QJsonDocument::Compact)
      .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
}

bool sendWorkspaceCommand(QLocalSocket &client, const QByteArray &encoded)
{
  const auto message = QByteArray("workspace=") + encoded + '\n';
  const auto written = client.write(message);
  client.flush();
  return written == message.size();
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
  void workspaceProviderDisconnectReported();
  void onlyCurrentWorkspaceProviderDisconnectReported();

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

void WorkspaceIpcAuthTests::workspaceProviderDisconnectReported()
{
  using deskflow::core::ipc::CoreIpcServer;
  const auto name = uniqueServerName();
  CoreIpcServer server(nullptr, name);
  QCOMPARE(&CoreIpcServer::instance(), &server);
  QSignalSpy commands(&server, &CoreIpcServer::workspaceCommandReceived);
  QSignalSpy disconnected(&server, &CoreIpcServer::workspacePeerDisconnected);
  server.listen();

  QLocalSocket provider;
  provider.connectToServer(name);
  QTRY_COMPARE_WITH_TIMEOUT(provider.state(), QLocalSocket::ConnectedState, 3000);
  const auto payload = runtimeTopologyPayload();
  QVERIFY(sendWorkspaceCommand(provider, payload));
  QTRY_COMPARE_WITH_TIMEOUT(commands.count(), 1, 3000);
  QCOMPARE(commands.first().first().toString(), QString::fromLatin1(payload));
  QCOMPARE(disconnected.count(), 0);

  // Repeated topology snapshots still track one disconnect for this socket.
  QVERIFY(sendWorkspaceCommand(provider, payload));
  QTRY_COMPARE_WITH_TIMEOUT(commands.count(), 2, 3000);
  provider.disconnectFromServer();
  QTRY_COMPARE_WITH_TIMEOUT(disconnected.count(), 1, 3000);
}

void WorkspaceIpcAuthTests::onlyCurrentWorkspaceProviderDisconnectReported()
{
  using deskflow::core::ipc::CoreIpcServer;
  const auto name = uniqueServerName();
  CoreIpcServer server(nullptr, name);
  QSignalSpy commands(&server, &CoreIpcServer::workspaceCommandReceived);
  QSignalSpy disconnected(&server, &CoreIpcServer::workspacePeerDisconnected);
  server.listen();

  QLocalSocket firstProvider;
  QLocalSocket secondProvider;
  QLocalSocket legacyClient;
  for (auto *client : {&firstProvider, &secondProvider, &legacyClient}) {
    client->connectToServer(name);
    QTRY_COMPARE_WITH_TIMEOUT(client->state(), QLocalSocket::ConnectedState, 3000);
  }

  const auto payload = runtimeTopologyPayload();
  QVERIFY(sendWorkspaceCommand(firstProvider, payload));
  QTRY_COMPARE_WITH_TIMEOUT(commands.count(), 1, 3000);
  QVERIFY(sendWorkspaceCommand(secondProvider, payload));
  QTRY_COMPARE_WITH_TIMEOUT(commands.count(), 2, 3000);
  QVERIFY(sendWorkspaceCommand(firstProvider, payload));
  QTRY_COMPARE_WITH_TIMEOUT(commands.count(), 3, 3000);

  QCOMPARE(legacyClient.write("noop\n"), qint64(5));
  legacyClient.flush();
  QTRY_VERIFY_WITH_TIMEOUT(legacyClient.canReadLine(), 3000);
  QCOMPARE(legacyClient.readLine(), QByteArray("ok\n"));
  secondProvider.disconnectFromServer();
  legacyClient.disconnectFromServer();
  QTest::qWait(20);
  QCOMPARE(disconnected.count(), 0);

  firstProvider.disconnectFromServer();
  QTRY_COMPARE_WITH_TIMEOUT(disconnected.count(), 1, 3000);

  QLocalSocket reconnectedProvider;
  reconnectedProvider.connectToServer(name);
  QTRY_COMPARE_WITH_TIMEOUT(reconnectedProvider.state(), QLocalSocket::ConnectedState, 3000);
  QVERIFY(sendWorkspaceCommand(reconnectedProvider, payload));
  QTRY_COMPARE_WITH_TIMEOUT(commands.count(), 4, 3000);
  reconnectedProvider.disconnectFromServer();
  QTRY_COMPARE_WITH_TIMEOUT(disconnected.count(), 2, 3000);
}

QTEST_GUILESS_MAIN(WorkspaceIpcAuthTests)
#include "WorkspaceIpcAuthTests.moc"
