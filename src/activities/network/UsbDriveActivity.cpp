#include "UsbDriveActivity.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "HalPowerManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

void UsbDriveActivity::onEnter() {
  Activity::onEnter();
  preparing = true;
  startFailed = false;
  restartRequested = false;
  requestUpdateAndWait();

  if (!Storage.beginUsbDrive()) {
    LOG_ERR("USB", "Unable to start USB Drive");
    preparing = false;
    startFailed = true;
    state = UsbDriveState::IoError;
    startFailureStartedAt = millis();
    requestUpdate();
    return;
  }

  preparing = false;
  state = UsbDriveState::WaitingForHost;
  hostWaitStartedAt = millis();
  requestUpdate();
}

void UsbDriveActivity::onExit() {
  if (!restartRequested) Storage.endUsbDrive();
  Activity::onExit();
}

void UsbDriveActivity::loop() {
  if (!startFailed) {
    const UsbDriveState next = Storage.usbDriveState();
    if (next != state) {
      state = next;
      if (state == UsbDriveState::Connected) {
        // MSC Connected is definitive proof that USB power is present. X4 Pro
        // has no validated standalone VBUS GPIO, so use this event to keep
        // "since last charge" accurate instead of relying on the C3 UART pin.
        powerManager.markChargingNow();
      }
      requestUpdate();
    }
  }

  // Windows does not reliably send START STOP UNIT / EJECT for every
  // "Safely remove" path. In that case TinyUSB can remain mounted and the
  // activity would stay in Connected forever. Once the user asks to leave,
  // explicitly detach the USB device from the host, then hand the PHY back to
  // Serial/JTAG and reboot so SDMMC is mounted from a clean boot.
  if (exitRequested) {
    if (state == UsbDriveState::Ejected || state == UsbDriveState::Disconnected ||
        state == UsbDriveState::Unsupported || millis() - exitRequestedAt >= FORCED_DISCONNECT_TIMEOUT_MS) {
      restartHome();
    }
    return;
  }

  if (state == UsbDriveState::WaitingForHost && millis() - hostWaitStartedAt >= HOST_WAIT_TIMEOUT_MS) {
    restartHome();
    return;
  }
  if (startFailed && millis() - startFailureStartedAt >= START_FAILURE_TIMEOUT_MS) {
    restartHome();
    return;
  }

  const bool userExit = mappedInput.wasPressed(MappedInputManager::Button::Back) ||
                        mappedInput.wasReleased(MappedInputManager::Button::Back) ||
                        mappedInput.wasHomeGesture();

  if (userExit) {
    if (state == UsbDriveState::Connected) {
      LOG_INF("USB", "Exit requested while MSC connected; disconnecting host first");
      exitRequested = true;
      exitRequestedAt = millis();
      if (!Storage.disconnectUsbDriveHost()) {
        LOG_ERR("USB", "Unable to request USB host disconnect; forcing timed handoff");
      }
      requestUpdate();
      return;
    }

    if (state == UsbDriveState::WaitingForHost || startFailed || state == UsbDriveState::Ejected ||
        state == UsbDriveState::Disconnected || state == UsbDriveState::Unsupported ||
        state == UsbDriveState::IoError) {
      restartHome();
      return;
    }
  }

  if (state == UsbDriveState::Ejected || state == UsbDriveState::Disconnected ||
      state == UsbDriveState::Unsupported) {
    restartHome();
  }
}

void UsbDriveActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, width, metrics.headerHeight}, tr(STR_FILE_TRANSFER));

  const char* title = "USB-накопитель";
  const char* detail = "Подключите X4 Pro к компьютеру по USB";
  if (preparing) {
    title = "Подготовка USB…";
    detail = "SD-карта временно отключается от inkMOD";
  } else if (exitRequested) {
    title = "Отключение USB…";
    detail = "Возвращаем SD-карту и USB-порт читалке";
  } else if (startFailed || state == UsbDriveState::IoError) {
    title = "Ошибка USB";
    detail = "Не удалось передать SD-карту компьютеру";
  } else if (state == UsbDriveState::Connected) {
    title = "Подключено";
    detail = "Копируйте файлы. Перед отключением безопасно извлеките диск на компьютере";
  } else if (state == UsbDriveState::WaitingForHost) {
    title = "Ожидание компьютера";
    detail = "Подключите USB-кабель передачи данных";
  }

  const int centerY = metrics.topPadding + metrics.headerHeight +
                      (height - metrics.topPadding - metrics.headerHeight - metrics.buttonHintsHeight) / 2;
  renderer.drawCenteredText(UI_12_FONT_ID, centerY - 28, title, true, EpdFontFamily::BOLD);
  if (state == UsbDriveState::Connected) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY + 8, "Копируйте файлы.", true, EpdFontFamily::REGULAR);
    renderer.drawCenteredText(UI_10_FONT_ID, centerY + 34,
                              "Перед отключением безопасно извлеките диск", true, EpdFontFamily::REGULAR);
  } else {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY + 12, detail, true, EpdFontFamily::REGULAR);
  }

  if (!exitRequested && (state == UsbDriveState::WaitingForHost || startFailed)) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }
  renderer.displayBuffer();
}

void UsbDriveActivity::restartHome() {
  if (restartRequested) return;
  restartRequested = true;
  // Leaving an active USB storage session is the closest reliable equivalent
  // of "charger just disconnected" on X4 Pro. Record it before the SW reset;
  // RTC_DATA_ATTR survives that reset and main.cpp persists it immediately.
  powerManager.markChargingNow();
  Storage.endUsbDrive();
  delay(30);
  restartToHomeAfterStorageHandoff();
}
