#include "EpubChapterSplitter.h"

#include <HalStorage.h>
#include <Logging.h>
#include <expat.h>

#include <algorithm>
#include <cstring>
#include <utility>

namespace EpubStreamingChapterSplitter {

namespace {

struct ScanContext {
  XML_Parser parser = nullptr;  // set right after XML_ParserCreate(), needed by the callbacks below
  int depthSinceBody = -1;      // -1 = haven't seen <body> yet; 0 = inside <body>, at its direct children's level
  size_t bodyContentStartOffset = 0;  // byte right after <body ...>'s closing '>'
  bool sawBody = false;
  bool bodyClosed = false;
  std::vector<size_t> splitPoints;  // absolute byte offsets, each a safe boundary between top-level body children

  // Every element id="" seen anywhere under <body>, with the byte offset
  // of that element's own opening tag - resolved to a fragment index
  // after boundaries are chosen, so a toc.ncx entry pointing at "#someid"
  // can be redirected to the right fragment.
  std::vector<std::pair<std::string, size_t>> anchorOffsets;

  // Set once, right after </head> closes - everything from file start
  // through here (the <?xml?>/<html>/<head>...</head> prefix, including
  // any <link rel="stylesheet"> the original had) gets prepended to every
  // fragment so each one keeps the book's own styling. If the document
  // has no <head> (rare but not invalid for embedded HTML fragments),
  // this stays 0 and each fragment just gets a minimal, unstyled shell.
  size_t headEndOffset = 0;
  bool inHead = false;
};

// Byte offset of the character right after whatever expat just finished
// handling (its current event's start + length) - not exposed directly by
// expat as one call, but composable from the two that are.
size_t currentEventEndOffset(XML_Parser p) {
  const XML_Index start = XML_GetCurrentByteIndex(p);
  if (start < 0) return 0;
  return static_cast<size_t>(start) + static_cast<size_t>(XML_GetCurrentByteCount(p));
}

void XMLCALL onStart(void* userData, const XML_Char* name, const XML_Char** atts) {
  auto* ctx = static_cast<ScanContext*>(userData);
  const std::string tag(name);
  if (tag == "head") {
    ctx->inHead = true;
    return;
  }
  if (!ctx->sawBody) {
    if (tag == "body") {
      ctx->sawBody = true;
      ctx->depthSinceBody = 0;
      ctx->bodyContentStartOffset = currentEventEndOffset(ctx->parser);
    }
    return;
  }
  if (ctx->bodyClosed) return;
  ctx->depthSinceBody++;
  for (int i = 0; atts[i]; i += 2) {
    if (std::string(atts[i]) == "id" && atts[i + 1] && atts[i + 1][0] != '\0') {
      const XML_Index start = XML_GetCurrentByteIndex(ctx->parser);
      ctx->anchorOffsets.emplace_back(std::string(atts[i + 1]), start < 0 ? 0 : static_cast<size_t>(start));
      break;
    }
  }
}

void XMLCALL onEnd(void* userData, const XML_Char* name) {
  auto* ctx = static_cast<ScanContext*>(userData);
  const std::string tag(name);
  if (tag == "head" && ctx->inHead) {
    ctx->inHead = false;
    ctx->headEndOffset = currentEventEndOffset(ctx->parser);
    return;
  }
  if (!ctx->sawBody || ctx->bodyClosed) return;
  if (ctx->depthSinceBody == 0) {
    // This is </body> itself, since depth 0 means "direct child of body"
    // and everything at that depth already decremented back to 0 when
    // its own end tag fired - the only end-tag event left at depth 0 is
    // body's own.
    ctx->bodyClosed = true;
    return;
  }
  ctx->depthSinceBody--;
  if (ctx->depthSinceBody == 0) {
    // Just closed a direct child of <body> - a safe place to end a
    // fragment. Record every one of these; the caller decides which
    // subset to actually use as real split points based on accumulated
    // size, so a single scan serves any target fragment size.
    ctx->splitPoints.push_back(currentEventEndOffset(ctx->parser));
  }
}

bool writeFragment(const std::string& sourcePath, const std::string& outPath, const std::string& headerBytes,
                   size_t bodyStart, size_t bodyEnd) {
  HalFile in;
  if (!Storage.openFileForRead("EHS", sourcePath, in)) return false;
  HalFile out;
  if (!Storage.openFileForWrite("EHS", outPath, out)) {
    in.close();
    return false;
  }

  bool ok = out.write(headerBytes.data(), headerBytes.size()) == headerBytes.size();

  if (ok && !in.seek(bodyStart)) ok = false;
  constexpr size_t kBufSize = 4096;
  std::vector<uint8_t> buf(kBufSize);
  size_t remaining = bodyEnd - bodyStart;
  while (ok && remaining > 0) {
    const size_t want = std::min(remaining, kBufSize);
    const int got = in.read(buf.data(), want);
    if (got <= 0) {
      ok = false;
      break;
    }
    if (out.write(buf.data(), static_cast<size_t>(got)) != static_cast<size_t>(got)) {
      ok = false;
      break;
    }
    remaining -= static_cast<size_t>(got);
  }

  constexpr char kFooter[] = "</body></html>";
  if (ok) ok = out.write(kFooter, sizeof(kFooter) - 1) == sizeof(kFooter) - 1;

  in.close();
  out.close();
  if (!ok) Storage.remove(outPath.c_str());
  return ok;
}

}  // namespace

std::vector<std::string> splitToFragments(const std::string& sourcePath, const std::string& outputDir,
                                          const std::string& baseName,
                                          std::unordered_map<std::string, int>* anchorFragmentOut) {
  HalFile source;
  if (!Storage.openFileForRead("EHS", sourcePath, source)) {
    LOG_ERR("EHS", "splitToFragments: can't open %s", sourcePath.c_str());
    return {};
  }
  const size_t fileSize = static_cast<size_t>(source.fileSize64());

  XML_Parser parser = XML_ParserCreate(nullptr);
  if (!parser) {
    source.close();
    return {};
  }
  ScanContext ctx;
  ctx.parser = parser;
  XML_SetUserData(parser, &ctx);
  XML_SetElementHandler(parser, onStart, onEnd);

  bool parseOk = true;
  constexpr size_t kChunkSize = 4096;
  std::vector<char> chunk(kChunkSize);
  for (;;) {
    const int got = source.read(chunk.data(), chunk.size());
    if (got < 0) {
      parseOk = false;
      break;
    }
    const bool isFinal = got == 0;
    if (XML_Parse(parser, chunk.data(), got, isFinal) == XML_STATUS_ERROR) {
      LOG_ERR("EHS", "splitToFragments: XML parse error in %s at byte %ld: %s", sourcePath.c_str(),
              static_cast<long>(XML_GetCurrentByteIndex(parser)), XML_ErrorString(XML_GetErrorCode(parser)));
      parseOk = false;
      break;
    }
    if (isFinal) break;
  }
  source.close();
  XML_ParserFree(parser);

  if (!parseOk || !ctx.sawBody || ctx.splitPoints.empty()) {
    if (!ctx.sawBody) LOG_ERR("EHS", "splitToFragments: no <body> found in %s", sourcePath.c_str());
    return {};
  }

  // Build the shared header: original <?xml?>/<html>/<head>...</head>
  // (or, if there was no <head>, everything up through <body ...>'s own
  // opening tag) plus a synthetic opening <body> - every fragment gets
  // this verbatim, so stylesheet links and any <html>/xmlns attributes
  // the original had are preserved in each one.
  const size_t headerEnd = ctx.headEndOffset > 0 ? ctx.headEndOffset : 0;
  std::string headerBytes;
  {
    HalFile in;
    if (!Storage.openFileForRead("EHS", sourcePath, in)) return {};
    headerBytes.resize(headerEnd);
    if (headerEnd > 0 && in.read(headerBytes.data(), headerEnd) != static_cast<int>(headerEnd)) {
      in.close();
      return {};
    }
    in.close();
  }
  headerBytes += "<body>";

  // Pick actual split points from the candidates: the first one at or past
  // each TARGET_FRAGMENT_BYTES multiple, measured from the previous split
  // (or from the start of <body> for the first fragment).
  std::vector<size_t> boundaries;  // absolute offsets, in order; last is always the final split point
  size_t sinceLastSplit = ctx.bodyContentStartOffset;
  for (const size_t candidate : ctx.splitPoints) {
    if (candidate - sinceLastSplit >= TARGET_FRAGMENT_BYTES) {
      boundaries.push_back(candidate);
      sinceLastSplit = candidate;
    }
  }
  // Whatever's left after the last chosen boundary (from there to the very
  // last candidate, which is always right before </body>) becomes the
  // final fragment - captured by just appending ctx.splitPoints.back() if
  // it wasn't already chosen.
  if (boundaries.empty() || boundaries.back() != ctx.splitPoints.back()) {
    boundaries.push_back(ctx.splitPoints.back());
  }

  std::vector<std::string> fragmentNames;
  std::vector<std::pair<size_t, size_t>> fragmentRanges;  // parallel to fragmentNames
  size_t fragmentStart = ctx.bodyContentStartOffset;
  for (size_t i = 0; i < boundaries.size(); ++i) {
    const size_t fragmentEnd = boundaries[i];
    if (fragmentEnd <= fragmentStart) continue;  // shouldn't happen, but never emit an empty/inverted fragment
    const std::string fragmentName = baseName + "_" + std::to_string(fragmentNames.size()) + ".xhtml";
    const std::string outPath = outputDir + "/" + fragmentName;
    if (!writeFragment(sourcePath, outPath, headerBytes, fragmentStart, fragmentEnd)) {
      LOG_ERR("EHS", "splitToFragments: failed writing fragment %s", outPath.c_str());
      return {};
    }
    fragmentNames.push_back(fragmentName);
    fragmentRanges.emplace_back(fragmentStart, fragmentEnd);
    fragmentStart = fragmentEnd;
  }

  if (anchorFragmentOut) {
    for (const auto& [id, offset] : ctx.anchorOffsets) {
      for (size_t i = 0; i < fragmentRanges.size(); ++i) {
        if (offset >= fragmentRanges[i].first && offset < fragmentRanges[i].second) {
          (*anchorFragmentOut)[id] = static_cast<int>(i);
          break;
        }
      }
    }
  }

  LOG_INF("EHS", "Split %s (%zu bytes) into %zu fragments", sourcePath.c_str(), fileSize, fragmentNames.size());
  return fragmentNames;
}

}  // namespace EpubStreamingChapterSplitter
