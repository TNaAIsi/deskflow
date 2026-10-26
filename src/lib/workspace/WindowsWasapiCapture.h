/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QByteArray>
#include <QThread>
#include <QVector>

#include <memory>

namespace deskflow::workspace {

struct CapturedAudioFrame
{
  QByteArray pcm;
  QString streamId;
  quint32 sequence = 0;
  qint64 timestampUs = 0;
};

// Native capture lives on its own COM thread. Only one queued notification may
// be outstanding, and at most four PCM frames wait for the application thread.
class WindowsWasapiCapture : public QThread
{
  Q_OBJECT

public:
  explicit WindowsWasapiCapture(QObject *parent = nullptr);
  ~WindowsWasapiCapture() override;
  void requestStop();
  QVector<CapturedAudioFrame> takeFrames();

Q_SIGNALS:
  void audioAvailable();
  void statusChanged(const QString &message);
  void error(const QString &message);

protected:
  void run() override;

private:
  void publish(CapturedAudioFrame frame);
  struct Private;
  std::unique_ptr<Private> m_private;
};

} // namespace deskflow::workspace
