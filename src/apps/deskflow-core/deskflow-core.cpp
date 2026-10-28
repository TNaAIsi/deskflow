/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 Chris Rizzitello <sithlord48@gmail.com>
 * SPDX-FileCopyrightText: (C) 2012 - 2016, 2025 - 2026 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2002 Chris Schoeneman
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "CoreArgParser.h"

#include "arch/Arch.h"
#include "base/EventQueue.h"
#include "base/Log.h"
#include "common/Constants.h"
#include "common/ExitCodes.h"
#include "common/I18N.h"
#include "deskflow/ClientApp.h"
#include "deskflow/ServerApp.h"
#include "deskflow/ipc/CoreIpcServer.h"
#include "workspace/DisplayTopology.h"
#include "workspace/FileClipboardState.h"

#if defined(Q_OS_WIN)
#include "arch/win32/ArchMiscWindows.h"
#endif

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcessEnvironment>
#include <QSharedMemory>
#include <QTextStream>
#include <QThread>
#include <QTimer>

#ifdef Q_OS_MACOS
#include <QSslSocket>
#include <array>
#include <libproc.h>
#include <openssl/crypto.h>
#include <unistd.h>
#endif

void qtMessageHandler(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
  const auto utf8 = message.toUtf8();
  switch (type) {
  case QtDebugMsg:
    CLOG->print(context.file, context.line, CLOG_TAG_DEBUG "%s", utf8.constData());
    break;
  case QtInfoMsg:
    CLOG->print(context.file, context.line, CLOG_TAG_INFO "%s", utf8.constData());
    break;
  case QtWarningMsg:
    CLOG->print(context.file, context.line, CLOG_TAG_WARN "%s", utf8.constData());
    break;
  case QtCriticalMsg:
    CLOG->print(context.file, context.line, CLOG_TAG_ERR "%s", utf8.constData());
    break;
  case QtFatalMsg:
    CLOG->print(context.file, context.line, CLOG_TAG_CRIT "%s", utf8.constData());
    break;
  }
  if (type == QtFatalMsg) {
    abort();
  }
}

void showHelp(const CoreArgParser &parser)
{
  QTextStream(stdout) << parser.helpText();
}

App *createApp(const CoreArgParser &parser, EventQueue &events, const QString &processName)
{
  if (parser.serverMode()) {
    return new ServerApp(&events, processName);
  } else if (parser.clientMode()) {
    return new ClientApp(&events, processName);
  }
  return nullptr;
}

int main(int argc, char **argv)
{
#if defined(Q_OS_WIN)
  ArchMiscWindows::setInstanceWin32(GetModuleHandle(nullptr));
#endif

  QApplication::setApplicationName(QStringLiteral("%1 Core").arg(kAppName));
#ifdef Q_OS_MACOS
  // The native-input helper authorizes this hardened executable, so Qt must
  // never load a plugin selected by a caller's environment or Qt CLI flags.
  // Keep Deskflow's own CLI independent of QApplication's startup parser.
  QStringList coreArguments;
  for (int index = 0; index < argc; ++index)
    coreArguments.append(QString::fromLocal8Bit(argv[index]));
  const auto environment = QProcessEnvironment::systemEnvironment();
  for (const auto &name : environment.keys())
    if (name.startsWith("QT_") || name.startsWith("QML_") || name.startsWith("QML2_") || name.startsWith("QTWEBENGINE"))
      qunsetenv(name.toLocal8Bit().constData());
  // OpenSSL configuration can load providers/engines into this authorized
  // process. Ignore both caller overrides and a user-writable default config,
  // including a separate OpenSSL copy that a Qt TLS backend might initialize.
  qputenv("OPENSSL_CONF", "");
  qputenv("OPENSSL_CONF_INCLUDE", "/var/empty");
  qputenv("OPENSSL_MODULES", "/var/empty");
  qputenv("OPENSSL_ENGINES", "/var/empty");
  if (OPENSSL_init_crypto(OPENSSL_INIT_NO_LOAD_CONFIG, nullptr) != 1)
    return s_exitFailed;
  qputenv("QT_QPA_PLATFORM", "cocoa");
  std::array<char, PROC_PIDPATHINFO_MAXSIZE> executable{};
  if (proc_pidpath(getpid(), executable.data(), uint32_t(executable.size())) <= 0)
    return s_exitFailed;
  const auto pluginPath = QFileInfo(QString::fromUtf8(executable.data())).absoluteDir().absoluteFilePath("../PlugIns");
  if (QFileInfo::exists(pluginPath))
    QCoreApplication::setLibraryPaths({QDir::cleanPath(pluginPath)});
  int qtArgumentCount = 1;
  char *qtArguments[] = {argv[0], nullptr};
  QApplication app(qtArgumentCount, qtArguments);
  // Qt networking uses Apple's TLS backend rather than resolving a separate
  // libcrypto from a caller's machine. Deskflow's input TLS remains static.
  if (!QSslSocket::setActiveBackend(QStringLiteral("securetransport"))) {
    QTextStream(stderr) << "The macOS system TLS backend is missing.\n";
    return s_exitFailed;
  }
#else
  QApplication app(argc, argv);
#endif

  Arch arch;
  arch.init();

  Log log;
  qInstallMessageHandler(qtMessageHandler);

#ifdef Q_OS_MACOS
  CoreArgParser parser(coreArguments);
#else
  CoreArgParser parser(QCoreApplication::arguments());
#endif

  // Print any parser errors
  if (!parser.errorText().isEmpty()) {
    QTextStream(stdout) << parser.errorText() << "\n";
  }

  if (parser.help()) {
    showHelp(parser);
    return s_exitSuccess;
  }

  if (parser.version()) {
    QTextStream(stdout) << parser.versionText();
    return s_exitSuccess;
  }

  // Before we check any more args we need to check for a duplicate process.
  // Create a shared memory segment with a unique key
  // This is to prevent a new instance from running if one is already running
  QSharedMemory sharedMemory(kCoreBinName);

  // Attempt to attach first and detach in order to clean up stale shm chunks
  // This can happen if the previous instance was killed or crashed
  if (sharedMemory.attach())
    sharedMemory.detach();

  if (!sharedMemory.create(1) && parser.singleInstanceOnly()) {
    LOG_WARN("an instance of deskflow core is already running");
    return s_exitDuplicate;
  }

  parser.parse();
  I18N::instance();

  EventQueue events;
  const auto processName = QFileInfo(argv[0]).fileName();

  App *coreApp = createApp(parser, events, processName);

  const auto ipcServer = new deskflow::core::ipc::CoreIpcServer(&app); // NOSONAR - Qt managed
  QObject::connect(
      ipcServer, &deskflow::core::ipc::IpcServer::stopProcessRequested, coreApp, &App::quit, Qt::DirectConnection
  );
  ipcServer->listen();

  // Clipboard and audio run in the interactive GUI user, not the potentially
  // SYSTEM-elevated keyboard core. The core receives only validated geometry.
  auto &topology = deskflow::workspace::DisplayTopology::instance();
  topology.initialize(Settings::value(Settings::Core::ComputerName).toString());
  const auto storedLayout = QJsonDocument::fromJson(Settings::value(Settings::Workspace::Layout).toString().toUtf8());
  if (storedLayout.isObject() && !storedLayout.object().isEmpty()) {
    auto effectiveLayout = storedLayout.object();
    if (Settings::value(Settings::Server::ExternalConfig).toBool())
      effectiveLayout.insert("enabled", false);
    topology.setLayout(effectiveLayout);
  }
  const auto options =
      QJsonDocument::fromJson(Settings::value(Settings::Workspace::Options).toString().toUtf8()).object();
  deskflow::workspace::g_fileClipboardEnabled.store(
      options.value("enabled").toBool(true) && options.value("fileClipboard").toBool(true)
  );
  QObject::connect(
      ipcServer, &deskflow::core::ipc::CoreIpcServer::workspaceCommandReceived, &app,
      [&topology](const QString &encoded) {
        const auto bytes = QByteArray::fromBase64(encoded.toLatin1(), QByteArray::Base64UrlEncoding);
        const auto document = QJsonDocument::fromJson(bytes);
        const auto command = document.object();
        if (command.value("action").toString() != "runtimeTopology")
          return;
        auto effectiveLayout = command.value("topology").toObject();
        if (command.value("externalConfig").toBool(Settings::value(Settings::Server::ExternalConfig).toBool()))
          effectiveLayout.insert("enabled", false);
        topology.setLayout(effectiveLayout, true);
        const bool enabled = command.value("fileClipboardEnabled").toBool();
        deskflow::workspace::g_fileClipboardEnabled.store(enabled);
        if (!enabled)
          deskflow::workspace::g_fileClipboardActive.store(false);
      }
  );
  QObject::connect(ipcServer, &deskflow::core::ipc::CoreIpcServer::workspacePeerDisconnected, &app, [&topology] {
    // A service-mode core may outlive its GUI. Retain the selected monitor mode
    // and local pointer movement, but revoke stale remote geometry until an
    // authenticated provider returns. Legacy routing needs an explicit choice.
    topology.clearPeerDisplays();
  });

  QThread coreThread;
  QObject::connect(&coreThread, &QThread::finished, &app, &QApplication::quit);
  coreApp->run(coreThread);

  int exitCode = QApplication::exec();
  coreThread.wait();

  if (exitCode == s_exitSuccess) {
    exitCode = coreApp->getExitCode();
  }
  delete coreApp;

  LOG_DEBUG("core exited, code: %d", exitCode);
  return exitCode;
}
