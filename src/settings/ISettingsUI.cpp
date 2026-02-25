#include "crosspad/settings/ISettingsUI.hpp"

namespace crosspad {

static ISettingsUI* s_settingsUI = nullptr;

void setSettingsUI(ISettingsUI* ui) {
    s_settingsUI = ui;
}

ISettingsUI* getSettingsUI() {
    return s_settingsUI;
}

} // namespace crosspad
