/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "AudioServiceTests.h"

#include "workspace/AudioPcm.h"
#include "workspace/AudioService.h"

#include <QSignalSpy>
#include <QTest>
#include <QtEndian>

#include <bit>
#include <cmath>
#include <limits>

using namespace deskflow::workspace;

namespace {

QByteArray stereoSamples(int frames)
{
  QByteArray data(frames * 4, Qt::Uninitialized);
  for (int frame = 0; frame < frames; ++frame) {
    qToLittleEndian(static_cast<qint16>(frame * 100 - 10000), data.data() + frame * 4);
    qToLittleEndian(static_cast<qint16>(10000 - frame * 100), data.data() + frame * 4 + 2);
  }
  return data;
}

QJsonObject metadata()
{
  return {
      {QStringLiteral("format"), QStringLiteral("pcm_s16le")},
      {QStringLiteral("sampleRate"), kAudioSampleRate},
      {QStringLiteral("channels"), 2},
      {QStringLiteral("frames"), kAudioFrameSamples},
      {QStringLiteral("streamId"), QStringLiteral("test-stream")},
      {QStringLiteral("sequence"), 0}
  };
}

} // namespace

void AudioServiceTests::converter_preservesStereoInt16()
{
  const auto input = stereoSamples(100);
  PcmConverter converter;
  const auto converted = converter.convert(input);
  // Linear interpolation retains the final input sample for the next packet.
  QCOMPARE(converted, input.first(input.size() - 4));
}

void AudioServiceTests::converter_partialPacketsMatchWholePacket()
{
  const auto input = stereoSamples(100);
  PcmConverter complete;
  PcmConverter fragmented;
  QByteArray result;
  for (qsizetype offset = 0; offset < input.size(); offset += 7) {
    result.append(fragmented.convert(QByteArrayView(input).sliced(offset, std::min<qsizetype>(7, input.size() - offset))
    ));
  }
  QCOMPARE(result, complete.convert(input));
}

void AudioServiceTests::converter_floatAndPacked24_data()
{
  QTest::addColumn<int>("type");
  QTest::addColumn<QByteArray>("data");
  QTest::addColumn<int>("expected");
  QTest::newRow("packed24-positive") << static_cast<int>(PcmSampleType::Signed24)
                                     << QByteArray::fromHex("000040 000040 000040 000040") << 16384;
  QTest::newRow("packed24-negative") << static_cast<int>(PcmSampleType::Signed24)
                                     << QByteArray::fromHex("0000c0 0000c0 0000c0 0000c0") << -16384;
  QTest::newRow("signed32") << static_cast<int>(PcmSampleType::Signed32)
                            << QByteArray::fromHex("00000040 00000040 00000040 00000040") << 16384;
  QTest::newRow("float32") << static_cast<int>(PcmSampleType::Float32)
                           << QByteArray::fromHex("0000003f 0000003f 0000003f 0000003f") << 16384;
  QTest::newRow("float64") << static_cast<int>(PcmSampleType::Float64)
                           << QByteArray::fromHex("000000000000e03f 000000000000e03f 000000000000e03f 000000000000e03f")
                           << 16384;
  QTest::newRow("nan-is-silence") << static_cast<int>(PcmSampleType::Float32)
                                  << QByteArray::fromHex("0000c07f 0000c07f 0000c07f 0000c07f") << 0;
}

void AudioServiceTests::converter_floatAndPacked24()
{
  QFETCH(int, type);
  QFETCH(QByteArray, data);
  QFETCH(int, expected);
  PcmFormat format;
  format.sampleType = static_cast<PcmSampleType>(type);
  PcmConverter converter(format);
  const auto output = converter.convert(data);
  QCOMPARE(output.size(), 4);
  QCOMPARE(qFromLittleEndian<qint16>(output.constData()), expected);
  QCOMPARE(qFromLittleEndian<qint16>(output.constData() + 2), expected);
}

void AudioServiceTests::converter_resamplesAndDuplicatesMono()
{
  PcmFormat format;
  format.sampleRate = 44100;
  format.channels = 1;
  PcmConverter converter(format);
  QByteArray mono(441 * 2, Qt::Uninitialized);
  for (int frame = 0; frame < 441; ++frame) {
    qToLittleEndian(static_cast<qint16>(4096), mono.data() + frame * 2);
  }
  QByteArray output;
  for (int packet = 0; packet < 100; ++packet) {
    output.append(converter.convert(mono));
  }
  QVERIFY(std::abs(output.size() / 4 - 48000) <= 2);
  for (qsizetype sample = 0; sample < output.size() / 2; ++sample) {
    QCOMPARE(qFromLittleEndian<qint16>(output.constData() + sample * 2), 4096);
  }
}

void AudioServiceTests::converter_silenceAndInvalidFormats()
{
  PcmConverter converter;
  const auto silence = converter.convert({}, true, 961);
  QCOMPARE(silence.size(), kAudioFrameBytes);
  QCOMPARE(silence, QByteArray(kAudioFrameBytes, '\0'));
  PcmFormat invalid;
  invalid.channels = 0;
  converter.reset(invalid);
  QVERIFY(converter.convert(stereoSamples(3)).isEmpty());
  invalid.channels = 2;
  invalid.sampleRate = 0;
  converter.reset(invalid);
  QVERIFY(converter.convert(stereoSamples(3)).isEmpty());
}

void AudioServiceTests::converter_downmixDoesNotOverflow()
{
  PcmFormat format;
  format.channels = 6;
  format.channelMask = 0x3f;
  format.sampleType = PcmSampleType::Float32;
  PcmConverter converter(format);
  QByteArray input(2 * 6 * 4, Qt::Uninitialized);
  for (int sample = 0; sample < 12; ++sample) {
    qToLittleEndian(std::bit_cast<quint32>(1.0f), input.data() + sample * 4);
  }
  const auto output = converter.convert(input);
  QCOMPARE(output.size(), 4);
  QCOMPARE(qFromLittleEndian<qint16>(output.constData()), 32767);
  QCOMPARE(qFromLittleEndian<qint16>(output.constData() + 2), 32767);
}

void AudioServiceTests::ring_preservesWrapAndCapacity()
{
  PcmRingBuffer ring(16);
  const QByteArray first("abcdefghijkl", 12);
  QVERIFY(ring.append(first));
  char output[16]{};
  QCOMPARE(ring.read(output, 8), 8);
  QCOMPARE(QByteArray(output, 8), QByteArray("abcdefgh"));
  QVERIFY(ring.append(QByteArray("mnopqrst", 8)));
  QCOMPARE(ring.queuedBytes(), 12);
  QCOMPARE(ring.read(output, 16), 12);
  QCOMPARE(QByteArray(output, 12), QByteArray("ijklmnopqrst"));
  QCOMPARE(ring.queuedBytes(), 0);
  QCOMPARE(ring.droppedBytes(), 0);
}

void AudioServiceTests::clockBridge_correctsBothDriftDirections()
{
  const QByteArray input(kAudioFrameBytes, '\0');
  PcmClockBridge underfull;
  PcmClockBridge overfull;
  qsizetype added = 0;
  qsizetype removed = 0;
  for (int frame = 0; frame < 1000; ++frame) {
    added += underfull.convert(input, 0).size() / kAudioSampleBytes;
    removed += overfull.convert(input, 4 * kAudioFrameSamples).size() / kAudioSampleBytes;
  }
  QVERIFY(added > 1000 * kAudioFrameSamples);
  QVERIFY(removed < 1000 * kAudioFrameSamples);
  QVERIFY(added - 1000 * kAudioFrameSamples < 1100);
  QVERIFY(1000 * kAudioFrameSamples - removed < 1100);
  underfull.reset();
  QCOMPARE(underfull.convert(input, kAudioFrameSamples).size(), (kAudioFrameSamples - 1) * kAudioSampleBytes);
}

void AudioServiceTests::ring_dropsOldestAndRejectsPartialSamples()
{
  PcmRingBuffer ring(16);
  QVERIFY(ring.append(QByteArray("abcdefghijklmnop")));
  QVERIFY(ring.append(QByteArray("qrstuvwx")));
  QCOMPARE(ring.queuedBytes(), 16);
  QCOMPARE(ring.droppedBytes(), 8);
  QVERIFY(!ring.append(QByteArray("bad")));
  char output[16]{};
  QCOMPARE(ring.read(output, 16), 16);
  QCOMPARE(QByteArray(output, 16), QByteArray("ijklmnopqrstuvwx"));
  QVERIFY(ring.append(QByteArray("0123456789abcdefghijklmn")));
  QCOMPARE(ring.read(output, 16), 16);
  QCOMPARE(QByteArray(output, 16), QByteArray("89abcdefghijklmn"));
  ring.clear();
  QCOMPARE(ring.queuedBytes(), 0);
}

void AudioServiceTests::service_disabledByDefault()
{
  AudioService service;
  QSignalSpy errors(&service, &AudioService::error);
  QSignalSpy outgoing(&service, &AudioService::outbound);
  service.receiveAudio(QStringLiteral("windows"), metadata(), QByteArray(kAudioFrameBytes, '\0'));
  service.startCapture();
  QCOMPARE(errors.size(), 0);
  QCOMPARE(outgoing.size(), 0);
}

void AudioServiceTests::service_rejectsInvalidFrameBeforeOpeningOutput()
{
  AudioService service;
  service.configure({{QStringLiteral("receiveEnabled"), true}});
  QSignalSpy errors(&service, &AudioService::error);
  service.receiveAudio(QStringLiteral("windows"), metadata(), QByteArray(1, '\0'));
  QCOMPARE(errors.size(), 1);
  auto invalid = metadata();
  invalid.insert(QStringLiteral("sampleRate"), 0);
  service.receiveAudio(QStringLiteral("windows"), invalid, QByteArray(kAudioFrameBytes, '\0'));
  QCOMPARE(errors.size(), 2);
}

void AudioServiceTests::service_selectedPeerFilter()
{
  AudioService service;
  service.configure(
      {{QStringLiteral("receiveEnabled"), true}, {QStringLiteral("sourcePeer"), QStringLiteral("selected")}}
  );
  QSignalSpy errors(&service, &AudioService::error);
  service.receiveAudio(QStringLiteral("other"), {}, {});
  QCOMPARE(errors.size(), 0);
  service.peerDisconnected(QStringLiteral("other"));
  service.stopPlayback();
}

QTEST_GUILESS_MAIN(AudioServiceTests)
