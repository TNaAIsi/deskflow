/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QMutex>
#include <QVector>

#include <array>

namespace deskflow::workspace {

inline constexpr int kAudioSampleRate = 48000;
inline constexpr int kAudioChannels = 2;
inline constexpr int kAudioFrameSamples = 960;
inline constexpr int kAudioSampleBytes = 4;
inline constexpr int kAudioFrameBytes = kAudioFrameSamples * kAudioSampleBytes;

enum class PcmSampleType
{
  Signed16,
  Signed24,
  Signed32,
  Float32,
  Float64
};

struct PcmFormat
{
  int sampleRate = kAudioSampleRate;
  int channels = kAudioChannels;
  PcmSampleType sampleType = PcmSampleType::Signed16;
  quint32 channelMask = 0;

  int bytesPerSample() const;
  bool isValid() const;
};

// Stateful conversion accepts arbitrary packet boundaries. Integer PCM is
// little-endian; output is always interleaved stereo signed 16-bit at 48 kHz.
class PcmConverter
{
public:
  explicit PcmConverter(const PcmFormat &format = {});
  QByteArray convert(QByteArrayView input, bool silent = false, qsizetype silentFrames = 0);
  void reset(const PcmFormat &format);
  void setOutputRateRatio(double ratio);

private:
  float sample(const char *bytes) const;
  std::array<float, 2> stereoFrame(const char *bytes) const;

  PcmFormat m_format;
  QByteArray m_partial;
  QVector<std::array<float, 2>> m_samples;
  double m_position = 0.0;
  double m_rateRatio = 1.0;
};

// Independent capture/playback clocks slowly fill or drain any fixed buffer.
// Correct at most 0.1 percent using a smoothed occupancy controller, preserving
// interpolation state across packets instead of periodically dropping frames.
class PcmClockBridge
{
public:
  QByteArray convert(QByteArrayView input, qsizetype queuedSampleFrames);
  void reset();

private:
  PcmConverter m_converter;
  double m_smoothedQueue = kAudioFrameSamples;
};

// A bounded producer/consumer buffer. Overflow discards oldest complete PCM
// samples so a stalled connection cannot accumulate seconds of stale audio.
class PcmRingBuffer
{
public:
  explicit PcmRingBuffer(qsizetype capacity = 6 * kAudioFrameBytes);
  bool append(QByteArrayView pcm);
  qsizetype read(char *destination, qsizetype maximum);
  void clear();
  qsizetype queuedBytes() const;
  quint64 droppedBytes() const;

private:
  mutable QMutex m_mutex;
  QByteArray m_storage;
  qsizetype m_read = 0;
  qsizetype m_write = 0;
  qsizetype m_size = 0;
  quint64 m_dropped = 0;
};

} // namespace deskflow::workspace
