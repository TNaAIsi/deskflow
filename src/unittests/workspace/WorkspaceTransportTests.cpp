/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "WorkspaceTransportTests.h"

#include "net/FingerprintDatabase.h"
#include "workspace/FileTransferService.h"
#include "workspace/FrameCodec.h"
#include "workspace/WorkspaceTransport.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QSignalSpy>
#include <QSslCertificate>
#include <QSslKey>
#include <QSslSocket>
#include <QTest>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <memory>

using deskflow::workspace::FileTransferService;
using deskflow::workspace::FrameCodec;
using deskflow::workspace::WorkspaceFrame;
using deskflow::workspace::WorkspaceTransport;
using Channel = WorkspaceTransport::Channel;

namespace {

bool generateCertificate(const QString &path, int daysValid = 100)
{
  const std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(EVP_RSA_gen(2048), EVP_PKEY_free);
  const std::unique_ptr<X509, decltype(&X509_free)> certificate(X509_new(), X509_free);
  if (!key || !certificate) {
    return false;
  }
  X509_set_version(certificate.get(), 2);
  ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), 1);
  X509_gmtime_adj(X509_getm_notBefore(certificate.get()), daysValid > 0 ? -60 : (daysValid - 1) * 86400L);
  X509_gmtime_adj(X509_getm_notAfter(certificate.get()), daysValid * 86400L);
  X509_set_pubkey(certificate.get(), key.get());
  auto *name = X509_get_subject_name(certificate.get());
  X509_NAME_add_entry_by_txt(
      name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char *>("Deskflow Workspace Tests"), -1, -1, 0
  );
  X509_set_issuer_name(certificate.get(), name);
  if (X509_sign(certificate.get(), key.get(), EVP_sha256()) <= 0) {
    return false;
  }
  const std::unique_ptr<BIO, decltype(&BIO_free)> output(BIO_new(BIO_s_mem()), BIO_free);
  if (!output || PEM_write_bio_PrivateKey(output.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr) != 1 ||
      PEM_write_bio_X509(output.get(), certificate.get()) != 1) {
    return false;
  }
  char *data = nullptr;
  const auto size = BIO_get_mem_data(output.get(), &data);
  QFile file(path);
  return file.open(QIODevice::WriteOnly) && file.write(data, size) == size;
}

QByteArray certificateBytes(const QString &path)
{
  QFile file(path);
  return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}

Fingerprint certificateFingerprint(const QString &path)
{
  const auto certificates = QSslCertificate::fromData(certificateBytes(path));
  return certificates.isEmpty()
             ? Fingerprint{}
             : Fingerprint{QCryptographicHash::Sha256, certificates.first().digest(QCryptographicHash::Sha256)};
}

bool trust(const QString &path, const QStringList &certificates)
{
  FingerprintDatabase database;
  for (const auto &certificate : certificates) {
    const auto fingerprint = certificateFingerprint(certificate);
    if (!fingerprint.isValid()) {
      return false;
    }
    database.addTrusted(fingerprint);
  }
  return database.write(path);
}

bool ready(const WorkspaceTransport &transport, const QString &peer)
{
  return transport.channelReady(peer, Channel::Control) && transport.channelReady(peer, Channel::Audio) &&
         transport.channelReady(peer, Channel::Files);
}

void connectRaw(QSslSocket &socket, const QString &certificate, quint16 port)
{
  const auto bytes = certificateBytes(certificate);
  socket.setLocalCertificate(QSslCertificate(bytes));
  socket.setPrivateKey(QSslKey(bytes, QSsl::Rsa));
  // This deliberately malicious test peer trusts the loopback server. The
  // server must still authenticate its client certificate and session token.
  socket.setPeerVerifyMode(QSslSocket::VerifyNone);
  socket.connectToHostEncrypted(QStringLiteral("127.0.0.1"), port);
}

QJsonObject rawHello(QSslSocket &socket, const QString &device, const QString &channel, const QString &token = {})
{
  const auto bytes = FrameCodec::encode(
      {{QStringLiteral("type"), QStringLiteral("$hello")},
       {QStringLiteral("version"), 1},
       {QStringLiteral("device"), device},
       {QStringLiteral("channel"), channel},
       {QStringLiteral("token"), token}}
  );
  if (socket.write(bytes) != bytes.size()) {
    return {};
  }
  FrameCodec codec;
  QElapsedTimer timer;
  timer.start();
  while (timer.elapsed() < 5000 && socket.state() != QAbstractSocket::UnconnectedState) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    QList<WorkspaceFrame> frames;
    if (socket.bytesAvailable() > 0 && !codec.append(socket.readAll(), frames)) {
      return {};
    }
    for (const auto &frame : frames) {
      if (frame.metadata.value(QStringLiteral("type")).toString() == QStringLiteral("$welcome")) {
        return frame.metadata;
      }
    }
    QTest::qWait(5);
  }
  return {};
}

} // namespace

void WorkspaceTransportTests::initTestCase()
{
  QVERIFY(m_directory.isValid());
  QVERIFY2(QSslSocket::supportsSsl(), "TLS loopback tests require a working Qt TLS backend.");
  m_certificate = m_directory.filePath(QStringLiteral("identity.pem"));
  m_secondCertificate = m_directory.filePath(QStringLiteral("second.pem"));
  m_expiredCertificate = m_directory.filePath(QStringLiteral("expired.pem"));
  m_trusted = m_directory.filePath(QStringLiteral("trusted"));
  m_trustBoth = m_directory.filePath(QStringLiteral("trusted-both"));
  m_untrusted = m_directory.filePath(QStringLiteral("untrusted"));
  m_expiredTrusted = m_directory.filePath(QStringLiteral("trusted-expired"));
  QVERIFY(generateCertificate(m_certificate));
  QVERIFY(generateCertificate(m_secondCertificate));
  QVERIFY(generateCertificate(m_expiredCertificate, -1));
  QVERIFY(trust(m_trusted, {m_certificate}));
  QVERIFY(trust(m_trustBoth, {m_certificate, m_secondCertificate}));
  QVERIFY(trust(m_untrusted, {m_secondCertificate}));
  QVERIFY(trust(m_expiredTrusted, {m_expiredCertificate}));
}

void WorkspaceTransportTests::threeChannelsAndPayloadRoundTrip()
{
  WorkspaceTransport server;
  WorkspaceTransport client;
  QVERIFY(server.setIdentityFiles(m_certificate, m_trusted, m_trusted));
  QVERIFY(client.setIdentityFiles(m_certificate, m_trusted, m_trusted));
  QVERIFY(server.start(true, QStringLiteral("server"), 0, {}, QStringLiteral("127.0.0.1")));
  QVERIFY(server.listeningPort() > 0);
  QVERIFY(!server.setIdentityFiles(m_secondCertificate, m_trusted, m_trusted));
  QVERIFY(client.start(false, QStringLiteral("client"), server.listeningPort(), {QStringLiteral("127.0.0.1")}));
  QTRY_VERIFY_WITH_TIMEOUT(ready(server, QStringLiteral("client")), 10000);
  QTRY_VERIFY_WITH_TIMEOUT(ready(client, QStringLiteral("server")), 10000);
  QSignalSpy receivedByServer(&server, &WorkspaceTransport::messageReceived);
  QSignalSpy receivedByClient(&client, &WorkspaceTransport::messageReceived);
  const QByteArray forward(64 * 1024, 'f');
  const QByteArray reverse(64 * 1024, 'r');
  QVERIFY(client.send(
      QStringLiteral("server"), QStringLiteral("files.chunk"), {{QStringLiteral("offset"), 0}}, forward, Channel::Files
  ));
  QVERIFY(server.send(
      QStringLiteral("client"), QStringLiteral("files.chunk"), {{QStringLiteral("offset"), 0}}, reverse, Channel::Files
  ));
  QTRY_COMPARE_WITH_TIMEOUT(receivedByServer.size(), 1, 5000);
  QTRY_COMPARE_WITH_TIMEOUT(receivedByClient.size(), 1, 5000);
  QCOMPARE(receivedByServer.first().at(0).toString(), QStringLiteral("client"));
  QCOMPARE(receivedByServer.first().at(3).toByteArray(), forward);
  QCOMPARE(receivedByClient.first().at(3).toByteArray(), reverse);
}

void WorkspaceTransportTests::fileBackpressureLeavesControlAndAudioWritable()
{
  WorkspaceTransport server;
  WorkspaceTransport client;
  QVERIFY(server.setIdentityFiles(m_certificate, m_trusted, m_trusted));
  QVERIFY(client.setIdentityFiles(m_certificate, m_trusted, m_trusted));
  QVERIFY(server.start(true, QStringLiteral("server"), 0, {}, QStringLiteral("127.0.0.1")));
  QVERIFY(client.start(false, QStringLiteral("client"), server.listeningPort(), {QStringLiteral("127.0.0.1")}));
  QTRY_VERIFY_WITH_TIMEOUT(ready(client, QStringLiteral("server")), 10000);
  QSignalSpy received(&server, &WorkspaceTransport::messageReceived);
  const QByteArray chunk(64 * 1024, 'q');
  int accepted = 0;
  // No event-loop drain simulates a backed-up file consumer. The complete
  // frame, including metadata, must count toward the bounded send queue.
  while (accepted < 32 &&
         client.send(QStringLiteral("server"), QStringLiteral("files.chunk"), {}, chunk, Channel::Files)) {
    ++accepted;
  }
  QVERIFY(accepted > 0);
  QVERIFY(accepted < 16);
  QVERIFY(client.send(QStringLiteral("server"), QStringLiteral("identify"), {{QStringLiteral("test"), true}}));
  QVERIFY(client.send(QStringLiteral("server"), QStringLiteral("audio"), {}, QByteArray(3840, '\0'), Channel::Audio));
  QTRY_VERIFY_WITH_TIMEOUT(
      [&received] {
        bool control = false;
        bool audio = false;
        for (const auto &message : received) {
          control |= message.at(1).toString() == QStringLiteral("identify");
          audio |= message.at(1).toString() == QStringLiteral("audio");
        }
        return control && audio;
      }(),
      5000
  );
}

void WorkspaceTransportTests::fileEngineTransfersNestedBinaryOverTls()
{
  QTemporaryDir source;
  QTemporaryDir destination;
  QVERIFY(source.isValid());
  QVERIFY(destination.isValid());
  QVERIFY(QDir().mkpath(source.filePath(QStringLiteral("payload/nested"))));
  QByteArray binary(2 * 1024 * 1024 + 37, Qt::Uninitialized);
  for (qsizetype index = 0; index < binary.size(); ++index) {
    binary[index] = static_cast<char>((index * 131 + index / 257) & 0xff);
  }
  {
    QFile file(source.filePath(QStringLiteral("payload/nested/data.bin")));
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(binary), binary.size());
    QFile empty(source.filePath(QStringLiteral("payload/empty.txt")));
    QVERIFY(empty.open(QIODevice::WriteOnly));
  }

  WorkspaceTransport server;
  WorkspaceTransport client;
  QVERIFY(server.setIdentityFiles(m_certificate, m_trusted, m_trusted));
  QVERIFY(client.setIdentityFiles(m_certificate, m_trusted, m_trusted));
  QVERIFY(server.start(true, QStringLiteral("server"), 0, {}, QStringLiteral("127.0.0.1")));
  QVERIFY(client.start(false, QStringLiteral("client"), server.listeningPort(), {QStringLiteral("127.0.0.1")}));
  QTRY_VERIFY_WITH_TIMEOUT(ready(server, QStringLiteral("client")), 10000);
  QTRY_VERIFY_WITH_TIMEOUT(ready(client, QStringLiteral("server")), 10000);

  FileTransferService sender;
  FileTransferService receiver;
  sender.setClipboardEnabled(false);
  receiver.setClipboardEnabled(false);
  QSignalSpy offers(&receiver, &FileTransferService::offerAvailable);
  QSignalSpy sent(&sender, &FileTransferService::progressChanged);
  QSignalSpy received(&receiver, &FileTransferService::progressChanged);
  QSignalSpy senderErrors(&sender, &FileTransferService::error);
  QSignalSpy receiverErrors(&receiver, &FileTransferService::error);
  QSignalSpy wireMessages(&client, &WorkspaceTransport::messageReceived);

  connect(
      &server, &WorkspaceTransport::messageReceived, &sender,
      [&sender](const QString &peer, const QString &type, const QJsonObject &metadata, const QByteArray &payload) {
        if (type.startsWith(QStringLiteral("files."))) {
          sender.receiveMessage(peer, type, metadata, payload);
        }
      }
  );
  connect(
      &client, &WorkspaceTransport::messageReceived, &receiver,
      [&receiver](const QString &peer, const QString &type, const QJsonObject &metadata, const QByteArray &payload) {
        if (type.startsWith(QStringLiteral("files."))) {
          receiver.receiveMessage(peer, type, metadata, payload);
        }
      }
  );

  bool filled = false;
  bool controlAccepted = false;
  bool audioAccepted = false;
  int retries = 0;
  sender.setSendHandler([&](const QString &peer, const QString &type, const QJsonObject &metadata,
                            const QByteArray &payload) {
    if (type == QStringLiteral("files.chunk") && !filled) {
      filled = true;
      // Fill the actual TLS socket queue without yielding. Unsolicited chunk
      // IDs are ignored by the real receiver, and never create a disk file.
      const QByteArray padding(64 * 1024, '\0');
      for (int index = 0; index < 16; ++index) {
        if (!server.send(
                peer, QStringLiteral("files.chunk"), {{QStringLiteral("id"), QStringLiteral("no-offer")}}, padding,
                Channel::Files
            )) {
          break;
        }
      }
      controlAccepted = server.send(peer, QStringLiteral("identify"), {{QStringLiteral("integration"), true}});
      audioAccepted = server.send(peer, QStringLiteral("audio"), {}, QByteArray(3840, '\0'), Channel::Audio);
    }
    const bool accepted = server.send(peer, type, metadata, payload, Channel::Files);
    if (!accepted) {
      ++retries;
    }
    return accepted;
  });
  receiver.setSendHandler([&](const QString &peer, const QString &type, const QJsonObject &metadata,
                              const QByteArray &payload) {
    return client.send(peer, type, metadata, payload, Channel::Files);
  });
  sender.offerFiles({source.filePath(QStringLiteral("payload"))}, QStringLiteral("client"));
  QTRY_COMPARE_WITH_TIMEOUT(offers.size(), 1, 10000);
  receiver.requestOffer(
      QStringLiteral("server"), offers.first().first().toJsonObject().value(QStringLiteral("id")).toString(),
      destination.path()
  );
  const auto complete = [](const QSignalSpy &progress) {
    for (const auto &entry : progress) {
      if (entry.first().toJsonObject().value(QStringLiteral("state")).toString() == QStringLiteral("complete")) {
        return true;
      }
    }
    return false;
  };
  QTRY_VERIFY_WITH_TIMEOUT(complete(received), 15000);
  QTRY_VERIFY_WITH_TIMEOUT(complete(sent), 15000);
  QVERIFY(retries > 0);
  QVERIFY(controlAccepted);
  QVERIFY(audioAccepted);
  bool controlReceived = false;
  bool audioReceived = false;
  for (const auto &entry : wireMessages) {
    controlReceived |= entry.at(1).toString() == QStringLiteral("identify");
    audioReceived |= entry.at(1).toString() == QStringLiteral("audio");
  }
  QVERIFY(controlReceived);
  QVERIFY(audioReceived);
  QFile copied(destination.filePath(QStringLiteral("payload/nested/data.bin")));
  QVERIFY(copied.open(QIODevice::ReadOnly));
  QCOMPARE(copied.readAll(), binary);
  const QFileInfo empty(destination.filePath(QStringLiteral("payload/empty.txt")));
  QVERIFY(empty.isFile());
  QCOMPARE(empty.size(), 0);
  QVERIFY(!QFileInfo::exists(destination.filePath(QStringLiteral("no-offer"))));
  QCOMPARE(senderErrors.size(), 0);
  QCOMPARE(receiverErrors.size(), 0);
}

void WorkspaceTransportTests::largeManifestAcceptedOnEmptyFileChannel()
{
  WorkspaceTransport server;
  WorkspaceTransport client;
  QVERIFY(server.setIdentityFiles(m_certificate, m_trusted, m_trusted));
  QVERIFY(client.setIdentityFiles(m_certificate, m_trusted, m_trusted));
  QVERIFY(server.start(true, QStringLiteral("server"), 0, {}, QStringLiteral("127.0.0.1")));
  QVERIFY(client.start(false, QStringLiteral("client"), server.listeningPort(), {QStringLiteral("127.0.0.1")}));
  QTRY_VERIFY_WITH_TIMEOUT(ready(client, QStringLiteral("server")), 10000);
  QSignalSpy received(&server, &WorkspaceTransport::messageReceived);
  const QJsonObject manifest{{QStringLiteral("manifest"), QString(700 * 1024, QLatin1Char('m'))}};
  QVERIFY(client.send(QStringLiteral("server"), QStringLiteral("files.offer"), manifest, {}, Channel::Files));
  QTRY_COMPARE_WITH_TIMEOUT(received.size(), 1, 5000);
  QCOMPARE(
      received.first().at(2).toJsonObject().value(QStringLiteral("manifest")),
      manifest.value(QStringLiteral("manifest"))
  );
}

void WorkspaceTransportTests::untrustedCertificateRejected_data()
{
  QTest::addColumn<bool>("serverRejects");
  QTest::newRow("server-rejects-client") << true;
  QTest::newRow("client-rejects-server") << false;
}

void WorkspaceTransportTests::untrustedCertificateRejected()
{
  QFETCH(bool, serverRejects);
  WorkspaceTransport server;
  WorkspaceTransport client;
  QVERIFY(server.setIdentityFiles(m_certificate, m_trusted, serverRejects ? m_untrusted : m_trusted));
  QVERIFY(client.setIdentityFiles(m_certificate, serverRejects ? m_trusted : m_untrusted, m_trusted));
  QSignalSpy connected(&server, &WorkspaceTransport::peerConnected);
  QSignalSpy errors(&client, &WorkspaceTransport::error);
  QVERIFY(server.start(true, QStringLiteral("server"), 0, {}, QStringLiteral("127.0.0.1")));
  QVERIFY(client.start(false, QStringLiteral("client"), server.listeningPort(), {QStringLiteral("127.0.0.1")}));
  QTRY_VERIFY_WITH_TIMEOUT(!errors.isEmpty(), 10000);
  QTest::qWait(100);
  QCOMPARE(connected.size(), 0);
  QVERIFY(server.peers().isEmpty());
  QVERIFY(client.peers().isEmpty());
}

void WorkspaceTransportTests::auxiliaryChannelRequiresControlToken()
{
  WorkspaceTransport server;
  QVERIFY(server.setIdentityFiles(m_certificate, m_trusted, m_trusted));
  QVERIFY(server.start(true, QStringLiteral("server"), 0, {}, QStringLiteral("127.0.0.1")));
  QSslSocket control;
  connectRaw(control, m_certificate, server.listeningPort());
  QTRY_VERIFY_WITH_TIMEOUT(control.isEncrypted(), 5000);
  const auto welcome = rawHello(control, QStringLiteral("raw"), QStringLiteral("control"));
  const auto token = welcome.value(QStringLiteral("token")).toString();
  QCOMPARE(token.size(), 32);
  QSslSocket wrong;
  connectRaw(wrong, m_certificate, server.listeningPort());
  QTRY_VERIFY_WITH_TIMEOUT(wrong.isEncrypted(), 5000);
  QVERIFY(rawHello(wrong, QStringLiteral("raw"), QStringLiteral("audio"), QString(32, QLatin1Char('0'))).isEmpty());
  QTRY_COMPARE_WITH_TIMEOUT(wrong.state(), QAbstractSocket::UnconnectedState, 5000);
  QVERIFY(server.channelReady(QStringLiteral("raw"), Channel::Control));
  QVERIFY(!server.channelReady(QStringLiteral("raw"), Channel::Audio));
  QSslSocket correct;
  connectRaw(correct, m_certificate, server.listeningPort());
  QTRY_VERIFY_WITH_TIMEOUT(correct.isEncrypted(), 5000);
  QVERIFY(!rawHello(correct, QStringLiteral("raw"), QStringLiteral("audio"), token).isEmpty());
  QVERIFY(server.channelReady(QStringLiteral("raw"), Channel::Audio));
}

void WorkspaceTransportTests::unsolicitedAuxiliaryChannelIsRejected()
{
  WorkspaceTransport server;
  QVERIFY(server.setIdentityFiles(m_certificate, m_trusted, m_trusted));
  QVERIFY(server.start(true, QStringLiteral("server"), 0, {}, QStringLiteral("127.0.0.1")));
  QSslSocket socket;
  connectRaw(socket, m_certificate, server.listeningPort());
  QTRY_VERIFY_WITH_TIMEOUT(socket.isEncrypted(), 5000);
  QVERIFY(rawHello(socket, QStringLiteral("raw"), QStringLiteral("files"), QString(32, QLatin1Char('1'))).isEmpty());
  QTRY_COMPARE_WITH_TIMEOUT(socket.state(), QAbstractSocket::UnconnectedState, 5000);
  QVERIFY(server.peers().isEmpty());
}

void WorkspaceTransportTests::auxiliaryCertificateMustMatchControl()
{
  WorkspaceTransport server;
  QVERIFY(server.setIdentityFiles(m_certificate, m_trusted, m_trustBoth));
  QVERIFY(server.start(true, QStringLiteral("server"), 0, {}, QStringLiteral("127.0.0.1")));
  QSslSocket control;
  connectRaw(control, m_certificate, server.listeningPort());
  QTRY_VERIFY_WITH_TIMEOUT(control.isEncrypted(), 5000);
  const auto token =
      rawHello(control, QStringLiteral("raw"), QStringLiteral("control")).value(QStringLiteral("token")).toString();
  QCOMPARE(token.size(), 32);
  QSslSocket otherIdentity;
  connectRaw(otherIdentity, m_secondCertificate, server.listeningPort());
  QTRY_VERIFY_WITH_TIMEOUT(otherIdentity.isEncrypted(), 5000);
  QVERIFY(rawHello(otherIdentity, QStringLiteral("raw"), QStringLiteral("files"), token).isEmpty());
  QTRY_COMPARE_WITH_TIMEOUT(otherIdentity.state(), QAbstractSocket::UnconnectedState, 5000);
  QVERIFY(server.channelReady(QStringLiteral("raw"), Channel::Control));
  QVERIFY(!server.channelReady(QStringLiteral("raw"), Channel::Files));
}

void WorkspaceTransportTests::wrongChannelMessageClosesOnlyThatChannel()
{
  WorkspaceTransport server;
  WorkspaceTransport client;
  QVERIFY(server.setIdentityFiles(m_certificate, m_trusted, m_trusted));
  QVERIFY(client.setIdentityFiles(m_certificate, m_trusted, m_trusted));
  QVERIFY(server.start(true, QStringLiteral("server"), 0, {}, QStringLiteral("127.0.0.1")));
  QVERIFY(client.start(false, QStringLiteral("client"), server.listeningPort(), {QStringLiteral("127.0.0.1")}));
  QTRY_VERIFY_WITH_TIMEOUT(ready(client, QStringLiteral("server")), 10000);
  QSignalSpy received(&server, &WorkspaceTransport::messageReceived);
  QVERIFY(client.send(QStringLiteral("server"), QStringLiteral("files.chunk"), {}, QByteArray(32, 'x'), Channel::Audio)
  );
  QTRY_VERIFY_WITH_TIMEOUT(!server.channelReady(QStringLiteral("client"), Channel::Audio), 2000);
  QVERIFY(server.channelReady(QStringLiteral("client"), Channel::Control));
  QVERIFY(server.channelReady(QStringLiteral("client"), Channel::Files));
  QCOMPARE(received.size(), 0);
}

void WorkspaceTransportTests::controlDisconnectClosesAuxiliaries()
{
  WorkspaceTransport server;
  WorkspaceTransport client;
  QVERIFY(server.setIdentityFiles(m_certificate, m_trusted, m_trusted));
  QVERIFY(client.setIdentityFiles(m_certificate, m_trusted, m_trusted));
  QVERIFY(server.start(true, QStringLiteral("server"), 0, {}, QStringLiteral("127.0.0.1")));
  QVERIFY(client.start(false, QStringLiteral("client"), server.listeningPort(), {QStringLiteral("127.0.0.1")}));
  QTRY_VERIFY_WITH_TIMEOUT(ready(server, QStringLiteral("client")), 10000);
  QSignalSpy disconnected(&server, &WorkspaceTransport::peerDisconnected);
  client.stop();
  QTRY_VERIFY_WITH_TIMEOUT(server.peers().isEmpty(), 5000);
  QCOMPARE(disconnected.size(), 1);
  QVERIFY(!server.channelReady(QStringLiteral("client"), Channel::Audio));
  QVERIFY(!server.channelReady(QStringLiteral("client"), Channel::Files));
}

void WorkspaceTransportTests::expiredPinnedCertificateIsRejected()
{
  WorkspaceTransport server;
  WorkspaceTransport client;
  QVERIFY(server.setIdentityFiles(m_expiredCertificate, m_expiredTrusted, m_expiredTrusted));
  QVERIFY(client.setIdentityFiles(m_expiredCertificate, m_expiredTrusted, m_expiredTrusted));
  QSignalSpy connected(&server, &WorkspaceTransport::peerConnected);
  QSignalSpy errors(&client, &WorkspaceTransport::error);
  QVERIFY(server.start(true, QStringLiteral("server"), 0, {}, QStringLiteral("127.0.0.1")));
  QVERIFY(client.start(false, QStringLiteral("client"), server.listeningPort(), {QStringLiteral("127.0.0.1")}));
  QTRY_VERIFY_WITH_TIMEOUT(!errors.isEmpty(), 15000);
  QTest::qWait(100);
  QCOMPARE(connected.size(), 0);
  QVERIFY(client.peers().isEmpty());
}

QTEST_GUILESS_MAIN(WorkspaceTransportTests)
