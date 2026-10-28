/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "ServerProxyTests.h"

#include "base/Event.h"
#include "base/IEventQueue.h"
#include "client/Client.h"
#include "client/ServerProxy.h"
#include "client/ServerProxy1_10.h"
#include "client/ServerProxy1_9.h"
#include "deskflow/AppUtil.h"
#include "deskflow/DeskflowException.h"
#include "deskflow/ProtocolTypes.h"
#include "deskflow/ProtocolUtil.h"
#include "io/IStream.h"
#include "platform/NativeKeyboard.h"

#include <QScopeGuard>
#include <QTest>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>

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

class FakeStream : public deskflow::IStream
{
public:
  void push(const std::string &bytes)
  {
    m_chunks.push_back(bytes);
  }

  void close() override
  {
    m_chunks.clear();
    m_inputShutdown = true;
  }

  uint32_t read(void *buffer, uint32_t size) override
  {
    if (m_inputShutdown || m_chunks.empty() || size == 0) {
      return 0;
    }

    auto &front = m_chunks.front();
    const size_t bytesToRead = std::min(static_cast<size_t>(size), front.size());
    if (buffer != nullptr) {
      std::memcpy(buffer, front.data(), bytesToRead);
    }

    front.erase(0, bytesToRead);
    if (front.empty()) {
      m_chunks.pop_front();
    }
    return static_cast<uint32_t>(bytesToRead);
  }

  void write(const void *buffer, uint32_t size) override
  {
    m_written.append(static_cast<const char *>(buffer), size);
  }

  std::string takeWritten()
  {
    auto written = std::move(m_written);
    m_written.clear();
    return written;
  }

  void flush() override
  {
  }

  void shutdownInput() override
  {
    close();
  }

  void shutdownOutput() override
  {
  }

  void *getEventTarget() const override
  {
    return const_cast<FakeStream *>(this);
  }

  bool isReady() const override
  {
    return !m_inputShutdown && !m_chunks.empty();
  }

  uint32_t getSize() const override
  {
    size_t total = 0;
    for (const auto &chunk : m_chunks) {
      total += chunk.size();
    }
    return static_cast<uint32_t>(std::min<size_t>(total, UINT32_MAX));
  }

private:
  std::deque<std::string> m_chunks;
  bool m_inputShutdown = false;
  std::string m_written;
};

class RecordingEventQueue : public IEventQueue
{
public:
  ~RecordingEventQueue() override
  {
    for (const auto &event : m_addedEvents) {
      Event::deleteData(event);
    }
  }

  int loop() override
  {
    return 0;
  }

  void adoptBuffer(IEventQueueBuffer *) override
  {
  }

  bool getEvent(Event &, double = -1.0) override
  {
    return false;
  }

  bool dispatchEvent(const Event &event) override
  {
    const auto handler = m_handlers.find(HandlerKey{event.getType(), event.getTarget()});
    if (handler == m_handlers.end()) {
      return false;
    }

    const auto callback = handler->second;
    callback(event);
    return true;
  }

  void addEvent(Event &&event) override
  {
    m_addedEvents.emplace_back(std::move(event));
  }

  EventQueueTimer *newTimer(double, void *) override
  {
    return timer();
  }

  EventQueueTimer *newOneShotTimer(double, void *) override
  {
    return timer();
  }

  void deleteTimer(EventQueueTimer *) override
  {
  }

  void addHandler(EventTypes type, void *target, const EventHandler &handler) override
  {
    m_handlers[HandlerKey{type, target}] = handler;
  }

  void removeHandler(EventTypes type, void *target) override
  {
    m_handlers.erase(HandlerKey{type, target});
  }

  void removeHandlers(void *target) override
  {
    for (auto it = m_handlers.begin(); it != m_handlers.end();) {
      if (it->first.target == target) {
        it = m_handlers.erase(it);
      } else {
        ++it;
      }
    }
  }

  void waitForReady() const override
  {
  }

  void *getSystemTarget() override
  {
    return this;
  }

  EventQueueTimer *timer()
  {
    return reinterpret_cast<EventQueueTimer *>(&m_timerStorage);
  }

  const std::vector<Event> &addedEvents() const
  {
    return m_addedEvents;
  }

private:
  struct HandlerKey
  {
    EventTypes type;
    void *target;

    bool operator<(const HandlerKey &other) const
    {
      if (type != other.type) {
        return static_cast<uint32_t>(type) < static_cast<uint32_t>(other.type);
      }
      return std::less<void *>{}(target, other.target);
    }
  };

  int m_timerStorage = 0;
  std::map<HandlerKey, EventHandler> m_handlers;
  std::vector<Event> m_addedEvents;
};

class TestServerProxy : public ServerProxy
{
public:
  using ServerProxy::ServerProxy;

  bool parseHandshakeMessageReturnsDisconnect(const uint8_t *code)
  {
    return parseHandshakeMessage(code) == ConnectionResult::Disconnect;
  }
};

class TestAudioServerProxy : public ServerProxy1_9
{
public:
  using ServerProxy1_9::ServerProxy1_9;

  bool configureAudio()
  {
    return parseMessage(reinterpret_cast<const uint8_t *>(kMsgDAudioConfig)) == ConnectionResult::Okay;
  }
};

class TestNativeServerProxy : public ServerProxy1_10
{
public:
  using ServerProxy1_10::ServerProxy1_10;
  bool parse(const char *code)
  {
    return parseMessage(reinterpret_cast<const uint8_t *>(code)) == ConnectionResult::Okay;
  }
  bool capability()
  {
    return parseHandshakeMessage(reinterpret_cast<const uint8_t *>(kMsgNativeKeyboardCapability)) ==
           ConnectionResult::Okay;
  }
  void mapping(const OptionsList &options)
  {
    updateModifierTranslation(options);
  }
};

struct FakeNativeKeyboard
{
  bool available = true;
  bool accept = true;
  int releases = 0;
  int legacyReleases = 0;
  std::vector<std::tuple<uint16_t, uint16_t, bool>> events;
  ServerProxy1_10::Backend backend()
  {
    return {
        [this] { return available; },
        [this](uint16_t page, uint16_t usage, bool down) {
          if (!accept)
            return false;
          events.emplace_back(page, usage, down);
          return true;
        },
        [this] { ++releases; }, [this] { ++legacyReleases; }, [](const QString &) {}
    };
  }
};

template <typename... Arguments> void pushNative(FakeStream &stream, const char *format, Arguments... values)
{
  ProtocolUtil::writef(&stream, format, values...);
  stream.push(stream.takeWritten().substr(4));
}

Client *undereferenceableClient()
{
  // These paths must queue cleanup without calling through to Client.
  return reinterpret_cast<Client *>(0x1);
}

TestAppUtil &testAppUtil()
{
  static TestAppUtil util;
  return util;
}

const Client::DisconnectRequest *disconnectRequest(const RecordingEventQueue &events)
{
  if (events.addedEvents().size() != 1) {
    return nullptr;
  }
  return static_cast<const Client::DisconnectRequest *>(events.addedEvents().front().getDataObject());
}

} // namespace

void ServerProxyTests::initTestCase()
{
  (void)testAppUtil();
  m_log.setFilter(LogLevel::Level::Debug);
}

void ServerProxyTests::handleKeepAliveAlarm_timeout_queuesDisconnectRequest()
{
  RecordingEventQueue events;
  FakeStream stream;
  ServerProxy proxy(undereferenceableClient(), &stream, &events);

  QVERIFY(events.dispatchEvent(Event(EventTypes::Timer, events.timer())));

  QCOMPARE(events.addedEvents().size(), static_cast<size_t>(1));
  const auto &event = events.addedEvents().front();
  QVERIFY(event.getType() == EventTypes::ClientDisconnectRequested);
  QCOMPARE(event.getTarget(), stream.getEventTarget());

  const auto *request = disconnectRequest(events);
  QVERIFY(request != nullptr);
  QVERIFY(request->kind() == Client::DisconnectRequest::Kind::Disconnect);
  QCOMPARE(QString::fromUtf8(request->message()), QStringLiteral("server is not responding"));
}

void ServerProxyTests::handleData_incompleteMessage_queuesDisconnectRequest()
{
  RecordingEventQueue events;
  FakeStream stream;
  stream.push("DF");
  ServerProxy proxy(undereferenceableClient(), &stream, &events);

  QVERIFY(events.dispatchEvent(Event(EventTypes::StreamInputReady, stream.getEventTarget())));

  QCOMPARE(events.addedEvents().size(), static_cast<size_t>(1));
  const auto &event = events.addedEvents().front();
  QVERIFY(event.getType() == EventTypes::ClientDisconnectRequested);
  QCOMPARE(event.getTarget(), stream.getEventTarget());

  const auto *request = disconnectRequest(events);
  QVERIFY(request != nullptr);
  QVERIFY(request->kind() == Client::DisconnectRequest::Kind::Disconnect);
  QCOMPARE(QString::fromUtf8(request->message()), QStringLiteral("incomplete message from server"));
}

void ServerProxyTests::parseHandshakeMessage_protocolError_queuesRefusalRequest()
{
  RecordingEventQueue events;
  FakeStream stream;
  TestServerProxy proxy(undereferenceableClient(), &stream, &events);

  QVERIFY(proxy.parseHandshakeMessageReturnsDisconnect(reinterpret_cast<const uint8_t *>(kMsgEBad)));
  QCOMPARE(events.addedEvents().size(), static_cast<size_t>(1));
  const auto *request = disconnectRequest(events);
  QVERIFY(request != nullptr);
  QVERIFY(request->kind() == Client::DisconnectRequest::Kind::Refuse);
  QVERIFY(request->refusalReason() == deskflow::core::ConnectionRefusal::ProtocolError);
  QCOMPARE(QString::fromUtf8(request->message()), QStringLiteral("server reported a protocol error"));
}

void ServerProxyTests::audio_rejectsDisabledOrUnsupportedConfiguration_data()
{
  QTest::addColumn<bool>("optIn");
  QTest::addColumn<int>("enable");
  QTest::addColumn<int>("sampleRate");
  QTest::addColumn<int>("channels");
  QTest::addColumn<int>("bitDepth");
  QTest::addColumn<QString>("codec");

  QTest::newRow("disabled-by-default") << false << 1 << 48000 << 2 << 16 << "pcm_s16le";
  QTest::newRow("stop") << true << 0 << 48000 << 2 << 16 << "pcm_s16le";
  QTest::newRow("invalid-enable") << true << 2 << 48000 << 2 << 16 << "pcm_s16le";
  QTest::newRow("unsupported-codec") << true << 1 << 48000 << 2 << 16 << "opus";
  QTest::newRow("unsupported-rate") << true << 1 << 44100 << 2 << 16 << "pcm_s16le";
  QTest::newRow("unsupported-channels") << true << 1 << 48000 << 1 << 16 << "pcm_s16le";
  QTest::newRow("unsupported-depth") << true << 1 << 48000 << 2 << 32 << "pcm_s16le";
}

void ServerProxyTests::audio_rejectsDisabledOrUnsupportedConfiguration()
{
  QFETCH(bool, optIn);
  QFETCH(int, enable);
  QFETCH(int, sampleRate);
  QFETCH(int, channels);
  QFETCH(int, bitDepth);
  QFETCH(QString, codec);

  const auto previous = qgetenv("DESKFLOW_AUDIO_TEST_TONE");
  const auto restore = qScopeGuard([previous] {
    if (previous.isNull()) {
      qunsetenv("DESKFLOW_AUDIO_TEST_TONE");
    } else {
      qputenv("DESKFLOW_AUDIO_TEST_TONE", previous);
    }
  });
  if (optIn) {
    qputenv("DESKFLOW_AUDIO_TEST_TONE", "1");
  } else {
    qunsetenv("DESKFLOW_AUDIO_TEST_TONE");
  }

  RecordingEventQueue events;
  FakeStream stream;
  TestAudioServerProxy proxy(undereferenceableClient(), &stream, &events);
  const auto codecBytes = codec.toStdString();
  ProtocolUtil::writef(&stream, kMsgDAudioConfig, enable, sampleRate, channels, bitDepth, &codecBytes);
  stream.push(stream.takeWritten().substr(4));

  QVERIFY(proxy.configureAudio());
  QCOMPARE(QByteArray::fromStdString(stream.takeWritten()), QByteArray("ARTS\0", 5));
}

void ServerProxyTests::audio_malformedConfigurationDoesNotSignalReady()
{
  RecordingEventQueue events;
  FakeStream stream;
  TestAudioServerProxy proxy(undereferenceableClient(), &stream, &events);
  stream.push(std::string("\1", 1));

  QVERIFY_THROWS_EXCEPTION(BadClientException, proxy.configureAudio());
  QVERIFY(stream.takeWritten().empty());
}

void ServerProxyTests::nativeKeyboardRequiresBothCapabilityAndReadyBackend()
{
  RecordingEventQueue events;
  FakeStream stream;
  FakeNativeKeyboard keyboard;
  TestNativeServerProxy proxy(undereferenceableClient(), &stream, &events, keyboard.backend(), true);
  pushNative(stream, kMsgNativeKeyboardCapability, 0);
  QVERIFY(proxy.capability());
  QCOMPARE(stream.takeWritten(), std::string("NKRD\0", 5));
  keyboard.available = false;
  pushNative(stream, kMsgNativeKeyboardCapability, 1);
  QVERIFY(proxy.capability());
  QCOMPARE(stream.takeWritten(), std::string("NKRD\0", 5));
  keyboard.available = true;
  pushNative(stream, kMsgNativeKeyboardCapability, 1);
  QVERIFY(proxy.capability());
  QCOMPARE(stream.takeWritten(), std::string("NKRD\1", 5));
  QVERIFY(keyboard.events.empty());
}

void ServerProxyTests::nativeKeyboardPreservesPhysicalPressReleaseAndLocalRepeat()
{
  RecordingEventQueue events;
  FakeStream stream;
  FakeNativeKeyboard keyboard;
  TestNativeServerProxy proxy(undereferenceableClient(), &stream, &events, keyboard.backend(), true);
  pushNative(stream, kMsgNativeKeyboardCapability, 1);
  QVERIFY(proxy.capability());
  QVERIFY(!deskflow::nativeKeyboardSessionActive());
  stream.takeWritten();
  pushNative(stream, kMsgNativeKeyboardSession, 1, 0);
  QVERIFY(proxy.parse(kMsgNativeKeyboardSession));
  QVERIFY(deskflow::nativeKeyboardSessionActive());
  // A dead-key position is passed as a key, independently of its character.
  pushNative(stream, kMsgNativePhysicalKey, 7, 0x34, 1);
  QVERIFY(proxy.parse(kMsgNativePhysicalKey));
  pushNative(stream, kMsgNativePhysicalKey, 7, 0x34, 1);
  QVERIFY(proxy.parse(kMsgNativePhysicalKey));
  pushNative(stream, kMsgNativePhysicalKey, 7, 0x34, 0);
  QVERIFY(proxy.parse(kMsgNativePhysicalKey));
  pushNative(stream, kMsgNativePhysicalKey, 7, 0x34, 0);
  QVERIFY(proxy.parse(kMsgNativePhysicalKey));
  QCOMPARE(keyboard.events.size(), size_t(2));
  QCOMPARE(keyboard.events[0], std::make_tuple(uint16_t(7), uint16_t(0x34), true));
  QCOMPARE(keyboard.events[1], std::make_tuple(uint16_t(7), uint16_t(0x34), false));
  QCOMPARE(keyboard.legacyReleases, 1);
  pushNative(stream, kMsgNativeKeyboardSession, 0, 0);
  QVERIFY(proxy.parse(kMsgNativeKeyboardSession));
  QVERIFY(!deskflow::nativeKeyboardSessionActive());
  QVERIFY(keyboard.releases >= 2);
}

void ServerProxyTests::nativeKeyboardCarriesInitialModifiersAndRespectsRemaps()
{
  RecordingEventQueue events;
  FakeStream stream;
  FakeNativeKeyboard keyboard;
  TestNativeServerProxy proxy(undereferenceableClient(), &stream, &events, keyboard.backend(), true);
  proxy.mapping({kOptionModifierMapForControl, kKeyModifierIDSuper});
  pushNative(stream, kMsgNativeKeyboardCapability, 1);
  QVERIFY(proxy.capability());
  stream.takeWritten();
  pushNative(stream, kMsgNativeKeyboardSession, 1, 1);
  QVERIFY(proxy.parse(kMsgNativeKeyboardSession));
  QCOMPARE(keyboard.events.front(), std::make_tuple(uint16_t(7), uint16_t(0xe3), true));
  // Both source Control and Win map to the same target GUI key. Releasing one
  // source must not release the target while the other is still physically held.
  pushNative(stream, kMsgNativePhysicalKey, 7, 0xe3, 1);
  QVERIFY(proxy.parse(kMsgNativePhysicalKey));
  pushNative(stream, kMsgNativePhysicalKey, 7, 0xe0, 0);
  QVERIFY(proxy.parse(kMsgNativePhysicalKey));
  QCOMPARE(keyboard.events.size(), size_t(1));
  // A remap update while held does not change which key must be released.
  proxy.mapping({kOptionModifierMapForSuper, kKeyModifierIDAlt});
  pushNative(stream, kMsgNativePhysicalKey, 7, 0xe3, 0);
  QVERIFY(proxy.parse(kMsgNativePhysicalKey));
  QCOMPARE(keyboard.events.size(), size_t(2));
  QCOMPARE(keyboard.events.back(), std::make_tuple(uint16_t(7), uint16_t(0xe3), false));
  proxy.mapping({kOptionModifierMapForAltGr, kKeyModifierIDControl});
  pushNative(stream, kMsgNativePhysicalKey, 7, 0xe6, 1);
  QVERIFY(proxy.parse(kMsgNativePhysicalKey));
  QCOMPARE(keyboard.events.back(), std::make_tuple(uint16_t(7), uint16_t(0xe4), true));
  pushNative(stream, kMsgNativePhysicalKey, 7, 0xe6, 0);
  QVERIFY(proxy.parse(kMsgNativePhysicalKey));
  QCOMPARE(keyboard.events.back(), std::make_tuple(uint16_t(7), uint16_t(0xe4), false));
}

void ServerProxyTests::nativeKeyboardFailureReleasesEverything()
{
  RecordingEventQueue events;
  FakeStream stream;
  FakeNativeKeyboard keyboard;
  TestNativeServerProxy proxy(undereferenceableClient(), &stream, &events, keyboard.backend(), true);
  pushNative(stream, kMsgNativeKeyboardCapability, 1);
  QVERIFY(proxy.capability());
  stream.takeWritten();
  pushNative(stream, kMsgNativeKeyboardSession, 1, 0);
  QVERIFY(proxy.parse(kMsgNativeKeyboardSession));
  keyboard.accept = false;
  const int before = keyboard.releases;
  pushNative(stream, kMsgNativePhysicalKey, 7, 4, 1);
  QVERIFY_THROWS_EXCEPTION(BadClientException, proxy.parse(kMsgNativePhysicalKey));
  QVERIFY(keyboard.releases > before);
  QVERIFY(!deskflow::nativeKeyboardSessionActive());
  // The failed session cannot continue with an up or another down.
  pushNative(stream, kMsgNativePhysicalKey, 7, 4, 0);
  QVERIFY_THROWS_EXCEPTION(BadClientException, proxy.parse(kMsgNativePhysicalKey));
}

void ServerProxyTests::nativeKeyboardRejectsLegacyOrUnnegotiatedMessages()
{
  RecordingEventQueue events;
  FakeStream stream;
  FakeNativeKeyboard keyboard;
  TestNativeServerProxy proxy(undereferenceableClient(), &stream, &events, keyboard.backend(), true);
  pushNative(stream, kMsgNativePhysicalKey, 7, 4, 1);
  QVERIFY_THROWS_EXCEPTION(BadClientException, proxy.parse(kMsgNativePhysicalKey));
  pushNative(stream, kMsgNativeKeyboardCapability, 1);
  QVERIFY(proxy.capability());
  stream.takeWritten();
  pushNative(stream, kMsgNativeKeyboardSession, 1, 0);
  QVERIFY(proxy.parse(kMsgNativeKeyboardSession));
  QVERIFY_THROWS_EXCEPTION(BadClientException, proxy.parse(kMsgDKeyDown));
  pushNative(stream, kMsgNativePhysicalKey, 1, 0x82, 1);
  QVERIFY_THROWS_EXCEPTION(BadClientException, proxy.parse(kMsgNativePhysicalKey));
  pushNative(stream, kMsgNativeKeyboardSession, 1, 0);
  QVERIFY(proxy.parse(kMsgNativeKeyboardSession));
  QVERIFY_THROWS_EXCEPTION(BadClientException, proxy.parse("ZZZZ"));
}

void ServerProxyTests::nativeKeyboardActionsDoNotReleasePhysicalModifiers()
{
  RecordingEventQueue events;
  FakeStream stream;
  FakeNativeKeyboard keyboard;
  TestNativeServerProxy proxy(undereferenceableClient(), &stream, &events, keyboard.backend(), true);
  proxy.mapping({kOptionModifierMapForControl, kKeyModifierIDSuper});
  pushNative(stream, kMsgNativeKeyboardCapability, 1);
  QVERIFY(proxy.capability());
  stream.takeWritten();
  pushNative(stream, kMsgNativeKeyboardSession, 1, 1);
  QVERIFY(proxy.parse(kMsgNativeKeyboardSession));
  pushNative(stream, kMsgNativeSyntheticKey, uint32_t('c'), KeyModifierControl, 1);
  QVERIFY(proxy.parse(kMsgNativeSyntheticKey));
  pushNative(stream, kMsgNativeSyntheticKey, uint32_t('c'), KeyModifierControl, 0);
  QVERIFY(proxy.parse(kMsgNativeSyntheticKey));
  QCOMPARE(keyboard.events.size(), size_t(3));
  QCOMPARE(keyboard.events[0], std::make_tuple(uint16_t(7), uint16_t(0xe3), true));
  QCOMPARE(keyboard.events[1], std::make_tuple(uint16_t(7), uint16_t(6), true));
  QCOMPARE(keyboard.events[2], std::make_tuple(uint16_t(7), uint16_t(6), false));
  pushNative(stream, kMsgNativePhysicalKey, 7, 0xe0, 0);
  QVERIFY(proxy.parse(kMsgNativePhysicalKey));
  QCOMPARE(keyboard.events.back(), std::make_tuple(uint16_t(7), uint16_t(0xe3), false));
  pushNative(stream, kMsgNativeSyntheticKey, kKeySetModifiers, KeyModifierControl, 1);
  QVERIFY(proxy.parse(kMsgNativeSyntheticKey));
  pushNative(stream, kMsgNativeSyntheticKey, kKeyClearModifiers, KeyModifierControl, 1);
  QVERIFY(proxy.parse(kMsgNativeSyntheticKey));
  QCOMPARE(keyboard.events[keyboard.events.size() - 2], std::make_tuple(uint16_t(7), uint16_t(0xe3), true));
  QCOMPARE(keyboard.events.back(), std::make_tuple(uint16_t(7), uint16_t(0xe3), false));
}

QTEST_MAIN(ServerProxyTests)
