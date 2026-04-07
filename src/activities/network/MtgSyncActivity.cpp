#include "MtgSyncActivity.h"

#include <Logging.h>
#include <WiFi.h>

#include "CrossPointSettings.h"
#include "I18n.h"
#include "MappedInputManager.h"
#include "activities/ActivityManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "MtgCardViewerActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"

namespace {
const char* getIntervalString(uint8_t interval) {
  switch (interval) {
    case CrossPointSettings::MTG_SYNC_NONE: return tr(STR_NONE_OPT);
    case CrossPointSettings::MTG_SYNC_1_MIN: return tr(STR_MIN_1);
    case CrossPointSettings::MTG_SYNC_5_MIN: return tr(STR_MIN_5);
    case CrossPointSettings::MTG_SYNC_10_MIN: return tr(STR_MIN_10);
    case CrossPointSettings::MTG_SYNC_30_MIN: return tr(STR_MIN_30);
    case CrossPointSettings::MTG_SYNC_1_HR: return tr(STR_HR_1);
    case CrossPointSettings::MTG_SYNC_1_DAY: return tr(STR_DAY_1);
    default: return tr(STR_NONE_OPT);
  }
}
} // namespace

void MtgSyncActivity::onEnter() {
  Activity::onEnter();
  state = SyncState::MENU;
  selectorIndex = 0;
  requestUpdate();
}

void MtgSyncActivity::onExit() {
  Activity::onExit();
  if (WiFi.status() == WL_CONNECTED || WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect();
    WiFi.mode(WIFI_OFF);
  }
}

void MtgSyncActivity::loop() {
  if (state == SyncState::WIFI_SELECTION) {
    return;
  }

  if (state == SyncState::SYNCING) {
    performSync();
    return;
  }

  if (state == SyncState::SHOW_MESSAGE) {
    if (millis() - messageStartTime > 2000 || mappedInput.wasAnyReleased()) {
      state = SyncState::MENU;
      requestUpdate();
    }
    return;
  }

  if (state == SyncState::MENU) {
    buttonNavigator.onNext([this] {
      selectorIndex = ButtonNavigator::nextIndex(selectorIndex, 3);
      requestUpdate();
    });

    buttonNavigator.onPrevious([this] {
      selectorIndex = ButtonNavigator::previousIndex(selectorIndex, 3);
      requestUpdate();
    });

    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (selectorIndex == 0) { // View Card
        activityManager.pushActivity(std::make_unique<MtgCardViewerActivity>(renderer, mappedInput));
      } else if (selectorIndex == 1) { // Pull Card
        checkAndConnectWifi();
      } else if (selectorIndex == 2) { // Update Interval
        SETTINGS.mtgSyncInterval = (SETTINGS.mtgSyncInterval + 1) % CrossPointSettings::MTG_SYNC_INTERVAL_COUNT;
        SETTINGS.saveToFile();
        requestUpdate();
      }
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      onGoHome();
    }
  }
}

void MtgSyncActivity::render(RenderLock&&) {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_MTG_SYNC));

  if (state == SyncState::CHECK_WIFI || state == SyncState::SYNCING) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, statusMessage.c_str());
  } else if (state == SyncState::SHOW_MESSAGE) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, statusMessage.c_str());
  } else if (state == SyncState::MENU) {
    std::string intervalText = std::string(tr(STR_UPDATE_INTERVAL)) + ": " + getIntervalString(SETTINGS.mtgSyncInterval);
    
    GUI.drawButtonMenu(
        renderer,
        Rect{0, 110, pageWidth, pageHeight - 160},
        3, selectorIndex,
        [this, intervalText](int index) -> std::string {
          if (index == 0) return tr(STR_VIEW_CARD);
          if (index == 1) return tr(STR_PULL_CARD);
          return intervalText;
        },
        [](int index) -> UIIcon {
          if (index == 0) return Recent;
          if (index == 1) return Transfer;
          return Settings;
        });

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}

void MtgSyncActivity::checkAndConnectWifi() {
  if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
    state = SyncState::SYNCING;
    statusMessage = tr(STR_SYNCING);
    requestUpdate();
  } else {
    launchWifiSelection();
  }
}

void MtgSyncActivity::launchWifiSelection() {
  state = SyncState::WIFI_SELECTION;
  requestUpdate();

  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void MtgSyncActivity::onWifiSelectionComplete(const bool connected) {
  if (connected) {
    state = SyncState::SYNCING;
    statusMessage = tr(STR_SYNCING);
  } else {
    WiFi.disconnect();
    WiFi.mode(WIFI_OFF);
    state = SyncState::MENU;
  }
  requestUpdate();
}

void MtgSyncActivity::performSync() {
  // Ensure we render the "Syncing..." message before blocking
  requestUpdateAndWait();

  // Give WiFi stack a moment to fully stabilize
  delay(1000);

  Storage.mkdir("/.sleep");
  
  auto err = HttpDownloader::downloadToFile(
      "https://mtg-slab.sradentest.workers.dev/",
      "/.sleep/mtg_card.bmp");

  if (err == HttpDownloader::DownloadError::OK) {
    statusMessage = tr(STR_SYNC_SUCCESS);
  } else {
    statusMessage = tr(STR_SYNC_FAILED);
  }

  state = SyncState::SHOW_MESSAGE;
  messageStartTime = millis();
  requestUpdate();
}
