/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "WorkspaceService.h"
#include "AudioService.h"
#include "DisplayTopology.h"
#include "FileTransferService.h"
#include "WorkspaceActivity.h"
#include "common/Settings.h"

#include <QDebug>
#include <QJsonDocument>
#include <QStandardPaths>
#include <algorithm>
#include <limits>

namespace deskflow::workspace {

QJsonObject WorkspaceService::defaultOptions()
{
  return {{"enabled", true},         {"fileClipboard", true}, {"automaticFileMiB", 0}, {"sendEnabled", false},
          {"receiveEnabled", false}, {"targetPeer", ""},      {"sourcePeer", ""},      {"outputDeviceId", ""}};
}

WorkspaceService::WorkspaceService(bool server, QObject *parent)
    : QObject(parent),
      m_transport(this),
      m_audio(new AudioService(this)),
      m_files(new FileTransferService(this)),
      m_server(server)
{
  m_update.setSingleShot(true);
  m_update.setInterval(150);
  m_offerRetry.setInterval(25);
  connect(&m_offerRetry, &QTimer::timeout, this, [this] {
    const auto keys = m_pendingOffers.keys();
    for (const auto &key : keys) {
      const auto pending = m_pendingOffers.value(key).toObject();
      const auto peer = pending.value("peer").toString();
      if (!m_transport.peers().contains(peer) ||
          m_transport.send(
              peer, "files.offer", pending.value("offer").toObject(), {}, WorkspaceTransport::Channel::Files
          ))
        m_pendingOffers.remove(key);
    }
    if (m_pendingOffers.isEmpty())
      m_offerRetry.stop();
  });
  connect(&m_update, &QTimer::timeout, this, [this] { Q_EMIT stateChanged(state()); });
  connect(&m_transport, &WorkspaceTransport::error, this, &WorkspaceService::reportError);
  connect(m_audio, &AudioService::error, this, &WorkspaceService::reportError);
  connect(m_files, &FileTransferService::error, this, &WorkspaceService::reportError);
  connect(m_audio, &AudioService::statusChanged, this, [this](const QString &status) {
    m_audioStatus = status;
    update();
  });
  connect(
      m_audio, &AudioService::outbound, this,
      [this](const QString &type, const QJsonObject &metadata, const QByteArray &payload) {
        auto object = metadata;
        object.insert("sourceDevice", m_name);
        const auto target = m_options.value("targetPeer").toString();
        for (const auto &peer : m_transport.peers())
          if (target.isEmpty() || target == peer)
            m_transport.send(peer, type, object, payload, WorkspaceTransport::Channel::Audio);
      }
  );
  m_files->setSendHandler([this](
                              const QString &peer, const QString &type, const QJsonObject &meta, const QByteArray &data
                          ) { return sendFiles(peer, type, meta, data); });
  connect(m_files, &FileTransferService::offerAvailable, this, [this](QJsonObject offer) {
    // Manifests stay in the engine. GUI snapshots carry only display fields.
    offer.remove("entries");
    offer.remove("manifest");
    offer.remove("roots");
    const auto key = offer.value("peer").toString() + "/" + offer.value("id").toString();
    if (m_offers.size() >= 100)
      m_offers.remove(m_offers.begin().key());
    m_offers.insert(key, offer);
    update();
  });
  connect(m_files, &FileTransferService::progressChanged, this, [this](const QJsonObject &progress) {
    auto summary = progress;
    const auto paths = progress.value("localPaths").toArray();
    if (paths.size() > 8) {
      QJsonArray preview;
      for (qsizetype i = 0; i < 8; ++i)
        preview.append(paths.at(i));
      summary.insert("localPaths", preview);
    }
    const auto id = progress.value("peer").toString() + "/" + progress.value("id").toString();
    if (m_transfers.size() >= 100 && !m_transfers.contains(id))
      m_transfers.remove(m_transfers.begin().key());
    m_transfers.insert(id, summary);
    update();
  });
  connect(&m_transport, &WorkspaceTransport::peerConnected, this, [this](const QString &peer) {
    m_transport.send(peer, "displays", {{"displays", DisplayTopology::instance().localDisplays()}});
    if (m_server)
      m_transport.send(peer, "layout", layoutMessage());
    update();
  });
  connect(
      &m_transport, &WorkspaceTransport::channelConnected, this,
      [this](const QString &peer, WorkspaceTransport::Channel channel) {
        if (channel == WorkspaceTransport::Channel::Files)
          m_files->peerConnected(peer);
        update();
      }
  );
  connect(&m_transport, &WorkspaceTransport::peerDisconnected, this, [this](const QString &peer) {
    if (!m_server && m_transport.peers().isEmpty())
      m_remoteLayoutMode.clear();
    m_files->peerDisconnected(peer);
    m_audio->peerDisconnected(peer);
    DisplayTopology::instance().removePeer(peer);
    const auto pendingKeys = m_pendingOffers.keys();
    for (const auto &key : pendingKeys)
      if (m_pendingOffers.value(key).toObject().value("peer").toString() == peer)
        m_pendingOffers.remove(key);
    const auto keys = m_offers.keys();
    for (const auto &key : keys)
      if (m_offers.value(key).toObject().value("peer").toString() == peer)
        m_offers.remove(key);
    if (m_server)
      for (const auto &name : m_transport.peers())
        m_transport.send(name, "layout", layoutMessage());
    update();
  });
  connect(
      &m_transport, &WorkspaceTransport::channelDisconnected, this,
      [this](const QString &peer, WorkspaceTransport::Channel channel) {
        if (channel == WorkspaceTransport::Channel::Files)
          m_files->peerDisconnected(peer);
        if (channel == WorkspaceTransport::Channel::Audio)
          m_audio->peerDisconnected(peer);
        update();
      }
  );
  connect(&m_transport, &WorkspaceTransport::messageReceived, this, &WorkspaceService::message);
  connect(&DisplayTopology::instance(), &DisplayTopology::changed, this, &WorkspaceService::update);
  connect(Settings::instance(), &Settings::settingsChanged, this, [this](const QString &key) {
    if (key != Settings::Server::ExternalConfig)
      return;
    if (m_server)
      for (const auto &peer : m_transport.peers())
        m_transport.send(peer, "layout", layoutMessage());
    update();
  });
  connect(
      &DisplayTopology::instance(), &DisplayTopology::localDisplaysChanged, this,
      [this](const QJsonArray &displays) {
        for (const auto &peer : m_transport.peers())
          m_transport.send(peer, "displays", {{"displays", displays}});
        if (m_server)
          for (const auto &peer : m_transport.peers())
            m_transport.send(peer, "layout", layoutMessage());
        update();
      }
  );
}

void WorkspaceService::start()
{
  m_name = Settings::value(Settings::Core::ComputerName).toString();
  DisplayTopology::instance().initialize(m_name);
  const auto storedLayout = QJsonDocument::fromJson(Settings::value(Settings::Workspace::Layout).toString().toUtf8());
  if (storedLayout.isObject() && !storedLayout.object().isEmpty())
    DisplayTopology::instance().setLayout(storedLayout.object());
  m_options = defaultOptions();
  const auto stored =
      QJsonDocument::fromJson(Settings::value(Settings::Workspace::Options).toString().toUtf8()).object();
  for (auto it = stored.begin(); it != stored.end(); ++it)
    if (m_options.contains(it.key()))
      m_options.insert(it.key(), it.value());
  applyOptions();
}

WorkspaceService::~WorkspaceService()
{
  setWorkspaceResponsive(false);
  m_update.stop();
  m_offerRetry.stop();
  // Stop while all members and resource children are still alive. A transport
  // member destructor must not emit callbacks into already-destroyed state.
  m_transport.stop();
  disconnect(&m_transport, nullptr, this, nullptr);
  m_audio->stopCapture();
  m_audio->stopPlayback();
  m_files->setClipboardEnabled(false);
}

void WorkspaceService::applyOptions()
{
  setWorkspaceResponsive(m_options.value("enabled").toBool());
  if (!m_options.value("enabled").toBool() || !m_options.value("fileClipboard").toBool()) {
    const auto keys = m_pendingOffers.keys();
    for (const auto &key : keys)
      if (m_pendingOffers.value(key).toObject().value("offer").toObject().value("clipboard").toBool())
        m_pendingOffers.remove(key);
  }
  m_files->setClipboardEnabled(m_options.value("enabled").toBool() && m_options.value("fileClipboard").toBool());
  const auto autoMiB = std::clamp(m_options.value("automaticFileMiB").toInteger(), qint64(0), qint64(1024 * 1024));
  m_files->setAutomaticClipboardLimit(autoMiB == 0 ? std::numeric_limits<qint64>::max() : autoMiB * 1024 * 1024);
  auto audio = m_options;
  if (!m_options.value("enabled").toBool()) {
    audio.insert("sendEnabled", false);
    audio.insert("receiveEnabled", false);
  }
  m_audio->configure(audio);
  if (audio.value("sendEnabled").toBool())
    m_audio->startCapture();
  else
    m_audio->stopCapture();

  if (!m_options.value("enabled").toBool()) {
    m_transport.stop();
    m_started = false;
  } else if (!m_started) {
    const int inputPort = Settings::value(Settings::Core::Port).toInt();
    if (inputPort <= 0 || inputPort >= 65535) {
      reportError(tr("Workspace requires an input port from 1 to 65534."));
      return;
    }
    m_port = static_cast<quint16>(inputPort + 1);
    m_started = m_transport.start(
        m_server, m_name, m_port,
        Settings::value(Settings::Client::RemoteHost).toString().split(',', Qt::SkipEmptyParts),
        Settings::value(Settings::Core::Interface).toString()
    );
  }
  update();
}

bool WorkspaceService::sendFiles(
    const QString &peer, const QString &type, const QJsonObject &meta, const QByteArray &data
)
{
  if (!peer.isEmpty())
    return m_transport.send(peer, type, meta, data, WorkspaceTransport::Channel::Files);
  bool sent = false;
  for (const auto &name : m_transport.peers()) {
    if (!m_transport.channelReady(name, WorkspaceTransport::Channel::Files))
      continue;
    const bool accepted = m_transport.send(name, type, meta, data, WorkspaceTransport::Channel::Files);
    sent |= accepted;
    if (!accepted && type == "files.offer") {
      if (meta.value("clipboard").toBool()) {
        const auto keys = m_pendingOffers.keys();
        for (const auto &key : keys) {
          const auto pending = m_pendingOffers.value(key).toObject();
          if (pending.value("peer").toString() == name && pending.value("offer").toObject().value("clipboard").toBool())
            m_pendingOffers.remove(key);
        }
      }
      if (m_pendingOffers.size() >= 64)
        m_pendingOffers.remove(m_pendingOffers.begin().key());
      m_pendingOffers.insert(name + "/" + meta.value("id").toString(), QJsonObject{{"peer", name}, {"offer", meta}});
      m_offerRetry.start();
    }
  }
  // Offers are re-advertised when a file channel connects. A broadcast with no
  // peers is not a failed transfer and must not queue obsolete clipboard IDs.
  return type == "files.offer" || sent;
}

void WorkspaceService::message(
    const QString &peer, const QString &type, const QJsonObject &metadata, const QByteArray &payload
)
{
  if (type == "audio") {
    m_audio->receiveAudio(peer, metadata, payload);
  } else if (type.startsWith("files.")) {
    m_files->receiveMessage(peer, type, metadata, payload);
  } else if (type == "displays") {
    DisplayTopology::instance().setPeerDisplays(peer, metadata.value("displays").toArray());
    if (m_server)
      for (const auto &name : m_transport.peers())
        m_transport.send(name, "layout", layoutMessage());
    update();
  } else if (type == "layout") {
    if (!m_server) {
      const auto mode = metadata.value("mode").toString();
      if (mode == "external" || mode == "monitors" || mode == "computers")
        m_remoteLayoutMode = mode;
    }
    applyLayout(metadata.value("layout").toObject(), m_server);
  } else if (type == "identify") {
    const auto device = metadata.value("device").toString(m_name);
    if (device == m_name)
      DisplayTopology::instance().identifyLocalDisplay(metadata.value("id").toString());
    else if (m_server)
      m_transport.send(device, "identify", metadata);
  }
}

void WorkspaceService::applyLayout(const QJsonObject &layout, bool broadcast)
{
  if (m_server && Settings::value(Settings::Server::ExternalConfig).toBool()) {
    reportError(tr(
        "Layout is controlled by the server configuration file. Disable external configuration before editing displays."
    ));
    return;
  }
  if (!DisplayTopology::instance().setLayout(layout, !m_server)) {
    reportError(tr("Display layout was rejected; refresh the connected displays and try again."));
    return;
  }
  Settings::setValue(
      Settings::Workspace::Layout,
      QString::fromUtf8(QJsonDocument(DisplayTopology::instance().layout()).toJson(QJsonDocument::Compact))
  );
  Settings::save(false);
  if (broadcast)
    for (const auto &peer : m_transport.peers())
      m_transport.send(peer, "layout", layoutMessage());
  update();
}

void WorkspaceService::command(const QJsonObject &object)
{
  const auto action = object.value("action").toString();
  if (action == "options") {
    const auto options = object.value("options").toObject();
    for (auto it = options.begin(); it != options.end(); ++it)
      if (m_options.contains(it.key()))
        m_options.insert(it.key(), it.value());
    Settings::setValue(
        Settings::Workspace::Options, QString::fromUtf8(QJsonDocument(m_options).toJson(QJsonDocument::Compact))
    );
    Settings::save(false);
    applyOptions();
  } else if (action == "offer") {
    QStringList paths;
    for (const auto &path : object.value("paths").toArray())
      if (path.isString() && paths.size() < 4096)
        paths.append(path.toString());
    if (!paths.isEmpty())
      m_files->offerFiles(paths, object.value("peer").toString());
  } else if (action == "request") {
    m_files->requestOffer(
        object.value("peer").toString(), object.value("id").toString(), object.value("destination").toString()
    );
  } else if (action == "cancel") {
    m_files->cancel(object.value("id").toString());
  } else if (action == "clearFileCache") {
    m_files->clearClipboardCache();
  } else if (action == "layout") {
    const auto layout = object.value("layout").toObject();
    if (m_server)
      applyLayout(layout, true);
    else {
      for (const auto &peer : m_transport.peers())
        m_transport.send(peer, "layout", {{"layout", layout}});
    }
  } else if (action == "identify") {
    const auto device = object.value("device").toString();
    if (device == m_name)
      DisplayTopology::instance().identifyLocalDisplay(object.value("id").toString());
    else if (m_server)
      m_transport.send(device, "identify", {{"id", object.value("id")}, {"device", device}});
    else
      for (const auto &peer : m_transport.peers())
        m_transport.send(peer, "identify", {{"id", object.value("id")}, {"device", device}});
  }
  update();
}

QString WorkspaceService::selectedLayoutMode() const
{
  if (m_server && Settings::value(Settings::Server::ExternalConfig).toBool())
    return QStringLiteral("external");
  if (!m_server && !m_remoteLayoutMode.isEmpty())
    return m_remoteLayoutMode;
  return DisplayTopology::instance().enabled() ? QStringLiteral("monitors") : QStringLiteral("computers");
}

QJsonObject WorkspaceService::layoutMessage() const
{
  return {{"layout", DisplayTopology::instance().layout()}, {"mode", selectedLayoutMode()}};
}

QJsonObject WorkspaceService::state() const
{
  QJsonArray transfers, offers, errors;
  for (const auto &value : m_transfers)
    transfers.append(value);
  for (const auto &value : m_offers)
    offers.append(value);
  for (const auto &message : m_errors)
    errors.append(message);
  return {
      {"running", m_started},
      {"server", m_server},
      {"localDevice", m_name},
      {"port", m_port},
      {"peers", m_transport.peerState()},
      {"options", m_options},
      {"topology", DisplayTopology::instance().serializedState()},
      {"layoutMode", selectedLayoutMode()},
      {"audioDevices", AudioService::outputDevices()},
      {"audioStatus", m_audioStatus},
      {"transfers", transfers},
      {"offers", offers},
      {"errors", errors}
  };
}

void WorkspaceService::update()
{
  if (!m_update.isActive())
    m_update.start();
}

void WorkspaceService::reportError(const QString &error)
{
  if (m_errors.isEmpty() || m_errors.last() != error)
    m_errors.append(error);
  while (m_errors.size() > 10)
    m_errors.removeFirst();
  qWarning().noquote() << "Workspace:" << error;
  update();
}

} // namespace deskflow::workspace
