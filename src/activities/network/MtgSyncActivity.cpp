#include "MtgSyncActivity.h"

#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include "MappedInputManager.h"
#include "MtgCardViewerActivity.h"
#include "MtgPullActivity.h"
#include "activities/ActivityManager.h"
#include "components/UITheme.h"

void MtgSyncActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

void MtgSyncActivity::loop() {
  buttonNavigator.onNext([this] {
    selectorIndex = ButtonNavigator::nextIndex(selectorIndex, MENU_COUNT);
    requestUpdate();
  });

  buttonNavigator.onPrevious([this] {
    selectorIndex = ButtonNavigator::previousIndex(selectorIndex, MENU_COUNT);
    requestUpdate();
  });

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (selectorIndex == 0) {  // View Card
      auto viewer = makeUniqueNoThrow<MtgCardViewerActivity>(renderer, mappedInput);
      if (!viewer) {
        LOG_ERR("MTG", "OOM: card viewer");
        return;
      }
      activityManager.pushActivity(std::move(viewer));
    } else {  // Pull Card
      auto pull = makeUniqueNoThrow<MtgPullActivity>(renderer, mappedInput);
      if (!pull) {
        LOG_ERR("MTG", "OOM: card pull");
        return;
      }
      startActivityForResult(std::move(pull), [](const ActivityResult&) {});
    }
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoHome();
  }
}

void MtgSyncActivity::render(RenderLock&&) {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_MTG_SYNC));

  const int listTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int listHeight = pageHeight - listTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
  GUI.drawButtonMenu(
      renderer, Rect{0, listTop, pageWidth, listHeight}, MENU_COUNT, selectorIndex,
      [](int index) -> std::string { return index == 0 ? tr(STR_VIEW_CARD) : tr(STR_PULL_CARD); },
      [](int index) -> UIIcon { return index == 0 ? Recent : Transfer; });

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
