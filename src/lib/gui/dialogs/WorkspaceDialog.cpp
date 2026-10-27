/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "WorkspaceDialog.h"
#include "common/Settings.h"
#include "workspace/AudioService.h"
#include "workspace/DisplayLayoutDialog.h"
#include "workspace/WorkspaceService.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>

namespace {
QString bytesLabel(qint64 bytes)
{
  if (bytes >= 1024 * 1024 * 1024)
    return QString::number(bytes / double(1024 * 1024 * 1024), 'f', 2) + " GiB";
  if (bytes >= 1024 * 1024)
    return QString::number(bytes / double(1024 * 1024), 'f', 1) + " MiB";
  return QString::number(bytes / 1024.0, 'f', 1) + " KiB";
}

qint64 number(const QJsonValue &value)
{
  return value.isString() ? value.toString().toLongLong() : value.toInteger();
}

QString transferState(const QString &state)
{
  if (state == "complete")
    return WorkspaceDialog::tr("Complete");
  if (state == "publishing")
    return WorkspaceDialog::tr("Updating clipboard");
  if (state == "clipboardSkipped")
    return WorkspaceDialog::tr("Received; clipboard kept");
  if (state == "transferring")
    return WorkspaceDialog::tr("Transferring");
  if (state == "requesting")
    return WorkspaceDialog::tr("Requesting");
  if (state == "offered")
    return WorkspaceDialog::tr("Available");
  if (state == "cancelled")
    return WorkspaceDialog::tr("Cancelled");
  if (state == "error")
    return WorkspaceDialog::tr("Error");
  return state;
}
} // namespace

WorkspaceDropArea::WorkspaceDropArea(QWidget *parent) : QLabel(parent)
{
  setAcceptDrops(true);
  setAlignment(Qt::AlignCenter);
  setWordWrap(true);
  setMinimumHeight(70);
  setFrameShape(QFrame::StyledPanel);
  setText(tr("Drop files or folders here to share with the selected computer"));
}

void WorkspaceDropArea::dragEnterEvent(QDragEnterEvent *event)
{
  const auto urls = event->mimeData()->urls();
  if (!urls.isEmpty() && std::all_of(urls.cbegin(), urls.cend(), [](const QUrl &url) { return url.isLocalFile(); }))
    event->acceptProposedAction();
}

void WorkspaceDropArea::dropEvent(QDropEvent *event)
{
  QStringList paths;
  for (const auto &url : event->mimeData()->urls())
    if (url.isLocalFile())
      paths.append(url.toLocalFile());
  if (!paths.isEmpty()) {
    event->setDropAction(Qt::CopyAction);
    event->accept();
    Q_EMIT pathsDropped(paths);
  }
}

WorkspaceDialog::WorkspaceDialog(QWidget *parent)
    : QDialog(parent),
      m_status(new QLabel(this)),
      m_audioStatus(new QLabel(this)),
      m_errors(new QLabel(this)),
      m_enabled(new QCheckBox(tr("Enable shared workspace"), this)),
      m_clipboard(new QCheckBox(tr("Share copied files and folders"), this)),
      m_audioSend(new QCheckBox(tr("Share this Windows computer's system sound"), this)),
      m_audioReceive(new QCheckBox(tr("Play sound from the other computer"), this)),
      m_audioTarget(new QComboBox(this)),
      m_audioSource(new QComboBox(this)),
      m_audioOutput(new QComboBox(this)),
      m_fileTarget(new QComboBox(this)),
      m_autoLimit(new QSpinBox(this)),
      m_offers(new QListWidget(this)),
      m_transfers(new QTableWidget(this))
{
  setObjectName("workspaceDialog");
  setAttribute(Qt::WA_DeleteOnClose);
  setWindowTitle(tr("Shared workspace"));
  resize(900, 680);
  auto *layout = new QVBoxLayout(this);
  m_status->setWordWrap(true);
  layout->addWidget(m_status);
  layout->addWidget(m_enabled);

  auto *tabs = new QTabWidget(this);
  layout->addWidget(tabs, 1);
  auto *filesPage = new QWidget(tabs);
  auto *filesLayout = new QVBoxLayout(filesPage);
  filesLayout->addWidget(m_clipboard);
  auto *automatic = new QFormLayout;
  m_autoLimit->setRange(0, 1024 * 1024);
  m_autoLimit->setSuffix(tr(" MiB"));
  m_autoLimit->setSpecialValueText(tr("No size limit"));
  automatic->addRow(tr("Automatically receive copied files up to"), m_autoLimit);
  automatic->addRow(tr("Send selected files to"), m_fileTarget);
  filesLayout->addLayout(automatic);
  auto *drop = new WorkspaceDropArea(filesPage);
  filesLayout->addWidget(drop);
  auto *fileButtons = new QHBoxLayout;
  auto *chooseFiles = new QPushButton(tr("Choose files…"), filesPage);
  auto *chooseFolder = new QPushButton(tr("Choose folder…"), filesPage);
  fileButtons->addWidget(chooseFiles);
  fileButtons->addWidget(chooseFolder);
  fileButtons->addStretch();
  filesLayout->addLayout(fileButtons);
  auto *fileHelp = new QLabel(
      tr("Copy in Explorer or Finder, wait for transfer completion, then paste on the other computer. "
         "Source files remain unchanged. Larger offers and manually shared files are available below."),
      filesPage
  );
  fileHelp->setWordWrap(true);
  filesLayout->addWidget(fileHelp);
  filesLayout->addWidget(new QLabel(tr("Available from other computers"), filesPage));
  m_offers->setMaximumHeight(100);
  filesLayout->addWidget(m_offers);
  auto *receive = new QPushButton(tr("Save selected offer to…"), filesPage);
  filesLayout->addWidget(receive);
  m_transfers->setColumnCount(5);
  m_transfers->setHorizontalHeaderLabels({tr("File"), tr("Computer"), tr("Direction"), tr("Progress"), tr("State")});
  m_transfers->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
  m_transfers->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
  m_transfers->setSelectionBehavior(QAbstractItemView::SelectRows);
  m_transfers->setSelectionMode(QAbstractItemView::SingleSelection);
  m_transfers->setEditTriggers(QAbstractItemView::NoEditTriggers);
  filesLayout->addWidget(m_transfers, 1);
  auto *taskButtons = new QHBoxLayout;
  auto *cancel = new QPushButton(tr("Cancel transfer"), filesPage);
  auto *open = new QPushButton(tr("Open received location"), filesPage);
  auto *clearCache = new QPushButton(tr("Clear completed clipboard cache"), filesPage);
  taskButtons->addWidget(cancel);
  taskButtons->addWidget(open);
  taskButtons->addWidget(clearCache);
  taskButtons->addStretch();
  filesLayout->addLayout(taskButtons);
  tabs->addTab(filesPage, tr("Files"));

  auto *audioPage = new QWidget(tabs);
  auto *audioLayout = new QVBoxLayout(audioPage);
  auto *audioHelp = new QLabel(
      tr("Sound routing stays fixed when the mouse changes computers. "
         "Enable sending on Windows and playback on the Mac. Local Windows output continues playing."),
      audioPage
  );
  audioHelp->setWordWrap(true);
  audioLayout->addWidget(audioHelp);
  audioLayout->addWidget(m_audioSend);
#ifndef Q_OS_WIN
  m_audioSend->setEnabled(false);
  m_audioSend->setToolTip(tr("System sound capture is available on Windows."));
#endif
  audioLayout->addWidget(m_audioReceive);
  auto *audioForm = new QFormLayout;
  audioForm->addRow(tr("Send sound to"), m_audioTarget);
  audioForm->addRow(tr("Receive sound from"), m_audioSource);
  audioForm->addRow(tr("Play through"), m_audioOutput);
  audioLayout->addLayout(audioForm);
  m_audioStatus->setWordWrap(true);
  audioLayout->addWidget(m_audioStatus);
  auto *latency = new QLabel(
      tr("Use wired networking and wired headphones for the lowest latency. "
         "Bluetooth adds its own playback delay. Sending and receiving simultaneously is disabled to prevent audio "
         "loops."),
      audioPage
  );
  latency->setWordWrap(true);
  audioLayout->addWidget(latency);
  audioLayout->addStretch();
  tabs->addTab(audioPage, tr("Sound"));

  auto *displaysPage = new QWidget(tabs);
  auto *displaysLayout = new QVBoxLayout(displaysPage);
  auto *displayHelp = new QLabel(
      tr("Arrange every physical monitor independently, including monitors belonging to the same computer. "
         "Touching edges define where the pointer crosses. Use Identify to match the layout to your desk."),
      displaysPage
  );
  displayHelp->setWordWrap(true);
  displaysLayout->addWidget(displayHelp);
  m_layoutStatus = new QLabel(displaysPage);
  m_layoutStatus->setObjectName("workspaceLayoutStatus");
  m_layoutStatus->setWordWrap(true);
  displaysLayout->addWidget(m_layoutStatus);
  auto *editDisplays = new QPushButton(tr("Display layout…"), displaysPage);
  editDisplays->setObjectName("openDisplayLayout");
  displaysLayout->addWidget(editDisplays);
  displaysLayout->addStretch();
  tabs->addTab(displaysPage, tr("Monitors"));

  m_errors->setWordWrap(true);
  m_errors->setTextInteractionFlags(Qt::TextSelectableByMouse);
  layout->addWidget(m_errors);
  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Apply | QDialogButtonBox::Close, this);
  connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, this, &WorkspaceDialog::saveOptions);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
  layout->addWidget(buttons);
  connect(drop, &WorkspaceDropArea::pathsDropped, this, &WorkspaceDialog::offerPaths);
  connect(chooseFiles, &QPushButton::clicked, this, [this] {
    offerPaths(QFileDialog::getOpenFileNames(this, tr("Share files")));
  });
  connect(chooseFolder, &QPushButton::clicked, this, [this] {
    const auto path = QFileDialog::getExistingDirectory(this, tr("Share folder"));
    if (!path.isEmpty())
      offerPaths({path});
  });
  connect(receive, &QPushButton::clicked, this, &WorkspaceDialog::requestSelectedOffer);
  connect(cancel, &QPushButton::clicked, this, &WorkspaceDialog::cancelSelectedTransfer);
  connect(open, &QPushButton::clicked, this, &WorkspaceDialog::openSelectedTransfer);
  connect(clearCache, &QPushButton::clicked, this, [this] { send({{"action", "clearFileCache"}}); });
  connect(editDisplays, &QPushButton::clicked, this, &WorkspaceDialog::editLayout);
  connect(m_audioSend, &QCheckBox::toggled, this, [this](bool enabled) {
    if (enabled)
      m_audioReceive->setChecked(false);
  });
  connect(m_audioReceive, &QCheckBox::toggled, this, [this](bool enabled) {
    if (enabled)
      m_audioSend->setChecked(false);
  });
  m_poll.setInterval(2000);
  connect(&m_poll, &QTimer::timeout, this, [this] {
    if (m_service)
      refresh(m_service->state());
  });
  auto options = deskflow::workspace::WorkspaceService::defaultOptions();
  const auto stored =
      QJsonDocument::fromJson(Settings::value(Settings::Workspace::Options).toString().toUtf8()).object();
  for (auto it = stored.begin(); it != stored.end(); ++it)
    options.insert(it.key(), it.value());
  refresh(
      {{"options", options}, {"audioDevices", deskflow::workspace::AudioService::outputDevices()}, {"running", false}}
  );
  m_status->setText(tr("Start sharing to connect the shared workspace."));
}

void WorkspaceDialog::updatePeers(QComboBox *combo, const QJsonArray &peers, const QString &emptyLabel)
{
  const auto selected = combo->currentData().toString();
  combo->clear();
  combo->addItem(emptyLabel, QString());
  for (const auto &value : peers) {
    const auto name = value.toObject().value("name").toString();
    combo->addItem(name, name);
  }
  const auto index = combo->findData(selected);
  if (index >= 0)
    combo->setCurrentIndex(index);
  else if (!selected.isEmpty()) {
    combo->addItem(tr("%1 (offline)").arg(selected), selected);
    combo->setCurrentIndex(combo->count() - 1);
  }
}

void WorkspaceDialog::refresh(const QJsonObject &state)
{
  m_state = state;
  const auto mode = state.value("layoutMode").toString();
  if (mode == "external")
    m_layoutStatus->setText(tr("Layout is controlled by the server's configuration file."));
  else if (mode == "monitors" || (mode.isEmpty() && state.value("topology").toObject().value("enabled").toBool()))
    m_layoutStatus->setText(
        tr("Independent monitor layout is selected. The computer grid does not control pointer directions.")
    );
  else
    m_layoutStatus->setText(
        tr("Computer grid layout is selected. Enable independent monitors in Display layout to arrange each screen.")
    );
  const auto peers = state.value("peers").toArray();
  updatePeers(m_fileTarget, peers, tr("All connected computers"));
  updatePeers(m_audioTarget, peers, tr("All connected computers"));
  updatePeers(m_audioSource, peers, tr("First active source"));
  if (m_loaded && state.contains("audioDevices")) {
    const auto selected = m_audioOutput->currentData().toString();
    m_audioOutput->clear();
    m_audioOutput->addItem(tr("System default output"), QString());
    for (const auto &value : state.value("audioDevices").toArray()) {
      const auto device = value.toObject();
      m_audioOutput->addItem(device.value("name").toString(), device.value("id").toString());
    }
    if (!selected.isEmpty() && m_audioOutput->findData(selected) < 0)
      m_audioOutput->addItem(tr("%1 (offline)").arg(selected), selected);
    m_audioOutput->setCurrentIndex(qMax(0, m_audioOutput->findData(selected)));
  }
  if (!m_loaded) {
    const auto options = state.value("options").toObject();
    m_enabled->setChecked(options.value("enabled").toBool(true));
    m_clipboard->setChecked(options.value("fileClipboard").toBool(true));
    m_autoLimit->setValue(options.value("automaticFileMiB").toInt());
    m_audioSend->setChecked(options.value("sendEnabled").toBool());
    m_audioReceive->setChecked(options.value("receiveEnabled").toBool());
    for (auto *combo : {m_audioTarget, m_audioSource}) {
      const auto key = combo == m_audioTarget ? "targetPeer" : "sourcePeer";
      const auto selected = options.value(key).toString();
      if (!selected.isEmpty() && combo->findData(selected) < 0)
        combo->addItem(selected, selected);
      combo->setCurrentIndex(qMax(0, combo->findData(selected)));
    }
    m_audioOutput->addItem(tr("System default output"), QString());
    for (const auto &value : state.value("audioDevices").toArray()) {
      const auto device = value.toObject();
      m_audioOutput->addItem(device.value("name").toString(), device.value("id").toString());
    }
    const auto outputId = options.value("outputDeviceId").toString();
    if (!outputId.isEmpty() && m_audioOutput->findData(outputId) < 0)
      m_audioOutput->addItem(tr("%1 (offline)").arg(outputId), outputId);
    m_audioOutput->setCurrentIndex(qMax(0, m_audioOutput->findData(outputId)));
    // Initial local fallback has no output inventory or connected peer list.
    m_loaded = state.contains("running");
    if (!m_loaded)
      m_audioOutput->clear();
  }
  if (state.contains("running")) {
    m_status->setText(
        state.value("running").toBool()
            ? tr("%1 · workspace port %2 · %3 connected computer(s)")
                  .arg(state.value("localDevice").toString())
                  .arg(state.value("port").toInt())
                  .arg(peers.size())
            : tr("Shared workspace is disabled or unavailable. Keyboard and mouse use their normal connection.")
    );
  }
  m_audioStatus->setText(state.value("audioStatus").toString());
  QStringList errors;
  for (const auto &value : state.value("errors").toArray())
    errors.append(value.toString());
  m_errors->setText(errors.isEmpty() ? QString() : errors.last());
  const auto selectedOffer = m_offers->currentItem()
                                 ? m_offers->currentItem()->data(Qt::UserRole).toJsonObject().value("id").toString()
                                 : QString();
  m_offers->clear();
  for (const auto &value : state.value("offers").toArray()) {
    const auto offer = value.toObject();
    auto *item = new QListWidgetItem(
        QString("%1 — %2 (%3)")
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
  const auto row = m_transfers->currentRow();
  const auto selectedTransfer =
      row >= 0 && m_transfers->item(row, 0)
          ? m_transfers->item(row, 0)->data(Qt::UserRole).toJsonObject().value("id").toString()
          : QString();
  const auto transfers = state.value("transfers").toArray();
  m_transfers->setRowCount(transfers.size());
  for (qsizetype i = 0; i < transfers.size(); ++i) {
    const auto transfer = transfers.at(i).toObject();
    const qint64 total = number(transfer.value("totalBytes")), done = number(transfer.value("transferredBytes"));
    const QStringList columns{
        transfer.value("name").toString(), transfer.value("peer").toString(),
        transfer.value("direction").toString() == "receive" ? tr("Receive") : tr("Send"),
        QString("%1 / %2").arg(bytesLabel(done), bytesLabel(total)), transferState(transfer.value("state").toString())
    };
    for (int column = 0; column < columns.size(); ++column) {
      auto *item = new QTableWidgetItem(columns.at(column));
      item->setData(Qt::UserRole, transfer);
      item->setToolTip(transfer.value("message").toString());
      m_transfers->setItem(static_cast<int>(i), column, item);
    }
    if (transfer.value("id").toString() == selectedTransfer)
      m_transfers->selectRow(static_cast<int>(i));
  }
}

void WorkspaceDialog::send(const QJsonObject &command)
{
  if (m_service)
    m_service->command(command);
}

void WorkspaceDialog::setService(deskflow::workspace::WorkspaceService *service)
{
  if (m_service)
    disconnect(m_service, nullptr, this, nullptr);
  m_service = service;
  m_loaded = false;
  if (service) {
    connect(service, &deskflow::workspace::WorkspaceService::stateChanged, this, &WorkspaceDialog::refresh);
    refresh(service->state());
    m_poll.start();
  } else {
    m_poll.stop();
    m_status->setText(tr("Start sharing to connect the shared workspace."));
  }
}

void WorkspaceDialog::saveOptions()
{
  QJsonObject options{
      {"enabled", m_enabled->isChecked()},
      {"fileClipboard", m_clipboard->isChecked()},
      {"automaticFileMiB", m_autoLimit->value()},
      {"sendEnabled", m_audioSend->isChecked()},
      {"receiveEnabled", m_audioReceive->isChecked()},
      {"targetPeer", m_audioTarget->currentData().toString()},
      {"sourcePeer", m_audioSource->currentData().toString()},
      {"outputDeviceId", m_audioOutput->currentData().toString()}
  };
  Settings::setValue(
      Settings::Workspace::Options, QString::fromUtf8(QJsonDocument(options).toJson(QJsonDocument::Compact))
  );
  Settings::save(false);
  send({{"action", "options"}, {"options", options}});
}

void WorkspaceDialog::offerPaths(const QStringList &paths)
{
  if (paths.isEmpty())
    return;
  if (!m_service) {
    QMessageBox::information(this, windowTitle(), tr("Start sharing before sending files."));
    return;
  }
  send(
      {{"action", "offer"},
       {"paths", QJsonArray::fromStringList(paths)},
       {"peer", m_fileTarget->currentData().toString()}}
  );
}

void WorkspaceDialog::requestSelectedOffer()
{
  if (!m_offers->currentItem())
    return;
  const auto offer = m_offers->currentItem()->data(Qt::UserRole).toJsonObject();
  const auto folder = QFileDialog::getExistingDirectory(this, tr("Save received files"));
  if (!folder.isEmpty())
    send({{"action", "request"}, {"peer", offer.value("peer")}, {"id", offer.value("id")}, {"destination", folder}});
}

void WorkspaceDialog::cancelSelectedTransfer()
{
  const auto row = m_transfers->currentRow();
  if (row < 0 || !m_transfers->item(row, 0))
    return;
  const auto transfer = m_transfers->item(row, 0)->data(Qt::UserRole).toJsonObject();
  send({{"action", "cancel"}, {"id", transfer.value("id")}});
}

void WorkspaceDialog::openSelectedTransfer()
{
  const auto row = m_transfers->currentRow();
  if (row < 0 || !m_transfers->item(row, 0))
    return;
  const auto paths = m_transfers->item(row, 0)->data(Qt::UserRole).toJsonObject().value("localPaths").toArray();
  if (!paths.isEmpty())
    QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(paths.first().toString()).absolutePath()));
}

void WorkspaceDialog::editLayout()
{
  Q_EMIT displayLayoutRequested();
}
