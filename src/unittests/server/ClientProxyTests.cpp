/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Synergy App Ltd
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "ClientProxyTests.h"

#include "../deskflow/MockEventQueue.h"
#include "base/Event.h"
#include "deskflow/AppUtil.h"
#include "io/IStream.h"
#include "server/ClientProxy1_0.h"
#include "server/ClientProxy1_1.h"
#include "server/ClientProxy1_6.h"
#include "server/ClientProxy1_7.h"
#include "server/ClientProxy1_8.h"
#include "server/ClientProxy1_9.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <QByteArray>
#include <QScopeGuard>
#include <QTest>

namespace {

class TestAppUtil : public AppUtil
{
public:
  int run() override
  {
    return 0;
  }

  std::vector<std::string> getKeyboardLayoutList() override
  {
    return {"en"};
  }

  std::string getCurrentLanguageCode() override
  {
    return "en";
  }
};

class CapturingStream : public deskflow::IStream
{
public:
  QByteArray take()
  {
    auto bytes = m_buffer;
    m_buffer.clear();
    return bytes;
  }

  void write(const void *buffer, uint32_t n) override
  {
    m_buffer.append(static_cast<const char *>(buffer), n);
  }

  void close() override
  {
  }

  uint32_t read(void *buffer, uint32_t size) override
  {
    const auto count = std::min(static_cast<qsizetype>(size), m_input.size());
    if (buffer != nullptr) {
      std::memcpy(buffer, m_input.constData(), count);
    }
    m_input.remove(0, count);
    return static_cast<uint32_t>(count);
  }

  void push(const QByteArray &input)
  {
    m_input.append(input);
  }

  void flush() override
  {
  }

  void shutdownInput() override
  {
  }

  void shutdownOutput() override
  {
  }

  void *getEventTarget() const override
  {
    return const_cast<CapturingStream *>(this);
  }

  bool isReady() const override
  {
    return false;
  }

  uint32_t getSize() const override
  {
    return 0;
  }

private:
  QByteArray m_buffer;
  QByteArray m_input;
};

class AudioEventQueue : public MockEventQueue
{
public:
  EventQueueTimer *newTimer(double, void *) override
  {
    ++timersStarted;
    return reinterpret_cast<EventQueueTimer *>(&m_timerStorage);
  }

  void addHandler(EventTypes type, void *target, const EventHandler &handler) override
  {
    if (type == EventTypes::Timer && target == &m_timerStorage) {
      audioHandler = handler;
    }
  }

  int timersStarted = 0;
  EventHandler audioHandler;

private:
  int m_timerStorage = 0;
};

class TestAudioProxy : public ClientProxy1_9
{
public:
  using ClientProxy1_9::ClientProxy1_9;

  bool ready(CapturingStream *stream, uint8_t ready)
  {
    stream->push(QByteArray(1, static_cast<char>(ready)));
    return parseMessage(reinterpret_cast<const uint8_t *>(kMsgDAudioRTS));
  }
};

auto restoreAudioEnvironment()
{
  const auto previous = qgetenv("DESKFLOW_AUDIO_TEST_TONE");
  return qScopeGuard([previous] {
    if (previous.isNull()) {
      qunsetenv("DESKFLOW_AUDIO_TEST_TONE");
    } else {
      qputenv("DESKFLOW_AUDIO_TEST_TONE", previous);
    }
  });
}

std::unique_ptr<ClientProxy> makeProxy(int minor, deskflow::IStream *stream, IEventQueue *events)
{
  // the 1.4 and later constructors assert the server pointer is non-null but only store it
  auto *server = reinterpret_cast<Server *>(0x1);

  std::unique_ptr<ClientProxy> proxy;
  switch (minor) {
  case 0:
    proxy = std::make_unique<ClientProxy1_0>("client", stream, events);
    break;
  case 1:
    proxy = std::make_unique<ClientProxy1_1>("client", stream, events);
    break;
  case 6:
    proxy = std::make_unique<ClientProxy1_6>("client", stream, server, events);
    break;
  case 7:
    proxy = std::make_unique<ClientProxy1_7>("client", stream, server, events);
    break;
  case 8:
    proxy = std::make_unique<ClientProxy1_8>("client", stream, server, events);
    break;
  case 9:
    proxy = std::make_unique<ClientProxy1_9>("client", stream, server, events);
    break;
  default:
    break;
  }
  return proxy;
}

struct ProxyUnderTest
{
  MockEventQueue events;
  CapturingStream *stream = new CapturingStream;
  std::unique_ptr<ClientProxy> proxy;

  explicit ProxyUnderTest(int minor) : proxy(makeProxy(minor, stream, &events))
  {
    // drop the query-info and layout-sync messages the constructors send
    stream->take();
  }
};

const KeyID kKey = 0x61;
const KeyModifierMask kMask = 0;
const KeyButton kButton = 0x1e;
const int32_t kCount = 3;
const std::string kLang = "en";

} // namespace

void ClientProxyTests::initTestCase()
{
  // the 1.8 constructor reads the keyboard layouts through AppUtil::instance()
  static TestAppUtil appUtil;
}

// These formats are frozen because shipped third-party clients parse them byte
// for byte: Synergy 1.4 through 1.14.1 negotiate 1.4 through 1.7, Barrier and
// Input Leap negotiate 1.6, and Synergy 1.14.2 onwards and Deskflow negotiate 1.8.
void ClientProxyTests::keyDown_data()
{
  QTest::addColumn<int>("minor");
  QTest::addColumn<QByteArray>("expected");

  QTest::newRow("1.0") << 0 << "DKDN" + QByteArray::fromHex("0061 0000");
  QTest::newRow("1.1") << 1 << "DKDN" + QByteArray::fromHex("0061 0000 001e");
  QTest::newRow("1.6") << 6 << "DKDN" + QByteArray::fromHex("0061 0000 001e");
  QTest::newRow("1.7") << 7 << "DKDN" + QByteArray::fromHex("0061 0000 001e");
  QTest::newRow("1.8") << 8 << "DKDL" + QByteArray::fromHex("0061 0000 001e 00000002") + "en";
  QTest::newRow("1.9") << 9 << "DKDL" + QByteArray::fromHex("0061 0000 001e 00000002") + "en";
}

void ClientProxyTests::keyDown()
{
  QFETCH(int, minor);
  QFETCH(QByteArray, expected);

  ProxyUnderTest test(minor);
  test.proxy->keyDown(kKey, kMask, kButton, kLang);
  QCOMPARE(test.stream->take(), expected);
}

void ClientProxyTests::keyRepeat_data()
{
  QTest::addColumn<int>("minor");
  QTest::addColumn<QByteArray>("expected");

  QTest::newRow("1.0") << 0 << "DKRP" + QByteArray::fromHex("0061 0000 0003");
  QTest::newRow("1.1") << 1 << "DKRP" + QByteArray::fromHex("0061 0000 0003 001e");
  QTest::newRow("1.6") << 6 << "DKRP" + QByteArray::fromHex("0061 0000 0003 001e");
  QTest::newRow("1.7") << 7 << "DKRP" + QByteArray::fromHex("0061 0000 0003 001e");
  QTest::newRow("1.8") << 8 << "DKRP" + QByteArray::fromHex("0061 0000 0003 001e 00000002") + "en";
  QTest::newRow("1.9") << 9 << "DKRP" + QByteArray::fromHex("0061 0000 0003 001e 00000002") + "en";
}

void ClientProxyTests::keyRepeat()
{
  QFETCH(int, minor);
  QFETCH(QByteArray, expected);

  ProxyUnderTest test(minor);
  test.proxy->keyRepeat(kKey, kMask, kCount, kButton, kLang);
  QCOMPARE(test.stream->take(), expected);
}

void ClientProxyTests::keyUp_data()
{
  QTest::addColumn<int>("minor");
  QTest::addColumn<QByteArray>("expected");

  QTest::newRow("1.0") << 0 << "DKUP" + QByteArray::fromHex("0061 0000");
  QTest::newRow("1.1") << 1 << "DKUP" + QByteArray::fromHex("0061 0000 001e");
  QTest::newRow("1.6") << 6 << "DKUP" + QByteArray::fromHex("0061 0000 001e");
  QTest::newRow("1.7") << 7 << "DKUP" + QByteArray::fromHex("0061 0000 001e");
  QTest::newRow("1.8") << 8 << "DKUP" + QByteArray::fromHex("0061 0000 001e");
  QTest::newRow("1.9") << 9 << "DKUP" + QByteArray::fromHex("0061 0000 001e");
}

void ClientProxyTests::keyUp()
{
  QFETCH(int, minor);
  QFETCH(QByteArray, expected);

  ProxyUnderTest test(minor);
  test.proxy->keyUp(kKey, kMask, kButton);
  QCOMPARE(test.stream->take(), expected);
}

void ClientProxyTests::audio_defaultDisabled()
{
  const auto restore = restoreAudioEnvironment();
  qunsetenv("DESKFLOW_AUDIO_TEST_TONE");
  AudioEventQueue events;
  auto *stream = new CapturingStream;
  TestAudioProxy proxy("client", stream, reinterpret_cast<Server *>(0x1), &events);
  QVERIFY(!stream->take().contains("ACFG"));

  proxy.setOptions({});
  QCOMPARE(stream->take(), "DSOP" + QByteArray::fromHex("00000000"));
  QVERIFY(!proxy.isAudioActive());
  QCOMPARE(events.timersStarted, 0);
}

void ClientProxyTests::audio_startsAfterOptionsAndReceiverReady()
{
  const auto restore = restoreAudioEnvironment();
  qputenv("DESKFLOW_AUDIO_TEST_TONE", "1");
  AudioEventQueue events;
  auto *stream = new CapturingStream;
  TestAudioProxy proxy("client", stream, reinterpret_cast<Server *>(0x1), &events);
  QVERIFY(!stream->take().contains("ACFG"));
  QCOMPARE(events.timersStarted, 0);

  proxy.setOptions({});
  const auto configuration = stream->take();
  QVERIFY(configuration.startsWith("DSOP" + QByteArray::fromHex("00000000") + "ACFG"));
  QCOMPARE(configuration.count("ACFG"), 1);
  QVERIFY(!proxy.isAudioActive());
  QCOMPARE(events.timersStarted, 0);

  QVERIFY(proxy.ready(stream, 1));
  QVERIFY(proxy.isAudioActive());
  QCOMPARE(events.timersStarted, 1);
  QVERIFY(static_cast<bool>(events.audioHandler));
  events.audioHandler(Event(EventTypes::Timer, nullptr));
  const auto frame = stream->take();
  QVERIFY(frame.startsWith("ADAT" + QByteArray::fromHex("00000000 00000f00")));
  QCOMPARE(frame.size(), 12 + 3840);

  QVERIFY(proxy.ready(stream, 1));
  QCOMPARE(events.timersStarted, 1);
  proxy.setOptions({});
  QVERIFY(!stream->take().contains("ACFG"));
  QVERIFY(proxy.ready(stream, 0));
  QVERIFY(!proxy.isAudioActive());
  events.audioHandler(Event(EventTypes::Timer, nullptr));
  QVERIFY(stream->take().isEmpty());
}

void ClientProxyTests::audio_invalidOptionsDoNotStart()
{
  const auto restore = restoreAudioEnvironment();
  qputenv("DESKFLOW_AUDIO_TEST_TONE", "1");
  ProxyUnderTest test(9);
  test.proxy->setOptions({kOptionHeartbeat});
  QVERIFY(test.stream->take().isEmpty());
}

void ClientProxyTests::audio_unsolicitedReadyDoesNotStart()
{
  const auto restore = restoreAudioEnvironment();
  qunsetenv("DESKFLOW_AUDIO_TEST_TONE");
  AudioEventQueue events;
  auto *stream = new CapturingStream;
  TestAudioProxy proxy("client", stream, reinterpret_cast<Server *>(0x1), &events);
  stream->take();
  QVERIFY(proxy.ready(stream, 1));
  QVERIFY(!proxy.isAudioActive());
  QCOMPARE(events.timersStarted, 0);
  QVERIFY(stream->take().isEmpty());
  QVERIFY(!proxy.ready(stream, 2));
}

QTEST_MAIN(ClientProxyTests)
