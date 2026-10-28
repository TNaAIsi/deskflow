/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "ClientConfigDialog.h"
#include "ui_ClientConfigDialog.h"

#include "common/Settings.h"
#include "gui/widgets/SettingsDialogButtonBox.h"
#include "gui/widgets/SharingSettingsWidget.h"

#include <QCheckBox>
#include <QLabel>
#include <QPushButton>
#include <QTabWidget>
#include <QVBoxLayout>

ClientConfigDialog::ClientConfigDialog(QWidget *parent)
    : QDialog(parent),
      ui(new Ui::ClientConfigDialog),
      m_buttonBox{new SettingsDialogButtonBox(this)}
{
  ui->setupUi(this);
  setMinimumSize(820, 650);
  resize(920, 760);
  setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
  auto *outer = qobject_cast<QVBoxLayout *>(layout());
  m_tabs = new QTabWidget(this);
  m_tabs->setObjectName("clientSettingsTabs");
  auto *inputPage = new QWidget(m_tabs);
  inputPage->setObjectName("clientInputPage");
  auto *inputLayout = new QVBoxLayout(inputPage);
  while (outer->count() > 0) {
    auto *item = outer->takeAt(0);
    if (item->widget()) {
      inputLayout->addWidget(item->widget());
      delete item;
    } else {
      inputLayout->addItem(item);
    }
  }
  m_nativeKeyboard = new QCheckBox(tr("Use a virtual HID keyboard for physical key input"), inputPage);
  m_nativeKeyboard->setObjectName("nativeKeyboardEnabled");
  m_nativeKeyboard->setToolTip(
      tr("Use this Mac's keyboard layout, input method and Caps Lock behavior. Set up the keyboard component before "
         "enabling this option.")
  );
  m_nativeKeyboardStatus =
      new QLabel(tr("Virtual keyboard support requires the system component to be installed and enabled."), inputPage);
  m_nativeKeyboardStatus->setObjectName("nativeKeyboardStatus");
  m_nativeKeyboardStatus->setWordWrap(true);
  m_nativeKeyboardSetup = new QPushButton(tr("Set up virtual keyboard…"), inputPage);
  m_nativeKeyboardSetup->setObjectName("setupNativeKeyboard");
  m_nativeKeyboardSetup->setAutoDefault(false);
  auto *nativeRow = new QHBoxLayout;
  nativeRow->addWidget(m_nativeKeyboardStatus, 1);
  nativeRow->addWidget(m_nativeKeyboardSetup);
  inputLayout->insertWidget(1, m_nativeKeyboard);
  inputLayout->insertLayout(2, nativeRow);
#ifndef Q_OS_MACOS
  m_nativeKeyboard->hide();
  m_nativeKeyboardStatus->hide();
  m_nativeKeyboardSetup->hide();
#endif
  inputLayout->addStretch();
  m_sharing = new SharingSettingsWidget(false, this);
  m_tabs->addTab(inputPage, tr("Keyboard and mouse"));
  m_tabs->addTab(m_sharing->soundPage(), tr("Sound"));
  m_tabs->addTab(m_sharing->filesPage(), tr("Files and cache"));
  outer->addWidget(m_tabs, 1);
  layout()->addWidget(m_buttonBox);
  initConnections();
  load();
  updateControls();
  setButtonBoxEnabledButtons();
}

ClientConfigDialog::~ClientConfigDialog()
{
  for (auto *child : findChildren<QObject *>())
    disconnect(child, nullptr, this, nullptr);
  delete ui;
}

void ClientConfigDialog::setSharingService(deskflow::workspace::WorkspaceService *service)
{
  m_sharing->setService(service);
}

void ClientConfigDialog::setSharingState(const QJsonObject &state)
{
  m_sharing->refresh(state);
}

bool ClientConfigDialog::requiresCoreRestart() const
{
  return m_requiresCoreRestart;
}

void ClientConfigDialog::setNativeKeyboardStatus(const QString &status)
{
  m_nativeKeyboardStatus->setText(status);
}

void ClientConfigDialog::changeEvent(QEvent *e)
{
  QDialog::changeEvent(e);
  if (e->type() == QEvent::LanguageChange)
    ui->retranslateUi(this);
  if (e->type() == QEvent::LanguageChange && m_tabs) {
    m_tabs->setTabText(0, tr("Keyboard and mouse"));
    m_tabs->setTabText(1, tr("Sound"));
    m_tabs->setTabText(2, tr("Files and cache"));
  }
}

void ClientConfigDialog::updateControls() const
{
  const auto writable = Settings::isWritable();
  ui->cbDynamicConnectTime->setEnabled(writable);
#ifdef Q_OS_MACOS
  ui->cbLanguageSync->setEnabled(writable && !m_nativeKeyboard->isChecked() && !ui->cbCapsLockInputSource->isChecked());
  ui->cbCapsLockInputSource->setEnabled(writable && !m_nativeKeyboard->isChecked());
  m_nativeKeyboard->setEnabled(writable);
#else
  ui->cbLanguageSync->setEnabled(writable);
  ui->cbCapsLockInputSource->hide();
#endif
  ui->cbXScrollInvert->setEnabled(writable);
  ui->sbXScrollScale->setEnabled(writable);
  ui->cbYScrollInvert->setEnabled(writable);
  ui->sbYScrollScale->setEnabled(writable);
}

void ClientConfigDialog::initConnections() const
{
  connect(m_buttonBox, &SettingsDialogButtonBox::accepted, this, &ClientConfigDialog::save);
  connect(m_buttonBox, &SettingsDialogButtonBox::rejected, this, &QDialog::reject);
  connect(m_buttonBox, &SettingsDialogButtonBox::reset, this, &ClientConfigDialog::load);
  connect(m_buttonBox, &SettingsDialogButtonBox::restoreDefault, this, &ClientConfigDialog::resetToDefault);

  connect(
      ui->cbDynamicConnectTime, &QCheckBox::checkStateChanged, this, &ClientConfigDialog::setButtonBoxEnabledButtons
  );
  connect(ui->cbLanguageSync, &QCheckBox::checkStateChanged, this, &ClientConfigDialog::setButtonBoxEnabledButtons);
  connect(ui->cbCapsLockInputSource, &QCheckBox::checkStateChanged, this, [this] {
    updateControls();
    setButtonBoxEnabledButtons();
  });
  connect(m_nativeKeyboard, &QCheckBox::toggled, this, [this] {
    updateControls();
    setButtonBoxEnabledButtons();
  });
  connect(m_nativeKeyboardSetup, &QPushButton::clicked, this, &ClientConfigDialog::requestNativeKeyboardSetup);
  connect(ui->cbYScrollInvert, &QCheckBox::checkStateChanged, this, &ClientConfigDialog::setButtonBoxEnabledButtons);
  connect(ui->sbYScrollScale, &QDoubleSpinBox::valueChanged, this, &ClientConfigDialog::setButtonBoxEnabledButtons);
  connect(ui->cbXScrollInvert, &QCheckBox::checkStateChanged, this, &ClientConfigDialog::setButtonBoxEnabledButtons);
  connect(ui->sbXScrollScale, &QDoubleSpinBox::valueChanged, this, &ClientConfigDialog::setButtonBoxEnabledButtons);
  connect(Settings::instance(), &Settings::settingsWritableChanged, this, &ClientConfigDialog::updateControls);
  connect(m_sharing, &SharingSettingsWidget::modifiedChanged, this, &ClientConfigDialog::setButtonBoxEnabledButtons);
}

bool ClientConfigDialog::isModified() const
{
  return isInputModified() || m_sharing->isModified();
}

bool ClientConfigDialog::isInputModified() const
{
  return (ui->cbDynamicConnectTime->isChecked() != Settings::value(Settings::Client::DynamicConnectionRetry).toBool()
         ) ||
         (ui->cbLanguageSync->isChecked() != Settings::value(Settings::Client::LanguageSync).toBool()) ||
         (ui->cbCapsLockInputSource->isChecked() != Settings::value(Settings::Client::CapsLockInputSource).toBool()) ||
         (m_nativeKeyboard->isChecked() != Settings::value(Settings::Client::NativeKeyboard).toBool()) ||
         (ui->cbYScrollInvert->isChecked() != Settings::value(Settings::Client::InvertYScroll).toBool()) ||
         (ui->sbYScrollScale->value() != Settings::value(Settings::Client::YScrollScale).toDouble()) ||
         (ui->cbXScrollInvert->isChecked() != Settings::value(Settings::Client::InvertXScroll).toBool()) ||
         (ui->sbXScrollScale->value() != Settings::value(Settings::Client::XScrollScale).toDouble());
}

bool ClientConfigDialog::isDefault() const
{
  return (ui->cbDynamicConnectTime->isChecked() ==
          Settings::defaultValue(Settings::Client::DynamicConnectionRetry).toBool()) &&
         (ui->cbLanguageSync->isChecked() == Settings::defaultValue(Settings::Client::LanguageSync).toBool()) &&
         (ui->cbCapsLockInputSource->isChecked() ==
          Settings::defaultValue(Settings::Client::CapsLockInputSource).toBool()) &&
         (m_nativeKeyboard->isChecked() == Settings::defaultValue(Settings::Client::NativeKeyboard).toBool()) &&
         (ui->cbYScrollInvert->isChecked() == Settings::defaultValue(Settings::Client::InvertYScroll).toBool()) &&
         (ui->sbYScrollScale->value() == Settings::defaultValue(Settings::Client::YScrollScale).toDouble()) &&
         (ui->cbXScrollInvert->isChecked() == Settings::defaultValue(Settings::Client::InvertXScroll).toBool()) &&
         (ui->sbXScrollScale->value() == Settings::defaultValue(Settings::Client::XScrollScale).toDouble()) &&
         m_sharing->isDefault();
}

void ClientConfigDialog::setButtonBoxEnabledButtons() const
{
  const bool modified = isModified();
  m_buttonBox->enableSave(modified);
  m_buttonBox->enableReset(modified);
  m_buttonBox->enableRestoreDefaults(!isDefault());
}

void ClientConfigDialog::load()
{
  ui->cbDynamicConnectTime->setChecked(Settings::value(Settings::Client::DynamicConnectionRetry).toBool());
  ui->cbLanguageSync->setChecked(Settings::value(Settings::Client::LanguageSync).toBool());
  ui->cbCapsLockInputSource->setChecked(Settings::value(Settings::Client::CapsLockInputSource).toBool());
  m_nativeKeyboard->setChecked(Settings::value(Settings::Client::NativeKeyboard).toBool());
  ui->cbYScrollInvert->setChecked(Settings::value(Settings::Client::InvertYScroll).toBool());
  ui->sbYScrollScale->setValue(Settings::value(Settings::Client::YScrollScale).toDouble());
  ui->cbXScrollInvert->setChecked(Settings::value(Settings::Client::InvertXScroll).toBool());
  ui->sbXScrollScale->setValue(Settings::value(Settings::Client::XScrollScale).toDouble());
  m_sharing->reset();
  setButtonBoxEnabledButtons();
}

void ClientConfigDialog::resetToDefault()
{
  ui->cbDynamicConnectTime->setChecked(Settings::defaultValue(Settings::Client::DynamicConnectionRetry).toBool());
  ui->cbLanguageSync->setChecked(Settings::defaultValue(Settings::Client::LanguageSync).toBool());
  ui->cbCapsLockInputSource->setChecked(Settings::defaultValue(Settings::Client::CapsLockInputSource).toBool());
  m_nativeKeyboard->setChecked(Settings::defaultValue(Settings::Client::NativeKeyboard).toBool());
  ui->cbYScrollInvert->setChecked(Settings::defaultValue(Settings::Client::InvertYScroll).toBool());
  ui->sbYScrollScale->setValue(Settings::defaultValue(Settings::Client::YScrollScale).toDouble());
  ui->cbXScrollInvert->setChecked(Settings::defaultValue(Settings::Client::InvertXScroll).toBool());
  ui->sbXScrollScale->setValue(Settings::defaultValue(Settings::Client::XScrollScale).toDouble());
  m_sharing->restoreDefaults();
  setButtonBoxEnabledButtons();
}

void ClientConfigDialog::save()
{
  m_requiresCoreRestart = isInputModified();
  Settings::setValue(Settings::Client::DynamicConnectionRetry, ui->cbDynamicConnectTime->isChecked());
  Settings::setValue(Settings::Client::LanguageSync, ui->cbLanguageSync->isChecked());
  Settings::setValue(Settings::Client::CapsLockInputSource, ui->cbCapsLockInputSource->isChecked());
  Settings::setValue(Settings::Client::NativeKeyboard, m_nativeKeyboard->isChecked());
  Settings::setValue(Settings::Client::InvertYScroll, ui->cbYScrollInvert->isChecked());
  Settings::setValue(Settings::Client::YScrollScale, ui->sbYScrollScale->value());
  Settings::setValue(Settings::Client::InvertXScroll, ui->cbXScrollInvert->isChecked());
  Settings::setValue(Settings::Client::XScrollScale, ui->sbXScrollScale->value());
  m_sharing->saveOptions();
  QDialog::accept();
}
