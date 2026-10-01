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

void NearbyStatsSyncActivity::enqueueEspNowPacket(const uint8_t*, const uint8_t*, int) {}

void NearbyStatsSyncActivity::setState(const State state) {
  state_ = state;
  requestUpdate();
}

#else

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_mac.h>
#include <esp_now.h>
#include <esp_wifi.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "InkMODSettings.h"
#include "InkMODState.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "SdCardFontSystem.h"
#include "activities/reader/GlobalReadingStats.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

constexpr const char* LOG_TAG = "NSYNC";
constexpr const char* INKMOD_ROOT = "/.inkmod";
constexpr const char* GLOBAL_STATS_PATH = "/.inkmod/global_stats.bin";
constexpr const char* SYNCED_STATS_DIR = "/.inkmod/synced_stats";
constexpr uint8_t ESPNOW_CHANNEL = 1;
constexpr uint8_t PROTOCOL_VERSION = 1;
constexpr uint8_t MIN_STATS_BYTES = static_cast<uint8_t>(GlobalReadingStats::MIN_SUPPORTED_FILE_SIZE);
constexpr uint8_t MAX_STATS_BYTES = static_cast<uint8_t>(GlobalReadingStats::CURRENT_FILE_SIZE);
constexpr uint8_t PACKET_HEADER_BYTES = 14;
constexpr uint8_t MAX_DEVICE_NAME_BYTES = static_cast<uint8_t>(InkMODSettings::MAX_DEVICE_NAME_LENGTH);
constexpr uint32_t HELLO_INTERVAL_MS = 750;
constexpr uint32_t STATS_RETRY_INTERVAL_MS = 750;
constexpr uint32_t BOOK_RETRY_INTERVAL_MS = 500;
constexpr uint32_t SYNC_TIMEOUT_MS = 12000;
constexpr uint32_t BOOK_TRANSFER_IDLE_TIMEOUT_MS = 15000;
constexpr uint8_t MAX_BOOK_NAME_BYTES = 96;
constexpr uint8_t MAX_BOOK_CHUNK_BYTES = 220;
constexpr uint8_t BROADCAST_MAC[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

NearbyStatsSyncActivity* activeActivity = nullptr;

std::string bytesToHex(const uint8_t* data, const size_t length) {
  static constexpr char hex[] = "0123456789abcdef";
  std::string out;
  out.resize(length * 2);
  for (size_t i = 0; i < length; i++) {
    out[i * 2] = hex[data[i] >> 4];
    out[i * 2 + 1] = hex[data[i] & 0x0F];
  }
  return out;
}

std::string statsFileNameForDeviceMac(const std::array<uint8_t, 6>& mac) {
  char name[32];
  snprintf(name, sizeof(name), "device_%02x%02x%02x%02x%02x%02x.bin", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return name;
}

std::string syncedStatsPathForDeviceMac(const std::array<uint8_t, 6>& mac) {
  return std::string(SYNCED_STATS_DIR) + "/" + statsFileNameForDeviceMac(mac);
}

bool isZeroMac(const std::array<uint8_t, 6>& mac) { return mac == std::array<uint8_t, 6>{}; }

bool isValidStatsPayload(const uint8_t* data, const uint8_t size) {
  return (size == MIN_STATS_BYTES && data[0] == 1) || (size == 17 && data[0] == 2) ||
         (size == MAX_STATS_BYTES && data[0] == GlobalReadingStats::CURRENT_FILE_VERSION);
}

bool ensureSyncedStatsDirectory() {
  return Storage.ensureDirectoryExists(INKMOD_ROOT) && Storage.ensureDirectoryExists(SYNCED_STATS_DIR);
}

bool readSmallFile(const char* path, std::array<uint8_t, MAX_STATS_BYTES>& out, uint8_t& outSize) {
  outSize = 0;
  FsFile file;
  if (!Storage.openFileForRead(LOG_TAG, path, file)) return false;
  const size_t fileSize = file.fileSize();
  if (fileSize < MIN_STATS_BYTES || fileSize > MAX_STATS_BYTES) {
    file.close();
    return false;
  }

  const int read = file.read(out.data(), fileSize);
  file.close();
  if (read != static_cast<int>(fileSize) || !isValidStatsPayload(out.data(), static_cast<uint8_t>(fileSize)))
    return false;
  outSize = static_cast<uint8_t>(fileSize);
  return true;
}

bool writeSyncedStatsFile(const std::string& path, const uint8_t* data, const uint8_t size) {
  if (!isValidStatsPayload(data, size) || !ensureSyncedStatsDirectory()) return false;

  const std::string tmpPath = path + ".part";
  if (Storage.exists(tmpPath.c_str())) Storage.remove(tmpPath.c_str());

  FsFile file;
  if (!Storage.openFileForWrite(LOG_TAG, tmpPath, file)) return false;
  const size_t written = file.write(data, size);
  if (written != size) {
    file.close();
    Storage.remove(tmpPath.c_str());
    return false;
  }
  file.flush();
  if (!file.sync()) {
    file.close();
    Storage.remove(tmpPath.c_str());
    return false;
  }
  if (!file.close()) {
    Storage.remove(tmpPath.c_str());
    return false;
  }

  if (Storage.exists(path.c_str()) && !Storage.remove(path.c_str())) {
    Storage.remove(tmpPath.c_str());
    return false;
  }
  if (!Storage.rename(tmpPath.c_str(), path.c_str())) {
    Storage.remove(tmpPath.c_str());
    return false;
  }
  return true;
}


std::string fileNameFromPath(const std::string& path) {
  const size_t slash = path.find_last_of("/\\");
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

uint32_t crc32Update(uint32_t crc, const uint8_t* data, const size_t length) {
  for (size_t i = 0; i < length; ++i) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
  }
  return crc;
}

bool fileSizeAndCrc(const std::string& path, uint64_t& size, uint32_t& crc) {
  FsFile file;
  if (!Storage.openFileForRead(LOG_TAG, path, file)) return false;
  size = file.fileSize64();
  crc = 0xFFFFFFFFu;
  std::array<uint8_t, 1024> buffer{};
  while (true) {
    const int n = file.read(buffer.data(), buffer.size());
    if (n < 0) {
      file.close();
      return false;
    }
    if (n == 0) break;
    crc = crc32Update(crc, buffer.data(), static_cast<size_t>(n));
  }
  file.close();
  crc ^= 0xFFFFFFFFu;
  return true;
}

void writeU32(uint8_t* out, const uint32_t value) {
  out[0] = static_cast<uint8_t>(value);
  out[1] = static_cast<uint8_t>(value >> 8);
  out[2] = static_cast<uint8_t>(value >> 16);
  out[3] = static_cast<uint8_t>(value >> 24);
}

uint32_t readU32(const uint8_t* in) {
  return static_cast<uint32_t>(in[0]) | (static_cast<uint32_t>(in[1]) << 8) |
         (static_cast<uint32_t>(in[2]) << 16) | (static_cast<uint32_t>(in[3]) << 24);
}

void writeU64(uint8_t* out, const uint64_t value) {
  writeU32(out, static_cast<uint32_t>(value));
  writeU32(out + 4, static_cast<uint32_t>(value >> 32));
}

uint64_t readU64(const uint8_t* in) {
  return static_cast<uint64_t>(readU32(in)) | (static_cast<uint64_t>(readU32(in + 4)) << 32);
}

std::string cachePathForBook(const std::string& path) {
  if (FsHelpers::hasXtcExtension(path)) return Xtc(path, "/.inkmod").getCachePath();
  if (FsHelpers::hasTxtExtension(path) || FsHelpers::hasMarkdownExtension(path)) {
    return Txt(path, "/.inkmod").getCachePath();
  }
  return Epub(path, "/.inkmod").getCachePath();
}

bool readBookProgress(const std::string& path, std::array<uint8_t, 6>& out, uint8_t& outSize) {
  outSize = 0;
  const std::string progressPath = cachePathForBook(path) + "/progress.bin";
  FsFile file;
  if (!Storage.openFileForRead(LOG_TAG, progressPath, file)) return false;
  const size_t size = file.fileSize();
  if (size != 4 && size != 6) {
    file.close();
    return false;
  }
  const int n = file.read(out.data(), size);
  file.close();
  if (n != static_cast<int>(size)) return false;
  outSize = static_cast<uint8_t>(size);
  return true;
}

uint64_t progressScore(const std::string& path, const uint8_t* data, const uint8_t size) {
  if (!data || (size != 4 && size != 6)) return 0;
  if (FsHelpers::hasXtcExtension(path)) {
    return static_cast<uint64_t>(readU32(data));
  }
  if (FsHelpers::hasTxtExtension(path) || FsHelpers::hasMarkdownExtension(path)) {
    return static_cast<uint64_t>(data[0] | (static_cast<uint16_t>(data[1]) << 8));
  }

  const uint16_t spine = static_cast<uint16_t>(data[0] | (static_cast<uint16_t>(data[1]) << 8));
  const uint16_t page = static_cast<uint16_t>(data[2] | (static_cast<uint16_t>(data[3]) << 8));
  const uint16_t pageCount = size == 6
                                 ? static_cast<uint16_t>(data[4] | (static_cast<uint16_t>(data[5]) << 8))
                                 : 0;
  const uint64_t within = pageCount > 0 ? (static_cast<uint64_t>(page) * 1000000ULL) / pageCount : page;
  return (static_cast<uint64_t>(spine) << 32) | std::min<uint64_t>(within, 1000000ULL);
}

bool writeBookProgressIfNewer(const std::string& path, const uint8_t* incoming, const uint8_t incomingSize) {
  if (!incoming || (incomingSize != 4 && incomingSize != 6)) return true;
  std::array<uint8_t, 6> local{};
  uint8_t localSize = 0;
  if (readBookProgress(path, local, localSize) &&
      progressScore(path, incoming, incomingSize) <= progressScore(path, local.data(), localSize)) {
    return true;
  }

  const std::string cachePath = cachePathForBook(path);
  if (!Storage.mkdir(cachePath.c_str(), true) && !Storage.exists(cachePath.c_str())) return false;
  const std::string progressPath = cachePath + "/progress.bin";
  const std::string backupPath = progressPath + ".bak";
  if (Storage.exists(progressPath.c_str())) {
    Storage.remove(backupPath.c_str());
    Storage.rename(progressPath.c_str(), backupPath.c_str());
  }

  FsFile file;
  if (!Storage.openFileForWrite(LOG_TAG, progressPath, file)) return false;
  const size_t written = file.write(incoming, incomingSize);
  return written == incomingSize && file.close();
}

void onEspNowReceive(const esp_now_recv_info_t* info, const uint8_t* data, int length) {
  if (!activeActivity || !info || !info->src_addr) return;
  activeActivity->enqueueEspNowPacket(info->src_addr, data, length);
}

}  // namespace

NearbyStatsSyncActivity::NearbyStatsSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("NearbyStatsSync", renderer, mappedInput), eventMutex_(xSemaphoreCreateMutex()) {}

NearbyStatsSyncActivity::~NearbyStatsSyncActivity() {
  if (eventMutex_) {
    vSemaphoreDelete(eventMutex_);
    eventMutex_ = nullptr;
  }
}

void NearbyStatsSyncActivity::onEnter() {
  Activity::onEnter();
  sdFontSystem.releaseLoadedFont(renderer);
  setState(State::STARTING);

  if (esp_efuse_mac_get_default(localDeviceMac_.data()) != ESP_OK) {
    setError("Could not read device id");
    return;
  }

  if (!beginEspNow()) {
    setError("Could not start nearby sync");
    return;
  }

  setState(State::READY);
}

void NearbyStatsSyncActivity::onExit() {
  Activity::onExit();
  endEspNow();
}

void NearbyStatsSyncActivity::loop() {
  processEvents();

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (state_ == State::READY || state_ == State::SYNCED || state_ == State::ERROR) {
    const int buttonWidth = std::min(300, renderer.getScreenWidth() - 48);
    const int buttonHeight = 48;
    const int buttonX = (renderer.getScreenWidth() - buttonWidth) / 2;
    const int buttonY = renderer.getScreenHeight() - buttonHeight - 28;
    if ((mappedInput.hasTouch() && mappedInput.wasTapInRect(buttonX, buttonY, buttonWidth, buttonHeight)) ||
        mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      startSync();
      return;
    }
  }

  updateSyncProgress();
}

bool NearbyStatsSyncActivity::beginEspNow() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false);
  WiFi.setSleep(false);
  if (esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE) != ESP_OK) return false;
  esp_wifi_set_ps(WIFI_PS_NONE);

  if (esp_now_init() != ESP_OK) return false;
  espNowStarted_ = true;

  if (esp_now_register_recv_cb(onEspNowReceive) != ESP_OK) return false;
  if (!addPeer(BROADCAST_MAC)) return false;
  activeActivity = this;
  return true;
}

void NearbyStatsSyncActivity::endEspNow() {
  if (activeActivity == this) activeActivity = nullptr;
  if (espNowStarted_) {
    esp_now_unregister_recv_cb();
    esp_now_deinit();
    espNowStarted_ = false;
  }
  WiFi.disconnect(false);
  WiFi.mode(WIFI_OFF);
}

bool NearbyStatsSyncActivity::prepareLocalStats() {
  localStatsReady_ = false;
  if (!ensureSyncedStatsDirectory()) {
    setError("could not create synced stats directory");
    return false;
  }

  // Ensure a valid local stats payload exists before exchanging stats.
  GlobalReadingStats::load().save();

  if (!readSmallFile(GLOBAL_STATS_PATH, localStats_, localStatsSize_)) {
    setError("local stats unavailable");
    return false;
  }

  localStatsReady_ = true;
  return true;
}


void NearbyStatsSyncActivity::resetBookSyncState() {
  localBookPath_.clear();
  localBookName_.clear();
  localBookProgress_.fill(0);
  localBookProgressSize_ = 0;
  localBookSize_ = 0;
  localBookCrc32_ = 0;
  localBookReady_ = false;
  localBookInfoSent_ = false;
  localBookAcked_ = false;

  peerBookName_.clear();
  peerBookPath_.clear();
  peerBookProgress_.fill(0);
  peerBookProgressSize_ = 0;
  peerBookSize_ = 0;
  peerBookCrc32_ = 0;
  peerBookStateSeen_ = false;
  peerBookHandled_ = false;

  sendingBookFile_ = false;
  sendBookOffset_ = 0;
  sendBookAwaitingAckOffset_ = 0;
  lastBookChunkSendMs_ = 0;

  receivingBookFile_ = false;
  receiveBookOffset_ = 0;
  receiveBookTempPath_.clear();
  if (receiveBookFile_) receiveBookFile_.close();

  bookStatus_.clear();
}

bool NearbyStatsSyncActivity::prepareLocalBook() {
  localBookReady_ = false;

  std::string path = APP_STATE.openEpubPath;
  if (path.empty() || !Storage.exists(path.c_str())) {
    for (const auto& book : RECENT_BOOKS.getBooks()) {
      if (Storage.exists(book.path.c_str())) {
        path = book.path;
        break;
      }
    }
  }
  if (path.empty() || !Storage.exists(path.c_str())) return true;

  uint64_t size = 0;
  uint32_t crc = 0;
  if (!fileSizeAndCrc(path, size, crc)) return true;

  localBookPath_ = path;
  localBookName_ = fileNameFromPath(path);
  if (localBookName_.empty() || localBookName_.size() > MAX_BOOK_NAME_BYTES) return true;

  localBookSize_ = size;
  localBookCrc32_ = crc;
  readBookProgress(path, localBookProgress_, localBookProgressSize_);
  localBookReady_ = true;
  return true;
}

bool NearbyStatsSyncActivity::sendBookState() {
  if (!peerSeen_) return false;
  if (!localBookReady_) {
    localBookInfoSent_ = sendPacket(PacketType::BOOK_NONE, peerSourceMac_.data());
    return localBookInfoSent_;
  }

  std::array<uint8_t, MAX_PACKET_PAYLOAD> payload{};
  const size_t nameLen = std::min(localBookName_.size(), static_cast<size_t>(MAX_BOOK_NAME_BYTES));
  size_t off = 0;
  writeU64(payload.data() + off, localBookSize_); off += 8;
  writeU32(payload.data() + off, localBookCrc32_); off += 4;
  payload[off++] = localBookProgressSize_;
  if (localBookProgressSize_ > 0) {
    memcpy(payload.data() + off, localBookProgress_.data(), localBookProgressSize_);
    off += localBookProgressSize_;
  }
  payload[off++] = static_cast<uint8_t>(nameLen);
  memcpy(payload.data() + off, localBookName_.data(), nameLen);
  off += nameLen;

  localBookInfoSent_ =
      sendPacket(PacketType::BOOK_INFO, peerSourceMac_.data(), payload.data(), static_cast<uint8_t>(off));
  return localBookInfoSent_;
}

bool NearbyStatsSyncActivity::sendBookAck(const uint8_t* peerMac) {
  return sendPacket(PacketType::BOOK_ACK, peerMac);
}

bool NearbyStatsSyncActivity::sendFileRequest(const uint8_t* peerMac) {
  return sendPacket(PacketType::FILE_REQUEST, peerMac);
}

bool NearbyStatsSyncActivity::sendFileAck(const uint8_t* peerMac, const uint64_t nextOffset) {
  uint8_t payload[8];
  writeU64(payload, nextOffset);
  return sendPacket(PacketType::FILE_ACK, peerMac, payload, sizeof(payload));
}

bool NearbyStatsSyncActivity::applyPeerBookProgress(const std::string& localPath) {
  if (peerBookProgressSize_ == 0) return true;
  const bool ok = writeBookProgressIfNewer(localPath, peerBookProgress_.data(), peerBookProgressSize_);
  if (ok) bookStatus_ = "Прогресс книги синхронизирован";
  return ok;
}

bool NearbyStatsSyncActivity::startReceivingPeerBook() {
  if (peerBookName_.empty() || peerBookSize_ == 0) return false;
  if (!Storage.ensureDirectoryExists("/Books") || !Storage.ensureDirectoryExists("/Books/Nearby")) return false;

  std::string finalPath = std::string("/Books/Nearby/") + peerBookName_;
  if (Storage.exists(finalPath.c_str())) {
    finalPath = std::string("/Books/Nearby/") + peerId_.substr(0, std::min<size_t>(6, peerId_.size())) + "_" +
                peerBookName_;
  }
  receiveBookTempPath_ = finalPath + ".part";
  peerBookPath_ = finalPath;
  Storage.remove(receiveBookTempPath_.c_str());

  if (!Storage.openFileForWrite(LOG_TAG, receiveBookTempPath_, receiveBookFile_)) return false;
  receivingBookFile_ = true;
  receiveBookOffset_ = 0;
  bookStatus_ = "Получение книги: 0%";
  requestUpdate();
  return true;
}

bool NearbyStatsSyncActivity::finalizeReceivedPeerBook() {
  if (!receivingBookFile_) return false;
  if (!receiveBookFile_.close()) return false;
  receivingBookFile_ = false;

  uint64_t size = 0;
  uint32_t crc = 0;
  if (!fileSizeAndCrc(receiveBookTempPath_, size, crc) || size != peerBookSize_ || crc != peerBookCrc32_) {
    Storage.remove(receiveBookTempPath_.c_str());
    return false;
  }

  if (Storage.exists(peerBookPath_.c_str())) Storage.remove(peerBookPath_.c_str());
  if (!Storage.rename(receiveBookTempPath_.c_str(), peerBookPath_.c_str())) return false;

  const RecentBook metadata = RECENT_BOOKS.getDataFromBook(peerBookPath_);
  RECENT_BOOKS.addOrUpdateBook(peerBookPath_,
                              metadata.title.empty() ? peerBookName_ : metadata.title,
                              metadata.author, metadata.coverBmpPath);

  if (!applyPeerBookProgress(peerBookPath_)) return false;

  peerBookHandled_ = true;
  bookStatus_ = "Книга получена и прогресс применён";
  requestUpdate();
  return sendBookAck(peerSourceMac_.data());
}

bool NearbyStatsSyncActivity::handlePeerBookInfo(const SyncEvent& event) {
  if (event.payloadSize < 14) return false;
  size_t off = 0;
  peerBookSize_ = readU64(event.payload.data() + off); off += 8;
  peerBookCrc32_ = readU32(event.payload.data() + off); off += 4;
  peerBookProgressSize_ = event.payload[off++];
  if (peerBookProgressSize_ != 0 && peerBookProgressSize_ != 4 && peerBookProgressSize_ != 6) return false;
  if (off + peerBookProgressSize_ + 1 > event.payloadSize) return false;
  if (peerBookProgressSize_ > 0) {
    memcpy(peerBookProgress_.data(), event.payload.data() + off, peerBookProgressSize_);
    off += peerBookProgressSize_;
  }
  const uint8_t nameLen = event.payload[off++];
  if (nameLen == 0 || nameLen > MAX_BOOK_NAME_BYTES || off + nameLen != event.payloadSize) return false;
  peerBookName_.assign(reinterpret_cast<const char*>(event.payload.data() + off), nameLen);
  peerBookStateSeen_ = true;

  auto candidateMatches = [&](const std::string& path) -> bool {
    if (path.empty() || !Storage.exists(path.c_str()) || fileNameFromPath(path) != peerBookName_) return false;
    uint64_t size = 0;
    uint32_t crc = 0;
    return fileSizeAndCrc(path, size, crc) && size == peerBookSize_ && crc == peerBookCrc32_;
  };

  std::string match;
  if (candidateMatches(APP_STATE.openEpubPath)) match = APP_STATE.openEpubPath;
  if (match.empty()) {
    for (const auto& book : RECENT_BOOKS.getBooks()) {
      if (candidateMatches(book.path)) {
        match = book.path;
        break;
      }
    }
  }

  if (!match.empty()) {
    peerBookPath_ = match;
    peerBookHandled_ = applyPeerBookProgress(match);
    if (peerBookHandled_) sendBookAck(event.sourceMac.data());
    return peerBookHandled_;
  }

  if (!startReceivingPeerBook()) return false;
  return sendFileRequest(event.sourceMac.data());
}

bool NearbyStatsSyncActivity::handleBookChunk(const SyncEvent& event) {
  if (!receivingBookFile_ || event.payloadSize < 8) return false;
  const uint64_t offset = readU64(event.payload.data());
  const size_t chunkSize = event.payloadSize - 8;

  if (offset < receiveBookOffset_) {
    sendFileAck(event.sourceMac.data(), receiveBookOffset_);
    return true;
  }
  if (offset != receiveBookOffset_ || receiveBookOffset_ + chunkSize > peerBookSize_) {
    sendFileAck(event.sourceMac.data(), receiveBookOffset_);
    return false;
  }

  const size_t written = receiveBookFile_.write(event.payload.data() + 8, chunkSize);
  if (written != chunkSize) return false;
  receiveBookOffset_ += chunkSize;

  const unsigned percent = peerBookSize_ > 0 ? static_cast<unsigned>((receiveBookOffset_ * 100) / peerBookSize_) : 0;
  bookStatus_ = "Получение книги: " + std::to_string(std::min(percent, 100u)) + "%";
  requestUpdate();

  if (!sendFileAck(event.sourceMac.data(), receiveBookOffset_)) return false;
  if (receiveBookOffset_ >= peerBookSize_) return finalizeReceivedPeerBook();
  return true;
}

bool NearbyStatsSyncActivity::sendNextBookChunk(const bool retry) {
  if (!sendingBookFile_ || localBookPath_.empty() || sendBookOffset_ >= localBookSize_) return false;

  const uint64_t offset = retry ? sendBookAwaitingAckOffset_ : sendBookOffset_;
  FsFile file;
  if (!Storage.openFileForRead(LOG_TAG, localBookPath_, file)) return false;
  if (!file.seek64(offset)) {
    file.close();
    return false;
  }

  std::array<uint8_t, MAX_PACKET_PAYLOAD> payload{};
  writeU64(payload.data(), offset);
  const size_t want = static_cast<size_t>(std::min<uint64_t>(MAX_BOOK_CHUNK_BYTES, localBookSize_ - offset));
  const int n = file.read(payload.data() + 8, want);
  file.close();
  if (n <= 0) return false;

  sendBookAwaitingAckOffset_ = offset;
  lastBookChunkSendMs_ = millis();
  return sendPacket(PacketType::FILE_CHUNK, peerSourceMac_.data(), payload.data(), static_cast<uint8_t>(8 + n));
}

void NearbyStatsSyncActivity::startSync() {
  errorMessage_.clear();
  peerSeen_ = false;
  peerStatsSaved_ = false;
  localStatsSent_ = false;
  localStatsAcked_ = false;
  peerSourceMac_ = {};
  peerDeviceMac_ = {};
  peerId_.clear();
  peerName_.clear();
  syncStartedMs_ = millis();
  lastHelloMs_ = 0;
  lastStatsSendMs_ = 0;

  if (!prepareLocalStats()) return;

  setState(State::DISCOVERING);
  sendHello();
}

void NearbyStatsSyncActivity::enqueueEspNowPacket(const uint8_t* sourceMac, const uint8_t* data, const int length) {
  if (!eventMutex_ || !sourceMac || !data || length < PACKET_HEADER_BYTES) return;
  if (data[0] != 'C' || data[1] != 'I' || data[2] != 'S' || data[3] != 'S') return;
  if (data[4] != PROTOCOL_VERSION) return;

  SyncEvent event;
  const PacketType packetType = static_cast<PacketType>(data[5]);
  event.type = packetType;
  event.statsSize = data[6];
  std::copy(sourceMac, sourceMac + event.sourceMac.size(), event.sourceMac.begin());
  std::copy(data + 8, data + 14, event.deviceMac.begin());

  const int payloadLength = length - PACKET_HEADER_BYTES;
  const int expectedLength = PACKET_HEADER_BYTES + (event.type == PacketType::STATS ? event.statsSize : 0);
  if (packetType != PacketType::HELLO && packetType != PacketType::STATS && packetType != PacketType::ACK &&
      packetType != PacketType::NAME)
    return;
  if (event.deviceMac == localDeviceMac_) return;
  if (packetType == PacketType::STATS) {
    if (length != expectedLength || event.statsSize > event.stats.size() ||
        !isValidStatsPayload(data + PACKET_HEADER_BYTES, event.statsSize)) {
      event.type = PacketType::INVALID_STATS;
      event.statsSize = 0;
    } else {
      std::copy(data + PACKET_HEADER_BYTES, data + PACKET_HEADER_BYTES + event.statsSize, event.stats.begin());
    }
  } else if (packetType == PacketType::NAME) {
    if (event.statsSize < InkMODSettings::MIN_DEVICE_NAME_LENGTH || event.statsSize > MAX_DEVICE_NAME_BYTES ||
        payloadLength != event.statsSize) {
      return;
    }
    memcpy(event.deviceName.data(), data + PACKET_HEADER_BYTES, event.statsSize);
    event.deviceName[event.statsSize] = '\0';
  } else if (length != expectedLength) {
    return;
  } else if (packetType == PacketType::HELLO || packetType == PacketType::ACK) {
    if (event.statsSize != 0) return;
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

bool NearbyStatsSyncActivity::sendPacket(const PacketType type, const uint8_t* peerMac) {
  if (!peerMac || !espNowStarted_) return false;
  if (!addPeer(peerMac)) return false;

  std::array<uint8_t, PACKET_HEADER_BYTES + MAX_STATS_BYTES> packet = {};
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
