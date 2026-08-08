#include "EpubChapterSplitter.h"

#include <HalStorage.h>
#include <Logging.h>
#include <MemoryBudget.h>
#include <ZipFile.h>

#include <algorithm>
#include <cstdlib>
#include <deque>
#include <unordered_map>

#include "EpubOpfLite.h"
#include "Epub/preprocess/EpubChapterSplitter.h"
#include "Epub/preprocess/EpubOpfRewriter.h"

namespace {

// Matches the same guard EpubReaderActivity.cpp uses right before it would
// otherwise call Section::createSectionFile() on a spine item this large -
// kept in sync deliberately: that check is what a book falls back to if a
// cached split somehow doesn't cover every oversized item (this one being
// unreachable/failing, or a future book shaped in some way this doesn't
// handle), so the two thresholds agreeing means "needs splitting" and
// "would otherwise hang" describe exactly the same set of books.
constexpr size_t MAX_SANE_SPINE_ITEM_BYTES = 1024 * 1024;
constexpr size_t TARGET_CHUNK_BYTES = 250 * 1024;
// Inspecting a large OPF needs both the temporary decompression buffer and a
// std::string copy.  The splitter is optional, so do not let a catalogue with
// hundreds of already-small spine files exhaust a fragmented ESP32 heap just
// to discover that no split is required.
constexpr size_t MAX_OPF_BYTES_FOR_OPTIONAL_SPLIT = 48 * 1024;
// The splitter streams the oversized XHTML from SD in 4 KiB blocks.  It no
// longer needs a multi-megabyte allocation; keep only enough headroom for its
// small ZIP indexes, XML parser and page renderer.
constexpr uint32_t SPLITTER_MIN_FREE_HEAP = 112U * 1024U;
constexpr uint32_t SPLITTER_MIN_MAX_ALLOC = 64U * 1024U;
constexpr char CACHE_MAGIC[] = "EPUBSPLIT";
constexpr size_t CACHE_MAGIC_LEN = 9;
constexpr uint8_t CACHE_VERSION = 1;
constexpr char SIGNATURE_FILE[] = "/.split_signature.bin";
constexpr char PACKAGE_DIR[] = "/package.epub";

std::string dirnameWithSlash(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  return slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
}

std::string resolveHref(const std::string& baseDir, const std::string& href) {
  return baseDir + href;
}

bool findContentOpfPath(ZipFile& zip, std::string& outPath) {
  size_t containerSize = 0;
  if (!zip.getInflatedFileSize("META-INF/container.xml", &containerSize) || containerSize == 0 ||
      containerSize > 8192) {
    return false;
  }
  size_t readSize = 0;
  uint8_t* buf = zip.readFileToMemory("META-INF/container.xml", &readSize, true);
  if (!buf) return false;
  const std::string content(reinterpret_cast<char*>(buf), readSize);
  free(buf);

  const std::string needle = "full-path=\"";
  const size_t attrPos = content.find(needle);
  if (attrPos == std::string::npos) return false;
  const size_t valueStart = attrPos + needle.size();
  const size_t valueEnd = content.find('"', valueStart);
  if (valueEnd == std::string::npos) return false;
  outPath = content.substr(valueStart, valueEnd - valueStart);
  return !outPath.empty();
}

bool readWholeFile(ZipFile& zip, const std::string& zipPath, std::string& out) {
  size_t size = 0;
  uint8_t* buf = zip.readFileToMemory(zipPath.c_str(), &size, true);
  if (!buf) return false;
  out.assign(reinterpret_cast<char*>(buf), size);
  free(buf);
  return true;
}

bool writeWholeFile(const std::string& path, const std::string& content) {
  HalFile f;
  if (!Storage.openFileForWrite("EPS", path, f)) return false;
  const bool ok = f.write(content.data(), content.size()) == content.size();
  f.close();
  return ok;
}

// Copies every entry straight from the source zip into destDir, preserving
// the zip's own internal directory structure - the starting point before
// any split-specific files get overwritten on top.
bool unpackWholeZip(const std::string& originalPath, const std::string& destDir) {
  ZipFile zip(originalPath);
  // enumerateFilePaths keeps the ZIP's central-directory file handle open.
  // Calling readFileToStream() inside its callback seeks that same handle to
  // payload data, so enumeration silently stopped after the first entry.  Keep
  // the names first; each later stream read then opens/seeks the archive cleanly.
  std::vector<std::string> paths;
  if (!zip.enumerateFilePaths([&](std::string_view path) {
        if (!path.empty() && path.back() != '/') paths.emplace_back(path);
      })) {
    return false;
  }
  bool allOk = true;
  for (const std::string& path : paths) {
    const std::string destPath = destDir + "/" + path;
    const size_t lastSlash = destPath.find_last_of('/');
    if (lastSlash != std::string::npos) {
      Storage.mkdir(destPath.substr(0, lastSlash).c_str(), true);
    }
    HalFile out;
    if (!Storage.openFileForWrite("EPS", destPath, out)) {
      allOk = false;
      continue;
    }
    const bool streamOk = zip.readFileToStream(path.c_str(), out, 4096);
    out.close();
    if (!streamOk) allOk = false;
  }
  return allOk;
}

std::string basenameOf(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string chunkFileName(const std::string& originalHref, size_t chunkIndex) {
  // "book0.html" -> "book0_split0.html", keeping the original extension so
  // the manifest's media-type guess (and any tooling that sniffs by
  // extension) still sees a normal xhtml/html file.
  const size_t dot = originalHref.find_last_of('.');
  const std::string base = dot == std::string::npos ? originalHref : originalHref.substr(0, dot);
  const std::string ext = dot == std::string::npos ? std::string() : originalHref.substr(dot);
  return base + "_split" + std::to_string(chunkIndex) + ext;
}

// Rewrites every href="target[#anchor]" this book's other XHTML files (and
// the split parts themselves) might use to point at the file that just got
// split, redirecting each to whichever part actually contains that anchor
// now (or part 0, for a bare reference to the file with no anchor at all).
// idToChunk must already cover every id from every chunk (built while
// splitting, before this runs).
void rewriteLinksToSplitFile(std::string& xhtmlContent, const std::string& originalHref,
                              const std::unordered_map<std::string, size_t>& idToChunk,
                              const std::vector<std::string>& chunkNames) {
  const std::string needle = "href=\"" + originalHref;
  size_t pos = 0;
  while ((pos = xhtmlContent.find(needle, pos)) != std::string::npos) {
    const size_t hrefValueStart = pos + 6;  // strlen("href=\"")
    const size_t closeQuote = xhtmlContent.find('"', hrefValueStart);
    if (closeQuote == std::string::npos) break;
    // Only rewrite an exact match on the filename, not some other href that
    // merely starts with the same characters (e.g. "book0.html" vs a
    // hypothetical "book0.html.bak" elsewhere) - the next character must be
    // '#' (an anchor) or the closing quote (a bare reference).
    const size_t afterName = pos + needle.size();
    if (afterName != closeQuote && xhtmlContent[afterName] != '#') {
      pos = closeQuote + 1;
      continue;
    }
    std::string anchor;
    if (afterName < closeQuote && xhtmlContent[afterName] == '#') {
      anchor = xhtmlContent.substr(afterName + 1, closeQuote - afterName - 1);
    }
    size_t targetChunk = 0;
    if (!anchor.empty()) {
      const auto it = idToChunk.find(anchor);
      if (it != idToChunk.end()) targetChunk = it->second;
    }
    const std::string replacement = "href=\"" + chunkNames[targetChunk] + (anchor.empty() ? "" : "#" + anchor);
    xhtmlContent.replace(pos, closeQuote - pos, replacement);
    pos += replacement.size() + 1;  // past the closing quote we didn't touch
  }
}

}  // namespace

namespace {

// The actual work, once the caller already knows originalPath is a real
// zip file worth inspecting - split out mainly so it's testable on its own
// against a fake ZipFile backed by a plain directory, without needing a
// real .epub archive on hand.
std::string resolveReadPathForZip(const std::string& originalPath, const std::string& cacheBaseDir) {
  const std::string cacheKey = "epubsplit_" + std::to_string(std::hash<std::string>{}(originalPath));
  const std::string cachePath = cacheBaseDir + "/" + cacheKey;
  const std::string packagePath = cachePath + PACKAGE_DIR;

  uint64_t sourceSize = 0;
  {
    HalFile source;
    if (Storage.openFileForRead("EPS", originalPath, source)) {
      sourceSize = source.fileSize64();
      source.close();
    }
  }

  // A valid cache from a previous open - use it as-is.
  {
    HalFile sig;
    if (Storage.openFileForRead("EPS", cachePath + SIGNATURE_FILE, sig)) {
      char magic[CACHE_MAGIC_LEN];
      uint8_t version = 0;
      uint64_t cachedSize = 0;
      const bool valid = sig.read(magic, CACHE_MAGIC_LEN) == CACHE_MAGIC_LEN &&
                         memcmp(magic, CACHE_MAGIC, CACHE_MAGIC_LEN) == 0 &&
                         sig.read(&version, sizeof(version)) == sizeof(version) && version == CACHE_VERSION &&
                         sig.read(&cachedSize, sizeof(cachedSize)) == sizeof(cachedSize) && cachedSize == sourceSize;
      sig.close();
      if (valid) return packagePath;
    }
  }

  // A cached package needs no preparation.  With a new cache, leave the
  // EPUB untouched when memory is low: normal EPUBs will then open through
  // the regular reader path instead of resetting during optional splitting.
  const auto heap = MemoryBudget::snapshot();
  if (!MemoryBudget::hasHeap(heap, SPLITTER_MIN_FREE_HEAP, SPLITTER_MIN_MAX_ALLOC)) {
    LOG_INF("EPS", "Skipping optional EPUB split: low heap (%u free, %u max alloc)", heap.freeHeap,
            heap.maxAllocHeap);
    return originalPath;
  }

  // No valid cache yet - check whether this book even needs one. Cheap:
  // one small content.opf read, one batched zip-central-directory size
  // lookup. Any failure along the way (missing container.xml, unparseable
  // OPF, zip errors) falls back to "leave it alone" rather than treating
  // it as "needs splitting" - the size guard EpubReaderActivity.cpp
  // already has is what actually keeps a genuinely oversized, unhandled
  // book from hanging either way, so bailing out here risks nothing beyond
  // that same guard doing its job as it already did before this existed.
  ZipFile zip(originalPath);
  std::string opfPath;
  if (!findContentOpfPath(zip, opfPath)) return originalPath;
  size_t opfSize = 0;
  if (!zip.getInflatedFileSize(opfPath.c_str(), &opfSize)) return originalPath;
  if (opfSize > MAX_OPF_BYTES_FOR_OPTIONAL_SPLIT) {
    LOG_INF("EPS", "Skipping optional EPUB split: content.opf is %u bytes", static_cast<unsigned>(opfSize));
    return originalPath;
  }
  std::string opfContent;
  if (!readWholeFile(zip, opfPath, opfContent)) return originalPath;
  EpubOpfLite opf;
  if (!EpubOpfLite::parse(opfContent, opf) || opf.manifest.empty() || opf.spineIdrefs.empty()) return originalPath;

  const std::string opfDir = dirnameWithSlash(opfPath);
  const auto findManifestItem = [&opf](const std::string& idref) -> const EpubOpfManifestItem* {
    for (const auto& item : opf.manifest) {
      if (item.id == idref) return &item;
    }
    return nullptr;
  };

  std::deque<ZipFile::SizeTarget> targets;
  for (size_t i = 0; i < opf.spineIdrefs.size(); ++i) {
    const auto* item = findManifestItem(opf.spineIdrefs[i]);
    const std::string zipPath = item ? resolveHref(opfDir, item->href) : std::string();
    if (zipPath.empty() || i > 0xFFFF) continue;
    targets.push_back({ZipFile::fnvHash64(zipPath.data(), zipPath.size()), static_cast<uint16_t>(zipPath.size()),
                       static_cast<uint16_t>(i)});
  }
  std::sort(targets.begin(), targets.end(),
            [](const ZipFile::SizeTarget& a, const ZipFile::SizeTarget& b) {
              return a.hash != b.hash ? a.hash < b.hash : a.len < b.len;
            });
  std::deque<uint32_t> sizes(opf.spineIdrefs.size(), 0);
  zip.fillUncompressedSizes(targets, sizes);

  int oversizedSpineIndex = -1;
  for (size_t i = 0; i < sizes.size(); ++i) {
    if (sizes[i] > MAX_SANE_SPINE_ITEM_BYTES) {
      oversizedSpineIndex = static_cast<int>(i);
      // Handle one at a time - a book with more than one spine item this
      // large is exotic enough that fixing the first is enough for now;
      // the same size guard in EpubReaderActivity.cpp still protects
      // whichever one this pass doesn't get to.
      break;
    }
  }
  if (oversizedSpineIndex < 0) return originalPath;  // the common case: nothing here needs splitting

  const auto oversizedItem = findManifestItem(opf.spineIdrefs[oversizedSpineIndex]);
  if (!oversizedItem) return originalPath;
  const std::string oversizedSpinePath = resolveHref(opfDir, oversizedItem->href);

  LOG_INF("EPS", "Spine item '%s' is %u bytes - splitting into a cached copy",
          oversizedSpinePath.c_str(), sizes[oversizedSpineIndex]);

  if (Storage.exists(cachePath.c_str())) Storage.removeDir(cachePath.c_str());
  Storage.mkdir(packagePath.c_str(), true);
  if (!unpackWholeZip(originalPath, packagePath)) {
    LOG_ERR("EPS", "Failed to unpack %s for splitting", originalPath.c_str());
    Storage.removeDir(cachePath.c_str());
    return originalPath;
  }

  const std::string oversizedHref = oversizedSpinePath;
  const std::string oversizedDir = dirnameWithSlash(oversizedHref);
  const std::string sourcePath = packagePath + "/" + oversizedHref;
  std::string baseName = basenameOf(oversizedHref);
  const size_t extension = baseName.find_last_of('.');
  if (extension != std::string::npos) baseName.resize(extension);

  // The split scanner reads and writes 4 KiB blocks. It keeps only boundary
  // offsets and ids in RAM, never the multi-megabyte XHTML source itself.
  std::unordered_map<std::string, int> anchorFragments;
  auto chunkNames = EpubStreamingChapterSplitter::splitToFragments(
      sourcePath, packagePath + "/" + oversizedDir, baseName, &anchorFragments);
  if (chunkNames.size() < 2) {
    LOG_ERR("EPS", "Oversized spine item did not yield a usable streaming split");
    Storage.removeDir(cachePath.c_str());
    return originalPath;
  }

  std::vector<std::string> chunkHrefs;
  chunkHrefs.reserve(chunkNames.size());
  for (const auto& name : chunkNames) chunkHrefs.push_back(dirnameWithSlash(oversizedItem->href) + name);

  std::string originalItemId;
  const std::string rewrittenOpf =
      EpubOpfRewriter::rewriteForSplitItem(opfContent, oversizedItem->href, chunkHrefs, &originalItemId);
  if (rewrittenOpf.empty() || !writeWholeFile(packagePath + "/" + opfPath, rewrittenOpf)) {
    LOG_ERR("EPS", "Could not rewrite content.opf for streaming split");
    Storage.removeDir(cachePath.c_str());
    return originalPath;
  }

  // Preserve NCX navigation before removing the original target file.
  for (const auto& item : opf.manifest) {
    if (item.mediaType.find("ncx") == std::string::npos) continue;
    const std::string ncxPath = packagePath + "/" + resolveHref(opfDir, item.href);
    HalFile ncx;
    if (!Storage.openFileForRead("EPS", ncxPath, ncx)) continue;
    const size_t size = ncx.fileSize();
    std::string content(size, '\0');
    const bool readOk = ncx.read(content.data(), size) == static_cast<int>(size);
    ncx.close();
    if (!readOk) continue;
    const std::string rewritten =
        EpubNcxRewriter::redirectReferences(content, oversizedItem->href, chunkHrefs, anchorFragments);
    if (rewritten != content && !writeWholeFile(ncxPath, rewritten)) {
      LOG_ERR("EPS", "Could not rewrite NCX for streaming split");
      Storage.removeDir(cachePath.c_str());
      return originalPath;
    }
  }

  Storage.remove(sourcePath.c_str());

  // Signature last, only once everything else has succeeded - if the
  // device loses power mid-build, the next open finds no valid signature
  // and rebuilds cleanly instead of trusting a half-written cache.
  {
    HalFile sig;
    if (Storage.openFileForWrite("EPS", cachePath + SIGNATURE_FILE, sig)) {
      sig.write(CACHE_MAGIC, CACHE_MAGIC_LEN);
      const uint8_t version = CACHE_VERSION;
      sig.write(&version, sizeof(version));
      sig.write(&sourceSize, sizeof(sourceSize));
      sig.close();
    }
  }

  return packagePath;
}

}  // namespace

std::string EpubChapterSplitter::resolveReadPath(const std::string& originalPath, const std::string& cacheBaseDir) {
  // Only a real zip needs any of this - an already-unpacked directory (an
  // FB2-converted package, or a pre-unpacked EPUB some other tool made)
  // never does: its chapters were already bounded by whatever built it.
  HalFile probe;
  if (!Storage.openFileForRead("EPS", originalPath, probe)) return originalPath;
  const bool isDirectory = probe.isDirectory();
  probe.close();
  if (isDirectory) return originalPath;

  return resolveReadPathForZip(originalPath, cacheBaseDir);
}

#ifdef EPUB_CHAPTER_SPLITTER_EXPOSE_FOR_TESTS
std::string EpubChapterSplitter::resolveReadPathForZipTestOnly(const std::string& originalPath,
                                                                const std::string& cacheBaseDir) {
  return resolveReadPathForZip(originalPath, cacheBaseDir);
}
#endif
