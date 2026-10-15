#include <string>

extern "C" {
#include "config.h"
#include "prefs.h"
#include "tico/tico_entry.h"
#include "tico/tico_settings.h"
}

#include "tico/overlay/tico_config.h"

namespace TicoConfig = SwitchFrontend::TicoConfig;

extern "C" void tico_settings_derive(void) {
  const std::string build = prefs_get_string("Wrapper/CoreBuild", "4248");
  const bool disable_threaded = prefs_get_bool("EmuCore/GS/DisableThreadedPresentation", false);
  if (build == "3668") {
    // the classic core has the setting the other way round
    prefs_set_string("EmuCore/GS/ThreadedPresentation", disable_threaded ? "false" : "true");
    prefs_remove("EmuCore/GS/DisableThreadedPresentation");
  } else {
    prefs_set_string("EmuCore/GS/DisableThreadedPresentation", disable_threaded ? "true" : "false");
    prefs_remove("EmuCore/GS/ThreadedPresentation");
  }
  // OpenGL through Zink is the GL renderer with another Mesa driver
  const std::string renderer = prefs_get_string("EmuCore/GS/Renderer", "14");
  prefs_set_string("Wrapper/GLDriver", renderer == "13" ? "zink" : "nvc0");
  if (prefs_get_bool("Wrapper/LSFGEnabled", false)) {
    if (renderer != "14") prefs_set_bool("Wrapper/LSFGEnabled", false);
    // LSFG 2x must receive unique frames, not the repeated 30 fps ones
    else prefs_set_bool("EmuCore/GS/SkipDuplicateFrames", true);
  }
  prefs_set_bool("EmuCore/EnableCheats", true);
}

extern "C" void tico_settings_apply(const char *rom) {
  TicoConfig::ReloadConfig();
  // a game's own settings are read over the core's
  if (rom && rom[0]) TicoConfig::SetGame(rom);
  TicoConfig::ApplyToPrefs();
  tico_log("settings from %s (%zu values)%s\n", TicoConfig::GetLoadedConfigPath().c_str(),
           TicoConfig::GetLoadedOptionCount(),
           TicoConfig::GameSettingsActive() ? ", with the game's own" : "");
  tico_settings_derive();
}
