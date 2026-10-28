/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once
#include "ServerProxy1_9.h"
#include "common/PhysicalKey.h"
#include <QString>
#include <functional>
#include <map>
#include <optional>
#include <vector>

class ServerProxy1_10 : public ServerProxy1_9
{
public:
  struct Backend
  {
    std::function<bool()> ready;
    std::function<bool(uint16_t, uint16_t, bool)> post;
    std::function<void()> release;
    std::function<void()> releaseLegacy;
    std::function<void(const QString &)> notify;
  };
  ServerProxy1_10(
      Client *client, deskflow::IStream *stream, IEventQueue *events, Backend backend = {},
      std::optional<bool> requested = {}
  );
  ~ServerProxy1_10() override;

protected:
  ConnectionResult parseHandshakeMessage(const uint8_t *code) override;
  ConnectionResult parseMessage(const uint8_t *code) override;

private:
  void receiveCapability();
  void receiveSession();
  void receivePhysicalKey();
  void receiveSyntheticKey();
  void physicalKey(deskflow::physicalkey::Usage usage, bool down);
  void holdKey(uint32_t source, deskflow::physicalkey::Usage target, bool down);
  deskflow::physicalkey::Usage mapModifier(deskflow::physicalkey::Usage usage) const;
  void release();
  Backend m_backend;
  std::map<uint32_t, deskflow::physicalkey::Usage> m_pressed;
  std::map<KeyID, std::vector<uint32_t>> m_syntheticGroups;
  std::map<uint16_t, size_t> m_targetCounts;
  bool m_requested = false;
  bool m_sourceCapable = false;
  bool m_receiverReady = false;
  bool m_nativeActive = false;
};
