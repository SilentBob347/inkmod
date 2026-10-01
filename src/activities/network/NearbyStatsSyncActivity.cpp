#include "NearbyStatsSyncActivity.h"

#ifdef SIMULATOR

#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Txt.h>
#include <Xtc.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

NearbyStatsSyncActivity::NearbyStatsSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("NearbyStatsSync", renderer, mappedInput) {}

NearbyStatsSyncActivity::~NearbyStatsSyncActivity() = default;

void NearbyStatsSyncActivity::onEnter() {
  Activity::onEnter();
  setState(State::ERROR);
}

void NearbyStatsSyncActivity::onExit() { Activity::onExit(); }

void NearbyStatsSyncActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) finish();
}

void NearbyStatsSyncActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_NEARBY_STATS_SYNC));
  renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_NEARBY_STATS_SIMULATOR_UNAVAILABLE), true,
                            EpdFontFamily::BOLD);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

void NearbyStatsSyncActivity::enqueueEspNowPacket(const uint8_t* sourceMac, const uint8_t* data,
                                                       const int length) {
  if (!eventMutex_ || !sourceMac || !data || length < PACKET_HEADER_BYTES) return;
  if (data[0] != 'C' || data[1] != 'I' || data[2] != 'S' || data[3] != 'S') return;
  if (data[4] != PROTOCOL_VERSION) return;

  SyncEvent event;
  const PacketType packetType = static_cast<PacketType>(data[5]);
  event.type = packetType;
  event.statsSize = data[6];
  event.payloadSize = data[6];
  std::copy(sourceMac, sourceMac + event.sourceMac.size(), event.sourceMac.begin());
  std::copy(data + 8, data + 14, event.deviceMac.begin());
  if (event.deviceMac == localDeviceMac_) return;

  const int payloadLength = length - PACKET_HEADER_BYTES;
  const bool noPayloadType =
      packetType == PacketType::HELLO || packetType == PacketType::ACK || packetType == PacketType::BOOK_NONE ||
      packetType == PacketType::BOOK_ACK || packetType == PacketType::FILE_REQUEST;

  if (packetType != PacketType::HELLO && packetType != PacketType::STATS && packetType != PacketType::ACK &&
      packetType != PacketType::NAME && packetType != PacketType::BOOK_INFO && packetType != PacketType::BOOK_NONE &&
      packetType != PacketType::BOOK_ACK && packetType != PacketType::FILE_REQUEST &&
      packetType != PacketType::FILE_CHUNK && packetType != PacketType::FILE_ACK) {
    return;
  }

  if (packetType == PacketType::STATS) {
    if (payloadLength != event.statsSize || event.statsSize > event.stats.size() ||
        !isValidStatsPayload(data + PACKET_HEADER_BYTES, event.statsSize)) {
      event.type = PacketType::INVALID_STATS;
      event.statsSize = 0;
      event.payloadSize = 0;
    } else {
      std::copy(data + PACKET_HEADER_BYTES, data + PACKET_HEADER_BYTES + event.statsSize, event.stats.begin());
    }
  } else if (packetType == PacketType::NAME) {
    if (event.payloadSize < InkMODSettings::MIN_DEVICE_NAME_LENGTH || event.payloadSize > MAX_DEVICE_NAME_BYTES ||
        payloadLength != event.payloadSize) {
      return;
    }
    memcpy(event.deviceName.data(), data + PACKET_HEADER_BYTES, event.payloadSize);
    event.deviceName[event.payloadSize] = '\0';
  } else if (noPayloadType) {
    if (event.payloadSize != 0 || payloadLength != 0) return;
  } else {
    if (event.payloadSize == 0 || event.payloadSize > event.payload.size() || payloadLength != event.payloadSize) {
      return;
    }
    memcpy(event.payload.data(), data + PACKET_HEADER_BYTES, event.payloadSize);
  }

  if (xSemaphoreTake(eventMutex_, 0) != pdTRUE) return;
  if (eventOverflow_ || eventCount_ >= MAX_SYNC_EVENTS) {
    eventOverflow_ = true;
    eventHead_ = 0;
    eventCount_ = 0;
  } else {
    const uint8_t eventTail = static_cast<uint8_t>((eventHead_ + eventCount_) % MAX_SYNC_EVENTS);
    events_[eventTail] = event;
    eventCount_++;
  }
  xSemaphoreGive(eventMutex_);
}

void NearbyStatsSyncActivity::processEvents() {
  while (true) {
    SyncEvent event;
    bool hasEvent = false;
    bool hasOverflow = false;
    if (eventMutex_) {
      xSemaphoreTake(eventMutex_, portMAX_DELAY);
      if (eventOverflow_) {
        eventOverflow_ = false;
        eventHead_ = 0;
        eventCount_ = 0;
        hasOverflow = true;
      }
      if (eventCount_ > 0) {
        event = events_[eventHead_];
        eventHead_ = static_cast<uint8_t>((eventHead_ + 1) % MAX_SYNC_EVENTS);
        eventCount_--;
        hasEvent = true;
      }
      xSemaphoreGive(eventMutex_);
    }

    if (hasOverflow) {
      setError("sync event queue overflow");
      return;
    }
    if (!hasEvent) return;
    handleEvent(event);
  }
}

void NearbyStatsSyncActivity::handleEvent(const SyncEvent& event) {
  if (state_ == State::ERROR) return;

  if (event.type == PacketType::NAME) {
    if (event.deviceMac == peerDeviceMac_ || isZeroMac(peerDeviceMac_)) {
      peerSourceMac_ = event.sourceMac;
      peerDeviceMac_ = event.deviceMac;
      peerId_ = bytesToHex(peerDeviceMac_.data(), peerDeviceMac_.size());
      peerName_ = event.deviceName.data();
      requestUpdate();
    }
    return;
  }

  const bool startingPassiveSync = state_ != State::DISCOVERING && state_ != State::SYNCING;
  if (startingPassiveSync) {
    errorMessage_.clear();
    peerStatsSaved_ = false;
    localStatsSent_ = false;
    localStatsAcked_ = false;
    localStatsReady_ = false;
    syncStartedMs_ = millis();
    lastHelloMs_ = syncStartedMs_;
    lastStatsSendMs_ = 0;
  }

  peerSeen_ = true;
  if (event.deviceMac != peerDeviceMac_) {
    peerName_.clear();
  }
  peerSourceMac_ = event.sourceMac;
  peerDeviceMac_ = event.deviceMac;
  peerId_ = bytesToHex(peerDeviceMac_.data(), peerDeviceMac_.size());
  addPeer(peerSourceMac_.data());

  if (!localStatsReady_ && !prepareLocalStats()) return;
  if (state_ == State::READY || state_ == State::DISCOVERING || state_ == State::SYNCED) setState(State::SYNCING);

  if (event.type == PacketType::INVALID_STATS) {
    setError(tr(STR_NEARBY_STATS_VERSION_MISMATCH));
    return;
  }

  if (event.type == PacketType::HELLO) {
    sendDeviceName(peerSourceMac_.data());
    sendLocalStats();
    return;
  }

  if (event.type == PacketType::STATS) {
    if (!writeSyncedStatsFile(syncedStatsPathForDeviceMac(peerDeviceMac_), event.stats.data(), event.statsSize)) {
      setError("could not save stats");
      return;
    }
    peerStatsSaved_ = true;
    sendAck(peerSourceMac_.data());
    if (!localStatsSent_ || !localStatsAcked_) sendLocalStats();
    return;
  }

  if (event.type == PacketType::ACK) {
    localStatsAcked_ = true;
  }
}

bool NearbyStatsSyncActivity::addPeer(const uint8_t* peerMac) {
  if (!peerMac) return false;

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, peerMac, ESP_NOW_ETH_ALEN);
  peer.channel = ESPNOW_CHANNEL;
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = false;

  const esp_err_t result = esp_now_add_peer(&peer);
  return result == ESP_OK || result == ESP_ERR_ESPNOW_EXIST;
}

bool NearbyStatsSyncActivity::sendPacket(const PacketType type, const uint8_t* peerMac,
                                             const uint8_t* payload, const uint8_t payloadSize) {
  if (!peerMac || !espNowStarted_) return false;
  if (!addPeer(peerMac)) return false;

  std::array<uint8_t, PACKET_HEADER_BYTES + MAX_PACKET_PAYLOAD> packet = {};
  packet[0] = 'C';
  packet[1] = 'I';
  packet[2] = 'S';
  packet[3] = 'S';
  packet[4] = PROTOCOL_VERSION;
  packet[5] = static_cast<uint8_t>(type);
  packet[7] = 0;
  std::copy(localDeviceMac_.begin(), localDeviceMac_.end(), packet.begin() + 8);

  size_t length = PACKET_HEADER_BYTES;
  if (type == PacketType::STATS) {
    packet[6] = localStatsSize_;
    if (!localStatsReady_ || !isValidStatsPayload(localStats_.data(), localStatsSize_)) return false;
    std::copy(localStats_.begin(), localStats_.begin() + localStatsSize_, packet.begin() + PACKET_HEADER_BYTES);
    length += localStatsSize_;
  } else if (type == PacketType::NAME) {
    const char* name = SETTINGS.getEffectiveDeviceName();
    const size_t nameLength = std::min(std::strlen(name), static_cast<size_t>(MAX_DEVICE_NAME_BYTES));
    if (nameLength < InkMODSettings::MIN_DEVICE_NAME_LENGTH) return false;
    packet[6] = static_cast<uint8_t>(nameLength);
    memcpy(packet.data() + PACKET_HEADER_BYTES, name, nameLength);
    length += nameLength;
  } else if (payloadSize > 0) {
    if (!payload || payloadSize > MAX_PACKET_PAYLOAD) return false;
    packet[6] = payloadSize;
    memcpy(packet.data() + PACKET_HEADER_BYTES, payload, payloadSize);
    length += payloadSize;
  } else {
    packet[6] = 0;
  }

  const esp_err_t result = esp_now_send(peerMac, packet.data(), length);
  if (result != ESP_OK) {
    LOG_ERR(LOG_TAG, "esp_now_send failed: %d", static_cast<int>(result));
    return false;
  }
  return true;
}

bool NearbyStatsSyncActivity::sendHello() {
  lastHelloMs_ = millis();
  return sendPacket(PacketType::HELLO, BROADCAST_MAC);
}

bool NearbyStatsSyncActivity::sendDeviceName(const uint8_t* peerMac) { return sendPacket(PacketType::NAME, peerMac); }

bool NearbyStatsSyncActivity::sendLocalStats() {
  if (!peerSeen_) return false;
  sendDeviceName(peerSourceMac_.data());
  lastStatsSendMs_ = millis();
  localStatsSent_ = sendPacket(PacketType::STATS, peerSourceMac_.data());
  return localStatsSent_;
}

bool NearbyStatsSyncActivity::sendAck(const uint8_t* peerMac) { return sendPacket(PacketType::ACK, peerMac); }

void NearbyStatsSyncActivity::updateSyncProgress() {
  if (state_ != State::DISCOVERING && state_ != State::SYNCING) return;

  const uint32_t now = millis();
  if (now - syncStartedMs_ > SYNC_TIMEOUT_MS) {
    setError(peerSeen_ ? "stats sync timed out" : "no reader found");
    return;
  }

  if (peerStatsSaved_ && localStatsAcked_) {
    setState(State::SYNCED);
    return;
  }

  if (!peerSeen_ && now - lastHelloMs_ >= HELLO_INTERVAL_MS) {
    sendHello();
    return;
  }

  if (peerSeen_ && localStatsReady_ && !localStatsAcked_ && now - lastStatsSendMs_ >= STATS_RETRY_INTERVAL_MS) {
    sendLocalStats();
  }
}

void NearbyStatsSyncActivity::setState(const State state) {
  if (state_ == state) return;
  state_ = state;
  requestUpdate();
}

void NearbyStatsSyncActivity::setError(const std::string& error) {
  LOG_ERR(LOG_TAG, "%s", error.c_str());
  errorMessage_ = error;
  setState(State::ERROR);
}

void NearbyStatsSyncActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_NEARBY_STATS_SYNC));

  const int centerY = pageHeight / 2 - 20;
  std::string primary;
  std::string detailPrimary;
  std::string detailSecondary;

  switch (state_) {
    case State::STARTING:
      primary = tr(STR_LOADING_POPUP);
      break;
    case State::READY:
      primary = tr(STR_NEARBY_STATS_READY);
      detailPrimary = std::string(tr(STR_DEVICE_NAME)) + ": " + SETTINGS.getEffectiveDeviceName();
      break;
    case State::DISCOVERING:
      primary = tr(STR_NEARBY_STATS_SCANNING);
      break;
    case State::SYNCING:
      primary = tr(STR_NEARBY_STATS_SYNCING);
      detailPrimary = std::string(I18N.get(peerName_.empty() ? StrId::STR_SYSTEM_DEVICE : StrId::STR_DEVICE_NAME)) +
                      ": " + (peerName_.empty() ? peerId_ : peerName_);
      if (!isZeroMac(peerDeviceMac_)) {
        detailSecondary = std::string(tr(STR_FILENAME)) + ": " + statsFileNameForDeviceMac(peerDeviceMac_);
      }
      break;
    case State::SYNCED:
      primary = tr(STR_NEARBY_STATS_SYNCED);
      detailPrimary = std::string(I18N.get(peerName_.empty() ? StrId::STR_SYSTEM_DEVICE : StrId::STR_DEVICE_NAME)) +
                      ": " + (peerName_.empty() ? peerId_ : peerName_);
      if (!isZeroMac(peerDeviceMac_)) {
        detailSecondary = std::string(tr(STR_FILENAME)) + ": " + statsFileNameForDeviceMac(peerDeviceMac_);
      }
      break;
    case State::ERROR:
      primary = tr(STR_ERROR_MSG);
      detailPrimary = errorMessage_;
      break;
  }

  if (state_ == State::READY || state_ == State::SYNCED || state_ == State::ERROR) {
    renderReady(primary, detailPrimary, detailSecondary);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_NEARBY_STATS_SYNC_BUTTON), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  renderer.drawCenteredText(UI_10_FONT_ID, centerY, primary.c_str(), true, EpdFontFamily::BOLD);
  if (!detailPrimary.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY + renderer.getLineHeight(UI_10_FONT_ID) + 8,
                              detailPrimary.c_str());
  }
  if (!detailSecondary.empty()) {
    renderer.drawCenteredText(
        SMALL_FONT_ID, centerY + renderer.getLineHeight(UI_10_FONT_ID) + renderer.getLineHeight(SMALL_FONT_ID) + 14,
        detailSecondary.c_str());
  }
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

void NearbyStatsSyncActivity::renderReady(const std::string& primary, const std::string& detailPrimary,
                                          const std::string& detailSecondary) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  int y = contentTop + 70;

  renderer.drawCenteredText(UI_10_FONT_ID, y, primary.c_str(), true, EpdFontFamily::BOLD);
  y += lineHeight + metrics.verticalSpacing;
  if (!detailPrimary.empty()) {
    renderer.drawCenteredText(SMALL_FONT_ID, y, detailPrimary.c_str(), true);
    y += renderer.getLineHeight(SMALL_FONT_ID) + metrics.verticalSpacing;
  }
  if (!detailSecondary.empty()) {
    renderer.drawCenteredText(SMALL_FONT_ID, y, detailSecondary.c_str(), true);
    y += renderer.getLineHeight(SMALL_FONT_ID) + metrics.verticalSpacing;
  }
  if (state_ == State::READY) {
    renderer.drawCenteredText(SMALL_FONT_ID, y, tr(STR_NEARBY_STATS_READY_HINT), true);
  }

  if (mappedInput.hasTouch()) {
    const int buttonWidth = std::min(300, renderer.getScreenWidth() - 48);
    const int buttonHeight = 48;
    const int buttonX = (renderer.getScreenWidth() - buttonWidth) / 2;
    const int buttonY = renderer.getScreenHeight() - buttonHeight - 28;
    renderer.fillRect(buttonX, buttonY, buttonWidth, buttonHeight, false);
    renderer.drawRect(buttonX, buttonY, buttonWidth, buttonHeight, 2, true);
    const int textY = buttonY + (buttonHeight - renderer.getLineHeight(UI_10_FONT_ID)) / 2;
    renderer.drawCenteredText(UI_10_FONT_ID, textY, tr(STR_NEARBY_STATS_SYNC_BUTTON), true, EpdFontFamily::BOLD);
  }
}

#endif
