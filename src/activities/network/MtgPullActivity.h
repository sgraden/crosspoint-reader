#pragma once

#include "activities/Activity.h"

/**
 * One-shot "pull a fresh MTG card" flow, shared by the MTG menu and the card viewer.
 *
 * Connects with the saved WiFi credentials (falling back to the WiFi picker if there are none or the
 * connect times out), downloads the card BMP, shows the result for a moment, turns WiFi off and
 * finishes. The result is "cancelled" when the pull did not succeed.
 */
class MtgPullActivity final : public Activity {
 public:
  static constexpr char CARD_PATH[] = "/.sleep/mtg_card.bmp";

  explicit MtgPullActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("MtgPull", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum class State { START, CONNECTING, WIFI_SELECTION, DOWNLOADING, MESSAGE };

  State state = State::START;
  const char* statusMessage = "";  // always a tr() constant, never owned
  unsigned long stateStartTime = 0;
  bool retriedConnect = false;
  bool succeeded = false;

  void begin();
  bool startConnect();
  void pollConnect();
  void launchWifiSelection();
  void beginDownload();
  void runDownload();
  bool downloadCard();
  void showMessage(bool ok);
  void leave();
  static void wifiOff();
};
