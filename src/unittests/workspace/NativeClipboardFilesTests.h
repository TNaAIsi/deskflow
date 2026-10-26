/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QObject>

class NativeClipboardFilesTests : public QObject
{
  Q_OBJECT

private Q_SLOTS:
  void initTestCase();
  void nativePublicationRetainsFilesAndGeneration();
  void plainTextClearsNativeFiles();
  void textFileUrlIsNotAFileOffer();
  void staleRevisionPreservesNewLocalText();
};
