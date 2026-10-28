/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 - 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "StatusBar.h"
#include "common/Constants.h"
#include "common/Settings.h"

#include <QEvent>
#include <QLabel>
#include <QPushButton>
#include <QTimer>

StatusBar::StatusBar(QWidget *parent)
    : QStatusBar{parent},
      m_btnFingerprint{new QPushButton(this)},
      m_lblSecurityIcon{new QLabel(this)},
      m_lblStatus{new QLabel(this)},
      m_btnUpdate{new QPushButton(this)},
      m_retryTimer{new QTimer(this)}
{
  static const auto btnHeight = height() - 2;
  static const auto btnSize = QSize(btnHeight, btnHeight);
  static const auto iconSize = QSize(fontMetrics().height() + 2, fontMetrics().height() + 2);

  m_btnFingerprint->setFlat(true);
  m_btnFingerprint->setIcon(QIcon::fromTheme(QStringLiteral("fingerprint")));
  m_btnFingerprint->setFixedSize(btnSize);
  m_btnFingerprint->setIconSize(iconSize);
  insertPermanentWidget(0, m_btnFingerprint);
  connect(m_btnFingerprint, &QPushButton::clicked, this, &StatusBar::requestShowMyFingerprints);

  m_lblSecurityIcon->setVisible(false);
  m_lblSecurityIcon->setFixedSize(iconSize);
  m_lblSecurityIcon->setScaledContents(true);
  insertPermanentWidget(1, m_lblSecurityIcon);

  m_lblStatus->setText(tr("%1 is not running").arg(kAppName));
  insertPermanentWidget(2, m_lblStatus, 1);

  m_btnUpdate->setVisible(false);
  m_btnUpdate->setFlat(true);
  m_btnUpdate->setLayoutDirection(Qt::RightToLeft);
  m_btnUpdate->setIcon(QIcon::fromTheme(QStringLiteral("software-updates-release")));
  m_btnUpdate->setFixedHeight(btnHeight);
  m_btnUpdate->setIconSize(iconSize);
  insertPermanentWidget(3, m_btnUpdate);
  connect(m_btnUpdate, &QPushButton::clicked, this, &StatusBar::requestUpdateVersion);

  m_retryTimer->setInterval(1000);
  m_retryTimer->setSingleShot(false);
  connect(m_retryTimer, &QTimer::timeout, this, &StatusBar::updateTimerLabel);

  updateText();
  adjustSize();
}

void StatusBar::setStatus(ConnectionState connectionState, ProcessState processState, bool isServer)
{
  if (m_isServer != isServer || processState != ProcessState::Started || m_processState != ProcessState::Started ||
      connectionState == ConnectionState::Disconnected || connectionState == ConnectionState::Listening)
    m_serverClients.clear();
  if (connectionState != ConnectionState::Connecting || m_connectionState != ConnectionState::Connecting)
    m_connectionInterval = -1;
  m_connectionState = connectionState;
  m_processState = processState;
  m_isServer = isServer;
  renderStatus();
}

void StatusBar::renderStatus()
{
  m_retryTimer->stop();
  m_lblStatus->setToolTip({});
  setSecurityIconVisible(false);
  switch (m_processState) {
  case ProcessState::Starting:
    m_lblStatus->setText(tr("%1 is starting...").arg(kAppName));
    return;
  case ProcessState::RetryPending:
    m_lblStatus->setText(tr("%1 will retry in a moment...").arg(kAppName));
    return;
  case ProcessState::Stopping:
    m_lblStatus->setText(tr("%1 is stopping...").arg(kAppName));
    return;
  case ProcessState::Stopped:
    m_lblStatus->setText(tr("%1 is not running").arg(kAppName));
    return;
  case ProcessState::Started:
    break;
  }

  if (m_isServer && !m_serverClients.isEmpty()) {
    setSecurityIconVisible(true);
    m_connectionInterval = -1;
    //: %1 is the application name; %2 lists currently connected clients.
    m_lblStatus->setText(tr("%1 is connected, with %n client(s): %2", "", static_cast<int>(m_serverClients.size()))
                             .arg(kAppName, m_serverClients.join(QStringLiteral(", "))));
    if (m_serverClients.size() > 1)
      m_lblStatus->setToolTip(tr("Clients:\n %1").arg(m_serverClients.join(QLatin1Char('\n'))));
    return;
  }
  switch (m_connectionState) {
  case ConnectionState::Listening:
    if (m_isServer) {
      setSecurityIconVisible(true);
      m_lblStatus->setText(tr("%1 is waiting for clients").arg(kAppName));
    } else {
      m_lblStatus->setText(tr("%1 is disconnected").arg(kAppName));
    }
    break;
  case ConnectionState::Connecting:
    updateTimerLabel();
    if (Settings::value(Settings::Client::DynamicConnectionRetry).toBool())
      m_retryTimer->start();
    break;
  case ConnectionState::Connected:
    setSecurityIconVisible(true);
    if (m_isServer)
      m_lblStatus->setText(tr("%1 is connected").arg(kAppName));
    else
      m_lblStatus->setText(
          tr("%1 is connected as client of %2").arg(kAppName, Settings::value(Settings::Client::RemoteHost).toString())
      );
    break;
  case ConnectionState::Disconnected:
    m_lblStatus->setText(tr("%1 is disconnected").arg(kAppName));
    break;
  }
}

void StatusBar::setServerClients(const QStringList &clients)
{
  m_serverClients = clients;
  // Client-list updates and timer callbacks can already be queued when a core
  // stops. Retain the data, but render it only for a running server.
  if (m_isServer && m_processState == ProcessState::Started)
    renderStatus();
}

void StatusBar::setSecurityIconVisible(bool visible)
{
  m_lblSecurityIcon->setVisible(visible);
}

void StatusBar::setConnectionInterval(int newInterval)
{
  m_connectionInterval = newInterval;
}

bool StatusBar::securityIconVisible() const
{
  return m_lblSecurityIcon->isVisible();
}

void StatusBar::setBtnFingerprintVisible(bool visible)
{
  m_btnFingerprint->setVisible(visible);
}

void StatusBar::updateFound(const QString &version)
{
  m_btnUpdate->setVisible(true);
  m_btnUpdate->setToolTip(tr("A new version v%1 is available").arg(version));
}

void StatusBar::changeEvent(QEvent *e)
{
  QStatusBar::changeEvent(e);
  if (e->type() == QEvent::LanguageChange)
    updateText();
}

void StatusBar::updateText()
{
  m_btnFingerprint->setToolTip(tr("View local fingerprint"));
  m_btnUpdate->setText(tr("Update available"));
  setSecurityLevel(m_securityLevel);
  renderStatus();
}

void StatusBar::updateTimerLabel()
{
  if (m_processState != ProcessState::Started || m_connectionState != ConnectionState::Connecting ||
      (m_isServer && !m_serverClients.isEmpty()))
    return;
  QString text;
  if (m_connectionInterval < 2 || !Settings::value(Settings::Client::DynamicConnectionRetry).toBool()) {
    text = tr("%1 is connecting...").arg(kAppName);
  } else {
    text = tr("%1 is waiting %2 seconds before the next retry").arg(kAppName, QString::number(m_connectionInterval));
    m_connectionInterval--;
  }
  m_lblStatus->setText(text);
}

void StatusBar::setSecurityIcon(bool encrypted)
{
  const auto icon = QIcon::fromTheme(encrypted ? QIcon::ThemeIcon::SecurityHigh : QIcon::ThemeIcon::SecurityLow);
  m_lblSecurityIcon->setPixmap(icon.pixmap(QSize(32, 32)));
  m_encrypted = encrypted;
  setSecurityLevel(m_securityLevel);
}

void StatusBar::setSecurityLevel(const QString &securityLevel)
{
  m_securityLevel = securityLevel;
  const auto txt = m_encrypted ? tr("%1 Encryption Enabled").arg(m_securityLevel) : tr("Encryption Disabled");
  m_lblSecurityIcon->setToolTip(txt);
}
