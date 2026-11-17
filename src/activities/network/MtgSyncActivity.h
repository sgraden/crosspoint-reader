#pragma once

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// Two-entry MTG menu: view the card on the SD card, or pull a fresh one.
class MtgSyncActivity final : public Activity {
  static constexpr int MENU_COUNT = 2;  // View Card, Pull Card

  ButtonNavigator buttonNavigator;
  int selectorIndex = 0;

 public:
  explicit MtgSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("MtgSync", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
};
