/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "ServerProxy1_10.h"
#include "Client.h"
#include "base/Log.h"
#include "common/PhysicalKeyActions.h"
#include "common/Settings.h"
#include "deskflow/DeskflowException.h"
#include "deskflow/ProtocolTypes.h"
#include "deskflow/ProtocolUtil.h"
#include "deskflow/ipc/CoreIpc.h"
#include "platform/NativeKeyboard.h"
#include <cstring>
#include <utility>

ServerProxy1_10::ServerProxy1_10(
    Client *client, deskflow::IStream *stream, IEventQueue *events, Backend backend, std::optional<bool> requested
)
    : ServerProxy1_9(client, stream, events),
      m_backend(std::move(backend)),
      m_requested(requested.has_value() ? *requested : Settings::value(Settings::Client::NativeKeyboard).toBool())
{
  if (!m_backend.ready)
    m_backend.ready = deskflow::nativeKeyboardReady;
  if (!m_backend.post)
    m_backend.post = deskflow::postNativePhysicalKey;
  if (!m_backend.release)
    m_backend.release = deskflow::releaseNativePhysicalKeys;
  if (!m_backend.releaseLegacy)
    m_backend.releaseLegacy = [client] { client->releaseKeyboardState(); };
  if (!m_backend.notify)
    m_backend.notify = [](const QString &state) { ipcSendToClient(QStringLiteral("nativeKeyboardMode"), state); };
}

ServerProxy1_10::~ServerProxy1_10()
{
  release();
}

void ServerProxy1_10::release()
{
  m_backend.release();
  deskflow::setNativeKeyboardSessionActive(false);
  m_pressed.clear();
  m_syntheticGroups.clear();
  m_targetCounts.clear();
  m_nativeActive = false;
}

void ServerProxy1_10::receiveCapability()
{
  uint8_t format = 0;
  if (!ProtocolUtil::readf(getStream(), kMsgNativeKeyboardCapability + 4, &format) || format > 1 || m_nativeActive) {
    release();
    m_backend.notify(QStringLiteral("failed"));
    throw BadClientException("invalid physical keyboard source capability");
  }
  m_sourceCapable = format == 1;
  m_receiverReady = m_sourceCapable && m_requested && m_backend.ready();
  ProtocolUtil::writef(getStream(), kMsgNativeKeyboardReady, m_receiverReady ? 1 : 0);
  m_backend.notify(
      !m_requested       ? QStringLiteral("compatibility")
      : !m_sourceCapable ? QStringLiteral("unsupported")
      : m_receiverReady  ? QStringLiteral("ready")
                         : QStringLiteral("unavailable")
  );
  if (m_requested && !m_receiverReady)
    LOG_WARN("physical keyboard not ready; source support or approved virtual HID components are unavailable");
}

ServerProxy::ConnectionResult ServerProxy1_10::parseHandshakeMessage(const uint8_t *code)
{
  if (memcmp(code, kMsgNativeKeyboardCapability, 4) == 0) {
    receiveCapability();
    return ConnectionResult::Okay;
  }
  return ServerProxy1_9::parseHandshakeMessage(code);
}

ServerProxy::ConnectionResult ServerProxy1_10::parseMessage(const uint8_t *code)
{
  if (memcmp(code, kMsgNativeKeyboardCapability, 4) == 0) {
    receiveCapability();
    return ConnectionResult::Okay;
  }
  if (memcmp(code, kMsgNativeKeyboardSession, 4) == 0) {
    receiveSession();
    return ConnectionResult::Okay;
  }
  if (memcmp(code, kMsgNativePhysicalKey, 4) == 0) {
    receivePhysicalKey();
    return ConnectionResult::Okay;
  }
  if (memcmp(code, kMsgNativeSyntheticKey, 4) == 0) {
    receiveSyntheticKey();
    return ConnectionResult::Okay;
  }
  if (m_nativeActive && (memcmp(code, kMsgDKeyDown, 4) == 0 || memcmp(code, kMsgDKeyDown1_1, 4) == 0 ||
                         memcmp(code, kMsgDKeyUp, 4) == 0 || memcmp(code, kMsgDKeyRepeat, 4) == 0)) {
    release();
    m_backend.notify(QStringLiteral("failed"));
    throw BadClientException("legacy key message inside physical keyboard session");
  }
  if (memcmp(code, kMsgCLeave, 4) == 0 || memcmp(code, kMsgCClose, 4) == 0)
    release();
  const auto result = ServerProxy1_9::parseMessage(code);
  if (m_nativeActive && result == ConnectionResult::Unknown) {
    release();
    m_backend.notify(QStringLiteral("failed"));
    throw BadClientException("unknown message inside physical keyboard session");
  }
  return result;
}

void ServerProxy1_10::receiveSession()
{
  uint8_t enabled = 0, modifiers = 0;
  if (!ProtocolUtil::readf(getStream(), kMsgNativeKeyboardSession + 4, &enabled, &modifiers) || enabled > 1 ||
      (enabled == 0 && modifiers != 0)) {
    release();
    m_backend.notify(QStringLiteral("failed"));
    throw BadClientException("invalid physical keyboard session");
  }
  release();
  if (enabled == 0) {
    m_backend.notify(m_receiverReady ? QStringLiteral("ready") : QStringLiteral("compatibility"));
    return;
  }
  if (!m_sourceCapable || !m_receiverReady || !m_backend.ready())
    throw BadClientException("virtual HID keyboard unavailable");
  m_backend.releaseLegacy();
  m_nativeActive = true;
  deskflow::setNativeKeyboardSessionActive(true);
  try {
    for (uint16_t bit = 0; bit < 8; ++bit)
      if ((modifiers & (1u << bit)) != 0)
        physicalKey({0x07, uint16_t(0xe0 + bit)}, true);
    m_backend.notify(QStringLiteral("physical"));
  } catch (...) {
    release();
    m_backend.notify(QStringLiteral("failed"));
    throw;
  }
}

deskflow::physicalkey::Usage ServerProxy1_10::mapModifier(deskflow::physicalkey::Usage usage) const
{
  if (usage.page != 0x07 || usage.id < 0xe0 || usage.id > 0xe7)
    return usage;
  const KeyID keys[]{kKeyControl_L, kKeyShift_L, kKeyAlt_L, kKeySuper_L,
                     kKeyControl_R, kKeyShift_R, kKeyAlt_R, kKeySuper_R};
  auto translated = translatePhysicalModifier(keys[usage.id - 0xe0]);
  if (usage.id == 0xe6) {
    const auto altGr = translatePhysicalModifier(kKeyAltGr);
    if (altGr != kKeyAltGr)
      translated = altGr;
  }
  switch (translated) {
  case kKeyControl_L:
    return {0x07, 0xe0};
  case kKeyShift_L:
    return {0x07, 0xe1};
  case kKeyAlt_L:
    return {0x07, 0xe2};
  case kKeyMeta_L:
  case kKeySuper_L:
    return {0x07, 0xe3};
  case kKeyControl_R:
    return {0x07, 0xe4};
  case kKeyShift_R:
    return {0x07, 0xe5};
  case kKeyAlt_R:
  case kKeyAltGr:
    return {0x07, 0xe6};
  case kKeyMeta_R:
  case kKeySuper_R:
    return {0x07, 0xe7};
  default:
    return {}; // User explicitly disabled this modifier.
  }
}

void ServerProxy1_10::physicalKey(deskflow::physicalkey::Usage usage, bool down)
{
  holdKey(deskflow::physicalkey::encode(usage), mapModifier(usage), down);
}

void ServerProxy1_10::holdKey(uint32_t source, deskflow::physicalkey::Usage target, bool down)
{
  if (down) {
    if (m_pressed.contains(source))
      return;
    m_pressed.emplace(source, target);
    if (!target)
      return;
    auto &count = m_targetCounts[deskflow::physicalkey::encode(target)];
    if (count++ == 0 && !m_backend.post(target.page, target.id, true))
      throw BadClientException("virtual HID keyboard rejected a key press");
  } else {
    const auto pressed = m_pressed.find(source);
    if (pressed == m_pressed.end())
      return;
    const auto target = pressed->second;
    m_pressed.erase(pressed);
    if (!target)
      return;
    auto count = m_targetCounts.find(deskflow::physicalkey::encode(target));
    if (count != m_targetCounts.end() && --count->second == 0) {
      m_targetCounts.erase(count);
      if (!m_backend.post(target.page, target.id, false))
        throw BadClientException("virtual HID keyboard rejected a key release");
    }
  }
}

void ServerProxy1_10::receiveSyntheticKey()
{
  uint32_t key = 0;
  uint16_t mask = 0;
  uint8_t down = 0;
  if (!ProtocolUtil::readf(getStream(), kMsgNativeSyntheticKey + 4, &key, &mask, &down) || down > 1 ||
      !m_nativeActive || (mask & ~deskflow::physicalkey::kActionModifiers) != 0) {
    release();
    throw BadClientException("invalid native keyboard action");
  }
  const KeyID modifierKeys[]{kKeyShift_L, kKeyControl_L, kKeyAlt_L, kKeyMeta_L, kKeySuper_L, kKeyAltGr};
  try {
    if (key == kKeySetModifiers || key == kKeyClearModifiers) {
      for (uint32_t bit = 0; bit < 6; ++bit)
        if ((mask & (1u << bit)) != 0)
          holdKey(
              0x80000000u + bit, deskflow::physicalkey::modifierUsage(translatePhysicalModifier(modifierKeys[bit])),
              key == kKeySetModifiers
          );
      return;
    }
    const auto action = deskflow::physicalkey::actionKey(key);
    if (!action)
      throw BadClientException("unsupported native keyboard action");
    if (down != 0) {
      if (m_syntheticGroups.contains(key))
        return;
      auto &sources = m_syntheticGroups[key];
      const uint32_t base = 0x10000000u | (key << 4);
      const bool shortcut = (mask & ~KeyModifierShift) != 0;
      if (action.shift && (!shortcut || key < 'A' || key > 'Z'))
        mask |= KeyModifierShift;
      for (uint32_t bit = 0; bit < 6; ++bit) {
        if ((mask & (1u << bit)) != 0) {
          const auto source = base + 1 + bit;
          sources.push_back(source);
          holdKey(source, deskflow::physicalkey::modifierUsage(translatePhysicalModifier(modifierKeys[bit])), true);
        }
      }
      const auto main = deskflow::physicalkey::modifierUsage(key)
                            ? deskflow::physicalkey::modifierUsage(translatePhysicalModifier(key))
                            : action.usage;
      sources.push_back(base);
      holdKey(base, main, true);
    } else {
      const auto group = m_syntheticGroups.find(key);
      if (group == m_syntheticGroups.end())
        return;
      const auto sources = group->second;
      m_syntheticGroups.erase(group);
      for (auto source = sources.crbegin(); source != sources.crend(); ++source)
        holdKey(*source, {}, false);
    }
  } catch (...) {
    release();
    m_backend.notify(QStringLiteral("failed"));
    throw;
  }
}

void ServerProxy1_10::receivePhysicalKey()
{
  uint16_t page = 0, usage = 0;
  uint8_t down = 0;
  if (!ProtocolUtil::readf(getStream(), kMsgNativePhysicalKey + 4, &page, &usage, &down) || down > 1 ||
      !m_nativeActive || !deskflow::physicalkey::valid({page, usage})) {
    release();
    m_backend.notify(QStringLiteral("failed"));
    throw BadClientException("invalid or unnegotiated physical key report");
  }
  try {
    physicalKey({page, usage}, down != 0);
  } catch (...) {
    release();
    m_backend.notify(QStringLiteral("failed"));
    throw;
  }
}
