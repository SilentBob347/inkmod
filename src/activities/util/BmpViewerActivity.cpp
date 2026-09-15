#include "BmpViewerActivity.h"

#include <Bitmap.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>

#include "InkMODState.h"
#include "Epub/converters/PngToFramebufferConverter.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

bool isViewableImageFile(const std::string& filename) {
  return FsHelpers::hasBmpExtension(filename) || FsHelpers::hasPngExtension(filename);
}

bool isMacOSSidecarFile(const std::string& filename) { return filename.rfind("._", 0) == 0; }

struct ImageTouchControls { Rect previous; Rect cover; Rect next; };

ImageTouchControls imageTouchControls(const GfxRenderer& renderer) {
  constexpr int margin = 12;
  constexpr int gap = 8;
  constexpr int height = 44;
  const int totalWidth = renderer.getScreenWidth() - margin * 2;
  const int sideWidth = 64;
  const int coverWidth = totalWidth - sideWidth * 2 - gap * 2;
  const int y = renderer.getScreenHeight() - height - 12;
  return {Rect{margin, y, sideWidth, height}, Rect{margin + sideWidth + gap, y, coverWidth, height},
          Rect{margin + sideWidth + gap + coverWidth + gap, y, sideWidth, height}};
}

void drawImageTouchControl(GfxRenderer& renderer, Rect rect, const char* label, bool enabled = true) {
  if (!enabled) return;
  renderer.fillRect(rect.x, rect.y, rect.width, rect.height, false);
  renderer.drawRect(rect.x, rect.y, rect.width, rect.height, 2, true);
  const auto text = renderer.truncatedText(UI_10_FONT_ID, label, rect.width - 10);
  const int textWidth = renderer.getTextWidth(UI_10_FONT_ID, text.c_str());
  const int textY = rect.y + (rect.height - renderer.getLineHeight(UI_10_FONT_ID)) / 2;
  renderer.drawText(UI_10_FONT_ID, rect.x + (rect.width - textWidth) / 2, textY, text.c_str());
}

void drawImageTouchControls(GfxRenderer& renderer, bool hasPrevious, bool hasNext) {
  const auto controls = imageTouchControls(renderer);
  drawImageTouchControl(renderer, controls.previous, "<", hasPrevious);
  drawImageTouchControl(renderer, controls.cover, tr(STR_SET_SLEEP_COVER));
  drawImageTouchControl(renderer, controls.next, ">", hasNext);
}

void drawImageError(GfxRenderer& renderer, const MappedInputManager& mappedInput, const char* message) {
  renderer.clearScreen();
  renderer.drawCenteredText(UI_10_FONT_ID, renderer.getScreenHeight() / 2, message);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}

}  // namespace

BmpViewerActivity::BmpViewerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string path)
    : Activity("BmpViewer", renderer, mappedInput), filePath(std::move(path)) {}

void BmpViewerActivity::loadSiblingImages() {
  siblingImages.clear();
  currentImageIndex = -1;

  if (filePath.empty()) return;

  std::string dirPath = FsHelpers::extractFolderPath(filePath);
  size_t lastSlash = filePath.find_last_of('/');
  std::string fileName = (lastSlash != std::string::npos) ? filePath.substr(lastSlash + 1) : filePath;

  auto dir = Storage.open(dirPath.c_str());
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return;
  }

  char name[500];
  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    if (!file.isDirectory()) {
      file.getName(name, sizeof(name));
      if (name[0] != '.' && !isMacOSSidecarFile(name)) {
        std::string fname(name);
        if (isViewableImageFile(fname)) {
          siblingImages.push_back(fname);
        }
      }
    }
    file.close();
  }
  dir.close();

  FsHelpers::sortFileList(siblingImages);

  for (size_t i = 0; i < siblingImages.size(); ++i) {
    if (siblingImages[i] == fileName) {
      currentImageIndex = static_cast<int>(i);
      break;
    }
  }
}

bool BmpViewerActivity::renderPngImage() {
  ImageDimensions dims;
  if (!PngToFramebufferConverter::getDimensionsStatic(filePath, dims)) {
    drawImageError(renderer, mappedInput, "Invalid PNG File");
    return false;
  }

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  float scale = 1.0f;
  if (dims.width > pageWidth || dims.height > pageHeight) {
    const float scaleX = static_cast<float>(pageWidth) / static_cast<float>(dims.width);
    const float scaleY = static_cast<float>(pageHeight) / static_cast<float>(dims.height);
    scale = std::min(scaleX, scaleY);
  }

  const int drawWidth = std::max(1, static_cast<int>(static_cast<float>(dims.width) * scale));
  const int drawHeight = std::max(1, static_cast<int>(static_cast<float>(dims.height) * scale));
  const int x = (pageWidth - drawWidth) / 2;
  const int y = (pageHeight - drawHeight) / 2;

  RenderConfig config;
  config.x = x;
  config.y = y;
  config.maxWidth = drawWidth;
  config.maxHeight = drawHeight;
  config.useGrayscale = true;
  config.useDithering = true;
  config.performanceMode = false;
  config.useExactDimensions = true;

  PngToFramebufferConverter converter;
  renderer.clearScreen();
  if (!converter.decodeToFramebuffer(filePath, renderer, config)) {
    drawImageError(renderer, mappedInput, "Invalid PNG File");
    return false;
  }

  bool hasPrevious = (siblingImages.size() > 1 && currentImageIndex > 0);
  bool hasNext = (siblingImages.size() > 1 && currentImageIndex != -1 &&
                  currentImageIndex < static_cast<int>(siblingImages.size()) - 1);

  if (mappedInput.hasTouch()) {
    drawImageTouchControls(renderer, hasPrevious, hasNext);
  } else {
    const auto labels =
        mappedInput.mapLabels(tr(STR_BACK), tr(STR_SET_SLEEP_COVER), (hasPrevious ? "<" : ""), (hasNext ? ">" : ""));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  return true;
}

void BmpViewerActivity::onEnter() {
  Activity::onEnter();

  if (siblingImages.empty() && !filePath.empty()) {
    loadSiblingImages();
  }

  HalFile file;

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  Rect popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  GUI.fillPopupProgress(renderer, popupRect, 20);  // Initial 20% progress

  if (FsHelpers::hasPngExtension(filePath)) {
    renderPngImage();
    return;
  }

  // 1. Open the file
  if (Storage.openFileForRead("BMP", filePath, file)) {
    Bitmap bitmap(file, true);

    // 2. Parse headers to get dimensions
    if (bitmap.parseHeaders() == BmpReaderError::Ok) {
      int x, y;

      if (bitmap.getWidth() > pageWidth || bitmap.getHeight() > pageHeight) {
        float ratio = static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
        const float screenRatio = static_cast<float>(pageWidth) / static_cast<float>(pageHeight);

        if (ratio > screenRatio) {
          // Wider than screen
          x = 0;
          y = std::round((static_cast<float>(pageHeight) - static_cast<float>(pageWidth) / ratio) / 2);
        } else {
          // Taller than screen
          x = std::round((static_cast<float>(pageWidth) - static_cast<float>(pageHeight) * ratio) / 2);
          y = 0;
        }
      } else {
        // Center small images
        x = (pageWidth - bitmap.getWidth()) / 2;
        y = (pageHeight - bitmap.getHeight()) / 2;
      }

      // 4. Prepare Rendering
      bool hasPrevious = (siblingImages.size() > 1 && currentImageIndex > 0);
      bool hasNext = (siblingImages.size() > 1 && currentImageIndex != -1 &&
                      currentImageIndex < static_cast<int>(siblingImages.size()) - 1);

      GUI.fillPopupProgress(renderer, popupRect, 50);

      renderer.clearScreen();
      // Assuming drawBitmap defaults to 0,0 crop if omitted, or pass explicitly: drawBitmap(bitmap, x, y, pageWidth,
      // pageHeight, 0, 0)
      renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, 0, 0);

      // Draw UI controls on the base layer. Touch boards get explicit controls;
      // button boards retain the original button hints.
      if (mappedInput.hasTouch()) {
        drawImageTouchControls(renderer, hasPrevious, hasNext);
      } else {
        const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SET_SLEEP_COVER),
                                                  (hasPrevious ? "<" : ""), (hasNext ? ">" : ""));
        GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      }
      // Single pass for non-grayscale images

      renderer.displayBuffer(HalDisplay::FAST_REFRESH);

    } else {
      // Handle file parsing error
      drawImageError(renderer, mappedInput, "Invalid BMP File");
    }

    file.close();
  } else {
    // Handle file open error
    drawImageError(renderer, mappedInput, "Could not open file");
  }
}

void BmpViewerActivity::onExit() {
  Activity::onExit();
  renderer.clearScreen();
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}

void BmpViewerActivity::doSetSleepCover() {
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));

  // Keep one source of truth for both BMP and PNG: remember the selected
  // original file instead of copying BMPs to legacy /sleep.bmp.  Do not
  // modify SETTINGS.sleepScreen here; choosing an image and choosing the
  // lock-screen mode are independent settings.
  APP_STATE.favoriteSleepImagePath = filePath;
  const bool success = APP_STATE.saveToFile();

  GUI.drawPopup(renderer, success ? tr(STR_DONE) : tr(STR_FAILED_LOWER));
  delay(1000);
  onEnter();
}

void BmpViewerActivity::loop() {
  // Keep CPU awake/polling so 1st click works
  Activity::loop();

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    activityManager.goToFileBrowser(filePath);
    return;
  }

  bool goPrevious = false;
  bool goNext = false;
  bool setCover = false;

  if (mappedInput.hasTouch()) {
    const auto controls = imageTouchControls(renderer);
    if (mappedInput.wasTapInRect(controls.previous.x, controls.previous.y, controls.previous.width, controls.previous.height)) {
      goPrevious = true;
    } else if (mappedInput.wasTapInRect(controls.cover.x, controls.cover.y, controls.cover.width, controls.cover.height)) {
      setCover = true;
    } else if (mappedInput.wasTapInRect(controls.next.x, controls.next.y, controls.next.width, controls.next.height)) {
      goNext = true;
    } else {
      const auto swipe = mappedInput.wasSwipe();
      goNext = swipe == MappedInputManager::SwipeDir::Left;
      goPrevious = swipe == MappedInputManager::SwipeDir::Right;
    }
  }

  if (setCover || mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (isViewableImageFile(filePath)) doSetSleepCover();
    return;
  }

  if (goPrevious || mappedInput.wasReleased(MappedInputManager::Button::Left) ||
      mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    if (siblingImages.size() > 1 && currentImageIndex > 0) {
      currentImageIndex--;
      std::string dirPath = FsHelpers::extractFolderPath(filePath);
      if (dirPath.back() != '/') dirPath += "/";
      filePath = dirPath + siblingImages[currentImageIndex];
      onEnter();
    }
    return;
  }

  if (goNext || mappedInput.wasReleased(MappedInputManager::Button::Right) ||
      mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    if (siblingImages.size() > 1 && currentImageIndex != -1 &&
        currentImageIndex < static_cast<int>(siblingImages.size()) - 1) {
      currentImageIndex++;
      std::string dirPath = FsHelpers::extractFolderPath(filePath);
      if (dirPath.back() != '/') dirPath += "/";
      filePath = dirPath + siblingImages[currentImageIndex];
      onEnter();
    }
    return;
  }
}

