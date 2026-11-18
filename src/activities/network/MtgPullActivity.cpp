#include "MtgPullActivity.h"

#include <Arduino.h>
#include <FontCacheManager.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>
#include <esp_random.h>

#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "WifiCredentialStore.h"
#include "activities/ActivityManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"

namespace {
constexpr char CARD_URL[] = "https://mtg-slab.sradentest.workers.dev/";
constexpr char CARD_DIR[] = "/.sleep";

// The radio is slow to associate on this device, and WL_NO_SSID_AVAIL / WL_CONNECT_FAILED are often transient
// while it retries, so they are ignored: only a full timeout counts as failure. One re-begin halfway through.
constexpr unsigned long CONNECT_RETRY_MS = 15000;
constexpr unsigned long CONNECT_TIMEOUT_MS = 30000;
constexpr unsigned long MESSAGE_MS = 1500;
constexpr int DOWNLOAD_ATTEMPTS = 2;
constexpr unsigned long DOWNLOAD_RETRY_DELAY_MS = 1000;

bool isWifiReady() { return WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0); }

// True when any mapped button was released this frame (upstream removed MappedInputManager::wasAnyReleased).
bool anyButtonReleased(const MappedInputManager& input) {
  using B = MappedInputManager::Button;
  return input.wasReleased(B::Back) || input.wasReleased(B::Confirm) || input.wasReleased(B::Left) ||
         input.wasReleased(B::Right) || input.wasReleased(B::Up) || input.wasReleased(B::Down);
}
}  // namespace

void MtgPullActivity::onEnter() {
  Activity::onEnter();

  // The credential store is only loaded lazily (WifiSelectionActivity does it in onEnter). Without this, the
  // first pull after a boot sees no saved networks and always falls through to the picker.
  {
    RenderLock lock(*this);  // SD access shares the SPI bus with the display
    WIFI_STORE.loadFromFile();
  }

  state = State::START;
  retriedConnect = false;
  succeeded = false;
  statusMessage = tr(STR_CONNECTING);
  requestUpdate();
}

void MtgPullActivity::onExit() {
  Activity::onExit();

  // Same teardown as the other WiFi activities (CatalogActivity, ClockSyncActivity, ...): reboot instead of
  // switching the radio off. Bringing WiFi back up later in the same boot is not a path this firmware uses; a
  // second pull in one boot never associated in testing, while the first one after a boot connected fine.
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    if (succeeded || !standalone) {
      silentRestartToMtgCard();
    } else {
      silentRestart();  // failed or cancelled from Home: back to Home, not to a stale card
    }
  }
}

void MtgPullActivity::loop() {
  switch (state) {
    case State::START:
      begin();
      break;
    case State::CONNECTING:
      pollConnect();
      break;
    case State::WIFI_SELECTION:
      break;  // the picker's result handler drives the next step
    case State::DOWNLOADING:
      runDownload();
      break;
    case State::MESSAGE:
      if (millis() - stateStartTime >= MESSAGE_MS || anyButtonReleased(mappedInput)) leave();
      break;
  }
}

void MtgPullActivity::begin() {
  if (isWifiReady()) {
    beginDownload();
  } else if (startConnect()) {
    state = State::CONNECTING;
    stateStartTime = millis();
    requestUpdate();
  } else {
    launchWifiSelection();  // nothing saved: let the user pick a network
  }
}

// Starts a non-blocking connect to the last-used (else first) saved network. Returns false if none is saved.
bool MtgPullActivity::startConnect() {
  auto cred = WIFI_STORE.findCredential(WIFI_STORE.getLastConnectedSsid());
  if (!cred) cred = WIFI_STORE.getCredentialAt(0);
  if (!cred) return false;

  WiFi.persistent(false);  // credentials live in WifiCredentialStore, not the SDK's NVS
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, true);  // abort any in-progress SDK auto-connect
  delay(100);
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);  // pick the strongest AP for the SSID, not the first found
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
  if (cred->password.empty()) {
    WiFi.begin(cred->ssid.c_str());
  } else {
    WiFi.begin(cred->ssid.c_str(), cred->password.c_str());
  }
  return true;
}

void MtgPullActivity::pollConnect() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    leave();  // succeeded == false -> cancelled
    return;
  }
  if (isWifiReady()) {
    beginDownload();
    return;
  }

  const unsigned long elapsed = millis() - stateStartTime;
  if (elapsed >= CONNECT_TIMEOUT_MS) {
    LOG_ERR("MTG", "Saved-network connect timed out; falling back to the WiFi picker");
    WiFi.disconnect();
    launchWifiSelection();
  } else if (!retriedConnect && elapsed >= CONNECT_RETRY_MS) {
    retriedConnect = true;
    LOG_DBG("MTG", "Re-trying connect to saved network");
    startConnect();
  }
}

void MtgPullActivity::launchWifiSelection() {
  auto wifi = makeUniqueNoThrow<WifiSelectionActivity>(renderer, mappedInput);
  if (!wifi) {
    LOG_ERR("MTG", "OOM: WiFi selection");
    showMessage(false);
    return;
  }
  state = State::WIFI_SELECTION;
  requestUpdate();
  startActivityForResult(std::move(wifi), [this](const ActivityResult& result) {
    if (result.isCancelled) {
      leave();
    } else {
      beginDownload();
    }
  });
}

void MtgPullActivity::beginDownload() {
  state = State::DOWNLOADING;
  statusMessage = tr(STR_SYNCING);
  requestUpdate();
}

void MtgPullActivity::runDownload() {
  requestUpdateAndWait();  // make sure "Syncing..." is on screen before the download blocks
  showMessage(downloadCard());
}

bool MtgPullActivity::downloadCard() {
  // Rebuildable SD-font caches can hold tens of KB the TLS session needs; release them and refuse to start
  // below the floor rather than dying mid-stream (same guard the other downloaders use).
  if (auto* fcm = renderer.getFontCacheManager()) fcm->releaseSdFontCaches();
  if (ESP.getFreeHeap() < HttpDownloader::MIN_TLS_FREE_HEAP ||
      ESP.getMaxAllocHeap() < HttpDownloader::MIN_TLS_MAX_ALLOC) {
    LOG_ERR("MTG", "Low heap for download (%u free, %u max block)", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    return false;
  }

  Storage.mkdir(CARD_DIR);

  // Random query string defeats any cache between us and the Worker. Fixed-size: the URL is only built once.
  char url[sizeof(CARD_URL) + 16];
  snprintf(url, sizeof(url), "%s?t=%lu", CARD_URL, static_cast<unsigned long>(esp_random()));

  // downloadToFile stages in <dest>.part, so a failed attempt never clobbers the card already on the SD card.
  for (int attempt = 1; attempt <= DOWNLOAD_ATTEMPTS; attempt++) {
    const auto err = HttpDownloader::downloadToFile(url, CARD_PATH);
    if (err == HttpDownloader::DownloadError::OK) return true;
    LOG_ERR("MTG", "Download attempt %d/%d failed (error %d)", attempt, DOWNLOAD_ATTEMPTS, static_cast<int>(err));
    if (attempt < DOWNLOAD_ATTEMPTS) delay(DOWNLOAD_RETRY_DELAY_MS);
  }
  return false;
}

void MtgPullActivity::showMessage(const bool ok) {
  succeeded = ok;
  statusMessage = ok ? tr(STR_SYNC_SUCCESS) : tr(STR_SYNC_FAILED);
  state = State::MESSAGE;
  stateStartTime = millis();
  requestUpdate();
}

void MtgPullActivity::leave() {
  if (standalone) {
    if (succeeded) {
      activityManager.goToMtgCard();
    } else {
      onGoHome(HomeMenuItem::MTG_SYNC);
    }
    return;
  }

  ActivityResult result;
  result.isCancelled = !succeeded;
  setResult(std::move(result));
  finish();
}

void MtgPullActivity::render(RenderLock&&) {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_PULL_CARD));
  renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, statusMessage);

  if (state == State::CONNECTING) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
