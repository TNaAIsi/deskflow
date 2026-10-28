/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "common/Settings.h"
#include "gui/widgets/StatusBar.h"

#include <QLabel>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

class StatusBarTests : public QObject
{
  Q_OBJECT
private:
  QTemporaryDir m_profile;
private Q_SLOTS:
  void initTestCase()
  {
    QVERIFY(m_profile.isValid());
    qputenv("XDG_CONFIG_HOME", m_profile.path().toUtf8());
    Settings::setSettingsFile(m_profile.filePath("Deskflow.conf"));
    Settings::setStateFile(m_profile.filePath("Deskflow.state"));
    Settings::setValue(Settings::Client::DynamicConnectionRetry, true);
    Settings::setValue(Settings::Client::RemoteHost, "Windows");
  }
  void retryTickCannotOverwriteConnectedClient()
  {
    StatusBar bar;
    bar.setStatus(ConnectionState::Connecting, ProcessState::Started, false);
    bar.setConnectionInterval(10);
    QVERIFY(bar.m_retryTimer->isActive());
    bar.setStatus(ConnectionState::Connected, ProcessState::Started, false);
    const auto connected = bar.m_lblStatus->text();
    QVERIFY(connected.contains("connected as client"));
    QVERIFY(!bar.m_retryTimer->isActive());
    bar.updateTimerLabel();
    QCOMPARE(bar.m_lblStatus->text(), connected);
  }
  void serverClientListStopsConnectingTimerAndSurvivesRefresh()
  {
    StatusBar bar;
    bar.setStatus(ConnectionState::Connecting, ProcessState::Started, true);
    bar.setServerClients({"Mac"});
    const auto connected = bar.m_lblStatus->text();
    QVERIFY(connected.contains("connected"));
    QVERIFY(connected.contains("Mac"));
    QVERIFY(!bar.m_retryTimer->isActive());
    bar.setStatus(ConnectionState::Connected, ProcessState::Started, true);
    QCOMPARE(bar.m_lblStatus->text(), connected);
    bar.updateTimerLabel();
    QCOMPARE(bar.m_lblStatus->text(), connected);
  }
  void connectedServerWithoutListDoesNotKeepOldConnectingLabel()
  {
    StatusBar bar;
    bar.setStatus(ConnectionState::Connecting, ProcessState::Started, true);
    QVERIFY(bar.m_lblStatus->text().contains("connecting"));
    bar.setStatus(ConnectionState::Connected, ProcessState::Started, true);
    QVERIFY(bar.m_lblStatus->text().contains("is connected"));
    QVERIFY(!bar.m_lblStatus->text().contains("connecting"));
  }
  void staleListAndTickCannotReviveStoppedProcess()
  {
    StatusBar bar;
    bar.setStatus(ConnectionState::Connected, ProcessState::Started, true);
    bar.setServerClients({"Mac"});
    bar.setStatus(ConnectionState::Disconnected, ProcessState::Stopped, true);
    const auto stopped = bar.m_lblStatus->text();
    bar.setServerClients({"Mac"});
    bar.updateTimerLabel();
    QCOMPARE(bar.m_lblStatus->text(), stopped);
    bar.setStatus(ConnectionState::Connecting, ProcessState::Started, true);
    QVERIFY(bar.m_lblStatus->text().contains("connecting"));
    QVERIFY(!bar.m_lblStatus->text().contains("Mac"));
  }
  void actualDisconnectReplacesClientListAndStopsRetry()
  {
    StatusBar bar;
    bar.setStatus(ConnectionState::Connected, ProcessState::Started, true);
    bar.setServerClients({"Mac"});
    bar.setStatus(ConnectionState::Listening, ProcessState::Started, true);
    QVERIFY(bar.m_lblStatus->text().contains("waiting for clients"));
    bar.updateTimerLabel();
    QVERIFY(bar.m_lblStatus->text().contains("waiting for clients"));
    bar.setStatus(ConnectionState::Disconnected, ProcessState::Started, false);
    QVERIFY(bar.m_lblStatus->text().contains("disconnected"));
  }
};
QTEST_MAIN(StatusBarTests)
#include "StatusBarTests.moc"
