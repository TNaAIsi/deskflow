/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 Chris Rizzitello <sithlord48@gmail.com>
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "base/Log.h"

#include <Carbon/Carbon.h>
#include <QTest>

class OSXClipboardTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  // Test are run in order top to bottom
  void initTestCase();
  void cleanupTestCase();
  void open();
  void singleFormat();
  void formatConvert_UTF8();
  void readsLatestCopyBeforePolling();
  void readsPngScreenshot();
  void incomingCopyDoesNotEcho();

private:
  Log m_log;
  CFStringRef m_pasteboardName = nullptr;
  const std::string m_testString = "deskflow test string";
};
