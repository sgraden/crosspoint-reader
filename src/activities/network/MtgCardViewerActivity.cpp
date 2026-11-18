#include "MtgCardViewerActivity.h"

#include <Bitmap.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <cmath>

#include "MappedInputManager.h"
#include "MtgPullActivity.h"
#include "activities/ActivityManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

void MtgCardViewerActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

void MtgCardViewerActivity::onExit() {
  Activity::onExit();
  renderer.clearScreen();
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

void MtgCardViewerActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoHome(HomeMenuItem::MTG_SYNC);
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    auto pull = makeUniqueNoThrow<MtgPullActivity>(renderer, mappedInput);
    if (!pull) {
      LOG_ERR("MTG", "OOM: card pull");
      return;
    }
    // The viewer is redrawn automatically when the pull finishes, picking up the new BMP.
    startActivityForResult(std::move(pull), [](const ActivityResult&) {});
  }
}

void MtgCardViewerActivity::render(RenderLock&&) {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  HalFile file;
  if (Storage.openFileForRead("BMP", MtgPullActivity::CARD_PATH, file)) {
    Bitmap bitmap(file, true);
    if (bitmap.parseHeaders() == BmpReaderError::Ok) {
      int x = 0, y = 0;

      if (bitmap.getWidth() > pageWidth || bitmap.getHeight() > pageHeight) {
        const float ratio = static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
        const float screenRatio = static_cast<float>(pageWidth) / static_cast<float>(pageHeight);

        if (ratio > screenRatio) {
          y = std::round((static_cast<float>(pageHeight) - static_cast<float>(pageWidth) / ratio) / 2);
        } else {
          x = std::round((static_cast<float>(pageWidth) - static_cast<float>(pageHeight) * ratio) / 2);
        }
      } else {
        x = (pageWidth - bitmap.getWidth()) / 2;
        y = (pageHeight - bitmap.getHeight()) / 2;
      }

      renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, 0, 0);
    } else {
      renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_INVALID_MTG_CARD));
    }
    file.close();
  } else {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_NO_MTG_CARD));
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", tr(STR_PULL_CARD));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer(HalDisplay::FULL_REFRESH);
}
