/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "arch/Arch.h"
#include "base/Log.h"
#include "common/VersionInfo.h"
#include "deskflow/ipc/IpcServer.h"
#include "gui/core/CoreProcess.h"
#include "gui/ipc/CoreIpcClient.h"

#include <QCoreApplication>
#include <QLocalSocket>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>

#include <memory>

namespace {
QString socketName()
{
  return QStringLiteral("dfcs-%1").arg(QUuid::createUuid().toString(QUuid::Id128).left(12));
}
QByteArray hello(bool background = false)
{
  return QByteArray("hello=") + kVersion + '+' + kVersionGitSha + (background ? "=background\n" : "\n");
}
bool received(const QSignalSpy &spy, const QString &command, const QString &args = {})
{
  for (const auto &item : spy)
    if (item.at(0).toString() == command && (args.isNull() || item.at(1).toString() == args))
      return true;
  return false;
}
class StatusSource final : public deskflow::core::ipc::IpcServer
{
public:
  explicit StatusSource(const QString &name) : IpcServer(nullptr, name, QStringLiteral("core"))
  {
  }

private:
  void processCommand(QLocalSocket *socket, const QString &command, const QStringList &) override
  {
    if (command == "stop") {
      writeToClientSocket(socket, QStringLiteral("ok"));
      broadcastCommand(QStringLiteral("bye"));
    }
  }
};
} // namespace

namespace deskflow::gui {
class CoreProcessStatusTests : public QObject
{
  Q_OBJECT
private:
  Arch m_arch;
  Log m_log;
  QTemporaryDir m_profile;
private Q_SLOTS:
  void initTestCase()
  {
    m_arch.init();
    m_log.setFilter(LogLevel::Level::Warning);
    QVERIFY(m_profile.isValid());
    qputenv("XDG_CONFIG_HOME", m_profile.path().toUtf8());
    qputenv("XDG_STATE_HOME", m_profile.path().toUtf8());
    Settings::setSettingsFile(m_profile.filePath("Deskflow.conf"));
    Settings::setStateFile(m_profile.filePath("Deskflow.state"));
  }
  void lateObserverGetsCurrentStateEvenWithHelperConnected()
  {
    // Every test is isolated from the user's running Deskflow core and from
    // other test executables.
    const auto address = socketName();
    StatusSource actual(address);
    actual.listen();
    QLocalSocket helper;
    helper.connectToServer(address);
    QTRY_COMPARE(helper.state(), QLocalSocket::ConnectedState);
    helper.write(hello(true));
    helper.flush();
    QByteArray helperData;
    QTRY_VERIFY((helperData += helper.readAll()).contains("hello="));
    actual.broadcastCommand("connectionState", "Connected");
    actual.broadcastCommand("connectedClients", "Mac");
    actual.broadcastCommand("secureSocket", "TLSv1.3");
    actual.broadcastCommand("nativeKeyboardMode", "physical");
    QTRY_VERIFY((helperData += helper.readAll()).contains("nativeKeyboardMode=physical\n"));

    QLocalSocket status;
    status.connectToServer(address);
    QTRY_COMPARE(status.state(), QLocalSocket::ConnectedState);
    status.write(hello());
    status.flush();
    QByteArray statusData;
    QTRY_VERIFY((statusData += status.readAll()).contains("nativeKeyboardMode=physical\n"));
    QVERIFY(statusData.contains("connectionState=Connected\n"));
    QVERIFY(statusData.contains("connectedClients=Mac\n"));
    QVERIFY(statusData.contains("nativeKeyboardMode=physical\n"));
    QVERIFY(!statusData.contains("peerFingerprint"));
    QVERIFY(!statusData.contains("connectionRefused"));
  }
  void reconnectGetsLatestDisconnectedSnapshotWithoutOldTlsOrClients()
  {
    const auto address = socketName();
    StatusSource server(address);
    server.listen();
    server.broadcastCommand("connectionState", "Connected");
    server.broadcastCommand("connectedClients", "Mac");
    server.broadcastCommand("secureSocket", "TLSv1.3");
    server.broadcastCommand("nativeKeyboardMode", "physical");
    QLocalSocket first;
    first.connectToServer(address);
    QTRY_COMPARE(first.state(), QLocalSocket::ConnectedState);
    first.write(hello());
    first.flush();
    QByteArray firstData;
    QTRY_VERIFY((firstData += first.readAll()).contains("nativeKeyboardMode=physical\n"));
    first.disconnectFromServer();
    QTRY_COMPARE(first.state(), QLocalSocket::UnconnectedState);
    server.broadcastCommand("connectionState", "Disconnected");
    QLocalSocket second;
    second.connectToServer(address);
    QTRY_COMPARE(second.state(), QLocalSocket::ConnectedState);
    second.write(hello());
    second.flush();
    QByteArray secondData;
    QTRY_VERIFY((secondData += second.readAll()).contains("nativeKeyboardMode=unavailable\n"));
    QVERIFY(secondData.contains("connectionState=Disconnected\n"));
    QVERIFY(!secondData.contains("secureSocket"));
    QVERIFY(!secondData.contains("connectedClients=Mac"));
    QVERIFY(secondData.contains("nativeKeyboardMode=unavailable\n"));
  }
  void interactiveEventsWaitForInterfaceAndAreNotReplayedAgain()
  {
    const auto address = socketName();
    StatusSource server(address);
    server.listen();
    ipc::CoreIpcClient background(nullptr, address, ipc::IpcClient::Purpose::Background);
    QSignalSpy backgroundMessages(&background, &ipc::CoreIpcClient::commandReceived);
    background.connectToServer();
    QTRY_VERIFY(background.isConnected());
    server.broadcastCommand("peerFingerprint", "one-time-fingerprint");
    server.broadcastCommand("connectionRefused", "UnknownClient");
    server.broadcastCommand("connectionState", "Connected");
    QTRY_VERIFY(received(backgroundMessages, "connectionState", "Connected"));
    QVERIFY(!received(backgroundMessages, "peerFingerprint"));
    QVERIFY(!received(backgroundMessages, "connectionRefused"));

    ipc::CoreIpcClient interface(nullptr, address);
    QSignalSpy interfaceMessages(&interface, &ipc::CoreIpcClient::commandReceived);
    interface.connectToServer();
    QTRY_VERIFY(received(interfaceMessages, "connectionRefused", "UnknownClient"));
    QVERIFY(received(interfaceMessages, "peerFingerprint", "one-time-fingerprint"));
    QVERIFY(received(interfaceMessages, "connectionState", "Connected"));

    ipc::CoreIpcClient laterInterface(nullptr, address);
    QSignalSpy laterMessages(&laterInterface, &ipc::CoreIpcClient::commandReceived);
    laterInterface.connectToServer();
    QTRY_VERIFY(received(laterMessages, "connectionState", "Connected"));
    QVERIFY(!received(laterMessages, "peerFingerprint"));
    QVERIFY(!received(laterMessages, "connectionRefused"));
    server.broadcastCommand("peerFingerprint", "live-fingerprint");
    QTRY_VERIFY(received(interfaceMessages, "peerFingerprint", "live-fingerprint"));
    QVERIFY(!received(backgroundMessages, "peerFingerprint"));
  }
  void invalidVersionGetsNoConnectionSnapshot()
  {
    const auto address = socketName();
    StatusSource server(address);
    server.listen();
    server.broadcastCommand("connectionState", "Connected");
    QLocalSocket wrong;
    wrong.connectToServer(address);
    QTRY_COMPARE(wrong.state(), QLocalSocket::ConnectedState);
    wrong.write("hello=invalid-version\n");
    wrong.flush();
    QByteArray data;
    QTRY_VERIFY((data += wrong.readAll()).contains("versionMismatch="));
    QVERIFY(!data.contains("connectionState"));
    server.broadcastCommand("connectionState", "Disconnected");
    QTest::qWait(20);
    data += wrong.readAll();
    QVERIFY(!data.contains("connectionState"));
  }
  void statusObserverUsesKeyboardSnapshotAndRetriesLateIpcStartup()
  {
    const auto address = socketName();
    ServerConfig config;
    CoreProcess process(config, address);
    process.setMode(Settings::CoreMode::Client);
    process.setConnectionState(deskflow::core::ConnectionState::Connecting);
    process.setProcessState(deskflow::core::ProcessState::Started);
    QTRY_VERIFY_WITH_TIMEOUT(process.m_coreIpcRetryTimer.isActive(), 3000);
    QCOMPARE(process.connectionState(), deskflow::core::ConnectionState::Connecting);
    StatusSource server(address);
    server.listen();
    server.broadcastCommand("connectionState", "Connected");
    QTRY_COMPARE_WITH_TIMEOUT(process.connectionState(), deskflow::core::ConnectionState::Connected, 5000);
    QVERIFY(process.m_coreIpcClient && process.m_coreIpcClient->isConnected());
    process.setProcessState(deskflow::core::ProcessState::Stopped);
  }
  void mismatchedVersionCanStillReceiveUpgradeShutdown()
  {
    const auto address = socketName();
    StatusSource server(address);
    server.listen();
    server.broadcastCommand("connectionState", "Connected");
    QLocalSocket oldClient;
    oldClient.connectToServer(address);
    QTRY_COMPARE(oldClient.state(), QLocalSocket::ConnectedState);
    oldClient.write("hello=old-version\n");
    oldClient.flush();
    QByteArray data;
    QTRY_VERIFY((data += oldClient.readAll()).contains("versionMismatch="));
    oldClient.write("stop\n");
    oldClient.flush();
    QTRY_VERIFY((data += oldClient.readAll()).contains("bye\n"));
    QVERIFY(!data.contains("connectionState"));
  }
  void liveIpcReconnectRestoresAuthoritativeKeyboardState()
  {
    const auto address = socketName();
    auto server = std::make_unique<StatusSource>(address);
    server->listen();
    server->broadcastCommand("connectionState", "Connected");
    ServerConfig config;
    CoreProcess process(config, address);
    process.setMode(Settings::CoreMode::Client);
    process.setProcessState(deskflow::core::ProcessState::Started);
    QTRY_COMPARE(process.connectionState(), deskflow::core::ConnectionState::Connected);
    const auto generation = process.m_coreIpcGeneration;
    server.reset();
    QTRY_VERIFY_WITH_TIMEOUT(process.m_coreIpcRetryTimer.isActive(), 3000);
    server = std::make_unique<StatusSource>(address);
    server->listen();
    server->broadcastCommand("connectionState", "Connecting");
    QTRY_COMPARE_WITH_TIMEOUT(process.connectionState(), deskflow::core::ConnectionState::Connecting, 5000);
    QVERIFY(process.m_coreIpcGeneration > generation);
    server->broadcastCommand("connectionState", "Connected");
    QTRY_COMPARE(process.connectionState(), deskflow::core::ConnectionState::Connected);
    process.setProcessState(deskflow::core::ProcessState::Stopped);
  }
  void stoppedObserverRejectsAlreadyQueuedMessages()
  {
    const auto address = socketName();
    StatusSource server(address);
    server.listen();
    server.broadcastCommand("connectionState", "Connected");
    ServerConfig config;
    CoreProcess process(config, address);
    process.setMode(Settings::CoreMode::Client);
    process.setProcessState(deskflow::core::ProcessState::Started);
    QTRY_COMPARE(process.connectionState(), deskflow::core::ConnectionState::Connected);
    QVERIFY(
        QMetaObject::invokeMethod(
            process.m_coreIpcClient, "commandReceived", Qt::DirectConnection,
            Q_ARG(QString, QStringLiteral("connectionState")), Q_ARG(QString, QStringLiteral("Connected"))
        )
    );
    process.setConnectionState(deskflow::core::ConnectionState::Disconnected);
    process.setProcessState(deskflow::core::ProcessState::Stopped);
    QCoreApplication::processEvents();
    QCOMPARE(process.connectionState(), deskflow::core::ConnectionState::Disconnected);
    QVERIFY(!process.m_coreIpcRetryTimer.isActive());
  }
  void matchingExistingCoreIsObservedAndCanBeStopped()
  {
    const auto address = socketName();
    StatusSource server(address);
    server.listen();
    server.broadcastCommand("connectionState", "Connected");
    ServerConfig config;
    CoreProcess process(config, address);
    process.setMode(Settings::CoreMode::Client);
    process.checkExistingProcess();
    QTRY_COMPARE(process.processState(), deskflow::core::ProcessState::Started);
    QTRY_COMPARE(process.connectionState(), deskflow::core::ConnectionState::Connected);
    QVERIFY(process.m_adoptedCore);
    process.stop(Settings::ProcessMode::Desktop);
    QTRY_COMPARE(process.processState(), deskflow::core::ProcessState::Stopped);
    QCOMPARE(process.connectionState(), deskflow::core::ConnectionState::Disconnected);
  }
};
} // namespace deskflow::gui
QTEST_MAIN(deskflow::gui::CoreProcessStatusTests)
#include "CoreProcessStatusTests.moc"
