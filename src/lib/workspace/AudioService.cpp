/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "AudioService.h"

#include "AudioPcm.h"
#include "WindowsWasapiCapture.h"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QElapsedTimer>
#include <QIODevice>
#include <QMediaDevices>
#include <QPointer>
#include <QTimer>
#include <QtEndian>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

namespace deskflow::workspace {

namespace {

class PcmPullDevice : public QIODevice
{
public:
  explicit PcmPullDevice(QObject *parent) : QIODevice(parent)
  {
    open(QIODevice::ReadOnly | QIODevice::Unbuffered);
  }

  bool isSequential() const override
  {
    return true;
  }

  qint64 bytesAvailable() const override
  {
    // Keep a quiet stream alive during capture silence or a network underrun.
    // This is a source, so reporting one period never allocates a queue.
    return std::max<qint64>(ring.queuedBytes(), kAudioFrameBytes) + QIODevice::bytesAvailable();
  }

  void append(const QByteArray &pcm)
  {
    if (ring.append(pcm)) {
      Q_EMIT readyRead();
    }
  }

  PcmRingBuffer ring;

protected:
  qint64 readData(char *destination, qint64 maximum) override
  {
    const auto count = ring.read(destination, maximum);
    if (count < maximum) {
      std::memset(destination + count, 0, maximum - count);
    }
    return maximum;
  }

  qint64 writeData(const char *, qint64) override
  {
    return -1;
  }
};

QAudioFormat outputFormat(QAudioFormat::SampleFormat sampleFormat)
{
  QAudioFormat format;
  format.setSampleRate(kAudioSampleRate);
  format.setChannelCount(kAudioChannels);
  format.setSampleFormat(sampleFormat);
  return format;
}

QByteArray pcmToFloat(const QByteArray &pcm)
{
  QByteArray output(pcm.size() * 2, Qt::Uninitialized);
  for (qsizetype sample = 0; sample < pcm.size() / 2; ++sample) {
    const float value = qFromLittleEndian<qint16>(pcm.constData() + sample * 2) / 32768.0f;
    qToLittleEndian(std::bit_cast<quint32>(value), output.data() + sample * 4);
  }
  return output;
}

} // namespace

struct AudioService::Private
{
  bool sendEnabled = false;
  bool receiveEnabled = false;
  bool floatingOutput = false;
  QString outputDeviceId;
  QString sourcePeer;
  QString activePeer;
  QString streamId;
  quint32 lastSequence = 0;
  bool hasSequence = false;
  PcmClockBridge clockBridge;
  QElapsedTimer lastPacket;
  QElapsedTimer outputFailure;
  QPointer<WindowsWasapiCapture> capture;
  QMediaDevices *devices = nullptr;
  PcmPullDevice *input = nullptr;
  QAudioSink *sink = nullptr;
  QTimer *idleTimer = nullptr;
};

AudioService::AudioService(QObject *parent) : QObject(parent), m_private(std::make_unique<Private>())
{
  m_private->devices = new QMediaDevices(this);
  m_private->input = new PcmPullDevice(this);
  m_private->idleTimer = new QTimer(this);
  m_private->idleTimer->setInterval(1000);
  connect(m_private->idleTimer, &QTimer::timeout, this, [this] {
    if (!m_private->activePeer.isEmpty() && m_private->lastPacket.isValid() && m_private->lastPacket.elapsed() > 3000) {
      stopPlayback();
      Q_EMIT statusChanged(tr("Waiting for system audio."));
    }
  });
  connect(m_private->devices, &QMediaDevices::audioOutputsChanged, this, [this] {
    stopPlayback();
    m_private->outputFailure.invalidate();
    if (m_private->receiveEnabled) {
      Q_EMIT statusChanged(tr("Audio output changed; waiting for the next audio frame."));
    }
  });
}

AudioService::~AudioService()
{
  stopCapture();
  stopPlayback();
}

QJsonArray AudioService::outputDevices()
{
  QJsonArray outputs;
  const auto defaultId = QMediaDevices::defaultAudioOutput().id();
  const auto format = outputFormat(QAudioFormat::Int16);
  const auto floatingFormat = outputFormat(QAudioFormat::Float);
  for (const auto &device : QMediaDevices::audioOutputs()) {
    outputs.append(
        QJsonObject{
            {QStringLiteral("id"), QString::fromLatin1(device.id().toBase64())},
            {QStringLiteral("name"), device.description()},
            {QStringLiteral("isDefault"), device.id() == defaultId},
            {QStringLiteral("supportsPcm48k"),
             device.isFormatSupported(format) || device.isFormatSupported(floatingFormat)}
        }
    );
  }
  return outputs;
}

void AudioService::configure(const QJsonObject &configuration)
{
  const bool send = configuration.value(QStringLiteral("sendEnabled")).toBool(false);
  const bool receive = configuration.value(QStringLiteral("receiveEnabled")).toBool(false);
  const auto output = configuration.value(QStringLiteral("outputDeviceId")).toString();
  const auto source = configuration.value(QStringLiteral("sourcePeer")).toString();
  if (!receive || output != m_private->outputDeviceId || source != m_private->sourcePeer) {
    stopPlayback();
    m_private->outputFailure.invalidate();
  }
  m_private->outputDeviceId = output;
  m_private->sourcePeer = source;
  m_private->sendEnabled = send;
  // A Windows loopback source would capture our own received playback again.
  // Keep that digital feedback path closed while this endpoint is sending.
#ifdef Q_OS_WIN
  m_private->receiveEnabled = receive && !send;
  if (send) {
    stopPlayback();
  }
#else
  m_private->receiveEnabled = receive;
#endif
  if (m_private->receiveEnabled) {
    m_private->idleTimer->start();
  } else {
    m_private->idleTimer->stop();
  }
  if (send) {
    startCapture();
  } else {
    stopCapture();
  }
}

void AudioService::startCapture()
{
  if (!m_private->sendEnabled || m_private->capture != nullptr) {
    return;
  }
#ifdef Q_OS_WIN
  auto *capture = new WindowsWasapiCapture(this);
  m_private->capture = capture;
  connect(
      capture, &WindowsWasapiCapture::audioAvailable, this, &AudioService::capturedAudioAvailable, Qt::QueuedConnection
  );
  connect(capture, &WindowsWasapiCapture::statusChanged, this, &AudioService::statusChanged, Qt::QueuedConnection);
  connect(capture, &WindowsWasapiCapture::error, this, &AudioService::error, Qt::QueuedConnection);
  connect(capture, &QThread::finished, capture, &QObject::deleteLater);
  capture->start();
#else
  Q_EMIT error(tr("System audio sharing currently requires a Windows source computer."));
#endif
}

void AudioService::stopCapture()
{
  if (auto *capture = m_private->capture.data()) {
    disconnect(capture, nullptr, this, nullptr);
    capture->requestStop();
    capture->wait();
    delete capture;
    m_private->capture = nullptr;
    Q_EMIT statusChanged(tr("System audio sharing stopped."));
  }
}

void AudioService::capturedAudioAvailable()
{
  if (!m_private->sendEnabled || m_private->capture == nullptr) {
    return;
  }
  const auto frames = m_private->capture->takeFrames();
  for (const auto &frame : frames) {
    const QJsonObject metadata{
        {QStringLiteral("format"), QStringLiteral("pcm_s16le")},
        {QStringLiteral("sampleRate"), kAudioSampleRate},
        {QStringLiteral("channels"), kAudioChannels},
        {QStringLiteral("frames"), kAudioFrameSamples},
        {QStringLiteral("streamId"), frame.streamId},
        {QStringLiteral("sequence"), static_cast<double>(frame.sequence)},
        {QStringLiteral("timestampUs"), static_cast<double>(frame.timestampUs)}
    };
    Q_EMIT outbound(QStringLiteral("audio"), metadata, frame.pcm);
  }
}

bool AudioService::startPlayback()
{
  if (m_private->sink != nullptr) {
    return true;
  }
  if (m_private->outputFailure.isValid() && m_private->outputFailure.elapsed() < 1000) {
    return false;
  }
  auto device = QMediaDevices::defaultAudioOutput();
  if (!m_private->outputDeviceId.isEmpty()) {
    const auto requested = QByteArray::fromBase64(m_private->outputDeviceId.toLatin1());
    bool found = false;
    for (const auto &candidate : QMediaDevices::audioOutputs()) {
      if (candidate.id() == requested) {
        device = candidate;
        found = true;
        break;
      }
    }
    if (!found) {
      m_private->outputFailure.start();
      Q_EMIT error(tr("Selected audio output is disconnected. Choose another output or the system default."));
      return false;
    }
  }
  auto format = outputFormat(QAudioFormat::Int16);
  m_private->floatingOutput = false;
  if (!device.isFormatSupported(format)) {
    format = outputFormat(QAudioFormat::Float);
    m_private->floatingOutput = true;
  }
  if (device.isNull() || !device.isFormatSupported(format)) {
    m_private->outputFailure.start();
    Q_EMIT error(tr("The audio output does not support 48 kHz stereo audio."));
    return false;
  }
  auto *sink = new QAudioSink(device, format, this);
  if (m_private->floatingOutput) {
    QByteArray preroll(m_private->input->ring.queuedBytes(), Qt::Uninitialized);
    preroll.resize(m_private->input->ring.read(preroll.data(), preroll.size()));
    m_private->input->ring.clear();
    m_private->input->append(pcmToFloat(preroll));
  }
  sink->setBufferSize(kAudioFrameBytes * (m_private->floatingOutput ? 2 : 1));
  m_private->sink = sink;
  connect(sink, &QAudioSink::stateChanged, this, [this, sink](QAudio::State state) {
    if (state == QAudio::StoppedState && sink->error() != QAudio::NoError) {
      const auto code = static_cast<int>(sink->error());
      const QPointer<QAudioSink> guardedSink(sink);
      QTimer::singleShot(0, this, [this, guardedSink, code] {
        if (guardedSink != nullptr && m_private->sink == guardedSink) {
          stopPlayback();
          m_private->outputFailure.start();
          Q_EMIT error(tr("Audio output failed (%1); retrying when audio resumes.").arg(code));
        }
      });
    }
  });
  sink->start(m_private->input);
  if (sink->error() != QAudio::NoError) {
    stopPlayback();
    m_private->outputFailure.start();
    Q_EMIT error(tr("Could not start the audio output."));
    return false;
  }
  m_private->outputFailure.invalidate();
  Q_EMIT statusChanged(tr("Playing system audio from %1 on %2.").arg(m_private->activePeer, device.description()));
  return true;
}

void AudioService::stopPlayback()
{
  if (m_private->sink != nullptr) {
    auto *sink = m_private->sink;
    m_private->sink = nullptr;
    disconnect(sink, nullptr, this, nullptr);
    sink->stop();
    delete sink;
  }
  m_private->input->ring.clear();
  m_private->clockBridge.reset();
  m_private->floatingOutput = false;
  m_private->activePeer.clear();
  m_private->streamId.clear();
  m_private->hasSequence = false;
  m_private->lastPacket.invalidate();
}

void AudioService::peerDisconnected(const QString &peer)
{
  if (m_private->activePeer == peer) {
    stopPlayback();
    Q_EMIT statusChanged(tr("Audio source disconnected."));
  }
}

void AudioService::receiveAudio(const QString &peer, const QJsonObject &metadata, const QByteArray &payload)
{
  if (!m_private->receiveEnabled || peer.isEmpty() ||
      (!m_private->sourcePeer.isEmpty() && peer != m_private->sourcePeer)) {
    return;
  }
  const auto streamId = metadata.value(QStringLiteral("streamId")).toString();
  const auto sequence = metadata.value(QStringLiteral("sequence")).toDouble(-1);
  if (metadata.value(QStringLiteral("format")).toString() != QStringLiteral("pcm_s16le") ||
      metadata.value(QStringLiteral("sampleRate")).toInt() != kAudioSampleRate ||
      metadata.value(QStringLiteral("channels")).toInt() != kAudioChannels ||
      metadata.value(QStringLiteral("frames")).toInt() != kAudioFrameSamples || payload.size() != kAudioFrameBytes ||
      streamId.isEmpty() || streamId.size() > 80 || sequence < 0 || sequence > UINT32_MAX ||
      std::floor(sequence) != sequence) {
    Q_EMIT error(tr("Rejected an invalid system audio frame from %1.").arg(peer));
    return;
  }
  if (!m_private->activePeer.isEmpty() && peer != m_private->activePeer) {
    if (!m_private->lastPacket.isValid() || m_private->lastPacket.elapsed() < 3000) {
      return;
    }
    stopPlayback();
  }
  if (m_private->activePeer != peer || m_private->streamId != streamId) {
    stopPlayback();
    m_private->activePeer = peer;
    m_private->streamId = streamId;
  }
  const auto current = static_cast<quint32>(sequence);
  if (m_private->hasSequence && static_cast<qint32>(current - m_private->lastSequence) <= 0) {
    return;
  }
  if (m_private->hasSequence && current - m_private->lastSequence > 5) {
    m_private->input->ring.clear();
  }
  m_private->lastSequence = current;
  m_private->hasSequence = true;
  m_private->lastPacket.start();
  const auto adjusted = m_private->clockBridge.convert(
      payload, m_private->input->ring.queuedBytes() / (m_private->floatingOutput ? 8 : 4)
  );
  const auto pcm = m_private->floatingOutput ? pcmToFloat(adjusted) : adjusted;
  m_private->input->append(pcm);
  // Two frames of preroll absorb normal LAN scheduling jitter. Qt's own
  // requested hardware buffer is one frame; both buffers have fixed limits.
  if (m_private->sink == nullptr && m_private->input->ring.queuedBytes() >= 2 * kAudioFrameBytes) {
    startPlayback();
  }
}

} // namespace deskflow::workspace
