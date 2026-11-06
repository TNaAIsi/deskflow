/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 Chris Rizzitello <sithlord48@gmail.com>
 * SPDX-FileCopyrightText: (C) 2012 - 2016 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2011 Nick Bolton
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "OSXClipboardTests.h"

#include "platform/OSXClipboard.h"
#include "platform/OSXClipboardBMPConverter.h"
#include "platform/OSXClipboardUTF8Converter.h"

#include <QBuffer>
#include <QImage>

void OSXClipboardTests::initTestCase()
{
  // Private named pasteboard: tests never replace the user's copied data.
  m_pasteboardName = CFStringCreateWithFormat(
      kCFAllocatorDefault, nullptr, CFSTR("org.deskflow.tests.clipboard.%lld"), QCoreApplication::applicationPid()
  );
  QVERIFY(m_pasteboardName != nullptr);
}

void OSXClipboardTests::cleanupTestCase()
{
  CFRelease(m_pasteboardName);
}

void OSXClipboardTests::open()
{
  OSXClipboard clipboard(m_pasteboardName);
  QVERIFY(clipboard.open(0));
  QVERIFY(clipboard.empty());
  clipboard.close();
}

void OSXClipboardTests::singleFormat()
{
  using enum IClipboard::Format;

  OSXClipboard clipboard(m_pasteboardName);
  QVERIFY(clipboard.empty());
  clipboard.add(Text, m_testString);
  QVERIFY(clipboard.has(Text));
  QCOMPARE(clipboard.get(Text), m_testString);
}

void OSXClipboardTests::formatConvert_UTF8()
{
  OSXClipboardUTF8Converter converter;
  QCOMPARE(IClipboard::Format::Text, converter.getFormat());
  QCOMPARE(converter.getOSXFormat(), CFSTR("public.utf8-plain-text"));
  QCOMPARE(converter.fromIClipboard("test data\n"), "test data\r");
  QCOMPARE(converter.toIClipboard("test data\r"), "test data\n");
}

void OSXClipboardTests::readsLatestCopyBeforePolling()
{
  OSXClipboard writer(m_pasteboardName);
  QVERIFY(writer.open(0));
  QVERIFY(writer.empty());
  writer.add(IClipboard::Format::Text, "previous copy");
  writer.close();
  OSXClipboard reader(m_pasteboardName);
  QVERIFY(writer.open(0));
  QVERIFY(writer.empty());
  writer.add(IClipboard::Format::Text, "newly copied text");
  writer.close();

  QVERIFY(reader.open(0));
  QVERIFY(reader.has(IClipboard::Format::Text));
  QCOMPARE(reader.get(IClipboard::Format::Text), std::string("newly copied text"));
  reader.close();
  // Reading must not consume the change notification needed for sending.
  QVERIFY(reader.synchronize());
  QVERIFY(!reader.synchronize());
}

void OSXClipboardTests::readsPngScreenshot()
{
  OSXClipboard reader(m_pasteboardName);
  QImage image(3, 2, QImage::Format_RGB32);
  image.fill(QColor(32, 96, 192));
  QByteArray png;
  QBuffer buffer(&png);
  QVERIFY(buffer.open(QIODevice::WriteOnly));
  QVERIFY(image.save(&buffer, "PNG"));
  PasteboardRef writer = nullptr;
  QCOMPARE(PasteboardCreate(m_pasteboardName, &writer), noErr);
  QCOMPARE(PasteboardClear(writer), noErr);
  CFDataRef data = CFDataCreate(kCFAllocatorDefault, reinterpret_cast<const UInt8 *>(png.constData()), png.size());
  const auto status = PasteboardPutItemFlavor(
      writer, reinterpret_cast<PasteboardItemID>(1), CFSTR("public.png"), data, kPasteboardFlavorNoFlags
  );
  CFRelease(data);
  CFRelease(writer);
  QCOMPARE(status, noErr);

  QVERIFY(reader.open(0));
  QVERIFY(reader.has(IClipboard::Format::Bitmap));
  const auto dib = reader.get(IClipboard::Format::Bitmap);
  QVERIFY(!dib.empty());
  const auto bmp = OSXClipboardBMPConverter().fromIClipboard(dib);
  const auto actual = QImage::fromData(reinterpret_cast<const uchar *>(bmp.data()), int(bmp.size()), "BMP");
  QCOMPARE(actual.size(), image.size());
  QCOMPARE(actual.pixelColor(0, 0), image.pixelColor(0, 0));
  reader.close();
}

void OSXClipboardTests::incomingCopyDoesNotEcho()
{
  OSXClipboard reader(m_pasteboardName);
  OSXClipboard writer(m_pasteboardName);
  QVERIFY(writer.open(0));
  QVERIFY(writer.empty());
  writer.add(IClipboard::Format::Text, "previous local copy");
  writer.close();
  QVERIFY(reader.open(0));
  QVERIFY(reader.empty());
  reader.add(IClipboard::Format::Text, "received remote copy");
  reader.close();
  QVERIFY(!reader.synchronize());
}

QTEST_MAIN(OSXClipboardTests)
