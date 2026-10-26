/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "NativeClipboardFilesTests.h"

#include "workspace/ClipboardFiles.h"

#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QMimeData>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>
#include <QUuid>
#include <QVariant>

#include <memory>

using deskflow::workspace::nativeClipboardFiles;
using deskflow::workspace::nativeClipboardRevision;
using deskflow::workspace::publishNativeClipboardFiles;

namespace {

// This is deliberately scoped to each opt-in test so assertions and early
// returns still restore every format exposed by the previous Qt clipboard.
class ClipboardRestore final
{
public:
  explicit ClipboardRestore(QClipboard *clipboard) : m_clipboard(clipboard), m_saved(std::make_unique<QMimeData>())
  {
    if (const auto *previous = m_clipboard->mimeData(QClipboard::Clipboard)) {
      for (const auto &format : previous->formats()) {
        m_saved->setData(format, previous->data(format));
      }
      // Qt's typed image/color values do not necessarily serialize through
      // data(format), so preserve these representations explicitly too.
      if (previous->hasImage()) {
        m_saved->setImageData(previous->imageData());
      }
      if (previous->hasColor()) {
        m_saved->setColorData(previous->colorData());
      }
      if (previous->hasUrls()) {
        m_saved->setUrls(previous->urls());
      }
    }
  }

  ClipboardRestore(const ClipboardRestore &) = delete;
  ClipboardRestore &operator=(const ClipboardRestore &) = delete;

  ~ClipboardRestore()
  {
    m_clipboard->setMimeData(m_saved.release(), QClipboard::Clipboard);
  }

private:
  QClipboard *m_clipboard;
  std::unique_ptr<QMimeData> m_saved;
};

bool writeFile(const QString &path)
{
  QFile file(path);
  const QByteArray contents("native clipboard test\n");
  return file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size();
}

QStringList canonicalPaths(const QStringList &paths)
{
  QStringList canonical;
  for (const auto &path : paths) {
    canonical.append(QFileInfo(path).canonicalFilePath());
  }
  canonical.sort();
  return canonical;
}

QString generation()
{
  return QStringLiteral("native-%1-\u4ee3\u6b21").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
}

} // namespace

void NativeClipboardFilesTests::initTestCase()
{
  if (qEnvironmentVariable("DESKFLOW_NATIVE_CLIPBOARD_TESTS") != QStringLiteral("1")) {
    QSKIP("Native clipboard tests require DESKFLOW_NATIVE_CLIPBOARD_TESTS=1 on an isolated CI desktop.");
  }
#if !defined(Q_OS_WIN) && !defined(Q_OS_MACOS)
  QSKIP("Native file clipboard publication is supported by the Windows and macOS adapters.");
#endif
  const auto platform = QGuiApplication::platformName();
  QVERIFY2(
      platform != QStringLiteral("offscreen") && platform != QStringLiteral("minimal"),
      "Native clipboard tests must use the actual Windows or macOS Qt platform plugin."
  );
  QVERIFY(QApplication::clipboard());
}

void NativeClipboardFilesTests::nativePublicationRetainsFilesAndGeneration()
{
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const auto file = directory.filePath(QStringLiteral("design \u8bbe\u8ba1 file.txt"));
  const auto folder = directory.filePath(QStringLiteral("shared \u76ee\u5f55 folder"));
  QVERIFY(writeFile(file));
  QVERIFY(QDir().mkpath(folder));
  const QStringList paths{file, folder};
  const auto marker = generation();
  auto *clipboard = QApplication::clipboard();
  ClipboardRestore restore(clipboard);
  const auto previousRevision = nativeClipboardRevision();

  QVERIFY(publishNativeClipboardFiles(paths, marker));
  QTRY_COMPARE_WITH_TIMEOUT(nativeClipboardFiles().generation, marker, 3000);
  const auto native = nativeClipboardFiles();
  QVERIFY(native.available);
  QVERIFY(native.revision != previousRevision);
  QCOMPARE(native.revision, nativeClipboardRevision());
  QCOMPARE(canonicalPaths(native.paths), canonicalPaths(paths));

  // Qt is a separate consumer of the actual native file formats written by
  // the adapter; its URL view must agree with the native reader.
  QTRY_VERIFY_WITH_TIMEOUT(
      clipboard->mimeData(QClipboard::Clipboard) && clipboard->mimeData(QClipboard::Clipboard)->hasUrls(), 3000
  );
  QStringList qtPaths;
  for (const auto &url : clipboard->mimeData(QClipboard::Clipboard)->urls()) {
    QVERIFY(url.isLocalFile());
    qtPaths.append(url.toLocalFile());
  }
  QCOMPARE(canonicalPaths(qtPaths), canonicalPaths(paths));
}

void NativeClipboardFilesTests::plainTextClearsNativeFiles()
{
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const auto file = directory.filePath(QStringLiteral("old \u6587\u4ef6 selection.txt"));
  QVERIFY(writeFile(file));
  auto *clipboard = QApplication::clipboard();
  ClipboardRestore restore(clipboard);
  QVERIFY(publishNativeClipboardFiles({file}, generation()));
  QTRY_VERIFY_WITH_TIMEOUT(!nativeClipboardFiles().paths.isEmpty(), 3000);
  const auto filesRevision = nativeClipboardRevision();

  const auto text = QStringLiteral("a new plain text clipboard value");
  clipboard->setText(text, QClipboard::Clipboard);
  QTRY_VERIFY_WITH_TIMEOUT(
      nativeClipboardFiles().available && nativeClipboardFiles().paths.isEmpty() &&
          nativeClipboardFiles().generation.isEmpty(),
      3000
  );
  QVERIFY(nativeClipboardRevision() != filesRevision);
  QCOMPARE(clipboard->text(QClipboard::Clipboard), text);
}

void NativeClipboardFilesTests::textFileUrlIsNotAFileOffer()
{
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const auto file = directory.filePath(QStringLiteral("private \u6587\u6863 file.txt"));
  QVERIFY(writeFile(file));
  auto *clipboard = QApplication::clipboard();
  ClipboardRestore restore(clipboard);
  QVERIFY(publishNativeClipboardFiles({file}, generation()));
  QTRY_VERIFY_WITH_TIMEOUT(!nativeClipboardFiles().paths.isEmpty(), 3000);

  // The URL refers to a real file: interpreting this text as a file offer
  // would expose its contents without an explicit native file selection.
  const auto text = QUrl::fromLocalFile(file).toString(QUrl::FullyEncoded);
  QVERIFY(text.startsWith(QStringLiteral("file:")));
  clipboard->setText(text, QClipboard::Clipboard);
  QTRY_VERIFY_WITH_TIMEOUT(
      nativeClipboardFiles().available && nativeClipboardFiles().paths.isEmpty() &&
          nativeClipboardFiles().generation.isEmpty(),
      3000
  );
  QVERIFY(!clipboard->mimeData(QClipboard::Clipboard)->hasUrls());
  QCOMPARE(clipboard->text(QClipboard::Clipboard), text);
}

void NativeClipboardFilesTests::staleRevisionPreservesNewLocalText()
{
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const auto file = directory.filePath(QStringLiteral("remote completed file.txt"));
  QVERIFY(writeFile(file));
  auto *clipboard = QApplication::clipboard();
  ClipboardRestore restore(clipboard);
  clipboard->setText(QStringLiteral("previous clipboard value"), QClipboard::Clipboard);
  const auto expectedRevision = nativeClipboardRevision();

  const auto newerText = QStringLiteral("a newer local copy must survive");
  clipboard->setText(newerText, QClipboard::Clipboard);
  QTRY_VERIFY_WITH_TIMEOUT(nativeClipboardRevision() != expectedRevision, 3000);
  QVERIFY(!publishNativeClipboardFiles({file}, generation(), expectedRevision));
  QCOMPARE(clipboard->text(QClipboard::Clipboard), newerText);
  const auto native = nativeClipboardFiles();
  QVERIFY(native.available);
  QVERIFY(native.paths.isEmpty());
  QVERIFY(native.generation.isEmpty());
}

QTEST_MAIN(NativeClipboardFilesTests)
