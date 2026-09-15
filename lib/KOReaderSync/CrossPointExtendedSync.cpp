#include "CrossPointExtendedSync.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <SecureHttpClient.h>
#include <base64.h>
#include <esp_mac.h>
#include <mbedtls/sha256.h>
#include <mbedtls/base64.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../../src/BookmarkStore.h"
#include "../../src/ClippingStore.h"
#include "Fb2.h"
#include "../../src/InkMODSettings.h"
#include "KOReaderCredentialStore.h"
#include "ProgressMapper.h"
#include "../../src/activities/reader/GlobalReadingStats.h"
#include "Epub/Section.h"

namespace {
constexpr char TAG[] = "KOSX";
constexpr char STATE_DIR[] = "/.inkmod/kosync_ext";
constexpr char SYNCED_STATS_DIR[] = "/.inkmod/synced_stats";
constexpr char SERVER_SUMMARY_FILE[] = "server_summary.bin";
constexpr int MAX_DELTA_PAGES = 12;  // hard stop: 12*100 items is already far beyond normal device use

struct BookmarkWire {
  std::string id;
  std::string xpath;
  float percentage = 0.0f;
  std::string summary;
  int si = -1;
  int pc = -1;
  int pp = -1;
  bool deleted = false;
};

struct ClippingWire {
  std::string id;
  uint16_t spine = 0;
  uint16_t startPage = 0;
  uint16_t endPage = 0;
  uint16_t pages = 1;
  uint16_t startWord = 0;
  uint16_t endWord = 0;
  std::string chapter;
  std::string text;
  uint32_t createdAt = 0;
  bool deleted = false;
};

std::string sha256Id16(const std::string& value) {
  uint8_t digest[32] = {};
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  mbedtls_sha256_update(&ctx, reinterpret_cast<const unsigned char*>(value.data()), value.size());
  mbedtls_sha256_finish(&ctx, digest);
  mbedtls_sha256_free(&ctx);

  static constexpr char hex[] = "0123456789abcdef";
  std::string out;
  out.resize(16);
  for (size_t i = 0; i < 8; ++i) {
    out[i * 2] = hex[digest[i] >> 4];
    out[i * 2 + 1] = hex[digest[i] & 0x0f];
  }
  return out;
}

std::string deviceId() {
  uint8_t mac[6] = {};
  if (esp_efuse_mac_get_default(mac) != ESP_OK) return "inkmod-device";
  char buf[20];
  snprintf(buf, sizeof(buf), "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return std::string(buf);
}

std::string safeDeviceFileToken(const std::string& id) {
  std::string out;
  out.reserve(id.size());
  for (char c : id) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    out.push_back(ok ? c : '_');
  }
  if (out.empty()) out = "remote";
  if (out.size() > 48) out.resize(48);
  return out;
}

void addAuthHeaders(freeink::SecureHttpClient& http) {
  http.addHeader("Accept", "application/json");
  http.addHeader("x-auth-user", KOREADER_STORE.getUsername());
  http.addHeader("x-auth-key", KOREADER_STORE.getMd5Password());
}

bool httpGet(freeink::SecureHttpClient& http, const std::string& url, std::string& body, int& code) {
  if (!http.begin(url)) return false;
  addAuthHeaders(http);
  code = http.GET();
  body = http.getString();
  return code >= 200 && code < 300;
}

bool httpPut(freeink::SecureHttpClient& http, const std::string& url, const std::string& payload,
             std::string& body, int& code) {
  if (!http.begin(url)) return false;
  addAuthHeaders(http);
  http.addHeader("Content-Type", "application/json");
  code = http.sendRequest("PUT", payload);
  body = http.getString();
  return code >= 200 && code < 300;
}

std::string statePath(const std::string& documentHash) {
  return std::string(STATE_DIR) + "/" + documentHash + ".json";
}

bool containsId(const std::vector<std::string>& ids, const std::string& id) {
  return std::find(ids.begin(), ids.end(), id) != ids.end();
}

void loadKnownIds(const std::string& documentHash, std::vector<std::string>& bookmarks,
                  std::vector<std::string>& clippings) {
  bookmarks.clear();
  clippings.clear();
  const String json = Storage.readFile(statePath(documentHash).c_str());
  if (json.isEmpty()) return;
  JsonDocument doc;
  if (deserializeJson(doc, json)) return;
  for (JsonVariantConst v : doc["bookmark_ids"].as<JsonArrayConst>()) {
    const char* s = v.as<const char*>();
    if (s && *s) bookmarks.emplace_back(s);
  }
  for (JsonVariantConst v : doc["clipping_ids"].as<JsonArrayConst>()) {
    const char* s = v.as<const char*>();
    if (s && *s) clippings.emplace_back(s);
  }
}

bool saveKnownIds(const std::string& documentHash, const std::vector<std::string>& bookmarks,
                  const std::vector<std::string>& clippings) {
  if (!Storage.ensureDirectoryExists("/.inkmod") || !Storage.ensureDirectoryExists(STATE_DIR)) return false;
  JsonDocument doc;
  JsonArray b = doc["bookmark_ids"].to<JsonArray>();
  for (const auto& id : bookmarks) b.add(id);
  JsonArray c = doc["clipping_ids"].to<JsonArray>();
  for (const auto& id : clippings) c.add(id);

  const std::string path = statePath(documentHash);
  const std::string tmp = path + ".tmp";
  if (Storage.exists(tmp.c_str())) Storage.remove(tmp.c_str());
  FsFile f;
  if (!Storage.openFileForWrite(TAG, tmp, f)) return false;
  const size_t expected = measureJson(doc);
  const size_t written = serializeJson(doc, f);
  const bool closed = f.close();
  if (written != expected || !closed) {
    Storage.remove(tmp.c_str());
    return false;
  }
  if (Storage.exists(path.c_str()) && !Storage.remove(path.c_str())) {
    Storage.remove(tmp.c_str());
    return false;
  }
  if (!Storage.rename(tmp.c_str(), path.c_str())) {
    Storage.remove(tmp.c_str());
    return false;
  }
  return true;
}

std::string chapterTitleForSpine(const std::shared_ptr<Epub>& epub, int spine) {
  const int toc = epub->getTocIndexForSpineIndex(spine);
  return toc >= 0 ? epub->getTocItem(toc).title : std::string();
}

bool isFb2Backed(const std::string& epubPath) { return Fb2::resolveOriginalPath(epubPath) != epubPath; }

BookmarkWire encodeBookmark(const Bookmark& bm, const std::shared_ptr<Epub>& epub, const std::string& epubPath,
                            GfxRenderer& renderer) {
  BookmarkWire out;
  if (!epub || bm.spineIndex >= epub->getSpineItemsCount()) return out;

  Section section(epub, bm.spineIndex, renderer);
  const int pageCount = std::max<int>(1, section.pageCount);
  int page = static_cast<int>(bm.progress * static_cast<float>(pageCount) + 0.0001f);
  page = std::clamp(page, 0, pageCount - 1);

  InkMODPosition pos{};
  pos.spineIndex = bm.spineIndex;
  pos.pageNumber = page;
  pos.totalPages = pageCount;
  if (bm.paragraphIndex != UINT16_MAX && bm.paragraphIndex > 0) {
    pos.paragraphIndex = bm.paragraphIndex;
    pos.hasParagraphIndex = true;
  }

  KOReaderPosition ko;
  if (isFb2Backed(epubPath)) {
    int sourceOrdinal = bm.spineIndex + 1;
    Fb2::getOriginalSectionOrdinal(epub->getCachePath(), bm.spineIndex, sourceOrdinal);
    out.xpath = ProgressMapper::generateFb2SourceXPath(epub, epub->getCachePath(), pos, sourceOrdinal);
    ko = ProgressMapper::toKOReader(epub, pos);
  } else {
    ko = ProgressMapper::toKOReader(epub, pos);
    out.xpath = ko.xpath;
  }
  if (out.xpath.empty()) return out;
  out.id = sha256Id16(out.xpath);
  out.percentage = ko.percentage;
  out.summary = bm.snippet;
  out.si = bm.spineIndex;
  out.pc = pageCount;
  out.pp = page;
  return out;
}

bool removeBookmarkById(BookmarkStore& store, const std::string& targetId, const std::shared_ptr<Epub>& epub,
                        const std::string& epubPath, GfxRenderer& renderer) {
  const auto& items = store.getBookmarks();
  for (size_t i = 0; i < items.size(); ++i) {
    const BookmarkWire wire = encodeBookmark(items[i], epub, epubPath, renderer);
    if (!wire.id.empty() && wire.id == targetId) return store.removeBookmarkAt(i);
  }
  return true;
}

bool upsertRemoteBookmark(BookmarkStore& store, const BookmarkWire& remote, const std::shared_ptr<Epub>& epub,
                          const std::string& epubPath) {
  if (remote.xpath.empty()) return false;
  const KOReaderPosition ko{remote.xpath, remote.percentage};
  InkMODPosition pos{};
  if (isFb2Backed(epubPath))
    pos = ProgressMapper::toInkMODFb2(epub, ko, epub->getCachePath());
  else
    pos = ProgressMapper::toInkMOD(epub, ko);
  if (pos.spineIndex < 0 || pos.spineIndex >= epub->getSpineItemsCount()) return false;
  const int pageCount = std::max(1, pos.totalPages);
  const float progress = static_cast<float>(std::clamp(pos.pageNumber, 0, pageCount - 1)) / static_cast<float>(pageCount);
  const std::string chapter = chapterTitleForSpine(epub, pos.spineIndex);
  const uint16_t para = pos.hasParagraphIndex ? pos.paragraphIndex : UINT16_MAX;
  const auto result = store.addBookmark(static_cast<uint16_t>(pos.spineIndex), progress, pageCount, chapter.c_str(), para,
                                        remote.summary.c_str());
  return result != BookmarkStore::AddResult::LimitReached;
}

std::string clippingId(const Clipping& clip) {
  return sha256Id16(std::to_string(clip.timestamp) + std::string(clip.text));
}

bool fetchBookmarks(freeink::SecureHttpClient& http, const std::string& documentHash,
                    std::vector<BookmarkWire>& out) {
  out.clear();
  uint64_t since = 0;
  for (int page = 0; page < MAX_DELTA_PAGES; ++page) {
    const std::string url = KOREADER_STORE.getBaseUrl() + "/api/v1/bookmarks/" + documentHash +
                            "?since=" + std::to_string(since) + "&limit=100";
    std::string body;
    int code = 0;
    if (!httpGet(http, url, body, code)) {
      LOG_ERR(TAG, "Bookmark GET failed: HTTP %d", code);
      return false;
    }
    JsonDocument doc;
    if (deserializeJson(doc, body)) return false;
    for (JsonObjectConst item : doc["items"].as<JsonArrayConst>()) {
      BookmarkWire b;
      b.id = item["id"].as<const char*>() ? item["id"].as<const char*>() : "";
      b.xpath = item["xpath"].as<const char*>() ? item["xpath"].as<const char*>() : "";
      b.percentage = item["percentage"] | 0.0f;
      b.summary = item["summary"].as<const char*>() ? item["summary"].as<const char*>() : "";
      b.si = item["si"].isNull() ? -1 : item["si"].as<int>();
      b.pc = item["pc"].isNull() ? -1 : item["pc"].as<int>();
      b.pp = item["pp"].isNull() ? -1 : item["pp"].as<int>();
      b.deleted = (item["deleted"] | 0) != 0;
      if (!b.id.empty()) out.push_back(std::move(b));
    }
    const bool more = doc["more"] | false;
    const uint64_t until = doc["until"] | since;
    if (!more || until <= since) break;
    since = until;
  }
  return true;
}

bool putBookmarks(freeink::SecureHttpClient& http, const std::string& documentHash,
                  const std::vector<BookmarkWire>& items) {
  const std::string url = KOREADER_STORE.getBaseUrl() + "/api/v1/bookmarks/" + documentHash;
  for (size_t offset = 0; offset < items.size(); offset += 50) {
    JsonDocument doc;
    JsonArray arr = doc["items"].to<JsonArray>();
    const size_t end = std::min(items.size(), offset + 50);
    for (size_t i = offset; i < end; ++i) {
      const auto& b = items[i];
      JsonObject o = arr.add<JsonObject>();
      o["id"] = b.id;
      if (b.deleted) {
        o["deleted"] = 1;
        continue;
      }
      o["xpath"] = b.xpath;
      o["percentage"] = b.percentage;
      if (!b.summary.empty()) o["summary"] = b.summary;
      if (b.si >= 0) o["si"] = b.si;
      if (b.pc >= 0) o["pc"] = b.pc;
      if (b.pp >= 0) o["pp"] = b.pp;
    }
    std::string payload;
    serializeJson(doc, payload);
    std::string body;
    int code = 0;
    if (!httpPut(http, url, payload, body, code)) {
      LOG_ERR(TAG, "Bookmark PUT failed: HTTP %d", code);
      return false;
    }
  }
  return true;
}

bool fetchClippings(freeink::SecureHttpClient& http, const std::string& documentHash,
                    std::vector<ClippingWire>& out) {
  out.clear();
  uint64_t since = 0;
  for (int page = 0; page < MAX_DELTA_PAGES; ++page) {
    const std::string url = KOREADER_STORE.getBaseUrl() + "/api/v1/clippings/" + documentHash +
                            "?since=" + std::to_string(since) + "&limit=100";
    std::string body;
    int code = 0;
    if (!httpGet(http, url, body, code)) {
      LOG_ERR(TAG, "Clipping GET failed: HTTP %d", code);
      return false;
    }
    JsonDocument doc;
    if (deserializeJson(doc, body)) return false;
    for (JsonObjectConst item : doc["items"].as<JsonArrayConst>()) {
      ClippingWire c;
      c.id = item["id"].as<const char*>() ? item["id"].as<const char*>() : "";
      c.spine = item["spine"] | 0;
      c.startPage = item["start_page"] | 0;
      c.endPage = item["end_page"] | c.startPage;
      c.pages = item["pages"] | 1;
      c.startWord = item["start_word"] | 0;
      c.endWord = item["end_word"] | 0;
      c.chapter = item["chapter"].as<const char*>() ? item["chapter"].as<const char*>() : "";
      c.text = item["text"].as<const char*>() ? item["text"].as<const char*>() : "";
      c.createdAt = item["created_at"] | 0;
      c.deleted = (item["deleted"] | 0) != 0;
      if (!c.id.empty()) out.push_back(std::move(c));
    }
    const bool more = doc["more"] | false;
    const uint64_t until = doc["until"] | since;
    if (!more || until <= since) break;
    since = until;
  }
  return true;
}

bool putClippings(freeink::SecureHttpClient& http, const std::string& documentHash,
                  const std::vector<ClippingWire>& items) {
  const std::string url = KOREADER_STORE.getBaseUrl() + "/api/v1/clippings/" + documentHash;
  for (size_t offset = 0; offset < items.size(); offset += 50) {
    JsonDocument doc;
    JsonArray arr = doc["items"].to<JsonArray>();
    const size_t end = std::min(items.size(), offset + 50);
    for (size_t i = offset; i < end; ++i) {
      const auto& c = items[i];
      JsonObject o = arr.add<JsonObject>();
      o["id"] = c.id;
      if (c.deleted) {
        o["deleted"] = 1;
        continue;
      }
      o["spine"] = c.spine;
      o["start_page"] = c.startPage;
      o["end_page"] = c.endPage;
      o["pages"] = c.pages;
      o["start_word"] = c.startWord;
      o["end_word"] = c.endWord;
      if (!c.chapter.empty()) o["chapter"] = c.chapter;
      o["text"] = c.text;
      o["created_at"] = c.createdAt;
    }
    std::string payload;
    serializeJson(doc, payload);
    std::string body;
    int code = 0;
    if (!httpPut(http, url, payload, body, code)) {
      LOG_ERR(TAG, "Clipping PUT failed: HTTP %d", code);
      return false;
    }
  }
  return true;
}

void writeLe16(uint8_t* p, uint16_t v) { p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; }
void writeLe32(uint8_t* p, uint32_t v) {
  p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; p[2] = (v >> 16) & 0xff; p[3] = (v >> 24) & 0xff;
}

bool decodeHistory(const char* encoded, GlobalReadingStats& stats);

bool saveStatsSnapshotFile(const std::string& fileName, const GlobalReadingStats& s) {
  if (!Storage.ensureDirectoryExists("/.inkmod") || !Storage.ensureDirectoryExists(SYNCED_STATS_DIR)) return false;
  std::array<uint8_t, GlobalReadingStats::CURRENT_FILE_SIZE> data{};
  data[0] = GlobalReadingStats::CURRENT_FILE_VERSION;
  writeLe32(data.data() + 1, s.totalSessions);
  writeLe32(data.data() + 5, s.totalReadingSeconds);
  writeLe32(data.data() + 9, s.totalPagesTurned);
  writeLe32(data.data() + 13, s.completedBooks);
  for (size_t i = 0; i < s.timeOfDaySeconds.size(); ++i) writeLe32(data.data() + 17 + i * 4, s.timeOfDaySeconds[i]);
  for (size_t i = 0; i < s.dayOfWeekSeconds.size(); ++i) writeLe32(data.data() + 33 + i * 4, s.dayOfWeekSeconds[i]);
  writeLe32(data.data() + 61, s.readingHistoryAnchorDay);
  memcpy(data.data() + 65, s.readingHistoryBits.data(), s.readingHistoryBits.size());
  writeLe16(data.data() + 157, s.longestReadingStreak);

  const std::string path = std::string(SYNCED_STATS_DIR) + "/" + fileName;
  const std::string tmp = path + ".tmp";
  if (Storage.exists(tmp.c_str())) Storage.remove(tmp.c_str());
  FsFile f;
  if (!Storage.openFileForWrite(TAG, tmp, f)) return false;
  const size_t n = f.write(data.data(), data.size());
  f.flush();
  const bool closed = f.close();
  if (n != data.size() || !closed) {
    Storage.remove(tmp.c_str());
    return false;
  }
  if (Storage.exists(path.c_str()) && !Storage.remove(path.c_str())) {
    Storage.remove(tmp.c_str());
    return false;
  }
  if (!Storage.rename(tmp.c_str(), path.c_str())) {
    Storage.remove(tmp.c_str());
    return false;
  }
  return true;
}

bool saveRemoteStatsSnapshot(const std::string& remoteDeviceId, const GlobalReadingStats& s) {
  return saveStatsSnapshotFile("server_" + safeDeviceFileToken(remoteDeviceId) + ".bin", s);
}

bool parseStatsObject(JsonObjectConst s, GlobalReadingStats& out) {
  if (s.isNull()) return false;
  out = GlobalReadingStats{};
  out.totalSessions = s["sessions"] | 0u;
  out.totalReadingSeconds = s["seconds"] | 0u;
  out.totalPagesTurned = s["pages"] | 0u;
  out.completedBooks = s["completed"] | 0u;
  size_t i = 0;
  for (JsonVariantConst v : s["tod"].as<JsonArrayConst>()) {
    if (i >= out.timeOfDaySeconds.size()) break;
    out.timeOfDaySeconds[i++] = v.as<uint32_t>();
  }
  i = 0;
  for (JsonVariantConst v : s["dow"].as<JsonArrayConst>()) {
    if (i >= out.dayOfWeekSeconds.size()) break;
    out.dayOfWeekSeconds[i++] = v.as<uint32_t>();
  }
  out.readingHistoryAnchorDay = s["anchor_day"] | 0u;
  decodeHistory(s["history_b64"] | "", out);
  out.longestReadingStreak = s["streak"] | 0u;
  return true;
}

bool decodeHistory(const char* encoded, GlobalReadingStats& stats) {
  if (!encoded || !*encoded) return true;
  size_t outLen = 0;
  const int rc = mbedtls_base64_decode(stats.readingHistoryBits.data(), stats.readingHistoryBits.size(), &outLen,
                                       reinterpret_cast<const unsigned char*>(encoded), strlen(encoded));
  return rc == 0 && outLen == stats.readingHistoryBits.size();
}

bool syncStats(freeink::SecureHttpClient& http) {
  const GlobalReadingStats local = GlobalReadingStats::load();
  JsonDocument up;
  up["device_id"] = deviceId();
  up["device"] = SETTINGS.getEffectiveDeviceName();
  up["v"] = GlobalReadingStats::CURRENT_FILE_VERSION;
  up["sessions"] = local.totalSessions;
  up["seconds"] = local.totalReadingSeconds;
  up["pages"] = local.totalPagesTurned;
  up["completed"] = local.completedBooks;
  JsonArray tod = up["tod"].to<JsonArray>();
  for (uint32_t v : local.timeOfDaySeconds) tod.add(v);
  JsonArray dow = up["dow"].to<JsonArray>();
  for (uint32_t v : local.dayOfWeekSeconds) dow.add(v);
  up["anchor_day"] = local.readingHistoryAnchorDay;
  const String history = base64::encode(local.readingHistoryBits.data(), local.readingHistoryBits.size());
  up["history_b64"] = history.c_str();
  up["streak"] = local.longestReadingStreak;
  std::string payload;
  serializeJson(up, payload);
  std::string body;
  int code = 0;
  const std::string base = KOREADER_STORE.getBaseUrl();
  if (!httpPut(http, base + "/api/v1/stats/global", payload, body, code)) {
    LOG_ERR(TAG, "Stats PUT failed: HTTP %d", code);
    return false;
  }

  // Fetch the server-side combined snapshot for authoritative additive totals.
  // inkMOD still keeps the raw peer snapshots below because its richer
  // time-of-day/day-of-week/history fields are not guaranteed to be present in
  // every CrossPoint summary response.
  std::string summaryBody;
  int summaryCode = 0;
  if (httpGet(http, base + "/api/v1/stats/summary", summaryBody, summaryCode)) {
    JsonDocument summaryDoc;
    if (!deserializeJson(summaryDoc, summaryBody)) {
      GlobalReadingStats summary;
      if (parseStatsObject(summaryDoc.as<JsonObjectConst>(), summary) &&
          saveStatsSnapshotFile(SERVER_SUMMARY_FILE, summary)) {
        LOG_INF(TAG, "Stats summary saved: sessions=%u seconds=%u pages=%u completed=%u",
                static_cast<unsigned>(summary.totalSessions),
                static_cast<unsigned>(summary.totalReadingSeconds),
                static_cast<unsigned>(summary.totalPagesTurned),
                static_cast<unsigned>(summary.completedBooks));
      }
    }
  } else {
    LOG_ERR(TAG, "Stats summary GET failed: HTTP %d; falling back to per-device snapshots", summaryCode);
  }

  // Keep raw peer snapshots as a compatibility fallback and for older servers.
  if (!httpGet(http, base + "/api/v1/stats/global", body, code)) {
    LOG_ERR(TAG, "Stats GET failed: HTTP %d", code);
    return Storage.exists((std::string(SYNCED_STATS_DIR) + "/" + SERVER_SUMMARY_FILE).c_str());
  }
  JsonDocument doc;
  if (deserializeJson(doc, body)) return false;
  const std::string localId = deviceId();
  unsigned peers = 0;
  for (JsonObjectConst row : doc["devices"].as<JsonArrayConst>()) {
    const char* rid = row["device_id"] | "";
    if (!rid || !*rid || localId == rid) continue;
    GlobalReadingStats remote;
    if (!parseStatsObject(row["stats"].as<JsonObjectConst>(), remote)) continue;
    if (saveRemoteStatsSnapshot(rid, remote)) {
      ++peers;
      LOG_INF(TAG, "Peer stats saved: device=%s sessions=%u seconds=%u pages=%u", rid,
              static_cast<unsigned>(remote.totalSessions),
              static_cast<unsigned>(remote.totalReadingSeconds),
              static_cast<unsigned>(remote.totalPagesTurned));
    }
  }
  LOG_INF(TAG, "Reading stats synced: peers=%u", peers);
  return true;
}

}  // namespace

CrossPointExtendedSync::Result CrossPointExtendedSync::sync(const std::string& documentHash,
                                                             const std::shared_ptr<Epub>& epub,
                                                             const std::string& epubPath,
                                                             GfxRenderer& renderer) {
  Result result;
  if (!KOREADER_STORE.usesCrossPointSyncServer() || documentHash.empty() || !epub) return result;

  freeink::SecureHttpClient http;
  http.setInsecure();
  http.setReuse(true);
  http.setTimeout(12000);
  http.setUserAgent("inkMOD-KOSync/1.1-ext");

  std::vector<std::string> knownBookmarks;
  std::vector<std::string> knownClippings;
  loadKnownIds(documentHash, knownBookmarks, knownClippings);

  std::vector<std::string> finalBookmarkIds;
  std::vector<std::string> finalClippingIds;

  if (KOREADER_STORE.getSyncBookmarks()) {
    BookmarkStore localStore;
    if (!localStore.loadForBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), "epub")) {
      result.bookmarksOk = false;
    } else {
      std::vector<BookmarkWire> local;
      for (const auto& bm : localStore.getBookmarks()) {
        BookmarkWire w = encodeBookmark(bm, epub, epubPath, renderer);
        if (!w.id.empty()) local.push_back(std::move(w));
      }
      std::vector<std::string> currentIds;
      for (const auto& b : local) currentIds.push_back(b.id);
      std::vector<std::string> locallyDeleted;
      for (const auto& id : knownBookmarks) if (!containsId(currentIds, id)) locallyDeleted.push_back(id);

      std::vector<BookmarkWire> remote;
      if (!fetchBookmarks(http, documentHash, remote)) {
        result.bookmarksOk = false;
      } else {
        for (const auto& r : remote) {
          if (containsId(locallyDeleted, r.id)) continue;  // local delete wins this round
          if (r.deleted) {
            removeBookmarkById(localStore, r.id, epub, epubPath, renderer);
          } else if (!containsId(currentIds, r.id)) {
            if (upsertRemoteBookmark(localStore, r, epub, epubPath)) currentIds.push_back(r.id);
          }
        }

        local.clear();
        currentIds.clear();
        for (const auto& bm : localStore.getBookmarks()) {
          BookmarkWire w = encodeBookmark(bm, epub, epubPath, renderer);
          if (!w.id.empty()) {
            currentIds.push_back(w.id);
            local.push_back(std::move(w));
          }
        }
        for (const auto& id : locallyDeleted) {
          BookmarkWire tomb;
          tomb.id = id;
          tomb.deleted = true;
          local.push_back(std::move(tomb));
        }
        if (!putBookmarks(http, documentHash, local)) result.bookmarksOk = false;
        if (result.bookmarksOk) {
          finalBookmarkIds = currentIds;
          LOG_INF(TAG, "Bookmarks synced: %u local", static_cast<unsigned>(currentIds.size()));
        }
      }
    }
  } else {
    finalBookmarkIds = knownBookmarks;
  }

  if (KOREADER_STORE.getSyncClippings()) {
    ClippingStore localStore;
    const bool fb2 = isFb2Backed(epubPath);
    if (!localStore.loadForBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), fb2 ? "fb2" : "epub")) {
      result.clippingsOk = false;
    } else {
      std::vector<ClippingWire> local;
      std::vector<std::string> currentIds;
      for (const auto& c : localStore.getClippings()) {
        ClippingWire w;
        w.id = clippingId(c);
        w.spine = c.spineIndex;
        w.startPage = c.pageNumber;
        w.endPage = c.endPageNumber;
        w.pages = c.pageCount;
        w.startWord = c.startWordIndex;
        w.endWord = c.endWordIndex;
        w.chapter = c.chapterTitle;
        w.text = c.text;
        w.createdAt = c.timestamp;
        currentIds.push_back(w.id);
        local.push_back(std::move(w));
      }
      std::vector<std::string> locallyDeleted;
      for (const auto& id : knownClippings) if (!containsId(currentIds, id)) locallyDeleted.push_back(id);

      std::vector<ClippingWire> remote;
      if (!fetchClippings(http, documentHash, remote)) {
        result.clippingsOk = false;
      } else {
        // Apply tombstones first, from the back so vector indices remain valid.
        for (const auto& r : remote) {
          if (!r.deleted || containsId(locallyDeleted, r.id)) continue;
          const auto& clips = localStore.getClippings();
          for (size_t i = 0; i < clips.size(); ++i) {
            if (clippingId(clips[i]) == r.id) {
              localStore.removeAt(i);
              break;
            }
          }
        }
        for (const auto& r : remote) {
          if (r.deleted || containsId(locallyDeleted, r.id)) continue;
          bool exists = false;
          for (const auto& c : localStore.getClippings()) if (clippingId(c) == r.id) { exists = true; break; }
          if (exists) continue;
          Clipping c{};
          c.spineIndex = r.spine;
          c.pageNumber = r.startPage;
          c.endPageNumber = r.endPage;
          c.pageCount = std::max<uint16_t>(1, r.pages);
          c.startWordIndex = r.startWord;
          c.endWordIndex = r.endWord;
          c.timestamp = r.createdAt;
          snprintf(c.chapterTitle, sizeof(c.chapterTitle), "%s", r.chapter.c_str());
          snprintf(c.text, sizeof(c.text), "%s", r.text.c_str());
          if (!localStore.upsertFromSync(c)) {
            LOG_ERR(TAG, "Could not merge remote clipping %s", r.id.c_str());
            result.clippingsOk = false;
          }
        }

        local.clear();
        currentIds.clear();
        for (const auto& c : localStore.getClippings()) {
          ClippingWire w;
          w.id = clippingId(c);
          w.spine = c.spineIndex;
          w.startPage = c.pageNumber;
          w.endPage = c.endPageNumber;
          w.pages = c.pageCount;
          w.startWord = c.startWordIndex;
          w.endWord = c.endWordIndex;
          w.chapter = c.chapterTitle;
          w.text = c.text;
          w.createdAt = c.timestamp;
          currentIds.push_back(w.id);
          local.push_back(std::move(w));
        }
        for (const auto& id : locallyDeleted) {
          ClippingWire tomb;
          tomb.id = id;
          tomb.deleted = true;
          local.push_back(std::move(tomb));
        }
        if (!putClippings(http, documentHash, local)) result.clippingsOk = false;
        if (result.clippingsOk) {
          finalClippingIds = currentIds;
          LOG_INF(TAG, "Clippings synced: %u local", static_cast<unsigned>(currentIds.size()));
        }
      }
    }
  } else {
    finalClippingIds = knownClippings;
  }

  if (KOREADER_STORE.getSyncStats()) {
    result.statsOk = syncStats(http);
  }

  http.end();

  // Only advance local delete-tracking state for categories that completed.
  if (!result.bookmarksOk) finalBookmarkIds = knownBookmarks;
  if (!result.clippingsOk) finalClippingIds = knownClippings;
  if (!saveKnownIds(documentHash, finalBookmarkIds, finalClippingIds)) {
    LOG_ERR(TAG, "Could not persist extended-sync state");
  }
  return result;
}
