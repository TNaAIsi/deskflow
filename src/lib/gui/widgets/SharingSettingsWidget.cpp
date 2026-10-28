/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "SharingSettingsWidget.h"

#include "common/Settings.h"
#include "workspace/AudioService.h"
#include "workspace/WorkspaceService.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTableWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>

namespace {
qint64 number(const QJsonValue &value)
{
  return value.isString() ? value.toString().toLongLong() : value.toInteger();
}

QString bytesLabel(qint64 bytes)
{
  if (bytes >= 1024 * 1024 * 1024)
    return QString::number(bytes / double(1024 * 1024 * 1024), 'f', 2) + " GiB";
  if (bytes >= 1024 * 1024)
    return QString::number(bytes / double(1024 * 1024), 'f', 1) + " MiB";
  if (bytes >= 1024)
    return QString::number(bytes / 1024.0, 'f', 1) + " KiB";
  return QString::number(bytes) + " B";
}

QString transferState(const QString &state)
{
  if (state == "complete")
    return SharingSettingsWidget::tr("Complete");
  if (state == "publishing")
    return SharingSettingsWidget::tr("Updating clipboard");
  if (state == "clipboardSkipped")
    return SharingSettingsWidget::tr("Received; newer clipboard kept");
  if (state == "transferring")
    return SharingSettingsWidget::tr("Transferring");
  if (state == "requesting")
    return SharingSettingsWidget::tr("Requesting");
  if (state == "offered")
    return SharingSettingsWidget::tr("Waiting to receive");
  if (state == "cancelled")
    return SharingSettingsWidget::tr("Cancelled");
  if (state == "interrupted")
    return SharingSettingsWidget::tr("Interrupted");
  if (state == "error")
    return SharingSettingsWidget::tr("Error");
  return state;
}

QString recordKey(const QJsonObject &record)
{
  return record.value("historyKey")
      .toString(
          record.value("peer").toString() + "/" + record.value("direction").toString() + "/" +
          record.value("id").toString()
      );
}

QString cacheKey(const QJsonObject &record)
{
  return record.value("cacheId").toString(record.value("id").toString());
}

QTableWidget *makeTable(QWidget *parent, const QStringList &headers, const QString &name)
{
  auto *table = new QTableWidget(parent);
  table->setObjectName(name);
  table->setColumnCount(headers.size());
  table->setHorizontalHeaderLabels(headers);
  table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
  table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
  table->verticalHeader()->hide();
  table->setSelectionBehavior(QAbstractItemView::SelectRows);
  table->setSelectionMode(QAbstractItemView::SingleSelection);
  table->setEditTriggers(QAbstractItemView::NoEditTriggers);
  return table;
}

void selectData(QComboBox *combo, const QString &value, const QString &offlineLabel)
{
  if (!value.isEmpty() && combo->findData(value) < 0)
    combo->addItem(offlineLabel.arg(value), value);
  combo->setCurrentIndex(qMax(0, combo->findData(value)));
}
} // namespace

SharingSettingsWidget::SharingSettingsWidget(bool server, QWidget *parent)
    : QObject(parent),
      m_server(server),
#ifdef Q_OS_WIN
      m_canSendSound(server),
#else
      m_canSendSound(false),
#endif
      m_soundPage(new QWidget(parent)),
      m_filesPage(new QWidget(parent)),
      m_soundEnabled(
          new QCheckBox(server ? tr("Share this computer's system sound") : tr("Play the server's sound"), m_soundPage)
      ),
      m_soundPeer(new QComboBox(m_soundPage)),
      m_soundOutput(new QComboBox(m_soundPage)),
      m_soundStatus(new QLabel(m_soundPage)),
      m_fileClipboard(new QCheckBox(tr("Share copied files and folders"), m_filesPage)),
      m_autoLimit(new QSpinBox(m_filesPage)),
      m_filesStatus(new QLabel(m_filesPage)),
      m_errors(new QLabel(m_filesPage)),
      m_offers(new QListWidget(m_filesPage)),
      m_receiveOffer(new QPushButton(tr("Receive selected copied files…"), m_filesPage)),
      m_transfers(makeTable(
          m_filesPage, {tr("File"), tr("Computer"), tr("Direction"), tr("Progress"), tr("State"), tr("Updated")},
          "fileTransfers"
      )),
      m_cancelTransfer(new QPushButton(tr("Cancel transfer"), m_filesPage)),
      m_revealTransfer(new QPushButton(tr("Open received location"), m_filesPage)),
      m_clearHistory(new QPushButton(tr("Clear completed history"), m_filesPage)),
      m_cacheSummary(new QLabel(m_filesPage)),
      m_cacheLocation(new QLabel(m_filesPage)),
      m_cache(makeTable(m_filesPage, {tr("Cached files"), tr("Size"), tr("Files"), tr("State")}, "receivedFileCache")),
      m_revealCache(new QPushButton(tr("Open selected cache"), m_filesPage)),
      m_removeCache(new QPushButton(tr("Remove selected cache"), m_filesPage)),
      m_clearCache(new QPushButton(tr("Clear unused cache"), m_filesPage))
{
  setObjectName("sharingSettings");
  m_soundPage->setObjectName("soundSettingsPage");
  m_filesPage->setObjectName("fileManagementPage");
  m_soundEnabled->setObjectName("sharingSoundEnabled");
  m_soundPeer->setObjectName("sharingSoundPeer");
  m_soundOutput->setObjectName("sharingSoundOutput");
  m_soundStatus->setObjectName("sharingSoundStatus");
  m_fileClipboard->setObjectName("sharingFileClipboard");
  m_autoLimit->setObjectName("automaticFileLimit");
  m_filesStatus->setObjectName("fileSharingStatus");
  m_offers->setObjectName("pendingCopiedFiles");
  m_receiveOffer->setObjectName("receiveCopiedFiles");
  m_cancelTransfer->setObjectName("cancelFileTransfer");
  m_revealTransfer->setObjectName("openReceivedLocation");
  m_clearHistory->setObjectName("clearFileHistory");
  m_cacheSummary->setObjectName("fileCacheSummary");
  m_cacheLocation->setObjectName("fileCacheLocation");
  m_revealCache->setObjectName("openFileCache");
  m_removeCache->setObjectName("removeFileCache");
  m_clearCache->setObjectName("clearFileCache");

  auto *sound = new QVBoxLayout(m_soundPage);
  auto *soundHelp = new QLabel(
      server ? tr("Share the Windows system output with connected clients. Sound routing stays fixed when the mouse "
                  "changes computers; local playback continues.")
             : tr("Receive the server's system sound and play it through this computer's selected output. Sound keeps "
                  "playing when the pointer returns to the server."),
      m_soundPage
  );
  soundHelp->setWordWrap(true);
  sound->addWidget(soundHelp);
  sound->addWidget(m_soundEnabled);
  auto *soundForm = new QFormLayout;
  soundForm->addRow(server ? tr("Send sound to") : tr("Receive sound from"), m_soundPeer);
  if (!server) {
    soundForm->addRow(tr("Play through"), m_soundOutput);
  } else {
    m_soundOutput->hide();
  }
  sound->addLayout(soundForm);
  if (server && !m_canSendSound) {
    m_soundEnabled->hide();
    soundForm->setRowVisible(m_soundPeer, false);
    soundHelp->setText(tr("System sound capture is available when Windows is the server."));
  }
  m_soundStatus->setWordWrap(true);
  sound->addWidget(m_soundStatus);
  auto *latency = new QLabel(
      tr("Playback includes network and output-device buffering. Wired headphones avoid additional Bluetooth delay."),
      m_soundPage
  );
  latency->setWordWrap(true);
  sound->addWidget(latency);
  sound->addStretch();

  auto *files = new QVBoxLayout(m_filesPage);
  files->addWidget(m_fileClipboard);
  auto *fileOptions = new QFormLayout;
  m_autoLimit->setRange(0, 1024 * 1024);
  m_autoLimit->setSuffix(tr(" MiB"));
  m_autoLimit->setSpecialValueText(tr("No size limit"));
  fileOptions->addRow(tr("Automatically receive copied files up to"), m_autoLimit);
  files->addLayout(fileOptions);
  auto *fileHelp = new QLabel(
      tr("Copy files in Explorer or Finder, wait for the transfer to finish, then paste on the other computer. Manage "
         "transfer records and received cache below."),
      m_filesPage
  );
  fileHelp->setWordWrap(true);
  files->addWidget(fileHelp);
  m_filesStatus->setWordWrap(true);
  files->addWidget(m_filesStatus);
  m_offers->setMaximumHeight(65);
  files->addWidget(m_offers);
  files->addWidget(m_receiveOffer);
  files->addWidget(m_transfers, 3);
  auto *transferButtons = new QHBoxLayout;
  for (auto *button : {m_cancelTransfer, m_revealTransfer, m_clearHistory})
    transferButtons->addWidget(button);
  transferButtons->addStretch();
  files->addLayout(transferButtons);
  auto *cacheGroup = new QGroupBox(tr("Received file cache"), m_filesPage);
  auto *cacheLayout = new QVBoxLayout(cacheGroup);
  m_cacheSummary->setWordWrap(true);
  m_cacheLocation->setWordWrap(true);
  m_cacheLocation->setTextInteractionFlags(Qt::TextSelectableByMouse);
  cacheLayout->addWidget(m_cacheSummary);
  cacheLayout->addWidget(m_cacheLocation);
  m_cache->setMinimumHeight(100);
  cacheLayout->addWidget(m_cache, 1);
  auto *cacheButtons = new QHBoxLayout;
  auto *refreshCache = new QPushButton(tr("Refresh"), cacheGroup);
  refreshCache->setObjectName("refreshFileState");
  for (auto *button : {m_revealCache, m_removeCache, m_clearCache, refreshCache})
    cacheButtons->addWidget(button);
  cacheButtons->addStretch();
  cacheLayout->addLayout(cacheButtons);
  auto *cacheHelp = new QLabel(
      tr("Cache removal affects received copies only. Files used by the clipboard or an active transfer are protected. "
         "Clearing history does not delete files."),
      cacheGroup
  );
  cacheHelp->setWordWrap(true);
  cacheLayout->addWidget(cacheHelp);
  files->addWidget(cacheGroup, 2);
  m_errors->setWordWrap(true);
  m_errors->setTextInteractionFlags(Qt::TextSelectableByMouse);
  files->addWidget(m_errors);

  for (auto *button : m_filesPage->findChildren<QPushButton *>())
    button->setAutoDefault(false);
  connect(m_soundEnabled, &QCheckBox::toggled, this, &SharingSettingsWidget::optionsChanged);
  connect(m_fileClipboard, &QCheckBox::toggled, this, &SharingSettingsWidget::optionsChanged);
  connect(m_autoLimit, &QSpinBox::valueChanged, this, &SharingSettingsWidget::optionsChanged);
  for (auto *combo : {m_soundPeer, m_soundOutput})
    connect(combo, &QComboBox::currentIndexChanged, this, &SharingSettingsWidget::optionsChanged);
  connect(Settings::instance(), &Settings::settingsWritableChanged, this, &SharingSettingsWidget::updateOptionControls);
  connect(m_offers, &QListWidget::currentRowChanged, this, &SharingSettingsWidget::updateManagementControls);
  connect(m_transfers, &QTableWidget::itemSelectionChanged, this, &SharingSettingsWidget::updateManagementControls);
  connect(m_cache, &QTableWidget::itemSelectionChanged, this, &SharingSettingsWidget::updateManagementControls);
  connect(m_receiveOffer, &QPushButton::clicked, this, &SharingSettingsWidget::requestSelectedOffer);
  connect(m_cancelTransfer, &QPushButton::clicked, this, &SharingSettingsWidget::cancelSelectedTransfer);
  connect(m_revealTransfer, &QPushButton::clicked, this, &SharingSettingsWidget::revealSelectedTransfer);
  connect(m_revealCache, &QPushButton::clicked, this, &SharingSettingsWidget::revealSelectedCache);
  connect(m_removeCache, &QPushButton::clicked, this, &SharingSettingsWidget::removeSelectedCache);
  connect(m_clearHistory, &QPushButton::clicked, this, [this] { sendManagement({{"action", "clearFileHistory"}}); });
  connect(m_clearCache, &QPushButton::clicked, this, [this] { sendManagement({{"action", "clearFileCache"}}); });
  connect(refreshCache, &QPushButton::clicked, this, [this] { sendManagement({{"action", "refreshFileState"}}); });
  m_poll.setInterval(2000);
  connect(&m_poll, &QTimer::timeout, this, [this] {
    const auto *source = m_service ? m_service.data() : m_inventory.data();
    if (source)
      refresh(source->state());
  });
  reset();
  setService(nullptr);
}

SharingSettingsWidget::~SharingSettingsWidget() = default;

QWidget *SharingSettingsWidget::soundPage() const
{
  return m_soundPage;
}
QWidget *SharingSettingsWidget::filesPage() const
{
  return m_filesPage;
}

QJsonObject SharingSettingsWidget::readOptions() const
{
  auto options = deskflow::workspace::WorkspaceService::defaultOptions();
  const auto stored =
      QJsonDocument::fromJson(Settings::value(Settings::Workspace::Options).toString().toUtf8()).object();
  for (auto it = stored.begin(); it != stored.end(); ++it)
    options.insert(it.key(), it.value());
  // The removed master toggle used to disable every resource. Preserve that
  // intent with the individual controls, while keeping transport availability
  // independent of a hidden setting.
  if (!options.value("enabled").toBool(true)) {
    options["enabled"] = true;
    options["fileClipboard"] = false;
    options["sendEnabled"] = false;
    options["receiveEnabled"] = false;
  }
  return options;
}

QJsonObject SharingSettingsWidget::draftOptions() const
{
  auto options = m_originalOptions;
  options["enabled"] = true;
  options["fileClipboard"] = m_fileClipboard->isChecked();
  options["automaticFileMiB"] = m_autoLimit->value();
  if (m_server) {
    options["sendEnabled"] = m_canSendSound && m_soundEnabled->isChecked();
    options["receiveEnabled"] = false;
    options["targetPeer"] = m_soundPeer->currentData().toString();
  } else {
    options["sendEnabled"] = false;
    options["receiveEnabled"] = m_soundEnabled->isChecked();
    options["sourcePeer"] = m_soundPeer->currentData().toString();
    options["outputDeviceId"] = m_soundOutput->currentData().toString();
  }
  return options;
}

bool SharingSettingsWidget::isModified() const
{
  return draftOptions() != m_originalOptions;
}

bool SharingSettingsWidget::isDefault() const
{
  const auto options = draftOptions();
  const auto defaults = deskflow::workspace::WorkspaceService::defaultOptions();
  QStringList keys{"fileClipboard", "automaticFileMiB", "sendEnabled", "receiveEnabled"};
  keys.append(m_server ? QStringList{"targetPeer"} : QStringList{"sourcePeer", "outputDeviceId"});
  for (const auto &key : keys)
    if (options.value(key) != defaults.value(key))
      return false;
  return true;
}

void SharingSettingsWidget::loadOptions(const QJsonObject &options)
{
  m_loading = true;
  m_fileClipboard->setChecked(options.value("fileClipboard").toBool(true));
  m_autoLimit->setValue(options.value("automaticFileMiB").toInt());
  m_soundEnabled->setChecked(options.value(m_server ? "sendEnabled" : "receiveEnabled").toBool());
  updatePeers(
      m_soundPeer, m_state.value("peers").toArray(), m_server ? tr("All connected clients") : tr("Connected server")
  );
  selectData(m_soundPeer, options.value(m_server ? "targetPeer" : "sourcePeer").toString(), tr("%1 (offline)"));
  m_soundOutput->clear();
  m_soundOutput->addItem(tr("System default output"), QString());
  const auto devices = m_state.contains("audioDevices") ? m_state.value("audioDevices").toArray()
                                                        : deskflow::workspace::AudioService::outputDevices();
  for (const auto &value : devices) {
    const auto device = value.toObject();
    m_soundOutput->addItem(device.value("name").toString(), device.value("id").toString());
  }
  selectData(m_soundOutput, options.value("outputDeviceId").toString(), tr("%1 (offline)"));
  m_loading = false;
  updateOptionControls();
}

void SharingSettingsWidget::reset()
{
  m_originalOptions = readOptions();
  loadOptions(m_originalOptions);
  Q_EMIT modifiedChanged();
}

void SharingSettingsWidget::restoreDefaults()
{
  loadOptions(deskflow::workspace::WorkspaceService::defaultOptions());
  Q_EMIT modifiedChanged();
}

void SharingSettingsWidget::saveOptions()
{
  if (!Settings::isWritable())
    return;
  const auto edited = draftOptions();
  auto options = readOptions();
  QStringList keys{"enabled", "fileClipboard", "automaticFileMiB", "sendEnabled", "receiveEnabled"};
  keys.append(m_server ? QStringList{"targetPeer"} : QStringList{"sourcePeer", "outputDeviceId"});
  for (const auto &key : keys)
    options.insert(key, edited.value(key));
  Settings::setValue(
      Settings::Workspace::Options, QString::fromUtf8(QJsonDocument(options).toJson(QJsonDocument::Compact))
  );
  Settings::save(false);
  m_originalOptions = options;
  if (m_service)
    m_service->command({{"action", "options"}, {"options", options}});
  Q_EMIT modifiedChanged();
}

void SharingSettingsWidget::setService(deskflow::workspace::WorkspaceService *service)
{
  if (m_service == service && (service || m_inventory))
    return;
  if (m_service)
    disconnect(m_service, nullptr, this, nullptr);
  if (m_inventory) {
    delete m_inventory.data();
    m_inventory = nullptr;
  }
  m_service = service;
  if (service) {
    connect(service, &deskflow::workspace::WorkspaceService::stateChanged, this, &SharingSettingsWidget::refresh);
    connect(service, &QObject::destroyed, this, [this] { setService(nullptr); });
    refresh(service->state());
  } else {
    m_inventory = new deskflow::workspace::WorkspaceService(m_server, this);
    connect(m_inventory, &deskflow::workspace::WorkspaceService::stateChanged, this, &SharingSettingsWidget::refresh);
    refresh(m_inventory->state());
    m_inventory->command({{"action", "refreshFileState"}});
  }
  m_poll.start();
}

void SharingSettingsWidget::updatePeers(QComboBox *combo, const QJsonArray &peers, const QString &emptyLabel)
{
  const QSignalBlocker blocker(combo);
  const auto selected = combo->currentData().toString();
  combo->clear();
  combo->addItem(emptyLabel, QString());
  for (const auto &value : peers) {
    const auto name = value.toObject().value("name").toString();
    combo->addItem(name, name);
  }
  selectData(combo, selected, tr("%1 (offline)"));
}

void SharingSettingsWidget::refresh(const QJsonObject &state)
{
  const bool modified = isModified();
  m_state = state;
  if (!modified && !state.value("options").toObject().isEmpty()) {
    m_originalOptions = state.value("options").toObject();
    loadOptions(m_originalOptions);
  } else {
    updatePeers(
        m_soundPeer, state.value("peers").toArray(), m_server ? tr("All connected clients") : tr("Connected server")
    );
    if (state.contains("audioDevices")) {
      const QSignalBlocker blocker(m_soundOutput);
      const auto selected = m_soundOutput->currentData().toString();
      m_soundOutput->clear();
      m_soundOutput->addItem(tr("System default output"), QString());
      for (const auto &value : state.value("audioDevices").toArray()) {
        const auto device = value.toObject();
        m_soundOutput->addItem(device.value("name").toString(), device.value("id").toString());
      }
      selectData(m_soundOutput, selected, tr("%1 (offline)"));
    }
  }
  m_soundStatus->setText(state.value("audioStatus").toString());
  m_filesStatus->setText(
      state.value("running").toBool()
          ? tr("%1 connected computer(s). Copied files are shared automatically within the configured limit.")
                .arg(state.value("peers").toArray().size())
          : tr("File sharing is stopped. Received files and cache remain available for management.")
  );
  QStringList errors;
  for (const auto &value : state.value("errors").toArray())
    errors.append(value.toString());
  m_errors->setText(errors.isEmpty() ? QString() : errors.last());

  const auto offers = state.value("offers").toArray();
  if (offers != m_renderedOffers) {
    m_renderedOffers = offers;
    const auto selectedOffer = m_offers->currentItem()
                                   ? m_offers->currentItem()->data(Qt::UserRole).toJsonObject().value("id").toString()
                                   : QString();
    m_offers->clear();
    for (const auto &value : offers) {
      const auto offer = value.toObject();
      auto *item = new QListWidgetItem(
          tr("%1 — %2 (%3)")
              .arg(
                  offer.value("peer").toString(), offer.value("name").toString(),
                  bytesLabel(number(offer.value("totalBytes")))
              ),
          m_offers
      );
      item->setData(Qt::UserRole, offer);
      if (offer.value("id").toString() == selectedOffer)
        m_offers->setCurrentItem(item);
    }
  }
  m_offers->setVisible(m_offers->count() > 0);
  m_receiveOffer->setVisible(m_offers->count() > 0);

  const auto transferRecords = state.value("transfers").toArray();
  if (transferRecords != m_renderedTransfers) {
    m_renderedTransfers = transferRecords;
    const auto selectedRecord = recordKey(selectedTransfer());
    auto transfers = transferRecords.toVariantList();
    std::stable_sort(transfers.begin(), transfers.end(), [](const QVariant &left, const QVariant &right) {
      return left.toMap().value("updatedAt").toLongLong() > right.toMap().value("updatedAt").toLongLong();
    });
    m_transfers->setRowCount(transfers.size());
    for (qsizetype row = 0; row < transfers.size(); ++row) {
      const auto transfer = QJsonObject::fromVariantMap(transfers.at(row).toMap());
      const auto total = number(transfer.value("totalBytes")), done = number(transfer.value("transferredBytes"));
      auto status = transferState(transfer.value("state").toString());
      if (transfer.value("cacheState").toString() == "removed")
        status += tr(" · cache removed");
      const auto updated = number(transfer.value("updatedAt"));
      const QStringList columns{
          transfer.value("name").toString(),
          transfer.value("peer").toString(),
          transfer.value("direction").toString() == "receive" ? tr("Receive") : tr("Send"),
          tr("%1 / %2").arg(bytesLabel(done), bytesLabel(total)),
          status,
          updated > 0 ? QDateTime::fromMSecsSinceEpoch(updated).toLocalTime().toString("MM-dd HH:mm") : QString()
      };
      for (int column = 0; column < columns.size(); ++column) {
        auto *item = new QTableWidgetItem(columns.at(column));
        item->setData(Qt::UserRole, transfer);
        item->setToolTip(transfer.value("message").toString());
        m_transfers->setItem(static_cast<int>(row), column, item);
      }
      if (recordKey(transfer) == selectedRecord)
        m_transfers->selectRow(static_cast<int>(row));
    }
  }
  const auto cache = state.value("fileCache").toObject();
  m_cacheSummary->setText(
      cache.value("scanning").toBool() ? tr("Calculating cache usage…")
                                       : tr("%1 in %2 cached file(s)")
                                             .arg(bytesLabel(number(cache.value("bytes"))))
                                             .arg(number(cache.value("files")))
  );
  m_cacheLocation->setText(cache.value("path").toString());
  const auto entries = cache.value("entries").toArray();
  if (entries != m_renderedCache) {
    m_renderedCache = entries;
    const auto selectedCacheId = cacheKey(selectedCache());
    m_cache->setRowCount(entries.size());
    for (qsizetype row = 0; row < entries.size(); ++row) {
      const auto entry = entries.at(row).toObject();
      const auto status = entry.value("active").toBool()  ? tr("Active transfer")
                          : entry.value("inUse").toBool() ? tr("In clipboard")
                                                          : tr("Available");
      const QStringList columns{
          entry.value("name").toString(cacheKey(entry)), bytesLabel(number(entry.value("bytes"))),
          QString::number(number(entry.value("files"))), status
      };
      for (int column = 0; column < columns.size(); ++column) {
        auto *item = new QTableWidgetItem(columns.at(column));
        item->setData(Qt::UserRole, entry);
        item->setToolTip(entry.value("path").toString());
        m_cache->setItem(static_cast<int>(row), column, item);
      }
      if (cacheKey(entry) == selectedCacheId)
        m_cache->selectRow(static_cast<int>(row));
    }
  }
  updateOptionControls();
  updateManagementControls();
  Q_EMIT modifiedChanged();
}

void SharingSettingsWidget::updateOptionControls()
{
  const bool writable = Settings::isWritable();
  m_fileClipboard->setEnabled(writable);
  m_autoLimit->setEnabled(writable && m_fileClipboard->isChecked());
  m_soundEnabled->setEnabled(writable && (!m_server || m_canSendSound));
  m_soundPeer->setEnabled(m_soundEnabled->isEnabled() && m_soundEnabled->isChecked());
  m_soundOutput->setEnabled(writable && !m_server && m_soundEnabled->isChecked());
}

void SharingSettingsWidget::optionsChanged()
{
  if (m_loading)
    return;
  updateOptionControls();
  Q_EMIT modifiedChanged();
}

QJsonObject SharingSettingsWidget::selectedTransfer() const
{
  const auto row = m_transfers->currentRow();
  return row >= 0 && m_transfers->item(row, 0) ? m_transfers->item(row, 0)->data(Qt::UserRole).toJsonObject()
                                               : QJsonObject();
}

QJsonObject SharingSettingsWidget::selectedCache() const
{
  const auto row = m_cache->currentRow();
  return row >= 0 && m_cache->item(row, 0) ? m_cache->item(row, 0)->data(Qt::UserRole).toJsonObject() : QJsonObject();
}

void SharingSettingsWidget::updateManagementControls()
{
  const auto transfer = selectedTransfer();
  const auto state = transfer.value("state").toString();
  m_cancelTransfer->setEnabled(
      m_service && (state == "requesting" || state == "transferring" || state == "publishing")
  );
  m_revealTransfer->setEnabled(
      !transfer.value("localPaths").toArray().isEmpty() && transfer.value("cacheState").toString() != "removed"
  );
  m_receiveOffer->setEnabled(m_service && m_offers->currentItem());
  m_clearHistory->setEnabled(m_transfers->rowCount() > 0);
  const auto cache = selectedCache();
  const auto cacheState = m_state.value("fileCache").toObject();
  const bool busy = cacheState.value("busy").toBool() || cacheState.value("scanning").toBool();
  m_revealCache->setEnabled(!cache.isEmpty());
  m_removeCache->setEnabled(
      !cache.isEmpty() && !busy && !cache.value("inUse").toBool() && !cache.value("active").toBool()
  );
  m_clearCache->setEnabled(m_cache->rowCount() > 0 && !busy);
}

void SharingSettingsWidget::sendManagement(const QJsonObject &command)
{
  Q_EMIT commandRequested(command);
  auto *source = m_service ? m_service.data() : m_inventory.data();
  if (source)
    source->command(command);
}

void SharingSettingsWidget::requestSelectedOffer()
{
  if (!m_service || !m_offers->currentItem())
    return;
  const auto offer = m_offers->currentItem()->data(Qt::UserRole).toJsonObject();
  const auto folder = QFileDialog::getExistingDirectory(m_filesPage, tr("Save received files"));
  if (!folder.isEmpty())
    sendManagement(
        {{"action", "request"}, {"peer", offer.value("peer")}, {"id", offer.value("id")}, {"destination", folder}}
    );
}

void SharingSettingsWidget::cancelSelectedTransfer()
{
  const auto transfer = selectedTransfer();
  if (m_cancelTransfer->isEnabled())
    sendManagement({{"action", "cancel"}, {"id", transfer.value("id")}});
}

void SharingSettingsWidget::revealSelectedTransfer()
{
  const auto paths = selectedTransfer().value("localPaths").toArray();
  if (!paths.isEmpty())
    QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(paths.first().toString()).absolutePath()));
}

void SharingSettingsWidget::revealSelectedCache()
{
  const auto path = selectedCache().value("path").toString();
  if (!path.isEmpty())
    QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

void SharingSettingsWidget::removeSelectedCache()
{
  if (m_removeCache->isEnabled())
    sendManagement({{"action", "removeFileCache"}, {"id", cacheKey(selectedCache())}});
}
