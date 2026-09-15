#include "KOReaderSettingsActivity.h"

#include <cctype>

#include <GfxRenderer.h>
#include <I18n.h>

#include <cstring>

#include "KOReaderAuthActivity.h"
#include "KOReaderCredentialStore.h"
#include "MappedInputManager.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/TouchListNavigation.h"

namespace {
std::string trimCredentialField(std::string value) {
  size_t start = 0;
  while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start]))) start++;
  size_t end = value.size();
  while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1]))) end--;
  return value.substr(start, end - start);
}

constexpr int MENU_ITEMS = 9;
const StrId menuNames[MENU_ITEMS] = {StrId::STR_USERNAME, StrId::STR_PASSWORD, StrId::STR_SYNC_SERVER_URL,
                                     StrId::STR_DOCUMENT_MATCHING, StrId::STR_SEND_DOCUMENT_METADATA,
                                     StrId::STR_BOOKMARKS, StrId::STR_CLIPPINGS, StrId::STR_READING_STATS,
                                     StrId::STR_AUTHENTICATE};
}  // namespace

void KOReaderSettingsActivity::onEnter() {
  Activity::onEnter();

  selectedIndex = 0;
  requestUpdate();
}

void KOReaderSettingsActivity::onExit() { Activity::onExit(); }

void KOReaderSettingsActivity::loop() {
  bool touchActivate = false;
  if (mappedInput.hasTouch()) {
    const auto& metrics = UITheme::getInstance().getMetrics();
    const auto safeArea = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
    const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    const int contentHeight = safeArea.y + safeArea.height - contentTop - metrics.verticalSpacing * 2;
    int touchIndex = static_cast<int>(selectedIndex);
    auto touch = TouchListNavigation::handle(mappedInput, touchIndex, MENU_ITEMS,
                                             Rect{safeArea.x, contentTop, safeArea.width, contentHeight}, metrics.listRowHeight);
    selectedIndex = static_cast<decltype(selectedIndex)>(touchIndex);
    if (touch.handled && !touch.activate) { requestUpdate(); return; }
    touchActivate = touch.activate;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (touchActivate || mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    handleSelection();
    return;
  }

  // Handle navigation
  buttonNavigator.onNext([this] {
    selectedIndex = (selectedIndex + 1) % MENU_ITEMS;
    requestUpdate();
  });

  buttonNavigator.onPrevious([this] {
    selectedIndex = (selectedIndex + MENU_ITEMS - 1) % MENU_ITEMS;
    requestUpdate();
  });
}

void KOReaderSettingsActivity::handleSelection() {
  if (selectedIndex == 0) {
    // Username
    startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_KOREADER_USERNAME),
                                                                   KOREADER_STORE.getUsername(), 64, InputType::Text),
                           [this](const ActivityResult& result) {
                             if (!result.isCancelled) {
                               const auto& kb = std::get<KeyboardResult>(result.data);
                               KOREADER_STORE.setCredentials(trimCredentialField(kb.text), KOREADER_STORE.getPassword());
                               KOREADER_STORE.saveToFile();
                             }
                           });
  } else if (selectedIndex == 1) {
    // Password
    startActivityForResult(
        std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_KOREADER_PASSWORD),
                                                std::string(), 64, InputType::Password),
        [this](const ActivityResult& result) {
          if (!result.isCancelled) {
            const auto& kb = std::get<KeyboardResult>(result.data);
            // Password editor intentionally starts blank. Confirming an empty
            // field leaves the existing password untouched; a non-empty entry
            // replaces it exactly, avoiding accidental append/prepend corruption.
            if (!kb.text.empty()) {
              KOREADER_STORE.setCredentials(KOREADER_STORE.getUsername(), kb.text);
              KOREADER_STORE.saveToFile();
            }
          }
        });
  } else if (selectedIndex == 2) {
    // Sync Server URL - prefill with https:// if empty to save typing
    const std::string currentUrl = KOREADER_STORE.getServerUrl();
    const std::string prefillUrl = currentUrl.empty() ? "https://" : currentUrl;
    startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_SYNC_SERVER_URL),
                                                                   prefillUrl, 128, InputType::Url),
                           [this](const ActivityResult& result) {
                             if (!result.isCancelled) {
                               const auto& kb = std::get<KeyboardResult>(result.data);
                               const std::string urlToSave =
                                   (kb.text == "https://" || kb.text == "http://") ? "" : kb.text;
                               KOREADER_STORE.setServerUrl(urlToSave);
                               KOREADER_STORE.saveToFile();
                             }
                           });
  } else if (selectedIndex == 3) {
    // Document Matching - toggle between Filename and Binary
    const auto current = KOREADER_STORE.getMatchMethod();
    const auto newMethod =
        (current == DocumentMatchMethod::FILENAME) ? DocumentMatchMethod::BINARY : DocumentMatchMethod::FILENAME;
    KOREADER_STORE.setMatchMethod(newMethod);
    KOREADER_STORE.saveToFile();
    requestUpdate();
  } else if (selectedIndex == 4) {
    KOREADER_STORE.setSendMetadata(!KOREADER_STORE.getSendMetadata());
    KOREADER_STORE.saveToFile();
    requestUpdate();
  } else if (selectedIndex == 5) {
    KOREADER_STORE.setSyncBookmarks(!KOREADER_STORE.getSyncBookmarks());
    KOREADER_STORE.saveToFile();
    requestUpdate();
  } else if (selectedIndex == 6) {
    KOREADER_STORE.setSyncClippings(!KOREADER_STORE.getSyncClippings());
    KOREADER_STORE.saveToFile();
    requestUpdate();
  } else if (selectedIndex == 7) {
    KOREADER_STORE.setSyncStats(!KOREADER_STORE.getSyncStats());
    KOREADER_STORE.saveToFile();
    requestUpdate();
  } else if (selectedIndex == 8) {
    // Authenticate
    if (!KOREADER_STORE.hasCredentials()) return;
    startActivityForResult(std::make_unique<KOReaderAuthActivity>(renderer, mappedInput), [](const ActivityResult&) {});
  }
}

void KOReaderSettingsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto safeArea = UITheme::getInstance().getScreenSafeArea(renderer, /*hasFrontButtonHints=*/true, /*hasSideButtonHints=*/false);

  GUI.drawHeader(renderer, Rect{safeArea.x, metrics.topPadding, safeArea.width, metrics.headerHeight}, tr(STR_KOREADER_SYNC));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = safeArea.y + safeArea.height - contentTop - metrics.verticalSpacing * 2;
  GUI.drawList(
      renderer, Rect{safeArea.x, contentTop, safeArea.width, contentHeight}, static_cast<int>(MENU_ITEMS),
      static_cast<int>(selectedIndex), [](int index) { return std::string(I18N.get(menuNames[index])); }, nullptr,
      nullptr,
      [this](int index) {
        // Draw status for each setting
        if (index == 0) {
          auto username = KOREADER_STORE.getUsername();
          return username.empty() ? std::string(tr(STR_NOT_SET)) : username;
        } else if (index == 1) {
          return KOREADER_STORE.getPassword().empty() ? std::string(tr(STR_NOT_SET)) : std::string("******");
        } else if (index == 2) {
          auto serverUrl = KOREADER_STORE.getServerUrl();
          return serverUrl.empty() ? std::string(tr(STR_DEFAULT_VALUE)) : serverUrl;
        } else if (index == 3) {
          return KOREADER_STORE.getMatchMethod() == DocumentMatchMethod::FILENAME ? std::string(tr(STR_FILENAME))
                                                                                  : std::string(tr(STR_BINARY));
        } else if (index == 4) {
          return KOREADER_STORE.getSendMetadata() ? std::string(tr(STR_YES)) : std::string(tr(STR_NO));
        } else if (index == 5) {
          return KOREADER_STORE.getSyncBookmarks() ? std::string(tr(STR_YES)) : std::string(tr(STR_NO));
        } else if (index == 6) {
          return KOREADER_STORE.getSyncClippings() ? std::string(tr(STR_YES)) : std::string(tr(STR_NO));
        } else if (index == 7) {
          return KOREADER_STORE.getSyncStats() ? std::string(tr(STR_YES)) : std::string(tr(STR_NO));
        } else if (index == 8) {
          return KOREADER_STORE.hasCredentials() ? "" : std::string("[") + tr(STR_SET_CREDENTIALS_FIRST) + "]";
        }
        return std::string(tr(STR_NOT_SET));
      },
      true);

  // Draw help text at bottom
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
