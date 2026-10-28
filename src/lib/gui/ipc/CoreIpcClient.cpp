/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025-2026 Synergy App Ltd
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "CoreIpcClient.h"

#include "common/Constants.h"

#include <QJsonDocument>
#include <QString>

namespace deskflow::gui::ipc {

CoreIpcClient::CoreIpcClient(QObject *parent, const QString &serverName, IpcClient::Purpose purpose)
    : IpcClient(
          parent, serverName.isEmpty() ? QString::fromLatin1(kCoreIpcName) : serverName, QStringLiteral("core"), purpose
      )
{
  // do nothing
}

void CoreIpcClient::sendStop()
{
  sendMessage(QStringLiteral("stop"));
}

void CoreIpcClient::sendWorkspaceCommand(const QJsonObject &command)
{
  const auto encoded = QJsonDocument(command)
                           .toJson(QJsonDocument::Compact)
                           .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
  sendMessage(QStringLiteral("workspace=%1").arg(QString::fromLatin1(encoded)));
}

void CoreIpcClient::processCommand(const QString &command, const QStringList &parts)
{
  const auto args = parts.size() >= 2 ? parts.at(1) : QString();
  Q_EMIT commandReceived(command, args);
}

} // namespace deskflow::gui::ipc
