/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "client/ServerProxy1_9.h"

#include "base/Log.h"
#include "deskflow/ProtocolTypes.h"
#include "deskflow/ProtocolUtil.h"
#include "io/IStream.h"

#include <QAudioFormat>
#include <QAudioSink>
#include <QIODevice>
#include <QMediaDevices>

#include <cstring>

ServerProxy1_9::ServerProxy1_9(Client *client, deskflow::IStream *stream, IEventQueue *events)
    : ServerProxy1_8(client, stream, events)
{
  // Audio playback is started when the server sends ACFG
}

ServerProxy1_9::~ServerProxy1_9()
{
  stopPlayback();
}

ServerProxy::ConnectionResult ServerProxy1_9::parseMessage(const uint8_t *code)
{
  if (memcmp(code, kMsgDAudioConfig, 4) == 0) {
    handleAudioConfig();
    return ConnectionResult::Okay;
  }
  if (memcmp(code, kMsgDAudioData, 4) == 0) {
    handleAudioData();
    return ConnectionResult::Okay;
  }
  return ServerProxy1_8::parseMessage(code);
}

void ServerProxy1_9::handleAudioConfig()
{
  uint32_t enable = 0;
  uint32_t sampleRate = 0;
  uint32_t channels = 0;
  uint32_t bitDepth = 0;
  std::string codec;

  ProtocolUtil::readf(getStream(), kMsgDAudioConfig + 4, &enable, &sampleRate, &channels, &bitDepth, &codec);

  LOG_INFO("audio config: enable=%u rate=%u ch=%u bits=%u codec=%s", enable, sampleRate, channels, bitDepth, codec.c_str());

  if (enable) {
    startPlayback(static_cast<int>(sampleRate), static_cast<int>(channels), static_cast<int>(bitDepth));
    // Signal readiness to receive audio
    ProtocolUtil::writef(getStream(), kMsgDAudioRTS, 1);
  } else {
    stopPlayback();
  }
}

void ServerProxy1_9::handleAudioData()
{
  uint32_t seq = 0;
  std::string payload;

  ProtocolUtil::readf(getStream(), kMsgDAudioData + 4, &seq, &payload);

  if (!m_audioActive || m_audioDevice == nullptr) {
    return;
  }

  // Phase 1: write directly to QAudioSink's internal QIODevice.
  // Jitter buffer will be added in Phase 3.
  if (seq != m_expectedSequence) {
    LOG_VERBOSE("audio gap: expected %u got %u", m_expectedSequence, seq);
    m_expectedSequence = seq;
  }
  ++m_expectedSequence;

  m_audioDevice->write(payload.data(), payload.size());
}

void ServerProxy1_9::startPlayback(int sampleRate, int channels, int bitDepth)
{
  if (m_audioActive) {
    stopPlayback();
  }

  QAudioFormat format;
  format.setSampleRate(sampleRate);
  format.setChannelCount(channels);
  if (bitDepth == 16) {
    format.setSampleFormat(QAudioFormat::Int16);
  } else {
    LOG_ERR("unsupported audio bit depth: %d", bitDepth);
    return;
  }

  QAudioDevice device = QMediaDevices::defaultAudioOutput();
  if (!device.isFormatSupported(format)) {
    LOG_WARN("audio format not supported by default output, trying nearest");
    format = device.preferredFormat();
  }

  m_audioSink = new QAudioSink(device, format);

  // Set a low buffer (~80ms) to reduce latency.
  // Qt's default is 250ms which feels sluggish for real-time audio.
  const int bufferBytes = sampleRate * channels * (bitDepth / 8) * 80 / 1000;
  m_audioSink->setBufferSize(bufferBytes);

  m_audioDevice = m_audioSink->start();
  if (m_audioDevice == nullptr) {
    LOG_ERR("failed to start audio playback, error code: %d", static_cast<int>(m_audioSink->error()));
    delete m_audioSink;
    m_audioSink = nullptr;
    return;
  }

  m_audioActive = true;
  m_expectedSequence = 0;
  LOG_INFO("audio playback started: %dHz %dch %dbit", sampleRate, channels, bitDepth);
}

void ServerProxy1_9::stopPlayback()
{
  if (!m_audioActive && m_audioSink == nullptr) {
    return;
  }

  LOG_INFO("stopping audio playback");
  m_audioActive = false;

  if (m_audioSink != nullptr) {
    m_audioSink->stop();
    delete m_audioSink;
    m_audioSink = nullptr;
    m_audioDevice = nullptr;
  }
}
