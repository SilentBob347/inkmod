#pragma once

#include <Epub.h>

#include <memory>
#include <string>

class GfxRenderer;

// Best-effort sync for the CrossPoint-specific /api/v1 surface.
// It is deliberately isolated from the normal KOSync progress flow: failures
// here are logged and returned to the caller but must never turn a successful
// progress sync into a failure.
class CrossPointExtendedSync {
 public:
  struct Result {
    bool bookmarksOk = true;
    bool clippingsOk = true;
    bool statsOk = true;

    bool ok() const { return bookmarksOk && clippingsOk && statsOk; }
  };

  // Synchronize enabled extended data for the current book. `epub` must be
  // loaded; the implementation only performs this on the CrossPoint server.
  static Result sync(const std::string& documentHash, const std::shared_ptr<Epub>& epub,
                     const std::string& epubPath, GfxRenderer& renderer);
};
