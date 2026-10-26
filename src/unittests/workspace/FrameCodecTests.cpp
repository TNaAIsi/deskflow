/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "workspace/FileClipboardState.h"
#include "workspace/FrameCodec.h"
#include <QTest>
#include <QtEndian>

using namespace deskflow::workspace;

class FrameCodecTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void fragmentedAndCoalescedFrames()
  {
    const QByteArray payload(65536, '\xff');
    const auto first = FrameCodec::encode({{"type", "files.chunk"}, {"path", "中文/zero.bin"}}, payload);
    const auto second = FrameCodec::encode({{"type", "ping"}});
    FrameCodec codec;
    QList<WorkspaceFrame> frames;
    for (qsizetype offset = 0; offset < first.size(); ++offset)
      QVERIFY(codec.append(first.mid(offset, 1), frames));
    QCOMPARE(frames.size(), 1);
    QCOMPARE(frames.first().payload, payload);
    QCOMPARE(frames.first().metadata.value("path").toString(), QString("中文/zero.bin"));
    frames.clear();
    QVERIFY(codec.append(second + first + second, frames));
    QCOMPARE(frames.size(), 3);
    QCOMPARE(frames.at(1).payload, payload);
  }
  void rejectsLengthsBeforePayloadAllocation()
  {
    for (quint32 size : {quint32(0), quint32(5), quint32(FrameCodec::MaxFrame + 1), quint32(0xffffffff)}) {
      FrameCodec codec;
      QByteArray header(4, '\0');
      qToBigEndian(size, header.data());
      QList<WorkspaceFrame> frames;
      QVERIFY(!codec.append(header, frames));
      QVERIFY(codec.failed());
      QVERIFY(frames.isEmpty());
    }
  }
  void rejectsMalformedJsonAndMetadataBounds()
  {
    const auto valid = FrameCodec::encode({{"type", "x"}});
    for (const auto mutation : {0, 1}) {
      auto bytes = valid;
      if (mutation == 0)
        bytes[8] = '[';
      else
        qToBigEndian<quint32>(FrameCodec::MaxMetadata + 1, bytes.data() + 4);
      FrameCodec codec;
      QList<WorkspaceFrame> frames;
      QVERIFY(!codec.append(bytes, frames));
      QVERIFY(frames.isEmpty());
    }
  }
  void rejectsOversizeOutbound()
  {
    QVERIFY(FrameCodec::encode({{"type", "x"}}, QByteArray(FrameCodec::MaxPayload + 1, '\0')).isEmpty());
    QVERIFY(FrameCodec::encode({{"value", QString(FrameCodec::MaxMetadata + 1, 'a')}}).isEmpty());
  }
  void preservesFileClipboardUntilRealTextArrives()
  {
    g_fileClipboardActive.store(true);
    QVERIFY(preserveFileClipboard(false));
    QVERIFY(!preserveFileClipboard(true));
    g_fileClipboardActive.store(false);
    QVERIFY(!preserveFileClipboard(false));
  }
};
QTEST_GUILESS_MAIN(FrameCodecTests)
#include "FrameCodecTests.moc"
