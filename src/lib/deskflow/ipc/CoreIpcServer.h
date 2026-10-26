/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025-2026 Synergy App Ltd
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "IpcServer.h"

#include <QObject>
#include <QPointer>

class QLocalSocket;

namespace deskflow::core::ipc {

class CoreIpcServer : public IpcServer
{
  Q_OBJECT

public:
  explicit CoreIpcServer(QObject *parent, const QString &serverName = {});
  ~CoreIpcServer() override;

  static CoreIpcServer &instance();

Q_SIGNALS:
  void workspaceCommandReceived(const QString &encoded);
  void workspacePeerDisconnected();

private:
  void processCommand(QLocalSocket *clientSocket, const QString &command, const QStringList &parts) override;
  void handleWorkspacePeerDisconnected();

  QPointer<QLocalSocket> m_workspacePeer;
};

} // namespace deskflow::core::ipc
