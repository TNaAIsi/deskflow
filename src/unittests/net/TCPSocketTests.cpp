/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "TCPSocketTests.h"

#include "../deskflow/MockEventQueue.h"
#include "arch/IArchNetwork.h"
#include "net/SocketMultiplexer.h"
#include "net/TCPSocket.h"

#include <QElapsedTimer>
#include <QHostAddress>
#include <QNetworkProxy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>

#include <memory>

namespace {

ArchSocket openLoopbackSocket(QTcpServer &server, std::unique_ptr<QTcpSocket> &peer)
{
  if (!server.listen(QHostAddress::LocalHost, 0)) {
    return nullptr;
  }

  ArchSocket socket = ARCH->newSocket(IArchNetwork::AddressFamily::INet, IArchNetwork::SocketType::Stream);
  auto addresses = ARCH->nameToAddr("127.0.0.1");
  if (addresses.empty()) {
    ARCH->closeSocket(socket);
    return nullptr;
  }

  ArchNetAddress addr = addresses.front();
  ARCH->setAddrPort(addr, server.serverPort());
  const bool connectedImmediately = ARCH->connectSocket(socket, addr);
  for (auto *address : addresses) {
    ARCH->closeAddr(address);
  }

  if (!server.waitForNewConnection(2000)) {
    ARCH->closeSocket(socket);
    return nullptr;
  }

  peer.reset(server.nextPendingConnection());
  if (!connectedImmediately) {
    IArchNetwork::PollEntry entry{socket, IArchNetwork::PollEventMask::Out, 0};
    ARCH->pollSocket(&entry, 1, 2.0);
  }
  return socket;
}

class BreakOnWriteTCPSocket : public TCPSocket
{
public:
  using TCPSocket::TCPSocket;

protected:
  JobResult doWrite() override
  {
    return JobResult::Break;
  }
};

class StallFlushTCPSocket : public TCPSocket
{
public:
  using TCPSocket::TCPSocket;

  ISocketMultiplexerJob *newJob() override
  {
    return nullptr;
  }
};

} // namespace

void TCPSocketTests::initTestCase()
{
  m_arch.init();
  QNetworkProxy::setApplicationProxy(QNetworkProxy::NoProxy);
}

void TCPSocketTests::flush_returnsImmediatelyAfterServiceConnectedBreak()
{
  QTcpServer server;
  std::unique_ptr<QTcpSocket> peer;
  ArchSocket rawSocket = openLoopbackSocket(server, peer);
  QVERIFY(rawSocket != nullptr);
  QVERIFY(peer != nullptr);

  MockEventQueue events;
  SocketMultiplexer multiplexer;
  BreakOnWriteTCPSocket socket(&events, &multiplexer, rawSocket);

  QElapsedTimer timer;
  timer.start();
  socket.write("CALV", 4);
  socket.flush();
  const auto elapsed = timer.elapsed();
  socket.close();
  QVERIFY(elapsed < 1500);
}

void TCPSocketTests::flush_timesOutInsteadOfHangingForever()
{
  QTcpServer server;
  std::unique_ptr<QTcpSocket> peer;
  ArchSocket rawSocket = openLoopbackSocket(server, peer);
  QVERIFY(rawSocket != nullptr);
  QVERIFY(peer != nullptr);

  MockEventQueue events;
  SocketMultiplexer multiplexer;
  StallFlushTCPSocket socket(&events, &multiplexer, rawSocket);

  QElapsedTimer timer;
  timer.start();
  socket.write("CALV", 4);
  socket.flush();
  const auto elapsed = timer.elapsed();
  socket.close();
  QVERIFY(elapsed >= 1800);
  QVERIFY(elapsed < 5000);
}

QTEST_GUILESS_MAIN(TCPSocketTests)
