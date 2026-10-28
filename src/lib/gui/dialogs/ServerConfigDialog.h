/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 - 2026 Chris Rizzitello <sithlord48@gmail.com>
 * SPDX-FileCopyrightText: (C) 2012 - 2016 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2008 Volker Lanz <vl@fidra.de>
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "ComputerSetupModel.h"
#include "common/NetworkProtocol.h"
#include "config/ServerConfig.h"

#include <QDialog>
#include <QJsonObject>

class SettingsDialogButtonBox;
class SharingSettingsWidget;
namespace deskflow::workspace {
class DisplayLayoutWidget;
class WorkspaceService;
} // namespace deskflow::workspace
class QItemSelection;
class QLabel;
class QListWidget;
class QPushButton;

namespace Ui {
class ServerConfigDialog;
}

class ServerConfigDialog : public QDialog
{
  Q_OBJECT

public:
  ServerConfigDialog(QWidget *parent, ServerConfig &config);
  ~ServerConfigDialog() override;
  bool addClient(const QString &clientName);
  void setLayoutState(const QJsonObject &state);
  void setSharingService(deskflow::workspace::WorkspaceService *service);
  void setSharingState(const QJsonObject &state);
  bool requiresCoreRestart() const
  {
    return m_inputConfigChanged;
  }
  void message(const QString &message)
  {
    m_message = message;
  }

Q_SIGNALS:
  void displayLayoutRequested(); // Kept for source compatibility; no separate editor is opened.
  void displayLayoutSaved(const QJsonObject &layout);
  void identifyDisplayRequested(const QString &device, const QString &id);

protected:
  void onComputerRemoved();
  void addClient();
  bool addComputer(const QString &clientName, bool doSilent);

  void addHotkey();
  void editHotkey();
  void removeHotkey();
  void listHotkeysSelectionChanged(const QItemSelection &selected, [[maybe_unused]] const QItemSelection &deselected);

  void addAction();
  void editAction();
  void removeAction();
  void listActionsSelectionChanged(const QItemSelection &selected, [[maybe_unused]] const QItemSelection &deselected);

  void toggleSwitchDoubleTap(bool enable);
  void setSwitchDoubleTap(int within);

  void toggleSwitchDelay(bool enable);
  void setSwitchDelay(int delay);

  void toggleDefaultLockToComputerState(bool state);
  void toggleLockToComputer(bool disabled);
  void toggleWin32Foreground(bool enabled);

  void toggleClipboard(bool enabled);
  void setClipboardLimit(int limit);

  void toggleHeartbeat(bool enabled);
  void setHeartbeat(int rate);

  void toggleRelativeMouseMoves(bool enabled);
  void togglePreventFullscreenSwitch(bool enabled);
  void toggleProtocol();

  void toggleExternalConfig(bool enable = false);
  bool browseConfigFile();

  ServerConfig &serverConfig()
  {
    return m_serverConfig;
  }
  void setOriginalServerConfig(const ServerConfig &s)
  {
    m_originalServerConfig = s;
  }
  ComputerSetupModel &model()
  {
    return m_computerSetupModel;
  }

private:
  void save();
  void cancel();
  void loadFromConfig();
  void resetFromSettings();
  void refreshControls();
  void initConnections() const;
  void updateControls() const;
  void restoreFromDefaults();
  void setServerConfig();
  bool isGeneralConfigModified() const;
  bool isGeneralConfigDefault() const;
  void setButtonBoxEnabledButtons() const;
  void refreshLayoutControls() const;
  void refreshRegisteredComputers();
  void editRegisteredComputer();
  void removeRegisteredComputer();
  void addFullscreenGameProgram();
  void removeFullscreenGameProgram();
  void refreshFullscreenGamePrograms();
  void updateGameProgramControls() const;
  std::unique_ptr<Ui::ServerConfigDialog> ui;
  QString m_message = "";
  int m_columns;
  int m_rows;
  ServerConfig &m_originalServerConfig;
  NetworkProtocol m_protocol;
  bool m_enableClipboard;
  bool m_enableHeartbeat;
  int m_heartbeatRate;
  int m_switchDelay;
  int m_switchDoubleTap;
  uint m_clipboardSize;
  bool m_relativeMouseMoves;
  bool m_preventFullscreenSwitch;
  QStringList m_fullscreenGamePrograms;
  bool m_enableSwitchDelay;
  bool m_enableSwitchDoubleTap;
  bool m_originalServerConfigIsExternal;
  bool m_win32keepForeground;
  bool m_disableLockToComputer;
  bool m_defaultLockToComputerState;
  QString m_originalServerConfigUsesExternalFile;
  ServerConfig m_serverConfig;
  ComputerSetupModel m_computerSetupModel;
  SettingsDialogButtonBox *m_buttonBox = nullptr;
  QJsonObject m_layoutState;
  QLabel *m_layoutModeLabel = nullptr;
  deskflow::workspace::DisplayLayoutWidget *m_displayLayoutEditor = nullptr;
  SharingSettingsWidget *m_sharingSettings = nullptr;
  bool m_inputConfigChanged = false;
  QWidget *m_registeredComputersPanel = nullptr;
  QListWidget *m_registeredComputers = nullptr;
  QPushButton *m_addRegisteredComputer = nullptr;
  QPushButton *m_editRegisteredComputer = nullptr;
  QPushButton *m_removeRegisteredComputer = nullptr;
  QWidget *m_gameProgramsPanel = nullptr;
  QListWidget *m_gamePrograms = nullptr;
  QPushButton *m_addGameProgram = nullptr;
  QPushButton *m_removeGameProgram = nullptr;
};
