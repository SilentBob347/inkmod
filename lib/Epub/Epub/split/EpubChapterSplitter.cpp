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
#include "HtmlBodySplitter.h"

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
// Splitting is optional.  Its first pass uses temporary ZIP indexes and
// strings, so it must not start on a no-PSRAM X4 with fragmented heap.
constexpr uint32_t SPLITTER_MIN_FREE_HEAP = 160U * 1024U;
constexpr uint32_t SPLITTER_MIN_MAX_ALLOC = 128U * 1024U;
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
  bool allOk = true;
  zip.enumerateFilePaths([&](std::string_view path) {
    if (path.empty() || path.back() == '/') return;  // directory entry, nothing to copy
    const std::string destPath = destDir + "/" + std::string(path);
    const size_t lastSlash = destPath.find_last_of('/');
    if (lastSlash != std::string::npos) {
      Storage.mkdir(destPath.substr(0, lastSlash).c_str(), true);
    }
    HalFile out;
    if (!Storage.openFileForWrite("EPS", destPath, out)) {
      allOk = false;
      return;
    }
    const bool streamOk = zip.readFileToStream(std::string(path).c_str(), out, 4096);
    out.close();
    if (!streamOk) allOk = false;
  });
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

  // A regular EPUB only reaches the return above. Keep these two maps out of
  // that path: a book with hundreds of small chapters otherwise duplicates
  // every OPF id and path merely to prove that no split is needed.
  std::unordered_map<std::string, std::string> idToHref;
  idToHref.reserve(opf.manifest.size());
  for (const auto& item : opf.manifest) {
    idToHref.emplace(item.id, resolveHref(opfDir, item.href));
  }

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
  std::string oversizedContent;
  {
    HalFile f;
    if (!Storage.openFileForRead("EPS", packagePath + "/" + oversizedHref, f)) {
      Storage.removeDir(cachePath.c_str());
      return originalPath;
    }
    const size_t sz = f.fileSize();
    oversizedContent.resize(sz);
    f.read(oversizedContent.data(), sz);
    f.close();
  }

  const size_t headOpen = oversizedContent.find("<head");
  const size_t headClose = oversizedContent.find("</head>");
  const size_t bodyOpenTag = oversizedContent.find("<body");
  const size_t bodyOpenEnd =
      bodyOpenTag == std::string::npos ? std::string::npos : oversizedContent.find('>', bodyOpenTag);
  const size_t bodyClose = oversizedContent.find("</body>");
  if (headOpen == std::string::npos || headClose == std::string::npos || bodyOpenEnd == std::string::npos ||
      bodyClose == std::string::npos || bodyClose <= bodyOpenEnd) {
    LOG_ERR("EPS", "Could not find <head>/<body> in oversized spine item - leaving book unsplit");
    Storage.removeDir(cachePath.c_str());
    return originalPath;
  }
  const std::string headContent = oversizedContent.substr(headOpen, headClose + 7 - headOpen);
  const std::string bodyContent = oversizedContent.substr(bodyOpenEnd + 1, bodyClose - bodyOpenEnd - 1);

  std::vector<HtmlBodySplitter::Chunk> chunks;
  if (!HtmlBodySplitter::split(bodyContent, TARGET_CHUNK_BYTES, chunks) || chunks.size() < 2) {
    LOG_ERR("EPS", "Oversized spine item did not yield a usable split - leaving book unsplit");
    Storage.removeDir(cachePath.c_str());
    return originalPath;
  }

  std::vector<std::string> chunkNames;
  std::unordered_map<std::string, size_t> idToChunk;
  for (size_t i = 0; i < chunks.size(); ++i) {
    chunkNames.push_back(chunkFileName(basenameOf(oversizedHref), i));
    for (const auto& id : chunks[i].idsInChunk) idToChunk[id] = i;
  }

  const std::string oversizedDir = dirnameWithSlash(oversizedHref);
  bool writeFailed = false;
  for (size_t i = 0; i < chunks.size() && !writeFailed; ++i) {
    std::string doc = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<html xmlns=\"http://www.w3.org/1999/xhtml\">";
    doc += headContent;
    doc += "<body>";
    doc += chunks[i].html;
    doc += "</body></html>\n";
    writeFailed = !writeWholeFile(packagePath + "/" + oversizedDir + chunkNames[i], doc);
  }
  if (writeFailed) {
    Storage.removeDir(cachePath.c_str());
    return originalPath;
  }
  Storage.remove((packagePath + "/" + oversizedHref).c_str());

  // Rewrite every other XHTML/HTML manifest item, and the new split parts
  // themselves, for cross-references into the file that just moved.
  for (const auto& item : opf.manifest) {
    if (item.mediaType.find("html") == std::string::npos) continue;
    const std::string itemZipPath = resolveHref(opfDir, item.href);
    if (itemZipPath == oversizedHref) continue;  // that file no longer exists; its replacements are handled below
    HalFile f;
    if (!Storage.openFileForRead("EPS", packagePath + "/" + itemZipPath, f)) continue;
    const size_t sz = f.fileSize();
    std::string content(sz, '\0');
    f.read(content.data(), sz);
    f.close();
    const std::string before = content;
    rewriteLinksToSplitFile(content, basenameOf(oversizedHref), idToChunk, chunkNames);
    if (content != before) writeWholeFile(packagePath + "/" + itemZipPath, content);
  }
  for (size_t i = 0; i < chunks.size(); ++i) {
    const std::string chunkPath = packagePath + "/" + oversizedDir + chunkNames[i];
    HalFile f;
    if (!Storage.openFileForRead("EPS", chunkPath, f)) continue;
    const size_t sz = f.fileSize();
    std::string content(sz, '\0');
    f.read(content.data(), sz);
    f.close();
    const std::string before = content;
    rewriteLinksToSplitFile(content, basenameOf(oversizedHref), idToChunk, chunkNames);
    if (content != before) writeWholeFile(chunkPath, content);
  }

  // Rewrite content.opf: regenerate the whole <manifest>...</manifest> and
  // <spine ...>...</spine> blocks (each is unique and root-level in any
  // valid OPF, so finding them by tag name alone is reliable) rather than
  // trying to surgically edit individual <item>/<itemref> elements in
  // place - everything else in the file (metadata, guide, package
  // attributes) is copied through untouched.
  {
    const size_t manifestStart = opfContent.find("<manifest");
    const size_t manifestEnd = opfContent.find("</manifest>");
    const size_t spineStart = opfContent.find("<spine");
    const size_t spineEndTag = opfContent.find("</spine>");
    if (manifestStart == std::string::npos || manifestEnd == std::string::npos || spineStart == std::string::npos ||
        spineEndTag == std::string::npos || spineStart < manifestEnd) {
      LOG_ERR("EPS", "content.opf doesn't have the expected <manifest>/<spine> shape - leaving book unsplit");
      Storage.removeDir(cachePath.c_str());
      return originalPath;
    }
    const size_t manifestCloseEnd = manifestEnd + std::string("</manifest>").size();
    const size_t spineCloseEnd = spineEndTag + std::string("</spine>").size();

    std::string newManifest = "<manifest>";
    for (const auto& item : opf.manifest) {
      const std::string itemZipPath = resolveHref(opfDir, item.href);
      if (itemZipPath != oversizedHref) {
        newManifest += "<item id=\"" + item.id + "\" href=\"" + item.href + "\" media-type=\"" + item.mediaType + "\"/>";
        continue;
      }
      for (size_t i = 0; i < chunks.size(); ++i) {
        newManifest += "<item id=\"" + item.id + "_split" + std::to_string(i) + "\" href=\"" +
                       dirnameWithSlash(item.href) + chunkNames[i] + "\" media-type=\"" + item.mediaType + "\"/>";
      }
    }
    newManifest += "</manifest>";

    std::string newSpine = opfContent.substr(spineStart, opfContent.find('>', spineStart) + 1 - spineStart);
    for (const auto& idref : opf.spineIdrefs) {
      const auto it = idToHref.find(idref);
      const std::string itemZipPath = it == idToHref.end() ? std::string() : it->second;
      if (itemZipPath != oversizedHref) {
        newSpine += "<itemref idref=\"" + idref + "\"/>";
        continue;
      }
      for (size_t i = 0; i < chunks.size(); ++i) {
        newSpine += "<itemref idref=\"" + idref + "_split" + std::to_string(i) + "\"/>";
      }
    }
    newSpine += "</spine>";

    std::string newOpf = opfContent.substr(0, manifestStart);
    newOpf += newManifest;
    newOpf += opfContent.substr(manifestCloseEnd, spineStart - manifestCloseEnd);
    newOpf += newSpine;
    newOpf += opfContent.substr(spineCloseEnd);

    if (!writeWholeFile(packagePath + "/" + opfPath, newOpf)) {
      Storage.removeDir(cachePath.c_str());
      return originalPath;
    }
  }

  // Rewrite toc.ncx the same way readers everywhere expect - any
  // <content src="oversized.html#anchor"/> needs to point at whichever
  // split part actually contains that anchor now.
  for (const auto& item : opf.manifest) {
    if (item.mediaType.find("ncx") == std::string::npos) continue;
    const std::string ncxZipPath = resolveHref(opfDir, item.href);
    HalFile f;
    if (!Storage.openFileForRead("EPS", packagePath + "/" + ncxZipPath, f)) continue;
    const size_t sz = f.fileSize();
    std::string content(sz, '\0');
    f.read(content.data(), sz);
    f.close();
    const std::string before = content;
    // toc.ncx uses <content src="..."/> rather than <a href="...">, but the
    // same "does this file need redirecting to a split part" logic
    // applies - reuse it by temporarily presenting the attribute the same
    // way.
    size_t pos = 0;
    const std::string needle = "src=\"" + oversizedHref;
    while ((pos = content.find(needle, pos)) != std::string::npos) {
      content.replace(pos, 3, "href=\"");  // "src=" (3 chars incl. the shared '=') -> "href="
      pos += 6;
    }
    rewriteLinksToSplitFile(content, basenameOf(oversizedHref), idToChunk, chunkNames);
    pos = 0;
    const std::string hrefToChunkNeedle = "href=\"";
    // Only the ones just retargeted at a chunk file need converting back;
    // any genuinely unrelated href="..." elsewhere in the NCX (there
    // normally aren't any) would also match here, which is harmless - NCX
    // has no href attribute of its own to collide with.
    while ((pos = content.find(hrefToChunkNeedle, pos)) != std::string::npos) {
      content.replace(pos, 5, "src=\"");  // "href=" (5 chars incl. '=') -> "src="
      pos += 5;
    }
    if (content != before) writeWholeFile(packagePath + "/" + ncxZipPath, content);
  }

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
