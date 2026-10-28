/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once
#include "ClientProxy1_9.h"
#include <functional>
#include <optional>
#include <set>

class ClientProxy1_10 : public ClientProxy1_9
{
public:
  struct Source
  {
    std::optional<bool> available;
    std::function<uint8_t()> modifiers;
    std::function<void()> readinessChanged;
  };
  ClientProxy1_10(
      const std::string &name, deskflow::IStream *stream, Server *server, IEventQueue *events, Source source = {}
  );
  bool usesPhysicalKeyboard() const override
  {
    return m_nativeReady && m_nativeAllowed;
  }
  void setPhysicalKeyboardAllowed(bool allowed) override;
  void enter(int32_t x, int32_t y, uint32_t sequence, KeyModifierMask mask, bool screensaver) override;
  bool leave() override;
  void keyDown(KeyID, KeyModifierMask, KeyButton, const std::string &) override;
  void keyRepeat(KeyID, KeyModifierMask, int32_t, KeyButton, const std::string &) override;
  void keyUp(KeyID, KeyModifierMask, KeyButton) override;

protected:
  bool parseHandshakeMessage(const uint8_t *code) override;
  bool parseMessage(const uint8_t *code) override;

private:
  bool receiveReady();
  void startSession();
  void sendPhysical(KeyButton button, bool down);
  void sendSynthetic(KeyID key, KeyModifierMask mask, bool down);
  Source m_source;
  bool m_sourceAvailable = false;
  std::set<KeyButton> m_pressed;
  bool m_nativeReady = false;
  bool m_nativeActive = false;
  bool m_entered = false;
  bool m_languagesSent = false;
  bool m_nativeAllowed = true;
};
