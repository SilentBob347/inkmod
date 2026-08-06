#include "Fb2.h"

#include <Logging.h>
#include <freertos/task.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <limits>
#include <memory>

#include "Fb2Encoding.h"
#include "native/Fb2ZipOpener.h"
#include "native/FsFileReader.h"

namespace {

constexpr uint8_t PACKAGE_VERSION = 6;  // bumped: image-heavy sections split into virtual chapters
// A single FB2 <section> with more inline images than this gets split into
// several virtual chapters (see splitSectionsForImageLoad()), so a chapter
// that's actually opened never needs to extract more than this many images
// at once. Real-world crash trace: 23 images in one un-split section
// reliably tripped the reader's own low-heap image-suppression check
// (MemoryBudget::hasHeapForEpubInlineImage) on every single one of them.
constexpr uint32_t MAX_IMAGES_PER_CHAPTER = 6;

// One entry per chapter that will actually get written to the spine/section
// index - either a whole FB2 <section> (the common case) or, for a section
// with more than MAX_IMAGES_PER_CHAPTER inline images, one of several
// consecutive image-count-bounded slices of it. rangeEnd is exclusive;
// UINT32_MAX means "through the end of the section" (used for both
// unsplit sections and the last slice of a split one, so trailing text
// after the final counted image - if any - isn't dropped).
struct VirtualChapter {
  size_t sectionIndex;
  uint32_t imageRangeStart;
  uint32_t imageRangeEnd;
  bool isFirstOfSection;  // only this slice gets the section's real title
};

std::vector<VirtualChapter> splitSectionsForImageLoad(const std::vector<Fb2SectionIndexEntry>& sections) {
  std::vector<VirtualChapter> result;
  result.reserve(sections.size());
  for (size_t i = 0; i < sections.size(); ++i) {
    const uint32_t imageCount = sections[i].imageRefCount;
    if (imageCount <= MAX_IMAGES_PER_CHAPTER) {
      result.push_back({i, 0, UINT32_MAX, true});
      continue;
    }
    uint32_t start = 0;
    bool first = true;
    for (;;) {
      const uint32_t chunkEnd = start + MAX_IMAGES_PER_CHAPTER;
      const bool isLastChunk = chunkEnd >= imageCount;
      result.push_back({i, start, isLastChunk ? UINT32_MAX : chunkEnd, first});
      if (isLastChunk) break;
      first = false;
      start = chunkEnd;
    }
  }
  return result;
}
constexpr char CACHE_MAGIC[] = "FB2IDX";  // 6 bytes, no trailing NUL written
constexpr size_t CACHE_MAGIC_LEN = 6;

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

std::string lowercase(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

std::string normalizeImageMediaType(const std::string& value) {
  const std::string mediaType = lowercase(value);
  if (mediaType == "image/jpeg" || mediaType == "image/jpg" || mediaType == "image/pjpeg") return "image/jpeg";
  if (mediaType == "image/png" || mediaType == "image/x-png") return "image/png";
  return {};
}

void writeBytes(Print& out, const char* data, size_t length) {
  if (length > 0) out.write(reinterpret_cast<const uint8_t*>(data), length);
}

void writeBytes(Print& out, const std::string& value) { writeBytes(out, value.data(), value.size()); }

void writeBytes(Print& out, const char* value) { writeBytes(out, value, strlen(value)); }

void writeXmlEscaped(Print& out, const char* text, size_t length, bool attribute = false) {
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
    writeBytes(out, text + start, i - start);
    writeBytes(out, replacement, strlen(replacement));
    start = i + 1;
  }
  writeBytes(out, text + start, length - start);
}

void writeXmlEscaped(Print& out, const std::string& value, bool attribute = false) {
  writeXmlEscaped(out, value.data(), value.size(), attribute);
}

bool writeStaticFile(const std::string& path, const char* contents) {
  HalFile file;
  if (!Storage.openFileForWrite("FB2", path, file)) return false;
  const size_t length = strlen(contents);
  const bool success = file.write(contents, length) == length;
  file.close();
  return success;
}

// Every binary cache artifact (section index, image index, package state)
// starts with the same "FB2IDX" + version header, so a corrupted, truncated,
// or format-mismatched file is caught with one cheap check up front instead
// of misreading whatever bytes happen to follow as if they were valid
// records - which, worst case, could walk off the end of the file or hand
// back garbage a caller trusts.
void writeCacheHeader(HalFile& out) {
  out.write(CACHE_MAGIC, CACHE_MAGIC_LEN);
  const uint8_t version = PACKAGE_VERSION;
  out.write(&version, sizeof(version));
}

bool readAndCheckCacheHeader(HalFile& in) {
  char magic[CACHE_MAGIC_LEN];
  uint8_t version = 0;
  if (in.read(magic, CACHE_MAGIC_LEN) != CACHE_MAGIC_LEN || memcmp(magic, CACHE_MAGIC, CACHE_MAGIC_LEN) != 0) {
    return false;
  }
  return in.read(&version, sizeof(version)) == sizeof(version) && version == PACKAGE_VERSION;
}

// The native parser (native/Fb2XmlReader.h) intentionally never decodes
// bytes - it assumes UTF-8 input and forwards everything else verbatim, by
// design (see its own header comment). Real-world FB2 files frequently
// declare a legacy single-byte Russian encoding instead (windows-1251 is
// extremely common; koi8-r less so), so that assumption doesn't hold as-is.
// This mirrors what the old expat-based converter did via its
// XML_SetUnknownEncodingHandler: read the declared encoding out of the XML
// prolog and, if it's not already UTF-8, transcode the whole file to a UTF-8
// temp copy before handing it to the parser.
std::string extractDeclaredEncoding(const char* prolog, size_t length) {
  const char* needle = "encoding=";
  const char* found = nullptr;
  for (size_t i = 0; i + 9 <= length; ++i) {
    if (strncmp(prolog + i, needle, 9) == 0) {
      found = prolog + i + 9;
      break;
    }
  }
  if (!found) return {};
  const char quote = *found;
  if (quote != '"' && quote != '\'') return {};
  const char* end = static_cast<const char*>(memchr(found + 1, quote, length - (found + 1 - prolog)));
  if (!end) return {};
  return std::string(found + 1, end - (found + 1));
}

// Appends the UTF-8 encoding of a BMP code point (single-byte legacy
// encodings never produce anything outside the BMP) into a fixed buffer.
// Returns the number of bytes written (1-3).
size_t appendUtf8(char* out, int codePoint) {
  const uint32_t cp = static_cast<uint32_t>(codePoint);
  if (cp <= 0x7F) {
    out[0] = static_cast<char>(cp);
    return 1;
  }
  if (cp <= 0x7FF) {
    out[0] = static_cast<char>(0xC0 | (cp >> 6));
    out[1] = static_cast<char>(0x80 | (cp & 0x3F));
    return 2;
  }
  out[0] = static_cast<char>(0xE0 | (cp >> 12));
  out[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
  out[2] = static_cast<char>(0x80 | (cp & 0x3F));
  return 3;
}

// If `path` declares a supported non-UTF-8 encoding, transcodes it to a new
// UTF-8 temp file next to `cacheBaseFile` and rewrites `path` to point at
// it (so the caller can track/clean it up the same way as any other temp
// source). Leaves `path` untouched (and returns true) when the file is
// already UTF-8/ASCII or declares an encoding this module has no table
// for - in the latter case the native parser will pass the original bytes
// through as-is, same as it would for any other unrecognized encoding.
// Checks whether a chunk of `path`'s content is already well-formed UTF-8:
// every byte >= 0x80 must be part of a structurally valid multi-byte
// sequence (right number of 0x80-0xBF continuation bytes, no sequence
// truncated by EOF). Doesn't attempt to detect overlong encodings or
// validate the decoded code points are "sensible" - structural well-
// formedness over several KB is already strong enough evidence, since real
// single-byte-encoded text scatters bytes across the whole 0x80-0xFF range
// fairly uniformly and would only pass this by chance in a vanishingly
// small fraction of cases.
bool bodyLooksLikeUtf8Already(const std::string& path) {
  HalFile file;
  if (!Storage.openFileForRead("FB2", path, file)) return false;
  // Skip roughly past the XML prolog/declaration so the sample is actual
  // book content, not the (pure-ASCII, hence UTF-8-compatible either way)
  // header line.
  file.seek(64);
  uint8_t buf[4096];
  const int got = file.read(buf, sizeof(buf));
  file.close();
  if (got <= 0) return false;

  int multiByteSequences = 0;
  for (int i = 0; i < got;) {
    const uint8_t b = buf[i];
    if (b < 0x80) {
      ++i;
      continue;
    }
    int extra;
    if ((b & 0xE0) == 0xC0) extra = 1;
    else if ((b & 0xF0) == 0xE0) extra = 2;
    else if ((b & 0xF8) == 0xF0) extra = 3;
    else return false;  // 0x80-0xBF or 0xF8-0xFF as a lead byte: not valid UTF-8
    if (i + extra >= got) break;  // sequence runs past the sample; stop, don't guess
    for (int k = 1; k <= extra; ++k) {
      if ((buf[i + k] & 0xC0) != 0x80) return false;
    }
    ++multiByteSequences;
    i += 1 + extra;
  }
  // Require a reasonable amount of evidence, not just "no bytes contradicted
  // it" (a sample with zero high-bit bytes at all would trivially "pass"
  // otherwise, telling us nothing about which encoding is actually in use).
  return multiByteSequences >= 20;
}

bool transcodeToUtf8IfNeeded(std::string& path, const std::string& tempPathBase, const Fb2::ProgressFn& onProgress) {
  char prolog[256];
  const size_t prologLen = Storage.readFileToBuffer(path.c_str(), prolog, sizeof(prolog));
  const std::string declared = extractDeclaredEncoding(prolog, prologLen);
  if (declared.empty()) return true;
  const std::string lower = [&] {
    std::string s = declared;
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
  }();
  if (lower == "utf-8" || lower == "utf8" || lower == "us-ascii" || lower == "ascii") return true;
  // Probe with a representative high byte to see if this is an encoding we
  // actually have a conversion table for.
  if (Fb2Encoding::decodeByte(declared.c_str(), 0xC0) < 0) return true;

  // Some real-world FB2s declare a legacy encoding but were actually
  // re-saved as UTF-8 at some point without the <?xml?> line being
  // updated to match - the declaration lies. Re-encoding already-correct
  // UTF-8 through a single-byte table produces exactly the kind of
  // well-formed-but-wrong-text "double encoding" garbage that's otherwise
  // very hard to tell apart from a genuine decode bug after the fact, so
  // it's worth checking for directly: sample a chunk of the body and see
  // if it's already well-formed UTF-8. Coincidentally-valid multi-byte
  // sequences over a large enough sample are vanishingly unlikely for
  // real single-byte-encoded text (which uses the whole 0x80-0xFF range
  // fairly uniformly), so this is a reliable signal, not a guess.
  if (bodyLooksLikeUtf8Already(path)) return true;

  HalFile in;
  if (!Storage.openFileForRead("FB2", path, in)) return false;
  const std::string outPath = tempPathBase + ".utf8.fb2";
  HalFile out;
  if (!Storage.openFileForWrite("FB2", outPath, out)) {
    in.close();
    return false;
  }

  const size_t totalSize = in.fileSize();
  size_t processed = 0;
  int chunkCount = 0;
  uint8_t inBuf[1024];
  char outBuf[1024 * 3];
  bool ok = true;
  for (;;) {
    const int got = in.read(inBuf, sizeof(inBuf));
    if (got <= 0) break;
    processed += static_cast<size_t>(got);
    ++chunkCount;
    if (onProgress && totalSize > 0 && chunkCount % 16 == 0) {
      onProgress(30 + static_cast<int>(processed * 10 / totalSize));
    }
    // vTaskDelay only exists here so a very large book can't starve the
    // watchdog - it does NOT need to run anywhere near every chunk, and on
    // this firmware it apparently isn't cheap: something else runs a ~500ms
    // display refresh on its own timer, and yielding at all seems to be
    // enough to let a pending one go ahead before returning control here,
    // so yielding too often turns into a slow drip of ~500ms stalls (this
    // is very likely what regressed the "Девчата" load from ~38s to ~138s
    // between builds - too many yield points, not too few this time).
    // Once every ~1MB is still far more than needed to avoid the watchdog.
    if (chunkCount % 256 == 0) vTaskDelay(1);
    size_t outLen = 0;
    for (int i = 0; i < got; ++i) {
      int cp = Fb2Encoding::decodeByte(declared.c_str(), inBuf[i]);
      if (cp < 0) cp = 0xFFFD;  // undefined byte in this encoding: Unicode replacement char
      outLen += appendUtf8(outBuf + outLen, cp);
      if (outLen > sizeof(outBuf) - 8) {
        if (out.write(outBuf, outLen) != outLen) { ok = false; }
        outLen = 0;
      }
    }
    if (outLen && out.write(outBuf, outLen) != outLen) ok = false;
    if (!ok) break;
  }
  in.close();
  out.close();
  if (!ok) {
    Storage.remove(outPath.c_str());
    return false;
  }
  path = outPath;
  return true;
}

// Bounds how many fully-converted FB2 -> EPUB package caches are kept on disk
// at once. FB2 source files stay untouched in the library; only the unpacked
// package copy is subject to this budget, evicted least-recently-used first.
constexpr int MAX_CACHED_FB2_PACKAGES = 5;
constexpr char LRU_INDEX_FILE[] = "/.fb2_lru_index";
constexpr char METADATA_FILE[] = "/fb2_metadata.txt";
constexpr char PACKAGE_STATE_FILE[] = "/fb2_package.bin";
constexpr char SECTIONS_INDEX_FILE[] = "/.fb2_sections.bin";
constexpr char IMAGES_INDEX_FILE[] = "/.fb2_images.bin";

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

// ---------------------------------------------------------------------
// StreamSink: turns Fb2Parser's content events for a single <section> into
// XHTML written straight to a Print& (no intermediate chapter file, no
// chapter-splitting - one call renders exactly one section, the same way a
// real EPUB's single chapter file can be arbitrarily long).
// ---------------------------------------------------------------------
class StreamSink : public Fb2ContentSink {
 public:
  // resolveLink(targetId) -> XHTML href to point at (e.g. "chapter_5.xhtml#fb2-...")
  // for a resolvable target, or an empty string if targetId doesn't match
  // any known section (in which case the link renders as plain text - no
  // point emitting an <a href=""> that goes nowhere).
  using LinkResolver = std::function<std::string(const std::string&)>;

  StreamSink(Print& out, const std::vector<Fb2::ImageInfoPublic>& images, LinkResolver resolveLink = nullptr)
      : out_(out), images_(images), resolveLink_(std::move(resolveLink)) {}

  void onParagraphBegin() override { writeBytes(out_, "<p>"); }
  void onParagraphEnd() override { writeBytes(out_, "</p>"); }

  void onSubtitle(const std::string& text) override {
    writeBytes(out_, "<h3 class=\"subtitle\">");
    writeXmlEscaped(out_, text);
    writeBytes(out_, "</h3>");
  }
  void onEmptyLine() override { writeBytes(out_, "<p class=\"empty-line\">&#160;</p>"); }
  void onHorizontalRule() override { writeBytes(out_, "<hr/>"); }

  void onPoemBegin() override { writeBytes(out_, "<div class=\"poem\">"); }
  void onPoemEnd() override { writeBytes(out_, "</div>"); }
  void onStanzaBegin() override { writeBytes(out_, "<div class=\"stanza\">"); }
  void onStanzaEnd() override { writeBytes(out_, "</div>"); }
  void onVerseLine(const std::string& text) override {
    writeBytes(out_, "<p class=\"v\">");
    writeXmlEscaped(out_, text);
    writeBytes(out_, "</p>");
  }

  void onCiteBegin() override { writeBytes(out_, "<blockquote class=\"cite\">"); }
  void onCiteEnd() override { writeBytes(out_, "</blockquote>"); }
  void onEpigraphBegin() override { writeBytes(out_, "<blockquote class=\"epigraph\">"); }
  void onEpigraphEnd() override { writeBytes(out_, "</blockquote>"); }

  void onText(const std::string& text, Fb2InlineStyle style) override {
    const auto has = [style](Fb2InlineStyle bit) {
      return (static_cast<uint8_t>(style) & static_cast<uint8_t>(bit)) != 0;
    };
    std::string open, close;
    auto wrap = [&](bool cond, const char* openTag, const char* closeTag) {
      if (!cond) return;
      open += openTag;
      close = closeTag + close;
    };
    wrap(has(Fb2InlineStyle::Bold), "<strong>", "</strong>");
    wrap(has(Fb2InlineStyle::Italic), "<em>", "</em>");
    wrap(has(Fb2InlineStyle::Underline), "<span class=\"underline\">", "</span>");
    wrap(has(Fb2InlineStyle::Strikethrough), "<span class=\"strike\">", "</span>");
    wrap(has(Fb2InlineStyle::SmallCaps), "<span class=\"smallcaps\">", "</span>");
    wrap(has(Fb2InlineStyle::Superscript), "<sup>", "</sup>");
    wrap(has(Fb2InlineStyle::Subscript), "<sub>", "</sub>");
    writeBytes(out_, open.c_str());
    writeXmlEscaped(out_, text);
    writeBytes(out_, close.c_str());
  }

  void onImage(const std::string& binaryId) override {
    const auto it = std::find_if(images_.begin(), images_.end(),
                                  [&](const Fb2::ImageInfoPublic& img) { return img.id == binaryId; });
    if (it == images_.end()) return;
    writeBytes(out_, "<img src=\"../images/");
    writeXmlEscaped(out_, it->filename, true);
    writeBytes(out_, "\" alt=\"\"/>");
  }

  void onLinkBegin(const std::string& targetId) override {
    linkWasEmitted_ = false;
    if (!resolveLink_) return;
    const std::string href = resolveLink_(targetId);
    if (href.empty()) return;  // unresolvable target: render as plain text, no <a> wrapper
    writeBytes(out_, "<a href=\"");
    writeXmlEscaped(out_, href, true);
    writeBytes(out_, "\">");
    linkWasEmitted_ = true;
  }
  void onLinkEnd() override {
    if (linkWasEmitted_) writeBytes(out_, "</a>");
    linkWasEmitted_ = false;
  }

  void onTableBegin() override { writeBytes(out_, "<table>"); }
  void onTableEnd() override { writeBytes(out_, "</table>"); }
  void onTableRowBegin() override { writeBytes(out_, "<tr>"); }
  void onTableRowEnd() override { writeBytes(out_, "</tr>"); }
  void onTableCell(const std::string& text, const Fb2TableCellAttrs& attrs) override {
    const char* tag = attrs.isHeader ? "th" : "td";
    writeBytes(out_, "<");
    writeBytes(out_, tag);
    if (attrs.colspan != 1) writeBytes(out_, " colspan=\"" + std::to_string(attrs.colspan) + "\"");
    if (attrs.rowspan != 1) writeBytes(out_, " rowspan=\"" + std::to_string(attrs.rowspan) + "\"");
    if (!attrs.align.empty()) {
      writeBytes(out_, " style=\"text-align:");
      writeXmlEscaped(out_, attrs.align, true);
      writeBytes(out_, "\"");
    }
    writeBytes(out_, ">");
    writeXmlEscaped(out_, text);
    writeBytes(out_, "</");
    writeBytes(out_, tag);
    writeBytes(out_, ">");
  }

 private:
  Print& out_;
  const std::vector<Fb2::ImageInfoPublic>& images_;
  LinkResolver resolveLink_;
  bool linkWasEmitted_ = false;  // whether onLinkBegin actually wrote an <a> for the currently-open link
};

// Wraps another sink, forwarding events only while the running count of
// onImage() calls seen so far is within [rangeStart, rangeEnd) - used to
// split a single FB2 <section> with many images into several virtual
// chapters (see MAX_IMAGES_PER_CHAPTER / splitSectionsForImageLoad() below).
// Content before/after the assigned image range - including the images
// themselves that fall outside it - is silently dropped rather than
// forwarded, since it belongs to a sibling virtual chapter's own render
// pass, not this one.
class RangeFilterSink : public Fb2ContentSink {
 public:
  RangeFilterSink(Fb2ContentSink& inner, uint32_t rangeStart, uint32_t rangeEnd)
      : inner_(inner), rangeStart_(rangeStart), rangeEnd_(rangeEnd) {}

  void onParagraphBegin() override { if (active()) inner_.onParagraphBegin(); }
  void onParagraphEnd() override { if (active()) inner_.onParagraphEnd(); }
  void onSubtitle(const std::string& text) override { if (active()) inner_.onSubtitle(text); }
  void onEmptyLine() override { if (active()) inner_.onEmptyLine(); }
  void onHorizontalRule() override { if (active()) inner_.onHorizontalRule(); }
  void onPoemBegin() override { if (active()) inner_.onPoemBegin(); }
  void onPoemEnd() override { if (active()) inner_.onPoemEnd(); }
  void onStanzaBegin() override { if (active()) inner_.onStanzaBegin(); }
  void onStanzaEnd() override { if (active()) inner_.onStanzaEnd(); }
  void onVerseLine(const std::string& text) override { if (active()) inner_.onVerseLine(text); }
  void onCiteBegin() override { if (active()) inner_.onCiteBegin(); }
  void onCiteEnd() override { if (active()) inner_.onCiteEnd(); }
  void onEpigraphBegin() override { if (active()) inner_.onEpigraphBegin(); }
  void onEpigraphEnd() override { if (active()) inner_.onEpigraphEnd(); }
  void onText(const std::string& text, Fb2InlineStyle style) override {
    if (active()) inner_.onText(text, style);
  }
  void onImage(const std::string& binaryId) override {
    // Checked against the count *before* this image, then incremented
    // after: the range-ending image itself belongs to the next chunk, not
    // this one, matching how [start, end) slices normally work.
    if (active()) inner_.onImage(binaryId);
    ++imageOrdinal_;
  }
  void onLinkBegin(const std::string& targetId) override { if (active()) inner_.onLinkBegin(targetId); }
  void onLinkEnd() override { if (active()) inner_.onLinkEnd(); }
  void onTableBegin() override { if (active()) inner_.onTableBegin(); }
  void onTableEnd() override { if (active()) inner_.onTableEnd(); }
  void onTableRowBegin() override { if (active()) inner_.onTableRowBegin(); }
  void onTableRowEnd() override { if (active()) inner_.onTableRowEnd(); }
  void onTableCell(const std::string& text, const Fb2TableCellAttrs& attrs) override {
    if (active()) inner_.onTableCell(text, attrs);
  }

 private:
  bool active() const { return imageOrdinal_ >= rangeStart_ && imageOrdinal_ < rangeEnd_; }

  Fb2ContentSink& inner_;
  uint32_t rangeStart_;
  uint32_t rangeEnd_;
  uint32_t imageOrdinal_ = 0;
};

}  // namespace

Fb2::Fb2(std::string path, std::string cacheBasePath) : filepath(std::move(path)) {
  const std::string key = std::to_string(std::hash<std::string>{}(filepath));
  cacheKey = "fb2_" + key;
  cacheBaseDir = cacheBasePath;
  cachePath = cacheBaseDir + "/" + cacheKey;
  // Cache dirs from before the epub_ -> fb2_ rename; still cleaned up here so
  // upgrading firmware doesn't leave orphaned caches behind.
  legacyCachePath = std::move(cacheBasePath) + "/epub_" + key;
  packagePath = cachePath + "/package.epub";
  sourcePath = filepath;

  const size_t slash = filepath.find_last_of('/');
  const size_t start = slash == std::string::npos ? 0 : slash + 1;
  const size_t dot = filepath.find_last_of('.');
  title = filepath.substr(start, dot == std::string::npos || dot <= start ? std::string::npos : dot - start);
}

bool Fb2::isCompressedFb2() const {
  // Only ".zip" itself matters here, not a specific "*.fb2.zip" naming
  // convention: this class is only ever constructed once something else
  // has already decided the file is an FB2 book (see FileBrowserActivity's
  // own extension checks), so any ".zip" that reaches here needs
  // extracting - requiring an exact double-extension additionally missed
  // real books renamed with e.g. "_fb2.zip" instead of ".fb2.zip", which
  // used to mean prepareSource() skipped extraction entirely and scan()
  // ended up reading raw zip container bytes as if they were the FB2 XML.
  const std::string name = lowercase(filepath);
  return name.size() >= 4 && name.compare(name.size() - 4, 4, ".zip") == 0;
}

bool Fb2::prepareSource(const ProgressFn& onProgress) {
  sourcePath = filepath;
  temporarySourcePath.clear();

  if (isCompressedFb2()) {
    HalFile zipFile;
    if (!Storage.openFileForRead("FB2", filepath, zipFile)) return false;
    FsFileReader zipReader(zipFile);

    setupCacheDir();
    temporarySourcePath = cachePath + "/.source.fb2";
    Storage.remove(temporarySourcePath.c_str());
    HalFile extracted;
    if (!Storage.openFileForWrite("FB2", temporarySourcePath, extracted)) {
      zipFile.close();
      return false;
    }
    // inflateRaw()'s output callback (Window::putByte(), native/Inflate.cpp)
    // fires one decompressed byte at a time - that's how the DEFLATE window
    // itself works, not something to change there. Writing each of those
    // straight to SD is the same "tens of thousands of tiny mutex/SPI-locked
    // writes in one tight call chain" problem the base64 image path had:
    // for a multi-MB FB2 packed into a .fb2.zip, on real hardware this is
    // what silently eats 30+ seconds with no chance to update the loading
    // popup's progress. Buffer before writing, same fix as decodeImageOnDemand.
    constexpr size_t kFlushBufSize = 4096;
    std::vector<uint8_t> flushBuf;
    flushBuf.reserve(kFlushBufSize);
    int flushCount = 0;
    const uint32_t zipSize = zipReader.size();
    std::string entryName;
    const bool extractedOk = extractFb2FromZip(
        zipReader,
        [&](const uint8_t* d, size_t n) {
          flushBuf.insert(flushBuf.end(), d, d + n);
          if (flushBuf.size() >= kFlushBufSize) {
            extracted.write(flushBuf.data(), flushBuf.size());
            flushBuf.clear();
            ++flushCount;
            // Extraction is roughly linear in compressed bytes consumed;
            // map that to the first third of the loading popup so it
            // visibly moves instead of sitting at 0 for however long this
            // takes, which is exactly what looked like a hang before.
            if (onProgress && zipSize > 0 && flushCount % 8 == 0) {
              onProgress(static_cast<int>(zipReader.position() * 30 / zipSize));
            }
            // See the long comment on the equivalent yield in
            // transcodeToUtf8IfNeeded() below - this needs to be rare, not
            // frequent, on this firmware. Once every ~1MB.
            if (flushCount % 256 == 0) vTaskDelay(1);
          }
        },
        &entryName);
    if (!flushBuf.empty()) extracted.write(flushBuf.data(), flushBuf.size());
    extracted.close();
    zipFile.close();
    if (!extractedOk) {
      Storage.remove(temporarySourcePath.c_str());
      LOG_ERR("FB2", "No FB2 file found in archive or extraction failed: %s", filepath.c_str());
      return false;
    }
    sourcePath = temporarySourcePath;
  }
  if (onProgress) onProgress(30);

  // Normalize a declared non-UTF-8 encoding (windows-1251 is common for
  // older Russian FB2s) to UTF-8: the native parser assumes UTF-8 input and
  // otherwise just forwards raw bytes, which would corrupt such files.
  setupCacheDir();
  const std::string beforeTranscode = sourcePath;
  const std::string previousTemp = temporarySourcePath;
  if (!transcodeToUtf8IfNeeded(sourcePath, cachePath + "/.source", onProgress)) return false;
  if (sourcePath != beforeTranscode) {
    if (!previousTemp.empty()) Storage.remove(previousTemp.c_str());
    temporarySourcePath = sourcePath;
  }
  return true;
}

void Fb2::setupCacheDir() const { Storage.mkdir(cachePath.c_str(), true); }

bool Fb2::cacheIsCurrent() {
  if (!Storage.exists((packagePath + "/META-INF/container.xml").c_str()) ||
      !Storage.exists((packagePath + "/OEBPS/content.opf").c_str())) {
    return false;
  }

  // Validate the section index's own header too, not just its existence -
  // a truncated/corrupted write (power loss, SD error) would otherwise
  // pass this check and only surface much later, as every single chapter
  // silently failing to render instead of a clean rebuild here.
  {
    HalFile sections;
    if (!Storage.openFileForRead("FB2", cachePath + SECTIONS_INDEX_FILE, sections)) return false;
    const bool sectionsOk = readAndCheckCacheHeader(sections);
    sections.close();
    if (!sectionsOk) return false;
  }

  HalFile state;
  if (!Storage.openFileForRead("FB2", cachePath + PACKAGE_STATE_FILE, state)) return false;

  uint64_t cachedSize = 0;
  uint16_t cachedChapters = 0;
  const bool valid = readAndCheckCacheHeader(state) && state.read(&cachedSize, sizeof(cachedSize)) == sizeof(cachedSize) &&
                     state.read(&cachedChapters, sizeof(cachedChapters)) == sizeof(cachedChapters) &&
                     cachedSize == sourceSize && cachedChapters > 0;
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
  const uint16_t chapters = static_cast<uint16_t>(std::min(chapterCount, static_cast<int>(UINT16_MAX)));
  writeCacheHeader(state);
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

bool Fb2::load(const ProgressFn& onProgress) {
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

  LOG_INF("FB2", "Indexing FB2: %llu bytes", static_cast<unsigned long long>(sourceSize));
  // Rebuild the cache directory fresh before prepareSource() writes a
  // zip-extracted/transcoded source copy into it - that copy has to survive
  // this wipe, not get created before it and then deleted a moment later.
  if (Storage.exists(cachePath.c_str())) Storage.removeDir(cachePath.c_str());
  setupCacheDir();
  if (!prepareSource(onProgress)) return false;
  // temporarySourcePath (that zip-extracted/transcoded copy) is NOT deleted
  // after this: renderChapterOnDemand() reopens whatever `sourcePath`
  // ended up as, on every future chapter render, however much later that
  // is. It lives inside cachePath, so normal cache clearing/eviction
  // cleans it up along with everything else.
  const bool converted = convertToPackage(onProgress);
  if (!converted) return false;
  loaded = true;
  LOG_INF("FB2", "Indexed FB2: %d chapters, %zu images (chapters render on demand)", chapterCount, images.size());
  maintainCacheBudget();
  return true;
}

const Fb2::ImageInfoPublic* Fb2::findImage(const std::string& id) const {
  const auto it = std::find_if(images.begin(), images.end(), [&](const ImageInfoPublic& image) { return image.id == id; });
  return it == images.end() ? nullptr : &*it;
}

bool Fb2::convertToPackage(const ProgressFn& onProgress) {
  // Cache directory was already wiped fresh and recreated in load(), before
  // prepareSource() wrote a possibly-transcoded source copy into it.
  setupCacheDir();
  Storage.mkdir((packagePath + "/META-INF").c_str(), true);
  Storage.mkdir((packagePath + "/OEBPS/text").c_str(), true);
  Storage.mkdir((packagePath + "/OEBPS/images").c_str(), true);

  const auto fail = [this]() {
    if (Storage.exists(cachePath.c_str())) Storage.removeDir(cachePath.c_str());
    return false;
  };

  HalFile source;
  if (!Storage.openFileForRead("FB2", sourcePath, source)) return fail();
  FsFileReader reader(source);

  Fb2Parser parser;
  Fb2ScanResult scan;
  if (!parser.scan(reader, scan)) {
    source.close();
    LOG_ERR("FB2", "FB2 scan failed (not well-formed?): %s", filepath.c_str());
    return fail();
  }
  source.close();
  if (onProgress) onProgress(40);

  title = scan.metadata.title;
  author = scan.metadata.author;
  language = scan.metadata.language;
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
  coverImageId = scan.metadata.coverBinaryId;

  images.clear();
  for (const auto& binary : scan.binaries) {
    const std::string mediaType = normalizeImageMediaType(binary.contentType);
    if (mediaType.empty()) continue;
    ImageInfoPublic image;
    image.id = binary.id;
    image.mediaType = mediaType;
    image.filename = "image_" + std::to_string(images.size()) + (mediaType == "image/png" ? ".png" : ".jpg");
    images.push_back(std::move(image));
  }

  {
    const bool imagesOk = persistImageIndex(scan);
    if (!imagesOk) return fail();
  }
  if (onProgress) onProgress(70);

  // Persist the section index (level, innerStartOffset, id, title, image
  // range) - kept permanently, not deleted after this call, since
  // renderChapterOnDemand() reads it on every later chapter open to find
  // where to seek in the FB2 source. Id/title are needed to build the
  // anchor and heading; approxTextBytes is used by getApproxChapterSize()
  // to seed BookMetadataCache's progress-bar math, since a chapter isn't a
  // real file with a real size until it's actually been rendered once.
  // Offsets, parent/body indices are scan()-only bookkeeping and aren't
  // persisted. One record is written per *virtual* chapter, not per FB2
  // <section> - see splitSectionsForImageLoad().
  const std::vector<VirtualChapter> virtualChapters = splitSectionsForImageLoad(scan.sections);
  std::vector<int> sectionFirstChapterIndex(scan.sections.size(), -1);
  {
    HalFile sectionsOut;
    if (!Storage.openFileForWrite("FB2", cachePath + SECTIONS_INDEX_FILE, sectionsOut)) return fail();
    writeCacheHeader(sectionsOut);
    for (size_t chapterIdx = 0; chapterIdx < virtualChapters.size(); ++chapterIdx) {
      const auto& vc = virtualChapters[chapterIdx];
      const auto& section = scan.sections[vc.sectionIndex];
      if (sectionFirstChapterIndex[vc.sectionIndex] < 0) {
        sectionFirstChapterIndex[vc.sectionIndex] = static_cast<int>(chapterIdx);
      }
      // How many virtual chapters this section was split into, so
      // approxTextBytes (a whole-section estimate) can be divided evenly
      // across them instead of counting the same section's size once per
      // slice toward the book's total.
      uint32_t sliceCount = 0;
      for (const auto& other : virtualChapters) {
        if (other.sectionIndex == vc.sectionIndex) ++sliceCount;
      }

      const uint8_t level = static_cast<uint8_t>(std::min<uint16_t>(section.level, 255));
      const uint32_t innerStartOffset = section.innerStartOffset;
      const std::string& title = vc.isFirstOfSection ? section.title : std::string();
      const uint16_t idLen = static_cast<uint16_t>(std::min(section.id.size(), static_cast<size_t>(4096)));
      const uint16_t titleLen = static_cast<uint16_t>(std::min(title.size(), static_cast<size_t>(4096)));
      const uint32_t approxBytes = sliceCount > 0 ? section.approxTextBytes / sliceCount : section.approxTextBytes;
      sectionsOut.write(&level, sizeof(level));
      sectionsOut.write(&innerStartOffset, sizeof(innerStartOffset));
      sectionsOut.write(&idLen, sizeof(idLen));
      if (idLen) sectionsOut.write(section.id.data(), idLen);
      sectionsOut.write(&titleLen, sizeof(titleLen));
      if (titleLen) sectionsOut.write(title.data(), titleLen);
      sectionsOut.write(&approxBytes, sizeof(approxBytes));
      sectionsOut.write(&vc.imageRangeStart, sizeof(vc.imageRangeStart));
      sectionsOut.write(&vc.imageRangeEnd, sizeof(vc.imageRangeEnd));
    }
    sectionsOut.close();
  }

  chapterCount = static_cast<int>(virtualChapters.size());
  if (chapterCount <= 0 || chapterCount > UINT16_MAX) {
    LOG_ERR("FB2", "FB2 has no readable chapters: %s", filepath.c_str());
    return fail();
  }

  // The marker file renderChapterOnDemand()/Epub::readItemContentsToStream()
  // key off of: its presence is what says "this package's chapters aren't
  // real files, render them from this FB2 source instead."
  if (!writeStaticFile(cachePath + SOURCE_MARKER_FILE, sourcePath.c_str())) return fail();

  if (!writeContainerFile() || !writeStyleFile() || !writeOpfFile() || !writeNcxFile(scan, sectionFirstChapterIndex))
    return fail();
  if (onProgress) onProgress(95);

  saveMetadataCache();
  saveCacheSignature();
  return true;
}

bool Fb2::persistImageIndex(const Fb2ScanResult& scan) {
  // Images are NOT decoded here - only their (id, filename, byte-offset
  // range) is recorded, so a later decodeImageOnDemand() call can seek
  // straight to the right <binary> and decode just that one image, the
  // first time it's actually about to be rendered.
  HalFile imagesOut;
  if (!Storage.openFileForWrite("FB2", cachePath + IMAGES_INDEX_FILE, imagesOut)) return false;
  writeCacheHeader(imagesOut);
  for (const auto& image : images) {
    const auto it = std::find_if(scan.binaries.begin(), scan.binaries.end(),
                                 [&](const Fb2BinaryIndexEntry& b) { return b.id == image.id; });
    if (it == scan.binaries.end()) continue;
    const uint16_t idLen = static_cast<uint16_t>(std::min(image.id.size(), static_cast<size_t>(4096)));
    const uint16_t nameLen = static_cast<uint16_t>(std::min(image.filename.size(), static_cast<size_t>(4096)));
    imagesOut.write(&idLen, sizeof(idLen));
    if (idLen) imagesOut.write(image.id.data(), idLen);
    imagesOut.write(&nameLen, sizeof(nameLen));
    if (nameLen) imagesOut.write(image.filename.data(), nameLen);
    imagesOut.write(&it->payloadStartOffset, sizeof(it->payloadStartOffset));
    imagesOut.write(&it->payloadEndOffset, sizeof(it->payloadEndOffset));
  }
  imagesOut.close();
  return true;
}

namespace {
// Bounds how many raw decoded images (the original, often much larger than
// screen-sized, .png/.jpg straight out of the FB2) stay on disk at once.
// Once a view has happened, Epub's own .pxc pixel-cache - a small,
// already-downsampled-to-screen bitmap - satisfies every later render of
// that same image, so keeping more than a couple of raw sources around
// mostly just wastes SD space on a book with many illustrations.
constexpr int MAX_CACHED_RAW_IMAGES = 3;
constexpr char IMAGE_LRU_FILE[] = "/.fb2_image_lru";
}  // namespace

// static
bool Fb2::decodeImageOnDemand(const std::string& imagePath) {
  // imagePath looks like ".../<cachePrefix>_<hash>/package.epub/OEBPS/images/image_7.png".
  // Recover that package's own cache dir from it rather than needing an
  // Fb2 instance (ImageBlock only has the path baked into its serialized
  // cache entry, from whenever the chapter was first rendered).
  constexpr char kPackageMarker[] = "/package.epub/";
  const size_t markerPos = imagePath.find(kPackageMarker);
  if (markerPos == std::string::npos) return false;
  const std::string packageCachePath = imagePath.substr(0, markerPos);
  const size_t nameStart = imagePath.find_last_of('/');
  if (nameStart == std::string::npos) return false;
  const std::string filename = imagePath.substr(nameStart + 1);

  char sourcePathBuf[600];
  const size_t sourcePathLen =
      Storage.readFileToBuffer((packageCachePath + SOURCE_MARKER_FILE).c_str(), sourcePathBuf, sizeof(sourcePathBuf));
  if (sourcePathLen == 0) return false;  // not an FB2-origin package - nothing to do
  const std::string sourcePath(sourcePathBuf, sourcePathLen);

  HalFile imagesIn;
  if (!Storage.openFileForRead("FB2", packageCachePath + IMAGES_INDEX_FILE, imagesIn)) return false;
  if (!readAndCheckCacheHeader(imagesIn)) {
    imagesIn.close();
    return false;
  }
  Fb2BinaryIndexEntry binary;
  bool found = false;
  for (;;) {
    uint16_t idLen = 0, nameLen = 0;
    if (imagesIn.read(&idLen, sizeof(idLen)) != sizeof(idLen)) break;
    std::string id(idLen, '\0');
    if (idLen && imagesIn.read(id.data(), idLen) != idLen) break;
    if (imagesIn.read(&nameLen, sizeof(nameLen)) != sizeof(nameLen)) break;
    std::string name(nameLen, '\0');
    if (nameLen && imagesIn.read(name.data(), nameLen) != nameLen) break;
    uint32_t startOffset = 0, endOffset = 0;
    if (imagesIn.read(&startOffset, sizeof(startOffset)) != sizeof(startOffset) ||
        imagesIn.read(&endOffset, sizeof(endOffset)) != sizeof(endOffset)) {
      break;
    }
    if (name == filename) {
      binary.id = std::move(id);
      binary.payloadStartOffset = startOffset;
      binary.payloadEndOffset = endOffset;
      found = true;
      break;
    }
  }
  imagesIn.close();
  if (!found) return false;

  HalFile source;
  if (!Storage.openFileForRead("FB2", sourcePath, source)) return false;
  FsFileReader reader(source);

  HalFile out;
  if (!Storage.openFileForWrite("FB2", imagePath, out)) {
    source.close();
    return false;
  }

  // See the equivalent buffering note that used to live on the old eager
  // decodeImages(): Base64Decoder's callback fires 1-3 bytes at a time, so
  // writing straight through would be tens of thousands of individual SD
  // writes for one sizeable illustration.
  constexpr size_t kFlushBufSize = 4096;
  std::vector<uint8_t> flushBuf;
  flushBuf.reserve(kFlushBufSize);
  int flushCount = 0;
  Fb2Parser parser;
  parser.decodeBinary(reader, binary, [&](const uint8_t* d, size_t n) {
    flushBuf.insert(flushBuf.end(), d, d + n);
    if (flushBuf.size() >= kFlushBufSize) {
      out.write(flushBuf.data(), flushBuf.size());
      flushBuf.clear();
      // Rare on purpose - see the comment on the equivalent yield in
      // transcodeToUtf8IfNeeded(). A single image is rarely more than a
      // few hundred KB, so this will often not fire at all, which is fine.
      if (++flushCount % 256 == 0) vTaskDelay(1);
    }
  });
  if (!flushBuf.empty()) out.write(flushBuf.data(), flushBuf.size());
  out.close();
  source.close();

  // LRU bookkeeping: note this filename as most-recently-decoded, and evict
  // the raw file for anything that's fallen out of the last
  // MAX_CACHED_RAW_IMAGES. A plain newline-separated list, same format as
  // the existing package-cache LRU index.
  const std::string lruPath = packageCachePath + IMAGE_LRU_FILE;
  std::vector<std::string> recent = readLruIndex(lruPath);
  recent.erase(std::remove(recent.begin(), recent.end(), filename), recent.end());
  recent.insert(recent.begin(), filename);
  while (static_cast<int>(recent.size()) > MAX_CACHED_RAW_IMAGES) {
    const std::string evictName = recent.back();
    recent.pop_back();
    if (evictName != filename) {
      Storage.remove((packageCachePath + "/package.epub/OEBPS/images/" + evictName).c_str());
    }
  }
  writeLruIndex(lruPath, recent);
  return true;
}

// static
uint32_t Fb2::getApproxChapterSize(const std::string& packageCachePath, int chapterIndex) {
  if (chapterIndex < 0) return 0;

  HalFile sectionsIn;
  if (!Storage.openFileForRead("FB2", packageCachePath + SECTIONS_INDEX_FILE, sectionsIn)) return 0;
  if (!readAndCheckCacheHeader(sectionsIn)) {
    sectionsIn.close();
    return 0;
  }

  uint32_t result = 0;
  for (int i = 0; i <= chapterIndex; ++i) {
    uint8_t level = 0;
    uint32_t innerStartOffset = 0;
    uint16_t idLen = 0, titleLen = 0;
    uint32_t approxTextBytes = 0;
    if (sectionsIn.read(&level, sizeof(level)) != sizeof(level) ||
        sectionsIn.read(&innerStartOffset, sizeof(innerStartOffset)) != sizeof(innerStartOffset) ||
        sectionsIn.read(&idLen, sizeof(idLen)) != sizeof(idLen)) {
      break;
    }
    if (idLen) {
      std::string skip(idLen, '\0');
      if (sectionsIn.read(skip.data(), idLen) != idLen) break;
    }
    if (sectionsIn.read(&titleLen, sizeof(titleLen)) != sizeof(titleLen)) break;
    if (titleLen) {
      std::string skip(titleLen, '\0');
      if (sectionsIn.read(skip.data(), titleLen) != titleLen) break;
    }
    if (sectionsIn.read(&approxTextBytes, sizeof(approxTextBytes)) != sizeof(approxTextBytes)) break;
    uint32_t skipRangeStart = 0, skipRangeEnd = 0;
    if (sectionsIn.read(&skipRangeStart, sizeof(skipRangeStart)) != sizeof(skipRangeStart) ||
        sectionsIn.read(&skipRangeEnd, sizeof(skipRangeEnd)) != sizeof(skipRangeEnd)) {
      break;
    }
    if (i == chapterIndex) {
      result = approxTextBytes;
      break;
    }
  }
  sectionsIn.close();
  return result;
}

// Reads the whole persisted section index once and returns a resolver
// (FB2 section id -> XHTML href) for StreamSink's onLinkBegin(), so an
// in-book link like <a l:href="#n1"> - typically a footnote reference -
// points at the right chapter file and anchor instead of rendering as
// dead plain text. Built fresh per chapter render rather than cached:
// section/chapter counts are small (tens to a few hundred), so one linear
// pass costs little, and it avoids holding this alongside everything else
// already read for the chapter being rendered.
StreamSink::LinkResolver buildLinkResolver(const std::string& packageCachePath) {
  auto idToChapter = std::make_shared<std::vector<std::pair<std::string, int>>>();

  HalFile sectionsIn;
  if (Storage.openFileForRead("FB2", packageCachePath + SECTIONS_INDEX_FILE, sectionsIn) &&
      readAndCheckCacheHeader(sectionsIn)) {
    for (int i = 0;; ++i) {
      uint8_t level = 0;
      uint32_t innerStartOffset = 0;
      uint16_t idLen = 0, titleLen = 0;
      if (sectionsIn.read(&level, sizeof(level)) != sizeof(level) ||
          sectionsIn.read(&innerStartOffset, sizeof(innerStartOffset)) != sizeof(innerStartOffset) ||
          sectionsIn.read(&idLen, sizeof(idLen)) != sizeof(idLen)) {
        break;
      }
      std::string id(idLen, '\0');
      if (idLen && sectionsIn.read(id.data(), idLen) != idLen) break;
      if (sectionsIn.read(&titleLen, sizeof(titleLen)) != sizeof(titleLen)) break;
      std::string skipTitle(titleLen, '\0');
      if (titleLen && sectionsIn.read(skipTitle.data(), titleLen) != titleLen) break;
      uint32_t skipApprox = 0, skipRangeStart = 0, skipRangeEnd = 0;
      if (sectionsIn.read(&skipApprox, sizeof(skipApprox)) != sizeof(skipApprox) ||
          sectionsIn.read(&skipRangeStart, sizeof(skipRangeStart)) != sizeof(skipRangeStart) ||
          sectionsIn.read(&skipRangeEnd, sizeof(skipRangeEnd)) != sizeof(skipRangeEnd)) {
        break;
      }
      if (!id.empty()) idToChapter->emplace_back(std::move(id), i);
    }
    sectionsIn.close();
  }

  return [idToChapter](const std::string& targetId) -> std::string {
    for (const auto& [id, chapterIndex] : *idToChapter) {
      if (id != targetId) continue;
      // Matches the anchor renderChapterOnDemand() gives this same section
      // when it's the one being rendered (see its own anchor computation) -
      // has to, since that's the id actually written into that chapter's
      // <section id="..."> tag.
      const uint64_t anchor = fnvHash64(id.data(), id.size());
      return chapterHref(chapterIndex) + "#" + anchorName(anchor);
    }
    return {};  // unresolvable: caller renders the link as plain text instead
  };
}

// static
bool Fb2::renderChapterOnDemand(const std::string& packageCachePath, int chapterIndex, Print& out) {
  if (chapterIndex < 0) return false;

  char sourcePathBuf[600];
  const size_t sourcePathLen =
      Storage.readFileToBuffer((packageCachePath + SOURCE_MARKER_FILE).c_str(), sourcePathBuf, sizeof(sourcePathBuf));
  if (sourcePathLen == 0) return false;
  const std::string sourcePath(sourcePathBuf, sourcePathLen);

  HalFile sectionsIn;
  if (!Storage.openFileForRead("FB2", packageCachePath + SECTIONS_INDEX_FILE, sectionsIn)) return false;
  if (!readAndCheckCacheHeader(sectionsIn)) {
    sectionsIn.close();
    return false;
  }

  uint8_t level = 0;
  uint32_t innerStartOffset = 0;
  uint16_t idLen = 0, titleLen = 0;
  uint32_t approxTextBytes = 0;
  uint32_t imageRangeStart = 0, imageRangeEnd = 0;
  std::string id;
  std::string title;
  bool found = false;
  for (int i = 0; i <= chapterIndex; ++i) {
    if (sectionsIn.read(&level, sizeof(level)) != sizeof(level) ||
        sectionsIn.read(&innerStartOffset, sizeof(innerStartOffset)) != sizeof(innerStartOffset) ||
        sectionsIn.read(&idLen, sizeof(idLen)) != sizeof(idLen)) {
      break;
    }
    id.assign(idLen, '\0');
    if (idLen && sectionsIn.read(id.data(), idLen) != idLen) break;
    if (sectionsIn.read(&titleLen, sizeof(titleLen)) != sizeof(titleLen)) break;
    title.assign(titleLen, '\0');
    if (titleLen && sectionsIn.read(title.data(), titleLen) != titleLen) break;
    // Every record ends with approxTextBytes then the image range (see the
    // write loop building SECTIONS_INDEX_FILE in convertToPackage()) -
    // always consumed, even for a record we're skipping past, so the read
    // position doesn't end up misaligned for the next one.
    if (sectionsIn.read(&approxTextBytes, sizeof(approxTextBytes)) != sizeof(approxTextBytes) ||
        sectionsIn.read(&imageRangeStart, sizeof(imageRangeStart)) != sizeof(imageRangeStart) ||
        sectionsIn.read(&imageRangeEnd, sizeof(imageRangeEnd)) != sizeof(imageRangeEnd)) {
      break;
    }
    if (i == chapterIndex) {
      found = true;
      break;
    }
  }
  sectionsIn.close();
  if (!found) return false;
  normalizeText(title);

  std::vector<ImageInfoPublic> images;
  {
    HalFile imagesIn;
    if (Storage.openFileForRead("FB2", packageCachePath + IMAGES_INDEX_FILE, imagesIn) && readAndCheckCacheHeader(imagesIn)) {
      for (;;) {
        uint16_t imgIdLen = 0, nameLen = 0;
        if (imagesIn.read(&imgIdLen, sizeof(imgIdLen)) != sizeof(imgIdLen)) break;
        ImageInfoPublic image;
        image.id.assign(imgIdLen, '\0');
        if (imgIdLen && imagesIn.read(image.id.data(), imgIdLen) != imgIdLen) break;
        if (imagesIn.read(&nameLen, sizeof(nameLen)) != sizeof(nameLen)) break;
        image.filename.assign(nameLen, '\0');
        if (nameLen && imagesIn.read(image.filename.data(), nameLen) != nameLen) break;
        uint32_t skipStart = 0, skipEnd = 0;
        if (imagesIn.read(&skipStart, sizeof(skipStart)) != sizeof(skipStart) ||
            imagesIn.read(&skipEnd, sizeof(skipEnd)) != sizeof(skipEnd)) {
          break;
        }
        images.push_back(std::move(image));
      }
      imagesIn.close();
    }
  }

  HalFile source;
  if (!Storage.openFileForRead("FB2", sourcePath, source)) return false;
  FsFileReader reader(source);

  writeBytes(out, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
  writeBytes(out, "<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><meta charset=\"UTF-8\"/>");
  writeBytes(out, "<link rel=\"stylesheet\" type=\"text/css\" href=\"../style.css\"/><title>");
  writeXmlEscaped(out, title);
  writeBytes(out, "</title></head><body>\n");

  const uint64_t anchor = id.empty() ? automaticAnchor("section", chapterIndex) : fnvHash64(id.data(), id.size());
  writeBytes(out, "<section id=\"");
  writeXmlEscaped(out, anchorName(anchor), true);
  writeBytes(out, "\">");

  if (!title.empty()) {
    const int heading = std::min(std::max(static_cast<int>(level) + 1, 1), 6);
    writeBytes(out, "<h" + std::to_string(heading) + ">");
    writeXmlEscaped(out, title);
    writeBytes(out, "</h" + std::to_string(heading) + ">");
  }

  Fb2SectionIndexEntry section;  // only innerStartOffset is read by renderSection()
  section.innerStartOffset = innerStartOffset;
  section.level = level;
  Fb2Parser parser;
  StreamSink sink(out, images, buildLinkResolver(packageCachePath));
  RangeFilterSink rangeSink(sink, imageRangeStart, imageRangeEnd);
  const bool renderOk = parser.renderSection(reader, section, rangeSink);

  writeBytes(out, "</section>\n</body></html>\n");
  source.close();
  return renderOk;
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
      ".underline { text-decoration: underline; }\n"
      ".smallcaps { font-variant: small-caps; }\n"
      ".code { font-family: monospace; }\n"
      "table { border-collapse: collapse; }\n"
      "td, th { border: 1px solid; padding: 0.2em 0.5em; }\n"
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

  const ImageInfoPublic* cover = findImage(coverImageId);
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

bool Fb2::writeNcxFile(const Fb2ScanResult& scan, const std::vector<int>& sectionFirstChapterIndex) const {
  HalFile file;
  if (!Storage.openFileForWrite("FB2", packagePath + "/OEBPS/toc.ncx", file)) return false;

  writeBytes(file, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                   "<ncx xmlns=\"http://www.daisy.org/z3986/2005/ncx/\" version=\"2005-1\">"
                   "<head><meta name=\"dtb:uid\" content=\"fb2\"/></head><docTitle><text>");
  writeXmlEscaped(file, title);
  writeBytes(file, "</text></docTitle><navMap>\n");

  int openDepth = 0;
  int playOrder = 1;
  bool any = false;
  for (size_t i = 0; i < scan.sections.size(); ++i) {
    const auto& section = scan.sections[i];
    // Footnotes/comments (<body name="notes">, "comments", etc.) are only
    // ever reached via an in-text link (see buildLinkResolver()) - they
    // aren't part of the normal reading flow, so they don't belong in the
    // table of contents either. The main (unnamed) body is bodyIndex 0's
    // empty name; anything else gets skipped here.
    if (section.bodyIndex >= 0 && static_cast<size_t>(section.bodyIndex) < scan.bodies.size() &&
        !scan.bodies[section.bodyIndex].name.empty()) {
      continue;
    }
    std::string sectionTitle = section.title;
    normalizeText(sectionTitle);
    if (sectionTitle.empty()) continue;

    const int targetDepth = std::min(std::max(1, static_cast<int>(section.level) + 1), openDepth + 1);
    while (openDepth >= targetDepth) {
      writeBytes(file, "</navPoint>\n");
      --openDepth;
    }

    const uint64_t anchor =
        section.id.empty() ? automaticAnchor("section", static_cast<int>(i)) : fnvHash64(section.id.data(), section.id.size());
    writeBytes(file, "<navPoint id=\"nav-" + std::to_string(playOrder) + "\" playOrder=\"" +
                         std::to_string(playOrder) + "\"><navLabel><text>");
    writeXmlEscaped(file, sectionTitle);
    writeBytes(file, "</text></navLabel><content src=\"");
    // A split section's TOC entry always points at its first virtual
    // chapter (see splitSectionsForImageLoad()) - i may not equal the
    // chapter index once any earlier section has been split.
    writeBytes(file, chapterHref(sectionFirstChapterIndex[i]));
    writeBytes(file, "#" + anchorName(anchor));
    writeBytes(file, "\"/>\n");
    openDepth = targetDepth;
    ++playOrder;
    any = true;
  }

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
