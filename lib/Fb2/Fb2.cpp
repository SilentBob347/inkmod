#include "Fb2.h"

#include <Logging.h>
#include <ZipFile.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <limits>

#include "Fb2Encoding.h"

namespace {
constexpr size_t XML_CHUNK_SIZE = 4096;
// EpubReader indexes one spine item at a time. Real-world FB2 generators
// sometimes put an entire novel in one <section>; split such content at safe
// paragraph boundaries so opening a chapter remains fast on ESP32-C3.
constexpr size_t MAX_CHAPTER_TEXT_BYTES = 48 * 1024;
constexpr uint8_t PACKAGE_VERSION = 3;
constexpr char METADATA_FILE[] = "/fb2_metadata.txt";
constexpr char PACKAGE_STATE_FILE[] = "/fb2_package.bin";
constexpr char TOC_RECORDS_FILE[] = "/.fb2_toc.bin";
constexpr char ANCHOR_RECORDS_FILE[] = "/.fb2_anchors.bin";

const char* localName(const char* name) {
  const char* colon = name ? strrchr(name, ':') : nullptr;
  return colon ? colon + 1 : (name ? name : "");
}

const char* getAttribute(const XML_Char** atts, const char* expected) {
  if (!atts) return nullptr;
  for (int i = 0; atts[i]; i += 2) {
    if (strcmp(localName(atts[i]), expected) == 0) return atts[i + 1];
  }
  return nullptr;
}

void normalizeText(std::string& value) {
  std::string normalized;
  normalized.reserve(value.size());
  bool pendingSpace = false;
  for (const unsigned char c : value) {
    if (std::isspace(c)) {
      pendingSpace = !normalized.empty();
      continue;
    }
    if (pendingSpace) normalized.push_back(' ');
    normalized.push_back(static_cast<char>(c));
    pendingSpace = false;
  }
  value.swap(normalized);
}

bool containsNonWhitespace(const char* text, size_t length) {
  for (size_t i = 0; i < length; ++i) {
    if (!std::isspace(static_cast<unsigned char>(text[i]))) return true;
  }
  return false;
}

uint64_t fnvHash64(const char* data, size_t length) {
  uint64_t hash = 14695981039346656037ull;
  for (size_t i = 0; i < length; ++i) {
    hash ^= static_cast<uint8_t>(data[i]);
    hash *= 1099511628211ull;
  }
  return hash;
}

uint64_t hashString(const std::string& value) { return fnvHash64(value.data(), value.size()); }

std::string anchorName(uint64_t hash) {
  constexpr char HEX_DIGITS[] = "0123456789abcdef";
  std::string result = "fb2-";
  result.resize(20);
  for (int i = 0; i < 16; ++i) {
    result[4 + i] = HEX_DIGITS[(hash >> ((15 - i) * 4)) & 0x0f];
  }
  return result;
}

uint64_t automaticAnchor(const char* type, int serial) {
  const std::string value = std::string(type) + ":" + std::to_string(serial);
  return hashString(value);
}

std::string chapterHref(int index) { return "text/chapter_" + std::to_string(index) + ".xhtml"; }

std::string chapterLink(int index) { return "chapter_" + std::to_string(index) + ".xhtml"; }

std::string lowercase(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

std::string normalizeImageMediaType(const char* value) {
  const std::string mediaType = lowercase(value ? value : "");
  if (mediaType == "image/jpeg" || mediaType == "image/jpg" || mediaType == "image/pjpeg") return "image/jpeg";
  if (mediaType == "image/png" || mediaType == "image/x-png") return "image/png";
  return {};
}

void writeBytes(HalFile& file, const char* data, size_t length) {
  if (length > 0) file.write(data, length);
}

void writeBytes(HalFile& file, const std::string& value) { writeBytes(file, value.data(), value.size()); }

void writeBytes(HalFile& file, const char* value) { writeBytes(file, value, strlen(value)); }

void writeXmlEscaped(HalFile& file, const char* text, size_t length, bool attribute = false) {
  size_t start = 0;
  for (size_t i = 0; i < length; ++i) {
    const char* replacement = nullptr;
    switch (text[i]) {
      case '&':
        replacement = "&amp;";
        break;
      case '<':
        replacement = "&lt;";
        break;
      case '>':
        replacement = "&gt;";
        break;
      case '"':
        if (attribute) replacement = "&quot;";
        break;
      case '\'':
        if (attribute) replacement = "&apos;";
        break;
      default:
        break;
    }
    if (!replacement) continue;
    writeBytes(file, text + start, i - start);
    writeBytes(file, replacement, strlen(replacement));
    start = i + 1;
  }
  writeBytes(file, text + start, length - start);
}

void writeXmlEscaped(HalFile& file, const std::string& value, bool attribute = false) {
  writeXmlEscaped(file, value.data(), value.size(), attribute);
}

bool writeStaticFile(const std::string& path, const char* contents) {
  HalFile file;
  if (!Storage.openFileForWrite("FB2", path, file)) return false;
  const size_t length = strlen(contents);
  const bool success = file.write(contents, length) == length;
  file.close();
  return success;
}

int base64Value(unsigned char value) {
  if (value >= 'A' && value <= 'Z') return value - 'A';
  if (value >= 'a' && value <= 'z') return value - 'a' + 26;
  if (value >= '0' && value <= '9') return value - '0' + 52;
  if (value == '+') return 62;
  if (value == '/') return 63;
  if (value == '=') return 64;
  return -1;
}

// Bounds how many fully-converted FB2 -> EPUB package caches are kept on disk
// at once. FB2 source files stay untouched in the library; only the unpacked
// package copy is subject to this budget, evicted least-recently-used first.
constexpr int MAX_CACHED_FB2_PACKAGES = 5;
constexpr char LRU_INDEX_FILE[] = "/.fb2_lru_index";

std::vector<std::string> readLruIndex(const std::string& path) {
  std::vector<std::string> keys;
  char buffer[1024];
  const size_t length = Storage.readFileToBuffer(path.c_str(), buffer, sizeof(buffer) - 1);
  if (length == 0) return keys;
  buffer[length] = '\0';
  size_t start = 0;
  for (size_t i = 0; i <= length; ++i) {
    if (i == length || buffer[i] == '\n') {
      if (i > start) keys.emplace_back(buffer + start, i - start);
      start = i + 1;
    }
  }
  return keys;
}

void writeLruIndex(const std::string& path, const std::vector<std::string>& keys) {
  HalFile file;
  if (!Storage.openFileForWrite("FB2", path, file)) return;
  for (const auto& key : keys) {
    writeBytes(file, key);
    file.write(static_cast<uint8_t>('\n'));
  }
  file.close();
}
}  // namespace

Fb2::Fb2(std::string path, std::string cacheBasePath) : filepath(std::move(path)) {
  const std::string key = std::to_string(std::hash<std::string>{}(filepath));
  cacheKey = "epub_" + key;
  cacheBaseDir = cacheBasePath;
  cachePath = cacheBaseDir + "/" + cacheKey;
  legacyCachePath = std::move(cacheBasePath) + "/fb2_" + key;
  packagePath = cachePath + "/package.epub";
  sourcePath = filepath;

  const size_t slash = filepath.find_last_of('/');
  const size_t start = slash == std::string::npos ? 0 : slash + 1;
  const size_t dot = filepath.find_last_of('.');
  title = filepath.substr(start, dot == std::string::npos || dot <= start ? std::string::npos : dot - start);
}

bool Fb2::isCompressedFb2() const {
  const std::string name = lowercase(filepath);
  return name.size() >= 8 && name.compare(name.size() - 8, 8, ".fb2.zip") == 0;
}

bool Fb2::prepareSource() {
  sourcePath = filepath;
  temporarySourcePath.clear();
  if (!isCompressedFb2()) return true;

  ZipFile archive(filepath);
  std::string fb2Entry;
  if (!archive.enumerateFilePaths([&](const std::string_view entry) {
        const std::string name = lowercase(std::string(entry));
        if (fb2Entry.empty() && name.size() >= 4 && name.compare(name.size() - 4, 4, ".fb2") == 0) {
          fb2Entry.assign(entry);
        }
      }) ||
      fb2Entry.empty()) {
    LOG_ERR("FB2", "No FB2 file found in archive: %s", filepath.c_str());
    return false;
  }

  // The temporary source lives next to the package directory, because
  // convertToPackage() rebuilds that directory from scratch.
  setupCacheDir();
  temporarySourcePath = cachePath + ".source.fb2";
  Storage.remove(temporarySourcePath.c_str());
  HalFile extracted;
  if (!Storage.openFileForWrite("FB2", temporarySourcePath, extracted)) return false;
  const bool extractedOk = archive.readFileToStream(fb2Entry.c_str(), extracted, XML_CHUNK_SIZE);
  extracted.close();
  if (!extractedOk) {
    Storage.remove(temporarySourcePath.c_str());
    LOG_ERR("FB2", "Could not extract %s from archive", fb2Entry.c_str());
    return false;
  }
  sourcePath = temporarySourcePath;
  return true;
}

Fb2::~Fb2() {
  if (parser) XML_ParserFree(parser);
}

void Fb2::setupCacheDir() const { Storage.mkdir(cachePath.c_str(), true); }

bool Fb2::cacheIsCurrent() {
  if (!Storage.exists((packagePath + "/META-INF/container.xml").c_str()) ||
      !Storage.exists((packagePath + "/OEBPS/content.opf").c_str())) {
    return false;
  }

  HalFile state;
  if (!Storage.openFileForRead("FB2", cachePath + PACKAGE_STATE_FILE, state)) return false;

  uint8_t version = 0;
  uint64_t cachedSize = 0;
  uint16_t cachedChapters = 0;
  const bool valid = state.read(&version, sizeof(version)) == sizeof(version) &&
                     state.read(&cachedSize, sizeof(cachedSize)) == sizeof(cachedSize) &&
                     state.read(&cachedChapters, sizeof(cachedChapters)) == sizeof(cachedChapters) &&
                     version == PACKAGE_VERSION && cachedSize == sourceSize && cachedChapters > 0;
  state.close();
  if (valid) chapterCount = cachedChapters;
  return valid;
}

bool Fb2::loadMetadataCache() {
  char buffer[1536];
  const size_t length = Storage.readFileToBuffer((cachePath + METADATA_FILE).c_str(), buffer, sizeof(buffer));
  if (length == 0) return false;

  const char* first = strchr(buffer, '\n');
  if (!first) return false;
  const char* second = strchr(first + 1, '\n');
  if (!second) return false;
  title.assign(buffer, first - buffer);
  author.assign(first + 1, second - first - 1);
  language.assign(second + 1);
  while (!language.empty() && (language.back() == '\n' || language.back() == '\r')) language.pop_back();
  if (language.empty()) language = "und";
  return !title.empty();
}

void Fb2::saveMetadataCache() const {
  HalFile metadata;
  if (!Storage.openFileForWrite("FB2", cachePath + METADATA_FILE, metadata)) return;
  writeBytes(metadata, title);
  metadata.write(static_cast<uint8_t>('\n'));
  writeBytes(metadata, author);
  metadata.write(static_cast<uint8_t>('\n'));
  writeBytes(metadata, language);
  metadata.write(static_cast<uint8_t>('\n'));
  metadata.close();
}

void Fb2::saveCacheSignature() const {
  HalFile state;
  if (!Storage.openFileForWrite("FB2", cachePath + PACKAGE_STATE_FILE, state)) return;
  const uint8_t version = PACKAGE_VERSION;
  const uint16_t chapters = static_cast<uint16_t>(std::min(chapterCount, static_cast<int>(UINT16_MAX)));
  state.write(&version, sizeof(version));
  state.write(&sourceSize, sizeof(sourceSize));
  state.write(&chapters, sizeof(chapters));
  state.close();
}

void Fb2::maintainCacheBudget() const {
  const std::string indexPath = cacheBaseDir + LRU_INDEX_FILE;
  std::vector<std::string> keys = readLruIndex(indexPath);

  keys.erase(std::remove(keys.begin(), keys.end(), cacheKey), keys.end());
  keys.insert(keys.begin(), cacheKey);

  while (static_cast<int>(keys.size()) > MAX_CACHED_FB2_PACKAGES) {
    const std::string evictKey = keys.back();
    keys.pop_back();
    const std::string evictPath = cacheBaseDir + "/" + evictKey;
    if (evictPath != cachePath && Storage.exists(evictPath.c_str())) {
      Storage.removeDir(evictPath.c_str());
      LOG_INF("FB2", "Evicted FB2 package cache: %s", evictKey.c_str());
    }
  }

  writeLruIndex(indexPath, keys);
}

bool Fb2::load() {
  if (loaded) return true;
  if (!Storage.exists(filepath.c_str())) {
    LOG_ERR("FB2", "File does not exist: %s", filepath.c_str());
    return false;
  }

  HalFile source;
  if (!Storage.openFileForRead("FB2", filepath, source)) return false;
  sourceSize = source.fileSize64();
  source.close();

  if (cacheIsCurrent() && loadMetadataCache()) {
    loaded = true;
    LOG_INF("FB2", "Loaded cached FB2 package: %d chapters", chapterCount);
    maintainCacheBudget();
    return true;
  }

  LOG_INF("FB2", "Building FB2 package: %llu bytes", static_cast<unsigned long long>(sourceSize));
  if (!prepareSource()) return false;
  const bool converted = convertToPackage();
  if (!temporarySourcePath.empty()) Storage.remove(temporarySourcePath.c_str());
  if (!converted) return false;
  loaded = true;
  LOG_INF("FB2", "Built FB2 package: %d chapters, %zu images", chapterCount, images.size());
  maintainCacheBudget();
  return true;
}

bool Fb2::convertToPackage() {
  // A package-version change invalidates Epub metadata/section caches too, so
  // rebuild the shared per-book cache as one coherent unit.
  if (Storage.exists(cachePath.c_str())) Storage.removeDir(cachePath.c_str());
  setupCacheDir();
  Storage.mkdir((packagePath + "/META-INF").c_str(), true);
  Storage.mkdir((packagePath + "/OEBPS/text").c_str(), true);
  Storage.mkdir((packagePath + "/OEBPS/images").c_str(), true);

  const auto fail = [this]() {
    if (Storage.exists(cachePath.c_str())) Storage.removeDir(cachePath.c_str());
    return false;
  };

  if (!Storage.openFileForWrite("FB2", cachePath + TOC_RECORDS_FILE, tocRecords) ||
      !Storage.openFileForWrite("FB2", cachePath + ANCHOR_RECORDS_FILE, anchorRecords)) {
    return fail();
  }

  if (!parseSource(ParsePass::Scan)) {
    tocRecords.close();
    anchorRecords.close();
    return fail();
  }
  tocRecords.close();
  anchorRecords.close();
  postProcessMetadata();

  if (chapterCount <= 0 || chapterCount > UINT16_MAX) {
    LOG_ERR("FB2", "FB2 has no readable chapters or too many chapters: %d", chapterCount);
    return fail();
  }

  if (!parseSource(ParsePass::Render) || nextRenderChapter != chapterCount) {
    LOG_ERR("FB2", "FB2 content render failed (%d/%d chapters)", nextRenderChapter, chapterCount);
    return fail();
  }

  if (!writeContainerFile() || !writeStyleFile() || !writeOpfFile() || !writeNcxFile()) return fail();

  saveMetadataCache();
  saveCacheSignature();
  Storage.remove((cachePath + TOC_RECORDS_FILE).c_str());
  Storage.remove((cachePath + ANCHOR_RECORDS_FILE).c_str());
  return true;
}

void Fb2::resetParserState(ParsePass parsePass) {
  pass = parsePass;
  depth = 0;
  titleInfoDepth = INT_MAX;
  authorDepth = INT_MAX;
  bodyDepth = INT_MAX;
  titleElementDepth = INT_MAX;
  binaryDepth = INT_MAX;
  sectionLevel = 0;
  sectionSerial = 0;
  titleSerial = 0;
  currentChapter = -1;
  nextRenderChapter = 0;
  chapterTextBytes = 0;
  titleParagraphCount = 0;
  chapterOpen = false;
  inBookTitle = false;
  inFirstName = false;
  inMiddleName = false;
  inLastName = false;
  inNickname = false;
  inLanguage = false;
  inCoverpage = false;
  binaryWriteOk = true;
  binaryOutputOpen = false;
  base64Count = 0;
  activeBinaryPath.clear();
  tocTitle.clear();
  sectionAnchors.clear();

  if (parsePass == ParsePass::Scan) {
    chapterCount = 0;
    author.clear();
    language = "und";
    coverImageId.clear();
    authorFirst.clear();
    authorMiddle.clear();
    authorLast.clear();
    authorNickname.clear();
    images.clear();
  }
}

bool Fb2::parseSource(ParsePass parsePass) {
  resetParserState(parsePass);
  HalFile source;
  if (!Storage.openFileForRead("FB2", sourcePath, source)) return false;

  parser = XML_ParserCreate(nullptr);
  if (!parser) {
    source.close();
    return false;
  }
  XML_SetUserData(parser, this);
  XML_SetElementHandler(parser, startElement, endElement);
  XML_SetCharacterDataHandler(parser, characterData);
  XML_SetUnknownEncodingHandler(parser, unknownEncoding, nullptr);

  uint8_t buffer[XML_CHUNK_SIZE];
  bool success = true;
  while (source.available() > 0) {
    const int bytesRead = source.read(buffer, sizeof(buffer));
    if (bytesRead <= 0) {
      success = false;
      break;
    }
    const bool isFinal = source.available() == 0;
    if (XML_Parse(parser, reinterpret_cast<const char*>(buffer), bytesRead, isFinal) == XML_STATUS_ERROR) {
      LOG_ERR("FB2", "XML error at line %lu: %s", static_cast<unsigned long>(XML_GetCurrentLineNumber(parser)),
              XML_ErrorString(XML_GetErrorCode(parser)));
      success = false;
      break;
    }
  }

  source.close();
  if (parser) {
    XML_ParserFree(parser);
    parser = nullptr;
  }
  if (chapterOpen) closeChapter();
  if (binaryOutputOpen) endBinary();
  return success;
}

int Fb2::ensureScanChapter() {
  if (currentChapter < 0) {
    currentChapter = chapterCount++;
    chapterTextBytes = 0;
  }
  return currentChapter;
}

bool Fb2::ensureRenderChapter() {
  if (chapterOpen) return true;
  currentChapter = nextRenderChapter++;
  chapterTextBytes = 0;
  return openChapter(currentChapter);
}

bool Fb2::openChapter(int index) {
  const std::string path = packagePath + "/OEBPS/" + chapterHref(index);
  if (!Storage.openFileForWrite("FB2", path, output)) return false;
  chapterOpen = true;
  writeLiteral("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
  writeLiteral("<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><meta charset=\"UTF-8\"/>");
  writeLiteral("<link rel=\"stylesheet\" type=\"text/css\" href=\"../style.css\"/><title>");
  writeEscaped(title.data(), title.size());
  writeLiteral("</title></head><body>\n");
  return true;
}

void Fb2::closeChapter() {
  if (!chapterOpen) return;
  writeLiteral("\n</body></html>\n");
  output.close();
  chapterOpen = false;
  currentChapter = -1;
}

void Fb2::writeEscaped(const char* text, size_t length, bool attribute) {
  if (chapterOpen) writeXmlEscaped(output, text, length, attribute);
}

void Fb2::writeString(const std::string& value) {
  if (chapterOpen) writeBytes(output, value);
}

void Fb2::writeLiteral(const char* value) {
  if (chapterOpen) writeBytes(output, value, strlen(value));
}

void Fb2::writeElementId(const XML_Char** atts) {
  const char* id = getAttribute(atts, "id");
  if (!id || id[0] == '\0') return;
  writeLiteral(" id=\"");
  const std::string anchor = anchorName(fnvHash64(id, strlen(id)));
  writeEscaped(anchor.data(), anchor.size(), true);
  writeLiteral("\"");
}

void Fb2::recordAnchor(const std::string& id, uint16_t chapter) {
  if (id.empty()) return;
  const uint64_t hash = hashString(id);
  const uint16_t length = static_cast<uint16_t>(std::min(id.size(), static_cast<size_t>(UINT16_MAX)));
  anchorRecords.write(&hash, sizeof(hash));
  anchorRecords.write(&length, sizeof(length));
  anchorRecords.write(&chapter, sizeof(chapter));
}

bool Fb2::findAnchorChapter(const std::string& id, uint16_t& chapter) const {
  HalFile records;
  if (!Storage.openFileForRead("FB2", cachePath + ANCHOR_RECORDS_FILE, records)) return false;
  const uint64_t wantedHash = hashString(id);
  const uint16_t wantedLength = static_cast<uint16_t>(std::min(id.size(), static_cast<size_t>(UINT16_MAX)));
  uint64_t hash = 0;
  uint16_t length = 0;
  uint16_t storedChapter = 0;
  bool found = false;
  while (records.available() > 0) {
    if (records.read(&hash, sizeof(hash)) != sizeof(hash) || records.read(&length, sizeof(length)) != sizeof(length) ||
        records.read(&storedChapter, sizeof(storedChapter)) != sizeof(storedChapter)) {
      break;
    }
    if (hash == wantedHash && length == wantedLength) {
      chapter = storedChapter;
      found = true;
      break;
    }
  }
  records.close();
  return found;
}

void Fb2::writeTocRecord() {
  normalizeText(tocTitle);
  if (tocTitle.empty()) return;
  const uint16_t length = static_cast<uint16_t>(std::min(tocTitle.size(), static_cast<size_t>(4096)));
  tocRecords.write(&tocLevel, sizeof(tocLevel));
  tocRecords.write(&tocChapter, sizeof(tocChapter));
  tocRecords.write(&tocAnchor, sizeof(tocAnchor));
  tocRecords.write(&length, sizeof(length));
  tocRecords.write(tocTitle.data(), length);
}

const Fb2::ImageInfo* Fb2::findImage(const std::string& id) const {
  const auto it = std::find_if(images.begin(), images.end(), [&](const ImageInfo& image) { return image.id == id; });
  return it == images.end() ? nullptr : &*it;
}

void Fb2::beginBinary(const XML_Char** atts) {
  binaryDepth = depth;
  base64Count = 0;
  binaryWriteOk = true;
  binaryOutputOpen = false;
  activeBinaryPath.clear();
  const char* idValue = getAttribute(atts, "id");
  if (!idValue) return;
  const ImageInfo* image = findImage(idValue);
  if (!image) return;
  activeBinaryPath = packagePath + "/OEBPS/images/" + image->filename;
  binaryOutputOpen = Storage.openFileForWrite("FB2", activeBinaryPath, binaryOutput);
  binaryWriteOk = binaryOutputOpen;
}

void Fb2::feedBase64(const char* text, size_t length) {
  if (!binaryOutputOpen || !binaryWriteOk) return;
  for (size_t i = 0; i < length; ++i) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    if (std::isspace(c)) continue;
    const int value = base64Value(c);
    if (value < 0) {
      binaryWriteOk = false;
      return;
    }
    base64Quartet[base64Count++] = static_cast<uint8_t>(value);
    if (base64Count != 4) continue;

    if (base64Quartet[0] >= 64 || base64Quartet[1] >= 64) {
      binaryWriteOk = false;
      return;
    }
    uint8_t decoded[3];
    size_t decodedCount = 1;
    decoded[0] = static_cast<uint8_t>((base64Quartet[0] << 2) | (base64Quartet[1] >> 4));
    if (base64Quartet[2] < 64) {
      decoded[1] = static_cast<uint8_t>((base64Quartet[1] << 4) | (base64Quartet[2] >> 2));
      decodedCount = 2;
      if (base64Quartet[3] < 64) {
        decoded[2] = static_cast<uint8_t>((base64Quartet[2] << 6) | base64Quartet[3]);
        decodedCount = 3;
      }
    }
    if (binaryOutput.write(decoded, decodedCount) != decodedCount) {
      binaryWriteOk = false;
      return;
    }
    base64Count = 0;
  }
}

void Fb2::endBinary() {
  if (binaryOutputOpen) {
    if (base64Count != 0) binaryWriteOk = false;
    binaryOutput.close();
    binaryOutputOpen = false;
    if (!binaryWriteOk && !activeBinaryPath.empty()) Storage.remove(activeBinaryPath.c_str());
  }
  base64Count = 0;
  binaryDepth = INT_MAX;
  activeBinaryPath.clear();
}

void XMLCALL Fb2::startElement(void* userData, const XML_Char* name, const XML_Char** atts) {
  auto* self = static_cast<Fb2*>(userData);
  self->depth++;
  const char* tag = localName(name);

  if (self->pass == ParsePass::Scan) {
    if (strcmp(tag, "title-info") == 0) {
      self->titleInfoDepth = self->depth;
    } else if (self->titleInfoDepth != INT_MAX && strcmp(tag, "book-title") == 0) {
      self->inBookTitle = true;
      self->title.clear();
    } else if (self->titleInfoDepth != INT_MAX && strcmp(tag, "author") == 0) {
      self->authorDepth = self->depth;
      self->authorFirst.clear();
      self->authorMiddle.clear();
      self->authorLast.clear();
      self->authorNickname.clear();
    } else if (self->authorDepth != INT_MAX && strcmp(tag, "first-name") == 0) {
      self->inFirstName = true;
    } else if (self->authorDepth != INT_MAX && strcmp(tag, "middle-name") == 0) {
      self->inMiddleName = true;
    } else if (self->authorDepth != INT_MAX && strcmp(tag, "last-name") == 0) {
      self->inLastName = true;
    } else if (self->authorDepth != INT_MAX && strcmp(tag, "nickname") == 0) {
      self->inNickname = true;
    } else if (self->titleInfoDepth != INT_MAX && strcmp(tag, "lang") == 0) {
      self->inLanguage = true;
      self->language.clear();
    } else if (self->titleInfoDepth != INT_MAX && strcmp(tag, "coverpage") == 0) {
      self->inCoverpage = true;
    }

    if (self->inCoverpage && strcmp(tag, "image") == 0) {
      const char* href = getAttribute(atts, "href");
      if (href) self->coverImageId = href[0] == '#' ? href + 1 : href;
    }

    if (strcmp(tag, "binary") == 0) {
      self->binaryDepth = self->depth;
      const char* idValue = getAttribute(atts, "id");
      const std::string mediaType = normalizeImageMediaType(getAttribute(atts, "content-type"));
      if (idValue && !mediaType.empty() && !self->findImage(idValue)) {
        ImageInfo image;
        image.id = idValue;
        image.mediaType = mediaType;
        image.filename = "image_" + std::to_string(self->images.size()) +
                         (mediaType == "image/png" ? ".png" : ".jpg");
        self->images.push_back(std::move(image));
      }
      return;
    }

    if (strcmp(tag, "body") == 0) {
      self->bodyDepth = self->depth;
      self->currentChapter = -1;
      self->sectionLevel = 0;
      return;
    }
    if (self->bodyDepth == INT_MAX) return;

    if (strcmp(tag, "section") == 0) {
      self->sectionLevel++;
      if (self->sectionLevel == 1) {
        self->currentChapter = self->chapterCount++;
        self->chapterTextBytes = 0;
      } else {
        self->ensureScanChapter();
      }
      self->sectionSerial++;
      const char* id = getAttribute(atts, "id");
      const uint64_t anchor = id && id[0] ? fnvHash64(id, strlen(id)) : automaticAnchor("section", self->sectionSerial);
      self->sectionAnchors.push_back(anchor);
      if (id && id[0]) self->recordAnchor(id, static_cast<uint16_t>(self->currentChapter));
      return;
    }

    const bool directParagraph = strcmp(tag, "p") == 0 &&
                                 ((self->sectionLevel == 1 && self->depth == self->bodyDepth + 2) ||
                                  (self->sectionLevel == 0 && self->depth == self->bodyDepth + 1));
    if (directParagraph && self->currentChapter >= 0 && self->chapterTextBytes >= MAX_CHAPTER_TEXT_BYTES) {
      self->currentChapter = self->chapterCount++;
      self->chapterTextBytes = 0;
    }

    const char* id = getAttribute(atts, "id");
    const bool startsContent = strcmp(tag, "title") == 0 || strcmp(tag, "p") == 0 || strcmp(tag, "subtitle") == 0 ||
                               strcmp(tag, "poem") == 0 || strcmp(tag, "cite") == 0 ||
                               strcmp(tag, "epigraph") == 0 || strcmp(tag, "image") == 0 ||
                               strcmp(tag, "empty-line") == 0 || id != nullptr;
    if (startsContent) self->ensureScanChapter();
    if (id && id[0]) self->recordAnchor(id, static_cast<uint16_t>(self->ensureScanChapter()));

    if (strcmp(tag, "title") == 0) {
      self->titleSerial++;
      self->titleElementDepth = self->depth;
      self->tocTitle.clear();
      self->tocLevel = static_cast<uint8_t>(std::clamp(self->sectionLevel, 1, 255));
      self->tocChapter = static_cast<uint16_t>(self->ensureScanChapter());
      self->tocAnchor = self->sectionAnchors.empty() ? automaticAnchor("title", self->titleSerial)
                                                     : self->sectionAnchors.back();
    }
    return;
  }

  // Render pass
  if (strcmp(tag, "binary") == 0) {
    self->beginBinary(atts);
    return;
  }
  if (strcmp(tag, "body") == 0) {
    self->bodyDepth = self->depth;
    self->currentChapter = -1;
    self->sectionLevel = 0;
    return;
  }
  if (self->bodyDepth == INT_MAX) return;

  if (strcmp(tag, "section") == 0) {
    self->sectionLevel++;
    if (self->sectionLevel == 1) {
      self->closeChapter();
      if (!self->ensureRenderChapter()) return;
    } else if (!self->ensureRenderChapter()) {
      return;
    }
    self->sectionSerial++;
    const char* id = getAttribute(atts, "id");
    const uint64_t anchor = id && id[0] ? fnvHash64(id, strlen(id)) : automaticAnchor("section", self->sectionSerial);
    self->sectionAnchors.push_back(anchor);
    self->writeLiteral("<section id=\"");
    const std::string value = anchorName(anchor);
    self->writeEscaped(value.data(), value.size(), true);
    self->writeLiteral("\">");
    return;
  }

  const bool directParagraph = strcmp(tag, "p") == 0 &&
                               ((self->sectionLevel == 1 && self->depth == self->bodyDepth + 2) ||
                                (self->sectionLevel == 0 && self->depth == self->bodyDepth + 1));
  if (directParagraph && self->chapterOpen && self->chapterTextBytes >= MAX_CHAPTER_TEXT_BYTES) {
    if (self->sectionLevel == 1) self->writeLiteral("</section>");
    self->closeChapter();
    if (!self->ensureRenderChapter()) return;
    if (self->sectionLevel == 1) self->writeLiteral("<section class=\"continuation\">");
  }

  const bool isEmptyLine = strcmp(tag, "empty-line") == 0;
  if (!self->ensureRenderChapter()) return;

  if (strcmp(tag, "title") == 0) {
    self->titleSerial++;
    self->titleElementDepth = self->depth;
    self->titleParagraphCount = 0;
    const int heading = std::clamp(self->sectionLevel, 1, 6);
    self->writeString("<h" + std::to_string(heading));
    if (self->sectionAnchors.empty()) {
      self->writeLiteral(" id=\"");
      const std::string value = anchorName(automaticAnchor("title", self->titleSerial));
      self->writeEscaped(value.data(), value.size(), true);
      self->writeLiteral("\"");
    }
    self->writeLiteral(">");
  } else if (strcmp(tag, "p") == 0) {
    if (self->titleElementDepth != INT_MAX) {
      if (self->titleParagraphCount++ > 0) self->writeLiteral("<br/>");
    } else {
      self->writeLiteral("<p");
      self->writeElementId(atts);
      self->writeLiteral(">");
    }
  } else if (strcmp(tag, "subtitle") == 0) {
    self->writeLiteral("<h3 class=\"subtitle\"");
    self->writeElementId(atts);
    self->writeLiteral(">");
  } else if (strcmp(tag, "emphasis") == 0) {
    self->writeLiteral("<em>");
  } else if (strcmp(tag, "strong") == 0) {
    self->writeLiteral("<strong>");
  } else if (strcmp(tag, "strikethrough") == 0) {
    self->writeLiteral("<span class=\"strike\">");
  } else if (strcmp(tag, "code") == 0) {
    self->writeLiteral("<span class=\"code\">");
  } else if (strcmp(tag, "sup") == 0 || strcmp(tag, "sub") == 0) {
    self->writeString(std::string("<") + tag + ">");
  } else if (strcmp(tag, "poem") == 0 || strcmp(tag, "stanza") == 0 || strcmp(tag, "annotation") == 0) {
    self->writeString(std::string("<div class=\"") + tag + "\"");
    self->writeElementId(atts);
    self->writeLiteral(">");
  } else if (strcmp(tag, "cite") == 0 || strcmp(tag, "epigraph") == 0) {
    self->writeString(std::string("<blockquote class=\"") + tag + "\"");
    self->writeElementId(atts);
    self->writeLiteral(">");
  } else if (strcmp(tag, "v") == 0 || strcmp(tag, "text-author") == 0 || strcmp(tag, "date") == 0) {
    self->writeString(std::string("<p class=\"") + tag + "\"");
    self->writeElementId(atts);
    self->writeLiteral(">");
  } else if (isEmptyLine) {
    self->writeLiteral("<p class=\"empty-line\">&#160;</p>");
  } else if (strcmp(tag, "image") == 0) {
    const char* href = getAttribute(atts, "href");
    const std::string id = href ? (href[0] == '#' ? href + 1 : href) : "";
    const ImageInfo* image = self->findImage(id);
    if (image) {
      self->writeLiteral("<img src=\"../images/");
      self->writeEscaped(image->filename.data(), image->filename.size(), true);
      self->writeLiteral("\" alt=\"\"/>");
    }
  } else if (strcmp(tag, "a") == 0) {
    self->writeLiteral("<a");
    self->writeElementId(atts);
    const char* href = getAttribute(atts, "href");
    if (href && href[0]) {
      std::string resolved = href;
      if (resolved.front() == '#') {
        const std::string targetId = resolved.substr(1);
        uint16_t targetChapter = 0;
        const std::string anchor = anchorName(hashString(targetId));
        if (self->findAnchorChapter(targetId, targetChapter)) {
          resolved = targetChapter == self->currentChapter
                         ? "#" + anchor
                         : chapterLink(targetChapter) + "#" + anchor;
        } else {
          resolved = "#" + anchor;
        }
      }
      self->writeLiteral(" href=\"");
      self->writeEscaped(resolved.data(), resolved.size(), true);
      self->writeLiteral("\"");
    }
    self->writeLiteral(">");
  } else if (strcmp(tag, "table") == 0 || strcmp(tag, "tr") == 0 || strcmp(tag, "td") == 0 ||
             strcmp(tag, "th") == 0 || strcmp(tag, "ol") == 0 || strcmp(tag, "ul") == 0 ||
             strcmp(tag, "li") == 0) {
    self->writeString(std::string("<") + tag);
    self->writeElementId(atts);
    self->writeLiteral(">");
  }
}

void XMLCALL Fb2::endElement(void* userData, const XML_Char* name) {
  auto* self = static_cast<Fb2*>(userData);
  const char* tag = localName(name);

  if (self->pass == ParsePass::Scan) {
    if (strcmp(tag, "book-title") == 0) {
      self->inBookTitle = false;
    } else if (strcmp(tag, "first-name") == 0) {
      self->inFirstName = false;
    } else if (strcmp(tag, "middle-name") == 0) {
      self->inMiddleName = false;
    } else if (strcmp(tag, "last-name") == 0) {
      self->inLastName = false;
    } else if (strcmp(tag, "nickname") == 0) {
      self->inNickname = false;
    } else if (strcmp(tag, "lang") == 0) {
      self->inLanguage = false;
    } else if (strcmp(tag, "coverpage") == 0) {
      self->inCoverpage = false;
    } else if (self->depth == self->authorDepth && strcmp(tag, "author") == 0) {
      self->finishAuthor();
      self->authorDepth = INT_MAX;
    } else if (self->depth == self->titleInfoDepth && strcmp(tag, "title-info") == 0) {
      self->titleInfoDepth = INT_MAX;
    }

    if (self->depth == self->titleElementDepth && strcmp(tag, "title") == 0) {
      self->writeTocRecord();
      self->titleElementDepth = INT_MAX;
      self->tocTitle.clear();
    }
    if (strcmp(tag, "section") == 0 && self->bodyDepth != INT_MAX) {
      if (!self->sectionAnchors.empty()) self->sectionAnchors.pop_back();
      if (self->sectionLevel == 1) self->currentChapter = -1;
      if (self->sectionLevel > 0) self->sectionLevel--;
    }
    if (self->depth == self->bodyDepth && strcmp(tag, "body") == 0) {
      self->bodyDepth = INT_MAX;
      self->currentChapter = -1;
      self->sectionLevel = 0;
    }
    if (self->depth == self->binaryDepth && strcmp(tag, "binary") == 0) self->binaryDepth = INT_MAX;
    self->depth--;
    return;
  }

  if (self->depth == self->binaryDepth && strcmp(tag, "binary") == 0) {
    self->endBinary();
    self->depth--;
    return;
  }

  if (self->bodyDepth != INT_MAX) {
    if (strcmp(tag, "title") == 0 && self->depth == self->titleElementDepth) {
      const int heading = std::clamp(self->sectionLevel, 1, 6);
      self->writeString("</h" + std::to_string(heading) + ">");
      self->titleElementDepth = INT_MAX;
    } else if (strcmp(tag, "p") == 0) {
      if (self->titleElementDepth == INT_MAX) self->writeLiteral("</p>");
    } else if (strcmp(tag, "subtitle") == 0) {
      self->writeLiteral("</h3>");
    } else if (strcmp(tag, "emphasis") == 0) {
      self->writeLiteral("</em>");
    } else if (strcmp(tag, "strong") == 0) {
      self->writeLiteral("</strong>");
    } else if (strcmp(tag, "strikethrough") == 0 || strcmp(tag, "code") == 0) {
      self->writeLiteral("</span>");
    } else if (strcmp(tag, "sup") == 0 || strcmp(tag, "sub") == 0) {
      self->writeString(std::string("</") + tag + ">");
    } else if (strcmp(tag, "poem") == 0 || strcmp(tag, "stanza") == 0 || strcmp(tag, "annotation") == 0) {
      self->writeLiteral("</div>");
    } else if (strcmp(tag, "cite") == 0 || strcmp(tag, "epigraph") == 0) {
      self->writeLiteral("</blockquote>");
    } else if (strcmp(tag, "v") == 0 || strcmp(tag, "text-author") == 0 || strcmp(tag, "date") == 0) {
      self->writeLiteral("</p>");
    } else if (strcmp(tag, "a") == 0) {
      self->writeLiteral("</a>");
    } else if (strcmp(tag, "table") == 0 || strcmp(tag, "tr") == 0 || strcmp(tag, "td") == 0 ||
               strcmp(tag, "th") == 0 || strcmp(tag, "ol") == 0 || strcmp(tag, "ul") == 0 ||
               strcmp(tag, "li") == 0) {
      self->writeString(std::string("</") + tag + ">");
    } else if (strcmp(tag, "section") == 0) {
      self->writeLiteral("</section>");
      if (!self->sectionAnchors.empty()) self->sectionAnchors.pop_back();
      if (self->sectionLevel == 1) self->closeChapter();
      if (self->sectionLevel > 0) self->sectionLevel--;
    }

    if (self->depth == self->bodyDepth && strcmp(tag, "body") == 0) {
      self->closeChapter();
      self->bodyDepth = INT_MAX;
      self->currentChapter = -1;
      self->sectionLevel = 0;
    }
  }
  self->depth--;
}

void XMLCALL Fb2::characterData(void* userData, const XML_Char* text, int length) {
  auto* self = static_cast<Fb2*>(userData);
  if (length <= 0) return;

  if (self->pass == ParsePass::Scan) {
    if (self->inBookTitle) self->title.append(text, length);
    if (self->inFirstName) self->authorFirst.append(text, length);
    if (self->inMiddleName) self->authorMiddle.append(text, length);
    if (self->inLastName) self->authorLast.append(text, length);
    if (self->inNickname) self->authorNickname.append(text, length);
    if (self->inLanguage) self->language.append(text, length);
    if (self->titleElementDepth != INT_MAX) self->tocTitle.append(text, length);
    if (self->bodyDepth != INT_MAX && self->currentChapter < 0 &&
        containsNonWhitespace(text, static_cast<size_t>(length))) {
      self->ensureScanChapter();
    }
    if (self->bodyDepth != INT_MAX && self->currentChapter >= 0) {
      self->chapterTextBytes += static_cast<size_t>(length);
    }
    return;
  }

  if (self->binaryDepth != INT_MAX) {
    self->feedBase64(text, static_cast<size_t>(length));
    return;
  }
  if (self->bodyDepth == INT_MAX) return;
  if (!self->chapterOpen) {
    if (!containsNonWhitespace(text, static_cast<size_t>(length)) || !self->ensureRenderChapter()) return;
  }
  self->writeEscaped(text, static_cast<size_t>(length));
  self->chapterTextBytes += static_cast<size_t>(length);
}

int XMLCALL Fb2::unknownEncoding(void*, const XML_Char* name, XML_Encoding* info) {
  if (Fb2Encoding::decodeByte(name, 0xC0) < 0) return XML_STATUS_ERROR;
  for (int i = 0; i < 256; ++i) info->map[i] = Fb2Encoding::decodeByte(name, static_cast<uint8_t>(i));
  info->data = nullptr;
  info->convert = nullptr;
  info->release = nullptr;
  return XML_STATUS_OK;
}

void Fb2::finishAuthor() {
  normalizeText(authorFirst);
  normalizeText(authorMiddle);
  normalizeText(authorLast);
  normalizeText(authorNickname);
  std::string fullName;
  for (const auto* part : {&authorFirst, &authorMiddle, &authorLast}) {
    if (part->empty()) continue;
    if (!fullName.empty()) fullName.push_back(' ');
    fullName += *part;
  }
  if (fullName.empty()) fullName = authorNickname;
  if (!fullName.empty()) {
    if (!author.empty()) author += ", ";
    author += fullName;
  }
}

void Fb2::postProcessMetadata() {
  normalizeText(title);
  normalizeText(author);
  normalizeText(language);
  if (title.empty()) {
    const size_t slash = filepath.find_last_of('/');
    const size_t start = slash == std::string::npos ? 0 : slash + 1;
    const size_t dot = filepath.find_last_of('.');
    title = filepath.substr(start, dot == std::string::npos || dot <= start ? std::string::npos : dot - start);
  }
  if (language.empty()) language = "und";
}

bool Fb2::writeContainerFile() const {
  return writeStaticFile(packagePath + "/META-INF/container.xml",
                         "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                         "<container version=\"1.0\" xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">"
                         "<rootfiles><rootfile full-path=\"OEBPS/content.opf\" "
                         "media-type=\"application/oebps-package+xml\"/></rootfiles></container>\n");
}

bool Fb2::writeStyleFile() const {
  return writeStaticFile(
      packagePath + "/OEBPS/style.css",
      "body { text-align: justify; }\n"
      "h1, h2, h3, h4, h5, h6 { text-align: center; font-weight: bold; margin: 1em 0 0.7em 0; }\n"
      ".subtitle { text-align: center; font-style: italic; }\n"
      "p { margin: 0.25em 0; }\n"
      ".epigraph, .cite { margin: 0.7em 1.5em; font-style: italic; }\n"
      ".poem { margin: 0.7em 1em; }\n"
      ".v { text-indent: 0; text-align: left; margin: 0; }\n"
      ".text-author { text-align: right; font-style: italic; text-indent: 0; }\n"
      ".empty-line { margin: 0.6em 0; text-indent: 0; }\n"
      ".annotation { font-style: italic; }\n"
      ".strike { text-decoration: line-through; }\n"
      ".code { font-family: monospace; }\n"
      "img { display: block; margin: 0.5em auto; max-width: 100%; }\n");
}

bool Fb2::writeOpfFile() const {
  HalFile file;
  if (!Storage.openFileForWrite("FB2", packagePath + "/OEBPS/content.opf", file)) return false;
  writeBytes(file, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                   "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"2.0\" unique-identifier=\"bookid\">"
                   "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:title>");
  writeXmlEscaped(file, title);
  writeBytes(file, "</dc:title><dc:creator>");
  writeXmlEscaped(file, author);
  writeBytes(file, "</dc:creator><dc:language>");
  writeXmlEscaped(file, language);
  writeBytes(file, "</dc:language><dc:identifier id=\"bookid\">fb2-");
  const std::string identifier = anchorName(hashString(filepath));
  writeXmlEscaped(file, identifier);
  writeBytes(file, "</dc:identifier>");

  const ImageInfo* cover = findImage(coverImageId);
  if (cover) {
    writeBytes(file, "<meta name=\"cover\" content=\"cover-image\"/>");
  }
  writeBytes(file, "</metadata><manifest>");
  writeBytes(file, "<item id=\"ncx\" href=\"toc.ncx\" media-type=\"application/x-dtbncx+xml\"/>");
  writeBytes(file, "<item id=\"style\" href=\"style.css\" media-type=\"text/css\"/>");
  for (int i = 0; i < chapterCount; ++i) {
    const std::string item = "<item id=\"chapter-" + std::to_string(i) + "\" href=\"" + chapterHref(i) +
                             "\" media-type=\"application/xhtml+xml\"/>";
    writeBytes(file, item);
  }
  for (size_t i = 0; i < images.size(); ++i) {
    const std::string id = cover && images[i].id == cover->id ? "cover-image" : "image-" + std::to_string(i);
    writeBytes(file, "<item id=\"");
    writeXmlEscaped(file, id, true);
    writeBytes(file, "\" href=\"images/");
    writeXmlEscaped(file, images[i].filename, true);
    writeBytes(file, "\" media-type=\"");
    writeXmlEscaped(file, images[i].mediaType, true);
    writeBytes(file, "\"/>");
  }
  writeBytes(file, "</manifest><spine toc=\"ncx\">");
  for (int i = 0; i < chapterCount; ++i) {
    writeBytes(file, "<itemref idref=\"chapter-" + std::to_string(i) + "\"/>");
  }
  writeBytes(file, "</spine><guide><reference type=\"text\" title=\"Start\" href=\"");
  writeBytes(file, chapterHref(0));
  writeBytes(file, "\"/></guide></package>\n");
  file.close();
  return true;
}

bool Fb2::writeNcxFile() const {
  HalFile input;
  if (!Storage.openFileForRead("FB2", cachePath + TOC_RECORDS_FILE, input)) return false;
  HalFile file;
  if (!Storage.openFileForWrite("FB2", packagePath + "/OEBPS/toc.ncx", file)) {
    input.close();
    return false;
  }

  writeBytes(file, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                   "<ncx xmlns=\"http://www.daisy.org/z3986/2005/ncx/\" version=\"2005-1\">"
                   "<head><meta name=\"dtb:uid\" content=\"fb2\"/></head><docTitle><text>");
  writeXmlEscaped(file, title);
  writeBytes(file, "</text></docTitle><navMap>\n");

  uint8_t level = 0;
  uint16_t chapter = 0;
  uint64_t anchor = 0;
  uint16_t titleLength = 0;
  int openDepth = 0;
  int playOrder = 1;
  bool any = false;
  while (input.available() > 0) {
    if (input.read(&level, sizeof(level)) != sizeof(level) || input.read(&chapter, sizeof(chapter)) != sizeof(chapter) ||
        input.read(&anchor, sizeof(anchor)) != sizeof(anchor) ||
        input.read(&titleLength, sizeof(titleLength)) != sizeof(titleLength) || titleLength > 4096) {
      break;
    }
    std::string entryTitle(titleLength, '\0');
    if (input.read(entryTitle.data(), titleLength) != titleLength) break;
    int targetDepth = std::max(1, static_cast<int>(level));
    targetDepth = std::min(targetDepth, openDepth + 1);
    while (openDepth >= targetDepth) {
      writeBytes(file, "</navPoint>\n");
      --openDepth;
    }
    writeBytes(file, "<navPoint id=\"nav-" + std::to_string(playOrder) + "\" playOrder=\"" +
                         std::to_string(playOrder) + "\"><navLabel><text>");
    writeXmlEscaped(file, entryTitle);
    writeBytes(file, "</text></navLabel><content src=\"");
    writeBytes(file, chapterHref(chapter));
    writeBytes(file, "#" + anchorName(anchor));
    writeBytes(file, "\"/>\n");
    openDepth = targetDepth;
    ++playOrder;
    any = true;
  }
  input.close();

  if (!any) {
    for (int chapterIndex = 0; chapterIndex < chapterCount; ++chapterIndex) {
      writeBytes(file, "<navPoint id=\"nav-" + std::to_string(playOrder) + "\" playOrder=\"" +
                           std::to_string(playOrder) + "\"><navLabel><text>");
      writeXmlEscaped(file, chapterIndex == 0 ? title : "Section " + std::to_string(chapterIndex + 1));
      writeBytes(file, "</text></navLabel><content src=\"" + chapterHref(chapterIndex) + "\"/></navPoint>\n");
      ++playOrder;
    }
  } else {
    while (openDepth-- > 0) writeBytes(file, "</navPoint>\n");
  }
  writeBytes(file, "</navMap></ncx>\n");
  file.close();
  return true;
}

bool Fb2::clearCache() const {
  bool success = true;
  if (Storage.exists(cachePath.c_str())) success = Storage.removeDir(cachePath.c_str()) && success;
  if (Storage.exists(legacyCachePath.c_str())) success = Storage.removeDir(legacyCachePath.c_str()) && success;

  const std::string indexPath = cacheBaseDir + LRU_INDEX_FILE;
  std::vector<std::string> keys = readLruIndex(indexPath);
  keys.erase(std::remove(keys.begin(), keys.end(), cacheKey), keys.end());
  writeLruIndex(indexPath, keys);
  return success;
}
