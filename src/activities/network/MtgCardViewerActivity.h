#pragma once

#include "activities/Activity.h"

// Shows the MTG card BMP that was last pulled to the SD card. Right pulls a fresh one.
class MtgCardViewerActivity final : public Activity {
 public:
  explicit MtgCardViewerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("MtgCardViewer", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
