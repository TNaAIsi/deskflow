/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once

#include <QObject>
#include <QTemporaryDir>

class WorkspaceTransportTests : public QObject
{
  Q_OBJECT

private Q_SLOTS:
  void initTestCase();
  void threeChannelsAndPayloadRoundTrip();
  void fileEngineTransfersNestedBinaryOverTls();
  void fileBackpressureLeavesControlAndAudioWritable();
  void largeManifestAcceptedOnEmptyFileChannel();
  void untrustedCertificateRejected_data();
  void untrustedCertificateRejected();
  void auxiliaryChannelRequiresControlToken();
  void unsolicitedAuxiliaryChannelIsRejected();
  void auxiliaryCertificateMustMatchControl();
  void wrongChannelMessageClosesOnlyThatChannel();
  void controlDisconnectClosesAuxiliaries();
  void expiredPinnedCertificateIsRejected();

private:
  QTemporaryDir m_directory;
  QString m_certificate;
  QString m_secondCertificate;
  QString m_expiredCertificate;
  QString m_trusted;
  QString m_trustBoth;
  QString m_untrusted;
  QString m_expiredTrusted;
};
