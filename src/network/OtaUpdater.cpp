#ifdef SIMULATOR
#include "OtaUpdater.h"

bool OtaUpdater::isUpdateNewer() const { return false; }
const std::string& OtaUpdater::getLatestVersion() const { return latestVersion; }
OtaUpdater::OtaUpdaterError OtaUpdater::checkForUpdate() { return NO_UPDATE; }
OtaUpdater::OtaUpdaterError OtaUpdater::installUpdate(ProgressCallback, void*, std::atomic<bool>*) { return NO_UPDATE; }
#else
#include <Logging.h>
#include <SecureHttpClient.h>
#include <mbedtls/sha256.h>

#include "FirmwareFlasher.h"
#include "HttpDownloader.h"
#include <ReleaseJsonParser.h>

#include <cstring>

#include "AppVersion.h"
#include "OtaUpdater.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "network/WifiPowerSaveGuard.h"

namespace {
#ifndef INKMOD_OTA_RELEASE_URL
#define INKMOD_OTA_RELEASE_URL "https://api.github.com/repos/alpzoloto-sudo/inkmod/releases/latest"
#endif

constexpr char latestReleaseUrl[] = INKMOD_OTA_RELEASE_URL;

#ifdef INKMOD_FIRMWARE_VARIANT
constexpr char firmwareAssetStem[] = "firmware-" INKMOD_FIRMWARE_VARIANT;
constexpr char firmwareAssetName[] = "firmware-" INKMOD_FIRMWARE_VARIANT ".bin";
#else
constexpr char firmwareAssetStem[] = "firmware";
constexpr char firmwareAssetName[] = "firmware.bin";
#endif

constexpr char binSuffix[] = ".bin";
constexpr size_t VERSION_SEGMENT_COUNT = 4;

// api.github.com currently chains to USERTrust ECC Certification Authority.
// On ESP32-C3 the ESP-IDF crt bundle can fail the signature verification step
// under memory pressure before OTA even starts. For X3/X4 we therefore fetch
// only the release metadata through the existing low-memory wolfSSL transport,
// while still verifying the server certificate against this pinned root CA.
// The actual firmware asset keeps the existing authenticated staging flow: the
// asset SHA256 comes from this verified GitHub API response and is checked
// before flashing.
constexpr char githubApiRootCa[] = R"PEM(-----BEGIN CERTIFICATE-----
MIICjzCCAhWgAwIBAgIQXIuZxVqUxdJxVt7NiYDMJjAKBggqhkjOPQQDAzCBiDELMAkGA1UEBhMC
VVMxEzARBgNVBAgTCk5ldyBKZXJzZXkxFDASBgNVBAcTC0plcnNleSBDaXR5MR4wHAYDVQQKExVU
aGUgVVNFUlRSVVNUIE5ldHdvcmsxLjAsBgNVBAMTJVVTRVJUcnVzdCBFQ0MgQ2VydGlmaWNhdGlv
biBBdXRob3JpdHkwHhcNMTAwMjAxMDAwMDAwWhcNMzgwMTE4MjM1OTU5WjCBiDELMAkGA1UEBhMC
VVMxEzARBgNVBAgTCk5ldyBKZXJzZXkxFDASBgNVBAcTC0plcnNleSBDaXR5MR4wHAYDVQQKExVU
aGUgVVNFUlRSVVNUIE5ldHdvcmsxLjAsBgNVBAMTJVVTRVJUcnVzdCBFQ0MgQ2VydGlmaWNhdGlv
biBBdXRob3JpdHkwdjAQBgcqhkjOPQIBBgUrgQQAIgNiAAQarFRaqfloI+d61SRvU8Za2EurxtW2
0eZzca7dnNYMYf3boIkDuAUU7FfO7l0/4iGzzvfUinngo4N+LZfQYcTxmdwlkWOrfzCjtHDix6Ez
nPO/LlxTsV+zfTJ/ijTjeXmjQjBAMB0GA1UdDgQWBBQ64QmG1M8ZwpZ2dEl23OA1xmNjmjAOBgNV
HQ8BAf8EBAMCAQYwDwYDVR0TAQH/BAUwAwEB/zAKBggqhkjOPQQDAwNoADBlAjA2Z6EWCNzklwBB
HU6+4WMBzzuqQhFkoJ2UOQIReVx7Hfpkue4WQrO/isIJxOzksU0CMQDpKmFHjFJKS04YcPbWRNZu
9YO6bVi9JNlWSOrvxKJGgYhqOkbRqZtNyWHa0V1Xahg=
-----END CERTIFICATE-----)PEM";

struct ParsedVersion {
  int segments[VERSION_SEGMENT_COUNT] = {0, 0, 0, 0};
  bool valid = false;
  bool releaseCandidate = false;
};

bool isDigit(const char c) { return c >= '0' && c <= '9'; }

bool startsWithNumberAfterOptionalV(const char* version) {
  if (version == nullptr) return false;
  if ((version[0] == 'v' || version[0] == 'V') && isDigit(version[1])) return true;
  return isDigit(version[0]);
}

bool containsRcMarker(const char* version) {
  if (version == nullptr) return false;
  for (const char* p = version; p[0] != '\0' && p[1] != '\0' && p[2] != '\0'; ++p) {
    if (p[0] == '-' && (p[1] == 'r' || p[1] == 'R') && (p[2] == 'c' || p[2] == 'C')) {
      return true;
    }
  }
  return false;
}

ParsedVersion parseVersion(const char* version) {
  ParsedVersion parsed;
  if (!startsWithNumberAfterOptionalV(version)) return parsed;

  const char* p = version;
  if (p[0] == 'v' || p[0] == 'V') ++p;

  size_t segmentIndex = 0;
  while (segmentIndex < VERSION_SEGMENT_COUNT) {
    if (!isDigit(*p)) return parsed;

    int value = 0;
    while (isDigit(*p)) {
      value = value * 10 + (*p - '0');
      ++p;
    }
    parsed.segments[segmentIndex] = value;
    ++segmentIndex;

    if (*p != '.') break;
    ++p;
  }

  parsed.valid = true;
  parsed.releaseCandidate = containsRcMarker(version);
  return parsed;
}

int compareVersions(const char* latestVersion, const char* currentVersion) {
  const ParsedVersion latest = parseVersion(latestVersion);
  const ParsedVersion current = parseVersion(currentVersion);
  if (!latest.valid || !current.valid) return 0;

  for (size_t i = 0; i < VERSION_SEGMENT_COUNT; ++i) {
    if (latest.segments[i] != current.segments[i]) {
      return latest.segments[i] > current.segments[i] ? 1 : -1;
    }
  }

  if (current.releaseCandidate && !latest.releaseCandidate) return 1;
  return 0;
}

bool startsWith(const char* value, const char* prefix) {
  if (value == nullptr || prefix == nullptr) return false;
  const size_t prefixLength = strlen(prefix);
  return strncmp(value, prefix, prefixLength) == 0;
}

bool endsWith(const char* value, const char* suffix) {
  if (value == nullptr || suffix == nullptr) return false;
  const size_t valueLength = strlen(value);
  const size_t suffixLength = strlen(suffix);
  if (suffixLength > valueLength) return false;
  return strcmp(value + valueLength - suffixLength, suffix) == 0;
}

bool isMatchingFirmwareAssetName(const char* assetName) {
  if (assetName == nullptr) return false;
  if (strcmp(assetName, firmwareAssetName) == 0) return true;
  if (!startsWith(assetName, firmwareAssetStem)) return false;
  if (assetName[strlen(firmwareAssetStem)] != '-') return false;
  return endsWith(assetName, binSuffix);
}

/*
 * When esp_crt_bundle.h included, it is pointing wrong header file
 * which is something under WifiClientSecure because of our framework based on arduno platform.
 * To manage this obstacle, don't include anything, just extern and it will point correct one.
 */
extern "C" {
extern esp_err_t esp_crt_bundle_attach(void* conf);
}

esp_err_t http_client_set_header_cb(esp_http_client_handle_t http_client) {
  return esp_http_client_set_header(http_client, "User-Agent", "inkMOD-ESP32-" INKMOD_VERSION);
}

size_t totalBytesReceived = 0;

constexpr const char* OTA_STAGING_PATH = "/.inkmod/ota-update.bin";

bool parseSha256Digest(const std::string& digest, uint8_t expected[32]) {
  constexpr const char* prefix = "sha256:";
  if (digest.size() != 7 + 64 || digest.compare(0, 7, prefix) != 0) return false;

  auto hexValue = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };

  for (size_t i = 0; i < 32; ++i) {
    const int hi = hexValue(digest[7 + i * 2]);
    const int lo = hexValue(digest[7 + i * 2 + 1]);
    if (hi < 0 || lo < 0) return false;
    expected[i] = static_cast<uint8_t>((hi << 4) | lo);
  }
  return true;
}

bool verifyStagedSha256(const char* path, const std::string& digest) {
  uint8_t expected[32];
  if (!parseSha256Digest(digest, expected)) {
    LOG_ERR("OTA", "No usable authenticated SHA256 digest for staging fallback");
    return false;
  }

  HalFile file;
  if (!Storage.openFileForRead("OTA", path, file) || !file) {
    LOG_ERR("OTA", "Failed to open staged firmware for SHA256 verification");
    return false;
  }

  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts(&sha, 0);
  uint8_t buffer[4096];
  while (true) {
    const int n = file.read(buffer, sizeof(buffer));
    if (n < 0) {
      file.close();
      mbedtls_sha256_free(&sha);
      LOG_ERR("OTA", "Read failure while hashing staged firmware");
      return false;
    }
    if (n == 0) break;
    mbedtls_sha256_update(&sha, buffer, static_cast<size_t>(n));
  }
  file.close();

  uint8_t actual[32];
  mbedtls_sha256_finish(&sha, actual);
  mbedtls_sha256_free(&sha);
  if (memcmp(actual, expected, sizeof(actual)) != 0) {
    LOG_ERR("OTA", "Staged firmware SHA256 does not match GitHub release digest");
    return false;
  }
  return true;
}

esp_err_t event_handler(esp_http_client_event_t* event) {
  if (event->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
  if (event->data_len <= 0) return ESP_OK;

  auto* parser = static_cast<ReleaseJsonParser*>(event->user_data);
  if (parser == nullptr) {
    LOG_ERR("OTA", "HTTP client parser missing");
    return ESP_ERR_INVALID_ARG;
  }

  totalBytesReceived += static_cast<size_t>(event->data_len);
  LOG_DBG("OTA", "HTTP chunk: %d bytes (total: %zu)", event->data_len, totalBytesReceived);
  parser->feed(static_cast<const char*>(event->data), event->data_len);
  return ESP_OK;
}
}  // namespace

OtaUpdater::OtaUpdaterError OtaUpdater::checkForUpdate() {
  WifiPowerSaveGuard wifiPowerSaveGuard;

  updateAvailable = false;
  latestVersion.clear();
  otaUrl.clear();
  otaDigest.clear();
  otaSize = 0;
  processedSize = 0;
  totalSize = 0;

  ReleaseJsonParser releaseParser(isMatchingFirmwareAssetName);
  totalBytesReceived = 0;
  LOG_DBG("OTA", "Checking for update (current: %s)", INKMOD_VERSION);

#if defined(CONFIG_IDF_TARGET_ESP32C3) && defined(FREEINK_NET_WOLFSSL)
  // X3/X4: use wolfSSL for the GitHub release API. The ESP-IDF certificate
  // bundle path can reach PK verification and then fail with an allocation
  // error on the PSRAM-less C3. wolfSSL keeps a smaller live working set.
  freeink::SecureHttpClient http;
  http.setCACert(githubApiRootCa);
  http.setTimeout(20000);
  http.setFollowRedirects(0);
  http.setAllowRedirectDowngrade(false);
  http.setReuse(false);
  http.setPreferTls12(true);
  http.setUserAgent(std::string("inkMOD-ESP32-") + INKMOD_VERSION);
  http.addHeader("Accept", "application/vnd.github+json");
  http.addHeader("Accept-Encoding", "identity");

  if (!http.begin(latestReleaseUrl)) {
    LOG_ERR("OTA", "GitHub API wolfSSL rejected URL");
    return INTERNAL_UPDATE_ERROR;
  }

  const int status = http.GET([&](const uint8_t* data, const size_t len) {
    if (len == 0) return true;
    totalBytesReceived += len;
    releaseParser.feed(reinterpret_cast<const char*>(data), len);
    return true;
  });
  const bool complete = http.responseComplete();
  http.end();

  if (status != 200 || !complete) {
    LOG_ERR("OTA", "GitHub API wolfSSL failed: status=%d complete=%d bytes=%zu", status, complete ? 1 : 0,
            totalBytesReceived);
    return HTTP_ERROR;
  }
#else
  esp_err_t esp_err;
  esp_http_client_config_t client_config = {
      .url = latestReleaseUrl,
      .event_handler = event_handler,
      .buffer_size = 4096,
      .buffer_size_tx = 1024,
      .user_data = &releaseParser,
      .skip_cert_common_name_check = true,
      .crt_bundle_attach = esp_crt_bundle_attach,
      .keep_alive_enable = true,
  };

  esp_http_client_handle_t client_handle = esp_http_client_init(&client_config);
  if (!client_handle) {
    LOG_ERR("OTA", "HTTP Client Handle Failed");
    return INTERNAL_UPDATE_ERROR;
  }

  esp_err = esp_http_client_set_header(client_handle, "User-Agent", "inkMOD-ESP32-" INKMOD_VERSION);
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_http_client_set_header Failed : %s", esp_err_to_name(esp_err));
    esp_http_client_cleanup(client_handle);
    return INTERNAL_UPDATE_ERROR;
  }

  esp_err = esp_http_client_perform(client_handle);
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_http_client_perform Failed : %s", esp_err_to_name(esp_err));
    esp_http_client_cleanup(client_handle);
    return HTTP_ERROR;
  }

  esp_err = esp_http_client_cleanup(client_handle);
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_http_client_cleanup Failed : %s", esp_err_to_name(esp_err));
    return INTERNAL_UPDATE_ERROR;
  }
#endif

  LOG_DBG("OTA", "Response received: %zu bytes total", totalBytesReceived);
  LOG_DBG("OTA", "Parser results: tag=%s firmware=%s", releaseParser.foundTag() ? "yes" : "no",
          releaseParser.foundFirmware() ? "yes" : "no");

  if (!releaseParser.foundTag()) {
    LOG_ERR("OTA", "No tag_name in release JSON");
    return JSON_PARSE_ERROR;
  }

  latestVersion = releaseParser.getTagName();

  if (!releaseParser.foundFirmware()) {
    LOG_ERR("OTA", "No matching %s asset found for release %s", firmwareAssetStem, latestVersion.c_str());
    return NO_UPDATE;
  }

  otaUrl = releaseParser.getFirmwareUrl();
  otaDigest = releaseParser.getFirmwareDigest();
  otaSize = releaseParser.getFirmwareSize();
  totalSize = otaSize;
  updateAvailable = true;

  LOG_DBG("OTA", "Found update: tag=%s size=%zu digest=%s", latestVersion.c_str(), otaSize,
          otaDigest.empty() ? "missing" : otaDigest.c_str());
  LOG_DBG("OTA", "Firmware URL: %s", otaUrl.c_str());
  return OK;
}

bool OtaUpdater::isUpdateNewer() const {
  if (!updateAvailable || latestVersion.empty() || latestVersion == INKMOD_VERSION) {
    return false;
  }

  const int comparison = compareVersions(latestVersion.c_str(), INKMOD_VERSION);
  LOG_DBG("OTA", "Version comparison latest=%s current=%s result=%d", latestVersion.c_str(), INKMOD_VERSION,
          comparison);
  return comparison > 0;
}

const std::string& OtaUpdater::getLatestVersion() const { return latestVersion; }

OtaUpdater::OtaUpdaterError OtaUpdater::installUpdate(ProgressCallback onProgress, void* ctx,
                                                      std::atomic<bool>* cancelRequested) {
  const auto isCancellationRequested = [cancelRequested]() -> bool {
    return cancelRequested != nullptr && cancelRequested->load(std::memory_order_relaxed);
  };

  if (!isUpdateNewer()) {
    return UPDATE_OLDER_ERROR;
  }

  if (isCancellationRequested()) {
    return CANCELLED_ERROR;
  }

  processedSize = 0;

  esp_https_ota_handle_t ota_handle = NULL;
  esp_err_t esp_err;

  esp_http_client_config_t client_config = {
      .url = otaUrl.c_str(),
      .timeout_ms = 15000,
      // 4096 holds the github->CDN redirect headers (the 512 default truncates
      // them); TX only carries our GET. Both are contiguous blocks contending
      // with the TLS handshake on a tight internal arena, so keep them minimal.
      .buffer_size = 4096,
      .buffer_size_tx = 1024,
      .skip_cert_common_name_check = true,
      .crt_bundle_attach = esp_crt_bundle_attach,
      // Was keep_alive_enable = true. ESP-IDF has a known, open bug
      // (espressif/esp-idf#14463) where esp_http_client's internal
      // "location" string isn't cleared between requests on a reused
      // client/connection, and a later request with a stale-but-non-null
      // old_str hits an assert in http_utils_append_string() - a hard
      // abort, not a recoverable error. Keeping the connection alive
      // across the redirect GitHub's asset URLs always involve (and any
      // retry after a transient failure, like the cert issue this method
      // has already hit once) is exactly the kind of client reuse that
      // can trigger it. A fresh connection per request costs a bit of
      // extra TLS handshake overhead, but avoids reusing that internal
      // state at all.
      .keep_alive_enable = false,
  };

  esp_https_ota_config_t ota_config = {
      .http_config = &client_config,
      .http_client_init_cb = http_client_set_header_cb,
  };

  WifiPowerSaveGuard wifiPowerSaveGuard;

  const auto installViaAuthenticatedStaging = [&]() -> OtaUpdaterError {
    // GitHub's release CDN occasionally moves to a certificate chain that the
    // ESP-IDF bundle in older inkMOD releases cannot verify. The release JSON
    // itself comes from api.github.com over verified TLS and includes GitHub's
    // SHA256 digest for each asset. We can therefore use the existing low-memory
    // wolfSSL downloader as a transport fallback, but only when that authenticated
    // digest is present and matches the complete downloaded file.
    uint8_t expectedDigest[32];
    if (!parseSha256Digest(otaDigest, expectedDigest)) {
      LOG_ERR("OTA", "TLS fallback refused: GitHub asset SHA256 digest missing/invalid");
      return HTTP_ERROR;
    }

    Storage.mkdir("/.inkmod");
    if (Storage.exists(OTA_STAGING_PATH)) Storage.remove(OTA_STAGING_PATH);

    totalSize = otaSize > 0 ? otaSize * 2 : 0;
    processedSize = 0;
    int downloadPct = -1;
    auto progress = [&](size_t downloaded, size_t total) {
      const size_t expectedTotal = otaSize > 0 ? otaSize : total;
      processedSize = downloaded;
      if (onProgress && expectedTotal > 0) {
        const int pct = static_cast<int>(static_cast<uint64_t>(downloaded) * 100 / expectedTotal);
        if (pct != downloadPct) {
          downloadPct = pct;
          onProgress(ctx);
        }
      }
    };

    // The C3 can occasionally lose a long GitHub CDN TLS stream near the end
    // (wolfSSL read error -125). Keep the authenticated partial file and resume
    // it with HTTP Range instead of throwing away 4+ MB and starting over.
    HttpDownloader::DownloadError dl = HttpDownloader::HTTP_ERROR;
    constexpr int kMaxDownloadAttempts = 4;
    for (int attempt = 0; attempt < kMaxDownloadAttempts; ++attempt) {
      if (isCancellationRequested()) {
        Storage.remove(OTA_STAGING_PATH);
        return CANCELLED_ERROR;
      }

      const bool resume = attempt > 0;
      if (resume) {
        size_t partialSize = 0;
        HalFile partial;
        if (Storage.openFileForRead("OTA", OTA_STAGING_PATH, partial) && partial) {
          partialSize = partial.fileSize();
          partial.close();
        }
        LOG_INF("OTA", "Retrying staged download %d/%d from byte %zu", attempt + 1,
                kMaxDownloadAttempts, partialSize);
        delay(500);
      }

      // preservePartial=true is essential: on a transient TLS/read failure the
      // downloader leaves the verified-by-final-SHA staging file on SD.
      HttpDownloader::DownloadOptions options(true, resume,
                                               [&]() { return isCancellationRequested(); }, 4096);
      dl = HttpDownloader::downloadToFile(otaUrl, OTA_STAGING_PATH, progress, nullptr, "", "", options);
      if (dl == HttpDownloader::OK) break;
      if (dl == HttpDownloader::ABORTED || isCancellationRequested()) {
        Storage.remove(OTA_STAGING_PATH);
        return CANCELLED_ERROR;
      }
    }

    if (dl != HttpDownloader::OK) {
      Storage.remove(OTA_STAGING_PATH);
      LOG_ERR("OTA", "Authenticated staging download failed after retries: %d", static_cast<int>(dl));
      return HTTP_ERROR;
    }

    HalFile staged;
    if (!Storage.openFileForRead("OTA", OTA_STAGING_PATH, staged) || !staged) {
      Storage.remove(OTA_STAGING_PATH);
      return INTERNAL_UPDATE_ERROR;
    }
    const size_t stagedSize = staged.fileSize();
    staged.close();
    if (otaSize > 0 && stagedSize != otaSize) {
      LOG_ERR("OTA", "Staged firmware size mismatch: got=%zu expected=%zu", stagedSize, otaSize);
      Storage.remove(OTA_STAGING_PATH);
      return HTTP_ERROR;
    }

    if (!verifyStagedSha256(OTA_STAGING_PATH, otaDigest)) {
      Storage.remove(OTA_STAGING_PATH);
      return HTTP_ERROR;
    }
    LOG_INF("OTA", "Staged firmware matches authenticated GitHub SHA256");

    struct FlashProgressCtx {
      OtaUpdater* self;
      ProgressCallback cb;
      void* cbCtx;
      size_t base;
      int lastPct;
    } flashCtx{this, onProgress, ctx, otaSize, -1};
    auto flashProgress = +[](size_t written, size_t total, void* opaque) {
      auto* p = static_cast<FlashProgressCtx*>(opaque);
      p->self->processedSize = p->base + written;
      if (!p->cb || total == 0) return;
      const int pct = static_cast<int>(static_cast<uint64_t>(written) * 100 / total);
      if (pct != p->lastPct) {
        p->lastPct = pct;
        p->cb(p->cbCtx);
      }
    };

    const auto flashResult = firmware_flash::flashFromSdPath(OTA_STAGING_PATH, flashProgress, &flashCtx);
    Storage.remove(OTA_STAGING_PATH);
    if (flashResult != firmware_flash::Result::OK) {
      LOG_ERR("OTA", "Staged firmware flash failed: %s", firmware_flash::resultName(flashResult));
      return INTERNAL_UPDATE_ERROR;
    }
    processedSize = totalSize;
    LOG_INF("OTA", "Authenticated staging OTA completed");
    return OK;
  };

#if defined(CONFIG_IDF_TARGET_ESP32C3) && defined(FREEINK_NET_WOLFSSL)
  // X3/X4 (ESP32-C3): do not try esp_https_ota first. On current GitHub/CDN
  // certificate chains the system mbedTLS path can fail with X.509 allocation
  // errors (-0x2880) or leave the socket/TLS allocator in a state where the
  // immediate wolfSSL fallback cannot even open github.com:443. The release
  // metadata above was already fetched over verified wolfSSL TLS and contains
  // GitHub's SHA256 digest for the selected asset, so go straight to the
  // authenticated SD staging path and verify that digest before flashing.
  LOG_INF("OTA", "C3: using authenticated wolfSSL staging OTA directly");
  return installViaAuthenticatedStaging();
#else
  esp_err = esp_https_ota_begin(&ota_config, &ota_handle);
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "HTTP OTA Begin Failed: %s; trying authenticated staging fallback", esp_err_to_name(esp_err));
    return installViaAuthenticatedStaging();
  }
#endif

  int lastReportedPct = -1;
  do {
    if (isCancellationRequested()) {
      LOG_INF("OTA", "Update cancelled");
      esp_https_ota_abort(ota_handle);
      return CANCELLED_ERROR;
    }

    esp_err = esp_https_ota_perform(ota_handle);
    processedSize = esp_https_ota_get_image_len_read(ota_handle);
    // Fire the callback only on whole-percent change. Without this it fired
    // every ~100ms perform iteration, waking the render task whose framebuffer
    // work contends with TLS on the same internal arena. E-ink can't repaint
    // faster than a percent tick anyway.
    if (onProgress && totalSize > 0) {
      const int pct = static_cast<int>(static_cast<uint64_t>(processedSize) * 100 / totalSize);
      if (pct != lastReportedPct) {
        lastReportedPct = pct;
        onProgress(ctx);
      }
    }
    delay(100);  // TODO: should we replace this with something better?
  } while (esp_err == ESP_ERR_HTTPS_OTA_IN_PROGRESS);

  if (isCancellationRequested()) {
    LOG_INF("OTA", "Update cancelled");
    esp_https_ota_abort(ota_handle);
    return CANCELLED_ERROR;
  }

  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_https_ota_perform Failed: %s; trying authenticated staging fallback", esp_err_to_name(esp_err));
    esp_https_ota_abort(ota_handle);
    processedSize = 0;
    return installViaAuthenticatedStaging();
  }

  if (!esp_https_ota_is_complete_data_received(ota_handle)) {
    LOG_ERR("OTA", "esp_https_ota_is_complete_data_received Failed: %s", esp_err_to_name(esp_err));
    esp_https_ota_finish(ota_handle);
    return INTERNAL_UPDATE_ERROR;
  }

  esp_err = esp_https_ota_finish(ota_handle);
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_https_ota_finish Failed: %s", esp_err_to_name(esp_err));
    return INTERNAL_UPDATE_ERROR;
  }

  LOG_INF("OTA", "Update completed");
  return OK;
}
#endif
