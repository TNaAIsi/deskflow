/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 - 2026 Chris Rizzitello <sithlord48@gmail.com>
 * SPDX-FileCopyrightText: (C) 2012 - 2016 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2008 Volker Lanz <vl@fidra.de>
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "ServerConfigDialog.h"
#include "ui_ServerConfigDialog.h"

#include "common/Constants.h"
#include "common/FullscreenGamePrograms.h"
#include "common/NetworkProtocol.h"
#include "common/PlatformInfo.h"
#include "common/Settings.h"
#include "dialogs/ActionDialog.h"
#include "dialogs/ComputerSettingsDialog.h"
#include "dialogs/HotkeyDialog.h"
#include "gui/widgets/SettingsDialogButtonBox.h"
#include "gui/widgets/SharingSettingsWidget.h"
#include "workspace/DisplayLayoutDialog.h"
#include "workspace/DisplayTopology.h"

#include <QFileDialog>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QVBoxLayout>

using enum ComputerConfig::SwitchCorner;

ServerConfigDialog::ServerConfigDialog(QWidget *parent, ServerConfig &config)
    : QDialog(parent, Qt::WindowTitleHint | Qt::WindowSystemMenuHint),
      ui{std::make_unique<Ui::ServerConfigDialog>()},
      m_columns{Settings::value(Settings::Server::GridWidth).toInt()},
      m_rows{Settings::value(Settings::Server::GridHeight).toInt()},
      m_originalServerConfig(config),
      m_originalServerConfigIsExternal(config.useExternalConfig()),
      m_originalServerConfigUsesExternalFile(config.configFile()),
      m_serverConfig(config),
      m_computerSetupModel(m_serverConfig.computers(), m_columns, m_rows),
      m_buttonBox{new SettingsDialogButtonBox(this)}
{
  ui->setupUi(this);
  ui->tabWidget->setCurrentIndex(0);
  layout()->addWidget(m_buttonBox);

  resize(1050, 800);
  auto *computersLayout = qobject_cast<QVBoxLayout *>(ui->tabComputers->layout());
  m_layoutModeLabel = new QLabel(ui->tabComputers);
  m_layoutModeLabel->setObjectName("layoutModeLabel");
  m_layoutModeLabel->setWordWrap(true);
  computersLayout->insertWidget(0, m_layoutModeLabel);
  m_displayLayoutEditor = new deskflow::workspace::DisplayLayoutWidget({}, ui->tabComputers);
  computersLayout->insertWidget(1, m_displayLayoutEditor, 1);

  m_sharingSettings = new SharingSettingsWidget(true, this);
  ui->tabWidget->addTab(m_sharingSettings->soundPage(), tr("Sound"));
  ui->tabWidget->addTab(m_sharingSettings->filesPage(), tr("Files and cache"));

  m_registeredComputersPanel = new QWidget(ui->tabComputers);
  m_registeredComputersPanel->setObjectName("registeredComputersPanel");
  auto *registeredLayout = new QVBoxLayout(m_registeredComputersPanel);
  registeredLayout->setContentsMargins(0, 0, 0, 0);
  auto *registeredHelp = new QLabel(
      tr("Registered computers and keyboard settings. This list does not set display positions."),
      m_registeredComputersPanel
  );
  registeredHelp->setWordWrap(true);
  registeredLayout->addWidget(registeredHelp);
  m_registeredComputers = new QListWidget(m_registeredComputersPanel);
  m_registeredComputers->setObjectName("registeredComputers");
  m_registeredComputers->setMaximumHeight(95);
  registeredLayout->addWidget(m_registeredComputers);
  auto *registeredButtons = new QHBoxLayout;
  m_addRegisteredComputer = new QPushButton(tr("Add computer…"), m_registeredComputersPanel);
  m_addRegisteredComputer->setObjectName("addRegisteredComputer");
  m_editRegisteredComputer = new QPushButton(tr("Edit computer…"), m_registeredComputersPanel);
  m_editRegisteredComputer->setObjectName("editRegisteredComputer");
  m_removeRegisteredComputer = new QPushButton(tr("Remove computer"), m_registeredComputersPanel);
  m_removeRegisteredComputer->setObjectName("removeRegisteredComputer");
  for (auto *button : {m_addRegisteredComputer, m_editRegisteredComputer, m_removeRegisteredComputer}) {
    button->setAutoDefault(false);
    registeredButtons->addWidget(button);
  }
  registeredButtons->addStretch();
  registeredLayout->addLayout(registeredButtons);
  computersLayout->addWidget(m_registeredComputersPanel);

  m_gameProgramsPanel = new QWidget(ui->cbPreventFullscreenSwitch->parentWidget());
  m_gameProgramsPanel->setObjectName("fullscreenGameProgramsPanel");
  auto *gameProgramsLayout = new QVBoxLayout(m_gameProgramsPanel);
  gameProgramsLayout->setContentsMargins(18, 0, 0, 0);
  auto *gameProgramsHelp = new QLabel(
      tr("Add a game if automatic detection misses it, such as a borderless game on a single monitor. Listed "
         "programs are protected only while full-screen."),
      m_gameProgramsPanel
  );
  gameProgramsHelp->setWordWrap(true);
  gameProgramsLayout->addWidget(gameProgramsHelp);
  m_gamePrograms = new QListWidget(m_gameProgramsPanel);
  m_gamePrograms->setObjectName("fullscreenGamePrograms");
  m_gamePrograms->setMaximumHeight(75);
  gameProgramsLayout->addWidget(m_gamePrograms);
  auto *gameProgramButtons = new QHBoxLayout;
  m_addGameProgram = new QPushButton(tr("Add game…"), m_gameProgramsPanel);
  m_addGameProgram->setObjectName("addFullscreenGameProgram");
  m_removeGameProgram = new QPushButton(tr("Remove selected game"), m_gameProgramsPanel);
  m_removeGameProgram->setObjectName("removeFullscreenGameProgram");
  for (auto *button : {m_addGameProgram, m_removeGameProgram}) {
    button->setAutoDefault(false);
    gameProgramButtons->addWidget(button);
  }
  gameProgramButtons->addStretch();
  gameProgramsLayout->addLayout(gameProgramButtons);
  if (auto *switchingLayout = qobject_cast<QVBoxLayout *>(ui->cbPreventFullscreenSwitch->parentWidget()->layout()))
    switchingLayout->insertWidget(switchingLayout->indexOf(ui->cbPreventFullscreenSwitch) + 1, m_gameProgramsPanel);

  // Keep the existing Advanced page usable on laptop-height displays as its
  // switching controls grow; the dialog's single Save/Cancel row stays fixed.
  auto *advancedContents = new QWidget(ui->tabAdvanced);
  advancedContents->setLayout(ui->tabAdvanced->layout());
  auto *advancedScroll = new QScrollArea(ui->tabAdvanced);
  advancedScroll->setObjectName("serverAdvancedScroll");
  advancedScroll->setWidgetResizable(true);
  advancedScroll->setFrameShape(QFrame::NoFrame);
  advancedScroll->setWidget(advancedContents);
  auto *advancedLayout = new QVBoxLayout(ui->tabAdvanced);
  advancedLayout->setContentsMargins(0, 0, 0, 0);
  advancedLayout->addWidget(advancedScroll);

  auto topology = deskflow::workspace::DisplayTopology::instance().serializedState();
  if (topology.isEmpty())
    topology = QJsonDocument::fromJson(Settings::value(Settings::Workspace::Layout).toString().toUtf8()).object();
  m_layoutState = {{"topology", topology}};
  m_displayLayoutEditor->setState(topology, false);

  loadFromConfig();

  ui->lblRemoveComputer->setPixmap(QIcon::fromTheme("user-trash").pixmap(QSize(64, 64)));
  ui->lblNewComputer->setEnabled(!model().isFull());
  ui->lblNewComputer->setPixmap(QIcon::fromTheme("video-display").pixmap(QSize(64, 64)));
  ui->btnBrowseConfigFile->setIcon(QIcon::fromTheme(QIcon::ThemeIcon::DocumentOpen));

  if (!deskflow::platform::isWindows()) {
    ui->cbWin32KeepForeground->setVisible(false);
    ui->cbPreventFullscreenSwitch->setVisible(false);
    m_gameProgramsPanel->setVisible(false);
  }
  initConnections();
  refreshRegisteredComputers();
}

ServerConfigDialog::~ServerConfigDialog()
{
  for (auto *child : findChildren<QObject *>())
    disconnect(child, nullptr, this, nullptr);
  disconnect(&m_computerSetupModel, nullptr, this, nullptr);
}

bool ServerConfigDialog::addClient(const QString &clientName)
{
  return addComputer(clientName, true);
}

void ServerConfigDialog::setLayoutState(const QJsonObject &state)
{
  m_layoutState = state;
  m_displayLayoutEditor->setState(state.value("topology").toObject());
  m_sharingSettings->refresh(state);
  setButtonBoxEnabledButtons();
}

void ServerConfigDialog::setSharingService(deskflow::workspace::WorkspaceService *service)
{
  m_sharingSettings->setService(service);
}

void ServerConfigDialog::setSharingState(const QJsonObject &state)
{
  m_sharingSettings->refresh(state);
}

void ServerConfigDialog::refreshLayoutControls() const
{
  const bool external = ui->groupExternalConfig->isChecked();
  const bool monitors = !external && m_displayLayoutEditor->independentEnabled();
  if (external) {
    m_layoutModeLabel->setText(
        tr("The external configuration file controls computer links. Turn it off to edit the layout here.")
    );
  } else if (monitors) {
    m_layoutModeLabel->setText(
        tr("Drag the displays below to match your desk. Save applies display positions and the other server settings "
           "together.")
    );
  } else {
    m_layoutModeLabel->setText(
        tr("Computer grid mode groups each computer into one block. Enable individual monitors below for independent "
           "screen placement.")
    );
  }
  m_displayLayoutEditor->setVisible(!external);
  m_displayLayoutEditor->setEnabled(Settings::isWritable() && !external);
  ui->computerSetupView->setVisible(!monitors && !external);
  ui->lblRemoveComputer->setVisible(!monitors && !external);
  ui->lblNewComputer->setVisible(!monitors && !external);
  ui->label_2->setVisible(!monitors && !external);
  ui->label_2->setText(tr("Compatibility layout: drag whole computers to set their crossing directions."));
  m_registeredComputersPanel->setVisible(monitors);

  const auto *selected = m_registeredComputers->currentItem();
  const int selectedIndex = selected ? selected->data(Qt::UserRole).toInt() : -1;
  const bool validSelection = selectedIndex >= 0 && selectedIndex < m_serverConfig.computers().size();
  const bool writable = Settings::isWritable() && !external;
  m_addRegisteredComputer->setEnabled(writable && !m_computerSetupModel.isFull());
  m_editRegisteredComputer->setEnabled(writable && validSelection);
  m_removeRegisteredComputer->setEnabled(
      writable && validSelection && !m_serverConfig.computers().at(selectedIndex).isServer() &&
      m_serverConfig.computers().at(selectedIndex).name() != m_serverConfig.getServerName()
  );
}

void ServerConfigDialog::refreshRegisteredComputers()
{
  const auto *selected = m_registeredComputers->currentItem();
  const int selectedIndex = selected ? selected->data(Qt::UserRole).toInt() : -1;
  const QSignalBlocker blocker(m_registeredComputers);
  m_registeredComputers->clear();
  const auto &computers = m_serverConfig.computers();
  for (int index = 0; index < computers.size(); index++) {
    const auto &computer = computers.at(index);
    if (computer.isNull())
      continue;
    auto *item = new QListWidgetItem(
        computer.isServer() ? tr("%1 (server)").arg(computer.name()) : computer.name(), m_registeredComputers
    );
    item->setData(Qt::UserRole, index);
    item->setToolTip(computer.aliases().isEmpty() ? QString() : tr("Aliases: %1").arg(computer.aliases().join(", ")));
    if (index == selectedIndex)
      m_registeredComputers->setCurrentItem(item);
  }
  if (!m_registeredComputers->currentItem() && m_registeredComputers->count() > 0)
    m_registeredComputers->setCurrentRow(0);
  refreshLayoutControls();
}

void ServerConfigDialog::editRegisteredComputer()
{
  if (!m_editRegisteredComputer->isEnabled() || !m_registeredComputers->currentItem())
    return;
  const auto index = m_registeredComputers->currentItem()->data(Qt::UserRole).toInt();
  auto edited = m_serverConfig.computers().at(index);
  ComputerSettingsDialog dialog(this, &edited, &m_serverConfig.computers());
  if (dialog.exec() != QDialog::Accepted)
    return;
  m_serverConfig.computers()[index] = edited;
  Q_EMIT m_computerSetupModel.computersChanged();
}

void ServerConfigDialog::removeRegisteredComputer()
{
  if (!m_removeRegisteredComputer->isEnabled() || !m_registeredComputers->currentItem())
    return;
  const auto index = m_registeredComputers->currentItem()->data(Qt::UserRole).toInt();
  m_serverConfig.computers()[index] = Computer();
  Q_EMIT m_computerSetupModel.computersChanged();
}

void ServerConfigDialog::addFullscreenGameProgram()
{
  if (!m_addGameProgram->isEnabled())
    return;
  const auto filename =
      QFileDialog::getOpenFileName(this, tr("Choose a game executable"), {}, tr("Windows programs (*.exe)"));
  if (filename.isEmpty() || !QFileInfo(filename).isFile())
    return;
  const auto path = deskflow::cleanFullscreenGameProgramPath(QFileInfo(filename).absoluteFilePath());
  if (path.isEmpty() || m_fullscreenGamePrograms.contains(path, Qt::CaseInsensitive))
    return;
  m_fullscreenGamePrograms.append(path);
  refreshFullscreenGamePrograms();
  setButtonBoxEnabledButtons();
}

void ServerConfigDialog::removeFullscreenGameProgram()
{
  if (!m_removeGameProgram->isEnabled() || !m_gamePrograms->currentItem())
    return;
  const auto row = m_gamePrograms->currentRow();
  if (row < 0 || row >= m_fullscreenGamePrograms.size())
    return;
  m_fullscreenGamePrograms.removeAt(row);
  refreshFullscreenGamePrograms();
  setButtonBoxEnabledButtons();
}

void ServerConfigDialog::refreshFullscreenGamePrograms()
{
  const QSignalBlocker blocker(m_gamePrograms);
  m_gamePrograms->clear();
  m_gamePrograms->addItems(m_fullscreenGamePrograms);
  if (m_gamePrograms->count())
    m_gamePrograms->setCurrentRow(0);
  for (int row = 0; row < m_gamePrograms->count(); ++row)
    m_gamePrograms->item(row)->setToolTip(m_gamePrograms->item(row)->text());
  updateGameProgramControls();
}

void ServerConfigDialog::updateGameProgramControls() const
{
  const bool enabled = Settings::isWritable() && m_preventFullscreenSwitch;
  m_gamePrograms->setEnabled(enabled);
  m_addGameProgram->setEnabled(enabled && m_fullscreenGamePrograms.size() < deskflow::kMaximumFullscreenGamePrograms);
  m_removeGameProgram->setEnabled(enabled && m_gamePrograms->currentRow() >= 0);
}

void ServerConfigDialog::save()
{
  QString layoutError;
  if (!ui->groupExternalConfig->isChecked() && !m_displayLayoutEditor->validate(&layoutError)) {
    ui->tabWidget->setCurrentIndex(0);
    m_displayLayoutEditor->setSaveError(layoutError);
    return;
  }
  m_inputConfigChanged = isGeneralConfigModified() || !(m_originalServerConfig == m_serverConfig);
  const bool layoutChanged = m_displayLayoutEditor->isModified() && !ui->groupExternalConfig->isChecked();
  const auto layout = m_displayLayoutEditor->layoutState();
  if (ui->groupExternalConfig->isChecked() && !QFile::exists(ui->lineConfigFile->text())) {

    auto selectedButton = QMessageBox::warning(
        this, "Filename invalid", "Please select a valid configuration file.", QMessageBox::Ok | QMessageBox::Ignore
    );

    if (selectedButton != QMessageBox::Ok || !browseConfigFile()) {
      return;
    }
  }
  // now that the dialog has been accepted, copy the new server config to the
  // original one, which is a reference to the one in MainWindow.
  setOriginalServerConfig(serverConfig());
  if (Settings::value(Settings::Server::EnableClipboard).toBool() != m_enableClipboard) {
    Settings::setValue(Settings::Server::XdpRestoreToken, QString());
    Settings::setValue(Settings::Server::XdpClipboardRetried, false);
  }
  Settings::setValue(Settings::Server::Protocol, networkProtocolToOption(m_protocol));
  Settings::setValue(Settings::Server::EnableClipboard, m_enableClipboard);
  Settings::setValue(Settings::Server::ClipboardSize, m_clipboardSize);
  Settings::setValue(Settings::Server::EnableHeartbeat, m_enableHeartbeat);
  Settings::setValue(Settings::Server::Heartbeat, m_heartbeatRate);
  Settings::setValue(Settings::Server::EnableSwitchDelay, m_enableSwitchDelay);
  Settings::setValue(Settings::Server::SwitchDelay, m_switchDelay);
  Settings::setValue(Settings::Server::DefaultLockToComputerState, m_defaultLockToComputerState);
  Settings::setValue(Settings::Server::DisableLockToComputer, m_disableLockToComputer);
  Settings::setValue(Settings::Server::EnableSwitchDoubleTap, m_enableSwitchDoubleTap);
  Settings::setValue(Settings::Server::SwitchDoubleTap, m_switchDoubleTap);
  Settings::setValue(Settings::Server::RelativeMouseMoves, m_relativeMouseMoves);
  Settings::setValue(Settings::Server::PreventFullscreenSwitch, m_preventFullscreenSwitch);
  Settings::setValue(Settings::Server::FullscreenGamePrograms, m_fullscreenGamePrograms);
  Settings::setValue(Settings::Server::Win32KeepForeground, m_win32keepForeground);
  Settings::setValue(Settings::Server::ExternalConfig, ui->groupExternalConfig->isChecked());
  Settings::setValue(Settings::Server::ExternalConfigFile, ui->lineConfigFile->text());

  QStringList computerNames;
  const auto computerList = m_computerSetupModel.m_computers;
  for (const auto &computer : computerList) {
    const auto &computerName = computer.name();
    if (computerName.isEmpty())
      continue;
    computerNames.append(QStringLiteral("computer_%1").arg(computerName));
    Settings::setValue(Settings::Computer::Aliases.arg(computerName), computer.aliases());
    Settings::setValue(Settings::Computer::Name.arg(computerName), computerName);
  }
  Settings::removeUnknownComputers(computerNames);
  if (layoutChanged) {
    Settings::setValue(
        Settings::Workspace::Layout, QString::fromUtf8(QJsonDocument(layout).toJson(QJsonDocument::Compact))
    );
    Q_EMIT displayLayoutSaved(layout);
  }
  m_sharingSettings->saveOptions();
  QDialog::accept();
}

void ServerConfigDialog::cancel()
{
  QDialog::reject();
}

void ServerConfigDialog::addHotkey()
{
  Hotkey hotkey;
  HotkeyDialog dlg(this, hotkey);
  if (dlg.exec() == QDialog::Accepted) {
    serverConfig().hotkeys().append(hotkey);
    ui->listHotkeys->addItem(hotkey.text());
    setButtonBoxEnabledButtons();
  }
}

void ServerConfigDialog::editHotkey()
{
  int row = ui->listHotkeys->currentRow();
  if (row < 0 || row >= serverConfig().hotkeys().size()) {
    qDebug() << "Attempt to editing out of bounds hotkey row: " << row;
    return;
  }

  Hotkey &hotkey = serverConfig().hotkeys()[row];
  HotkeyDialog dlg(this, hotkey);
  if (dlg.exec() == QDialog::Accepted) {
    ui->listHotkeys->currentItem()->setText(hotkey.text());
    setButtonBoxEnabledButtons();
  }
}

void ServerConfigDialog::removeHotkey()
{
  int row = ui->listHotkeys->currentRow();
  if (row < 0 || row >= serverConfig().hotkeys().size()) {
    qDebug() << "Attempt to remove out of bounds hotkey row: " << row;
    return;
  }

  serverConfig().hotkeys().removeAt(row);
  ui->listActions->clear();
  delete ui->listHotkeys->item(row);
  setButtonBoxEnabledButtons();
}

void ServerConfigDialog::listHotkeysSelectionChanged(const QItemSelection &selected, const QItemSelection &)
{
  bool itemsSelected = !selected.isEmpty();
  ui->btnEditHotkey->setEnabled(itemsSelected);
  ui->btnRemoveHotkey->setEnabled(itemsSelected);
  ui->btnNewAction->setEnabled(itemsSelected);

  if (itemsSelected && !serverConfig().hotkeys().isEmpty()) {
    ui->listActions->clear();
    const Hotkey &hotkey = serverConfig().hotkeys().at(selected.indexes().first().row());
    for (const Action &action : hotkey.actions())
      ui->listActions->addItem(action.text());
  }
}

void ServerConfigDialog::addAction()
{
  int row = ui->listHotkeys->currentRow();
  if (row < 0 || row >= serverConfig().hotkeys().size()) {
    qDebug() << "Attempt to add action to out of bounds hotkey row: " << row;
    return;
  }

  Hotkey &hotkey = serverConfig().hotkeys()[row];
  Action action;
  ActionDialog dlg(this, serverConfig(), hotkey, action);
  if (dlg.exec() == QDialog::Accepted) {
    hotkey.addAction(action);
    ui->listActions->addItem(action.text());
    setButtonBoxEnabledButtons();
  }
}

void ServerConfigDialog::editAction()
{
  int hotkeyRow = ui->listHotkeys->currentRow();
  if (hotkeyRow < 0 || hotkeyRow >= serverConfig().hotkeys().size()) {
    qDebug() << "Attempt to edit action from out of bounds hotkey row: " << hotkeyRow;
    return;
  }
  Hotkey &hotkey = serverConfig().hotkeys()[hotkeyRow];

  int actionRow = ui->listActions->currentRow();
  if (actionRow < 0 || actionRow >= hotkey.actions().size()) {
    qDebug() << "Attempt to remove out of bounds action row: " << actionRow;
    return;
  }
  Action &action = hotkey.actionAt(actionRow);

  ActionDialog dlg(this, serverConfig(), hotkey, action);
  if (dlg.exec() == QDialog::Accepted) {
    ui->listActions->currentItem()->setText(action.text());
    setButtonBoxEnabledButtons();
  }
}

void ServerConfigDialog::removeAction()
{
  int hotkeyRow = ui->listHotkeys->currentRow();
  if (hotkeyRow < 0 || hotkeyRow >= serverConfig().hotkeys().size()) {
    qDebug() << "Attempt to remove action from out of bounds hotkey row: " << hotkeyRow;
    return;
  }
  Hotkey &hotkey = serverConfig().hotkeys()[hotkeyRow];

  int actionRow = ui->listActions->currentRow();
  if (actionRow < 0 || actionRow >= hotkey.actions().size()) {
    qDebug() << "Attempt to remove out of bounds action row: " << actionRow;
    return;
  }

  hotkey.removeActionAt(actionRow);
  delete ui->listActions->currentItem();
  setButtonBoxEnabledButtons();
}

void ServerConfigDialog::toggleClipboard(bool enabled)
{
  if (m_enableClipboard == enabled)
    return;

  m_enableClipboard = enabled;

  ui->sbClipboardSizeLimit->setEnabled(enabled);
  if (enabled && !ui->sbClipboardSizeLimit->value()) {
    m_clipboardSize = Settings::defaultValue(Settings::Server::ClipboardSize).toUInt();
    ui->sbClipboardSizeLimit->setValue(m_clipboardSize ? m_clipboardSize : 1);
  }
  setButtonBoxEnabledButtons();
}

void ServerConfigDialog::setClipboardLimit(int limit)
{
  if (m_clipboardSize == limit)
    return;

  m_clipboardSize = limit;
  setButtonBoxEnabledButtons();
}

void ServerConfigDialog::toggleHeartbeat(bool enabled)
{
  m_enableHeartbeat = enabled;
  ui->sbHeartbeat->setEnabled(enabled);
  setButtonBoxEnabledButtons();
}

void ServerConfigDialog::setHeartbeat(int rate)
{
  if (rate == m_heartbeatRate)
    return;
  m_heartbeatRate = rate;
  setButtonBoxEnabledButtons();
}

void ServerConfigDialog::togglePreventFullscreenSwitch(bool enabled)
{
  if (m_preventFullscreenSwitch == enabled)
    return;
  m_preventFullscreenSwitch = enabled;
  setButtonBoxEnabledButtons();
}

void ServerConfigDialog::toggleRelativeMouseMoves(bool enabled)
{
  if (m_relativeMouseMoves == enabled)
    return;
  m_relativeMouseMoves = enabled;
  setButtonBoxEnabledButtons();
}

void ServerConfigDialog::toggleProtocol()
{
  m_protocol = ui->rbProtocolBarrier->isChecked() ? NetworkProtocol::Barrier : NetworkProtocol::Synergy;
  setButtonBoxEnabledButtons();
}

void ServerConfigDialog::listActionsSelectionChanged(const QItemSelection &selected, const QItemSelection &)
{
  bool enabled = !selected.isEmpty();
  ui->btnEditAction->setEnabled(enabled);
  ui->btnRemoveAction->setEnabled(enabled);
}

void ServerConfigDialog::toggleSwitchDoubleTap(bool enable)
{
  m_enableSwitchDoubleTap = enable;
  ui->sbSwitchDoubleTap->setEnabled(enable);
  setButtonBoxEnabledButtons();
}

void ServerConfigDialog::setSwitchDoubleTap(int within)
{
  if (m_switchDoubleTap == within)
    return;
  m_switchDoubleTap = within;
  setButtonBoxEnabledButtons();
}

void ServerConfigDialog::toggleSwitchDelay(bool enable)
{
  m_enableSwitchDelay = enable;
  ui->sbSwitchDelay->setEnabled(enable);
  setButtonBoxEnabledButtons();
}

void ServerConfigDialog::setSwitchDelay(int delay)
{
  if (m_switchDelay == delay)
    return;
  m_switchDelay = delay;
  setButtonBoxEnabledButtons();
}

void ServerConfigDialog::toggleDefaultLockToComputerState(bool state)
{
  if (m_defaultLockToComputerState == state)
    return;
  m_defaultLockToComputerState = state;
  setButtonBoxEnabledButtons();
}

void ServerConfigDialog::toggleLockToComputer(bool disabled)
{
  if (m_disableLockToComputer == disabled)
    return;
  m_disableLockToComputer = disabled;
  setButtonBoxEnabledButtons();
}

void ServerConfigDialog::toggleWin32Foreground(bool enabled)
{
  if (m_win32keepForeground == enabled)
    return;
  m_win32keepForeground = enabled;
  setButtonBoxEnabledButtons();
}

void ServerConfigDialog::addClient()
{
  addComputer("", false);
}

void ServerConfigDialog::onComputerRemoved()
{
  ui->lblNewComputer->setEnabled(true);
  setButtonBoxEnabledButtons();
}

void ServerConfigDialog::toggleExternalConfig(bool checked)
{
  ui->widgetExternalConfigControls->setEnabled(checked);
  ui->tabWidget->setTabEnabled(0, true);
  ui->tabWidget->setTabEnabled(1, !checked);
  setButtonBoxEnabledButtons();
}

bool ServerConfigDialog::browseConfigFile()
{
  //: %1 is replaced with the application names
  //: (*.conf) and (*.*) should not be translated
  const auto deskflowConfigFilter = tr("%1 Configurations (*.conf);;All files (*.*)");

  QString fileName =
      QFileDialog::getOpenFileName(this, tr("Browse for a config file"), "", deskflowConfigFilter.arg(kAppName));

  if (!fileName.isEmpty()) {
    ui->lineConfigFile->setText(fileName);
    setServerConfig();
    return true;
  }

  return false;
}

void ServerConfigDialog::loadFromConfig()
{
  m_protocol = Settings::networkProtocol();
  m_enableHeartbeat = Settings::value(Settings::Server::EnableHeartbeat).toBool();
  m_heartbeatRate = Settings::value(Settings::Server::Heartbeat).toInt();
  m_relativeMouseMoves = Settings::value(Settings::Server::RelativeMouseMoves).toBool();
  m_preventFullscreenSwitch = Settings::value(Settings::Server::PreventFullscreenSwitch).toBool();
  m_fullscreenGamePrograms = deskflow::cleanFullscreenGameProgramList(
      Settings::value(Settings::Server::FullscreenGamePrograms).toStringList()
  );
  m_win32keepForeground = Settings::value(Settings::Server::Win32KeepForeground).toBool();
  m_enableSwitchDelay = Settings::value(Settings::Server::EnableSwitchDelay).toBool();
  m_switchDelay = Settings::value(Settings::Server::SwitchDelay).toInt();
  m_enableSwitchDoubleTap = Settings::value(Settings::Server::EnableSwitchDoubleTap).toBool();
  m_switchDoubleTap = Settings::value(Settings::Server::SwitchDoubleTap).toInt();
  m_defaultLockToComputerState = Settings::value(Settings::Server::DefaultLockToComputerState).toBool();
  m_disableLockToComputer = Settings::value(Settings::Server::DisableLockToComputer).toBool();
  m_enableClipboard = Settings::value(Settings::Server::EnableClipboard).toBool();
  m_clipboardSize = Settings::value(Settings::Server::ClipboardSize).toUInt();

  ui->lineConfigFile->setText(serverConfig().configFile());
  ui->groupExternalConfig->setChecked(serverConfig().useExternalConfig());

  refreshControls();

  ui->listHotkeys->clear();
  for (const Hotkey &hotkey : std::as_const(serverConfig().hotkeys()))
    ui->listHotkeys->addItem(hotkey.text());

  ui->computerSetupView->setModel(&m_computerSetupModel);

  auto &computers = serverConfig().computers();
  auto server = std::ranges::find_if(computers, [this](const Computer &computer) {
    return (computer.name() == serverConfig().getServerName());
  });

  if (server == computers.end()) {
    Computer serverComputer(serverConfig().getServerName());
    serverComputer.markAsServer();
    model().computer(m_columns / 2, m_rows / 2) = serverComputer;
  } else {
    server->markAsServer();
  }
  refreshRegisteredComputers();
  updateControls();
}

void ServerConfigDialog::resetFromSettings()
{
  m_serverConfig = m_originalServerConfig;
  m_displayLayoutEditor->setState(m_layoutState.value("topology").toObject(), false);
  m_sharingSettings->reset();
  loadFromConfig();
  if (ui->tabWidget->currentWidget() == ui->tabComputers) {
    ui->computerSetupView->reset();
    ui->computerSetupView->update();
  }
}

void ServerConfigDialog::refreshControls()
{
  ui->rbProtocolSynergy->setChecked(m_protocol == NetworkProtocol::Synergy);
  ui->rbProtocolBarrier->setChecked(m_protocol == NetworkProtocol::Barrier);
  ui->cbHeartbeat->setChecked(m_enableHeartbeat);
  ui->sbHeartbeat->setEnabled(ui->cbHeartbeat->isChecked());
  ui->sbHeartbeat->setValue(m_heartbeatRate);
  ui->cbRelativeMouseMoves->setChecked(m_relativeMouseMoves);
  ui->cbPreventFullscreenSwitch->setChecked(m_preventFullscreenSwitch);
  refreshFullscreenGamePrograms();
  ui->cbWin32KeepForeground->setChecked(m_win32keepForeground);
  ui->cbSwitchDelay->setChecked(m_enableSwitchDelay);
  ui->sbSwitchDelay->setEnabled(ui->cbSwitchDelay->isChecked());
  ui->sbSwitchDelay->setValue(m_switchDelay);
  ui->cbSwitchDoubleTap->setChecked(m_enableSwitchDoubleTap);
  ui->sbSwitchDoubleTap->setEnabled(ui->cbSwitchDoubleTap->isChecked());
  ui->sbSwitchDoubleTap->setValue(m_switchDoubleTap);
  ui->widgetExternalConfigControls->setEnabled(ui->groupExternalConfig->isChecked());
  toggleExternalConfig(ui->groupExternalConfig->isChecked());
  ui->cbDefaultLockToComputerState->setChecked(m_defaultLockToComputerState);
  ui->cbDisableLockToComputer->setChecked(m_disableLockToComputer);
  ui->cbEnableClipboard->setChecked(m_enableClipboard);
  ui->sbClipboardSizeLimit->setEnabled(m_enableClipboard);
  ui->sbClipboardSizeLimit->setValue(m_clipboardSize);
}

void ServerConfigDialog::initConnections() const
{
  connect(m_buttonBox, &SettingsDialogButtonBox::accepted, this, &ServerConfigDialog::save);
  connect(m_buttonBox, &SettingsDialogButtonBox::rejected, this, &ServerConfigDialog::cancel);
  connect(m_buttonBox, &SettingsDialogButtonBox::reset, this, &ServerConfigDialog::resetFromSettings);
  connect(m_buttonBox, &SettingsDialogButtonBox::restoreDefault, this, &ServerConfigDialog::restoreFromDefaults);
  connect(
      m_displayLayoutEditor, &deskflow::workspace::DisplayLayoutWidget::changed, this,
      &ServerConfigDialog::setButtonBoxEnabledButtons
  );
  connect(
      m_displayLayoutEditor, &deskflow::workspace::DisplayLayoutWidget::requestIdentify, this,
      &ServerConfigDialog::identifyDisplayRequested
  );
  connect(
      m_sharingSettings, &SharingSettingsWidget::modifiedChanged, this, &ServerConfigDialog::setButtonBoxEnabledButtons
  );
  connect(m_addRegisteredComputer, &QPushButton::clicked, this, qOverload<>(&ServerConfigDialog::addClient));
  connect(m_editRegisteredComputer, &QPushButton::clicked, this, &ServerConfigDialog::editRegisteredComputer);
  connect(m_removeRegisteredComputer, &QPushButton::clicked, this, &ServerConfigDialog::removeRegisteredComputer);
  connect(m_registeredComputers, &QListWidget::currentRowChanged, this, [this] { refreshLayoutControls(); });
  connect(m_registeredComputers, &QListWidget::itemDoubleClicked, this, &ServerConfigDialog::editRegisteredComputer);
  connect(ui->tabWidget, &QTabWidget::currentChanged, this, &ServerConfigDialog::setButtonBoxEnabledButtons);
  connect(ui->lblRemoveComputer, &RemoveComputerWidget::computerRemoved, this, &ServerConfigDialog::onComputerRemoved);
  connect(ui->btnNewHotkey, &QPushButton::clicked, this, &ServerConfigDialog::addHotkey);
  connect(ui->btnEditHotkey, &QPushButton::clicked, this, &ServerConfigDialog::editHotkey);
  connect(ui->btnRemoveHotkey, &QPushButton::clicked, this, &ServerConfigDialog::removeHotkey);
  connect(ui->listHotkeys, &QListView::doubleClicked, this, &ServerConfigDialog::editHotkey);
  connect(
      ui->listHotkeys->selectionModel(), &QItemSelectionModel::selectionChanged, this,
      &ServerConfigDialog::listHotkeysSelectionChanged
  );

  connect(ui->btnNewAction, &QPushButton::clicked, this, &ServerConfigDialog::addAction);
  connect(ui->btnEditAction, &QPushButton::clicked, this, &ServerConfigDialog::editAction);
  connect(ui->btnRemoveAction, &QPushButton::clicked, this, &ServerConfigDialog::removeAction);
  connect(ui->listActions, &QListView::doubleClicked, this, &ServerConfigDialog::editAction);
  connect(
      ui->listActions->selectionModel(), &QItemSelectionModel::selectionChanged, this,
      &ServerConfigDialog::listActionsSelectionChanged
  );

  connect(ui->rbProtocolBarrier, &QRadioButton::toggled, this, &ServerConfigDialog::toggleProtocol);
  connect(ui->cbHeartbeat, &QCheckBox::toggled, this, &ServerConfigDialog::toggleHeartbeat);
  connect(ui->sbHeartbeat, QOverload<int>::of(&QSpinBox::valueChanged), this, &ServerConfigDialog::setHeartbeat);
  connect(ui->cbWin32KeepForeground, &QCheckBox::toggled, this, &ServerConfigDialog::toggleWin32Foreground);
  connect(ui->cbSwitchDelay, &QCheckBox::toggled, this, &ServerConfigDialog::toggleSwitchDelay);
  connect(ui->sbSwitchDelay, QOverload<int>::of(&QSpinBox::valueChanged), this, &ServerConfigDialog::setSwitchDelay);
  connect(ui->cbSwitchDoubleTap, &QCheckBox::toggled, this, &ServerConfigDialog::toggleSwitchDoubleTap);
  connect(
      ui->sbSwitchDoubleTap, QOverload<int>::of(&QSpinBox::valueChanged), this, &ServerConfigDialog::setSwitchDoubleTap
  );

  connect(ui->cbRelativeMouseMoves, &QCheckBox::toggled, this, &ServerConfigDialog::toggleRelativeMouseMoves);
  connect(ui->cbPreventFullscreenSwitch, &QCheckBox::toggled, this, &ServerConfigDialog::togglePreventFullscreenSwitch);
  connect(m_addGameProgram, &QPushButton::clicked, this, &ServerConfigDialog::addFullscreenGameProgram);
  connect(m_removeGameProgram, &QPushButton::clicked, this, &ServerConfigDialog::removeFullscreenGameProgram);
  connect(m_gamePrograms, &QListWidget::currentRowChanged, this, [this] { updateGameProgramControls(); });
  connect(ui->cbEnableClipboard, &QCheckBox::toggled, this, &ServerConfigDialog::toggleClipboard);
  connect(ui->btnBrowseConfigFile, &QPushButton::clicked, this, &ServerConfigDialog::browseConfigFile);
  connect(ui->groupExternalConfig, &QGroupBox::toggled, this, &ServerConfigDialog::toggleExternalConfig);
  connect(ui->lineConfigFile, &QLineEdit::textChanged, this, &ServerConfigDialog::setServerConfig);

  connect(
      ui->sbClipboardSizeLimit, QOverload<int>::of(&QSpinBox::valueChanged), this,
      &ServerConfigDialog::setClipboardLimit
  );
  connect(
      ui->cbDefaultLockToComputerState, &QCheckBox::toggled, this, &ServerConfigDialog::toggleDefaultLockToComputerState
  );
  connect(ui->cbDisableLockToComputer, &QCheckBox::toggled, this, &ServerConfigDialog::toggleLockToComputer);
  connect(
      &m_computerSetupModel, &ComputerSetupModel::computersChanged, this,
      &ServerConfigDialog::setButtonBoxEnabledButtons
  );
  connect(
      &m_computerSetupModel, &ComputerSetupModel::computersChanged, this,
      &ServerConfigDialog::refreshRegisteredComputers
  );
  connect(Settings::instance(), &Settings::settingsWritableChanged, this, &ServerConfigDialog::updateControls);
}

void ServerConfigDialog::updateControls() const
{
  const bool writable = Settings::isWritable();
  ui->cbDefaultLockToComputerState->setEnabled(writable);
  ui->cbDisableLockToComputer->setEnabled(writable);
  ui->cbEnableClipboard->setEnabled(writable);
  ui->sbClipboardSizeLimit->setEnabled(writable);
  ui->rbProtocolBarrier->setEnabled(writable);
  ui->rbProtocolSynergy->setEnabled(writable);
  ui->cbHeartbeat->setEnabled(writable);
  ui->cbRelativeMouseMoves->setEnabled(writable);
  ui->cbPreventFullscreenSwitch->setEnabled(writable);
  ui->cbSwitchDelay->setEnabled(writable);
  ui->cbWin32KeepForeground->setEnabled(writable);
  ui->cbSwitchDoubleTap->setEnabled(writable);
  ui->sbSwitchDoubleTap->setEnabled(writable && ui->cbSwitchDoubleTap->isChecked());
  ui->sbSwitchDelay->setEnabled(writable && ui->cbSwitchDelay->isChecked());
  ui->groupExternalConfig->setEnabled(writable);
  setButtonBoxEnabledButtons();
}

void ServerConfigDialog::restoreFromDefaults()
{
  m_displayLayoutEditor->setState(m_layoutState.value("topology").toObject(), false);
  if (auto *mode = m_displayLayoutEditor->findChild<QCheckBox *>("independentMonitorMode"))
    mode->setChecked(false);
  m_sharingSettings->restoreDefaults();
  m_protocol = networkProtocolFromString(Settings::defaultValue(Settings::Server::Protocol).toString());
  m_enableHeartbeat = Settings::defaultValue(Settings::Server::EnableHeartbeat).toBool();
  m_heartbeatRate = Settings::defaultValue(Settings::Server::Heartbeat).toInt();
  m_relativeMouseMoves = Settings::defaultValue(Settings::Server::RelativeMouseMoves).toBool();
  m_preventFullscreenSwitch = Settings::defaultValue(Settings::Server::PreventFullscreenSwitch).toBool();
  m_fullscreenGamePrograms = Settings::defaultValue(Settings::Server::FullscreenGamePrograms).toStringList();
  m_win32keepForeground = Settings::defaultValue(Settings::Server::Win32KeepForeground).toBool();
  m_enableSwitchDelay = Settings::defaultValue(Settings::Server::EnableSwitchDelay).toBool();
  m_switchDelay = Settings::defaultValue(Settings::Server::SwitchDelay).toInt();
  m_enableSwitchDoubleTap = Settings::defaultValue(Settings::Server::EnableSwitchDoubleTap).toBool();
  m_switchDoubleTap = Settings::defaultValue(Settings::Server::SwitchDoubleTap).toInt();
  m_defaultLockToComputerState = Settings::defaultValue(Settings::Server::DefaultLockToComputerState).toBool();
  m_disableLockToComputer = Settings::defaultValue(Settings::Server::DisableLockToComputer).toBool();
  m_enableClipboard = Settings::defaultValue(Settings::Server::EnableClipboard).toBool();
  m_clipboardSize = Settings::defaultValue(Settings::Server::ClipboardSize).toUInt();

  ui->groupExternalConfig->setChecked(Settings::defaultValue(Settings::Server::ExternalConfig).toBool());
  ui->lineConfigFile->setText(Settings::defaultValue(Settings::Server::ExternalConfigFile).toString());

  refreshControls();
  updateControls();
}

void ServerConfigDialog::setServerConfig()
{
  const auto configFile = ui->lineConfigFile->text();
  if (!QFile::exists(configFile)) {
    m_buttonBox->enableSave(false);
    return;
  }
  setButtonBoxEnabledButtons();
}

bool ServerConfigDialog::addComputer(const QString &clientName, bool doSilent)
{
  bool isAccepted = false;
  Computer newComputer(clientName);

  if (ComputerSettingsDialog dlg(this, &newComputer, &model().m_computers);
      doSilent || dlg.exec() == QDialog::Accepted) {
    model().addComputer(newComputer);
    isAccepted = true;
  }

  ui->lblNewComputer->setEnabled(!model().isFull());
  return isAccepted;
}

bool ServerConfigDialog::isGeneralConfigModified() const
{
  return m_originalServerConfigIsExternal != ui->groupExternalConfig->isChecked() ||
         m_originalServerConfigUsesExternalFile != ui->lineConfigFile->text() ||
         m_protocol != Settings::networkProtocol() ||
         m_enableClipboard != Settings::value(Settings::Server::EnableClipboard).toBool() ||
         m_clipboardSize != Settings::value(Settings::Server::ClipboardSize).toUInt() ||
         m_enableHeartbeat != Settings::value(Settings::Server::EnableHeartbeat).toBool() ||
         m_heartbeatRate != Settings::value(Settings::Server::Heartbeat).toInt() ||
         m_enableSwitchDelay != Settings::value(Settings::Server::EnableSwitchDelay).toBool() ||
         m_switchDelay != Settings::value(Settings::Server::SwitchDelay).toInt() ||
         m_enableSwitchDoubleTap != Settings::value(Settings::Server::EnableSwitchDoubleTap).toBool() ||
         m_switchDoubleTap != Settings::value(Settings::Server::SwitchDoubleTap).toInt() ||
         m_relativeMouseMoves != Settings::value(Settings::Server::RelativeMouseMoves).toBool() ||
         m_preventFullscreenSwitch != Settings::value(Settings::Server::PreventFullscreenSwitch).toBool() ||
         m_fullscreenGamePrograms != deskflow::cleanFullscreenGameProgramList(
                                         Settings::value(Settings::Server::FullscreenGamePrograms).toStringList()
                                     ) ||
         m_win32keepForeground != Settings::value(Settings::Server::Win32KeepForeground).toBool() ||
         m_disableLockToComputer != Settings::value(Settings::Server::DisableLockToComputer).toBool() ||
         m_defaultLockToComputerState != Settings::value(Settings::Server::DefaultLockToComputerState).toBool();
}

bool ServerConfigDialog::isGeneralConfigDefault() const
{
  return ui->groupExternalConfig->isChecked() == Settings::defaultValue(Settings::Server::ExternalConfig).toBool() &&
         ui->lineConfigFile->text() == Settings::defaultValue(Settings::Server::ExternalConfigFile).toString() &&
         m_protocol == networkProtocolFromString(Settings::defaultValue(Settings::Server::Protocol).toString()) &&
         m_enableClipboard == Settings::defaultValue(Settings::Server::EnableClipboard).toBool() &&
         m_clipboardSize == Settings::defaultValue(Settings::Server::ClipboardSize).toUInt() &&
         m_enableHeartbeat == Settings::defaultValue(Settings::Server::EnableHeartbeat).toBool() &&
         m_heartbeatRate == Settings::defaultValue(Settings::Server::Heartbeat).toInt() &&
         m_enableSwitchDelay == Settings::defaultValue(Settings::Server::EnableSwitchDelay).toBool() &&
         m_switchDelay == Settings::defaultValue(Settings::Server::SwitchDelay).toInt() &&
         m_enableSwitchDoubleTap == Settings::defaultValue(Settings::Server::EnableSwitchDoubleTap).toBool() &&
         m_switchDoubleTap == Settings::defaultValue(Settings::Server::SwitchDoubleTap).toInt() &&
         m_relativeMouseMoves == Settings::defaultValue(Settings::Server::RelativeMouseMoves).toBool() &&
         m_preventFullscreenSwitch == Settings::defaultValue(Settings::Server::PreventFullscreenSwitch).toBool() &&
         m_fullscreenGamePrograms == Settings::defaultValue(Settings::Server::FullscreenGamePrograms).toStringList() &&
         m_win32keepForeground == Settings::defaultValue(Settings::Server::Win32KeepForeground).toBool() &&
         m_disableLockToComputer == Settings::defaultValue(Settings::Server::DisableLockToComputer).toBool() &&
         m_defaultLockToComputerState == Settings::defaultValue(Settings::Server::DefaultLockToComputerState).toBool();
}

void ServerConfigDialog::setButtonBoxEnabledButtons() const
{
  const bool writable = Settings::isWritable();
  const bool modified = isGeneralConfigModified() || !(m_originalServerConfig == m_serverConfig) ||
                        m_displayLayoutEditor->isModified() || m_sharingSettings->isModified();
  m_buttonBox->enableSave(writable && modified);
  m_buttonBox->enableReset(writable && modified);
  m_buttonBox->enableRestoreDefaults(
      writable && ((ui->tabWidget->currentIndex() >= 3 && !m_sharingSettings->isDefault()) ||
                   (ui->tabWidget->currentIndex() == 0 && m_displayLayoutEditor->independentEnabled()) ||
                   (ui->tabWidget->currentIndex() == 2 && !isGeneralConfigDefault()))
  );
  refreshLayoutControls();
  updateGameProgramControls();
}
