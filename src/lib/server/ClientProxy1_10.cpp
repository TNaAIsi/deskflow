/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "ClientProxy1_10.h"
#include "Server.h"
#include "base/Log.h"
#include "common/PhysicalKey.h"
#include "common/PhysicalKeyActions.h"
#include "deskflow/DeskflowException.h"
#include "deskflow/ProtocolTypes.h"
#include "deskflow/ProtocolUtil.h"
#include "platform/PhysicalKeyboardCapture.h"
#include <cstring>
#include <utility>

namespace {
#ifdef _WIN32
constexpr uint8_t kPhysicalSourceFormat = 1;
#else
constexpr uint8_t kPhysicalSourceFormat = 0;
#endif
} // namespace

ClientProxy1_10::ClientProxy1_10(
    const std::string &name, deskflow::IStream *stream, Server *server, IEventQueue *events, Source source
)
    : ClientProxy1_9(name, stream, server, events, false),
      m_source(std::move(source))
{
  m_sourceAvailable = m_source.available.value_or(kPhysicalSourceFormat == 1);
  if (!m_source.modifiers)
    m_source.modifiers = deskflow::physicalKeyboardModifierSnapshot;
  if (!m_source.readinessChanged)
    m_source.readinessChanged = [server] { server->refreshPhysicalKeyboardCapture(); };
  ProtocolUtil::writef(getStream(), kMsgNativeKeyboardCapability, m_sourceAvailable ? 1 : 0);
}

bool ClientProxy1_10::parseHandshakeMessage(const uint8_t *code)
{
  if (memcmp(code, kMsgNativeKeyboardReady, 4) == 0)
    return receiveReady();
  return ClientProxy1_9::parseHandshakeMessage(code);
}

bool ClientProxy1_10::parseMessage(const uint8_t *code)
{
  if (memcmp(code, kMsgNativeKeyboardReady, 4) == 0)
    return receiveReady();
  return ClientProxy1_9::parseMessage(code);
}

bool ClientProxy1_10::receiveReady()
{
  uint8_t ready = 0;
  if (!ProtocolUtil::readf(getStream(), kMsgNativeKeyboardReady + 4, &ready) || ready > 1)
    throw BadClientException("invalid physical keyboard readiness");
  const bool enabled = ready == 1 && m_sourceAvailable;
  // Native input belongs to the Mac's own layout/IME. Delay the legacy source
  // layout metadata until negotiation actually selects compatibility input.
  const bool changed = m_nativeReady != enabled;
  m_nativeReady = enabled;
  if (changed && m_entered) {
    startSession();
    m_source.readinessChanged();
  }
  if (!enabled && !m_languagesSent) {
    synchronizeLanguages();
    m_languagesSent = true;
  }
  return true;
}

void ClientProxy1_10::startSession()
{
  m_pressed.clear();
  m_nativeActive = usesPhysicalKeyboard();
  const auto modifiers = m_nativeActive ? m_source.modifiers() : uint8_t(0);
  ProtocolUtil::writef(getStream(), kMsgNativeKeyboardSession, m_nativeActive ? 1 : 0, modifiers);
}

void ClientProxy1_10::enter(int32_t x, int32_t y, uint32_t sequence, KeyModifierMask mask, bool screensaver)
{
  m_entered = true;
  startSession();
  ClientProxy1_9::enter(x, y, sequence, mask, screensaver);
}

void ClientProxy1_10::setPhysicalKeyboardAllowed(bool allowed)
{
  if (m_nativeAllowed == allowed)
    return;
  m_nativeAllowed = allowed;
  if (m_entered) {
    startSession();
    m_source.readinessChanged();
  }
  if (!allowed && !m_languagesSent) {
    synchronizeLanguages();
    m_languagesSent = true;
  }
}

bool ClientProxy1_10::leave()
{
  m_entered = false;
  if (m_nativeActive)
    ProtocolUtil::writef(getStream(), kMsgNativeKeyboardSession, 0, 0);
  m_nativeActive = false;
  m_pressed.clear();
  return ClientProxy1_9::leave();
}

void ClientProxy1_10::sendPhysical(KeyButton button, bool down)
{
  const auto usage = deskflow::physicalkey::decode(button);
  if (!usage) {
    // Synthetic hotkey actions have no physical origin. Do not mix legacy
    // injection with a live virtual keyboard or guess a layout-dependent key.
    LOG_WARN("ignoring unmapped synthetic key in physical keyboard mode: %04x", button);
    return;
  }
  if (down) {
    if (!m_pressed.insert(button).second)
      return;
  } else {
    // Modifiers can be initially held across a boundary; their first packet
    // after entry may therefore be a release rather than a down.
    m_pressed.erase(button);
  }
  ProtocolUtil::writef(getStream(), kMsgNativePhysicalKey, usage.page, usage.id, down ? 1 : 0);
}

void ClientProxy1_10::keyDown(KeyID key, KeyModifierMask mask, KeyButton button, const std::string &language)
{
  if (m_nativeActive) {
    if (deskflow::physicalkey::decode(button))
      sendPhysical(button, true);
    else
      sendSynthetic(key, mask, true);
  } else
    ClientProxy1_9::keyDown(key, mask, button, language);
}

void ClientProxy1_10::keyRepeat(
    KeyID key, KeyModifierMask mask, int32_t count, KeyButton button, const std::string &language
)
{
  // macOS repeats a held virtual device key using its own repeat settings.
  if (!m_nativeActive)
    ClientProxy1_9::keyRepeat(key, mask, count, button, language);
}

void ClientProxy1_10::keyUp(KeyID key, KeyModifierMask mask, KeyButton button)
{
  if (m_nativeActive) {
    if (deskflow::physicalkey::decode(button))
      sendPhysical(button, false);
    else
      sendSynthetic(key, mask, false);
  } else
    ClientProxy1_9::keyUp(key, mask, button);
}

void ClientProxy1_10::sendSynthetic(KeyID key, KeyModifierMask mask, bool down)
{
  if (!deskflow::physicalkey::actionKey(key) && key != kKeySetModifiers && key != kKeyClearModifiers) {
    LOG_WARN("no physical keyboard action for synthetic key %08x", key);
    return;
  }
  ProtocolUtil::writef(
      getStream(), kMsgNativeSyntheticKey, key, mask & deskflow::physicalkey::kActionModifiers, down ? 1 : 0
  );
}
