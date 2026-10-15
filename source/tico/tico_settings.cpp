#include <cstdio>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include <json.hpp>

extern "C" {
#include "config.h"
#include "prefs.h"
#include "tico/tico_entry.h"
#include "tico/tico_settings.h"
}

namespace {

// The module's own copy, bundled in romfs by tools/build_tico.sh
constexpr const char *kSettingsPath = "romfs:/module/settings.json";
constexpr const char *kCoreConfigPath = "sdmc:/tico/config/cores/nethersx2.jsonc";
constexpr const char *kGameConfigDir = "sdmc:/tico/config/games/nethersx2/";

bool ReadFile(const char *path, std::string &out) {
  FILE *file = std::fopen(path, "rb");
  if (!file) return false;
  char buffer[16384];
  size_t count;
  out.clear();
  while ((count = std::fread(buffer, 1, sizeof(buffer), file)) > 0) out.append(buffer, count);
  std::fclose(file);
  return true;
}

// Strips // and /* */ comments so a .jsonc file parses as JSON.
std::string StripComments(const std::string &input) {
  std::string output;
  output.reserve(input.size());
  bool in_string = false, escaped = false;
  for (size_t i = 0; i < input.size(); ++i) {
    const char c = input[i];
    if (in_string) {
      output.push_back(c);
      if (escaped) escaped = false;
      else if (c == '\\') escaped = true;
      else if (c == '"') in_string = false;
      continue;
    }
    if (c == '"') {
      in_string = true;
      output.push_back(c);
    } else if (c == '/' && i + 1 < input.size() && input[i + 1] == '/') {
      while (i < input.size() && input[i] != '\n') ++i;
      output.push_back('\n');
    } else if (c == '/' && i + 1 < input.size() && input[i + 1] == '*') {
      i += 2;
      while (i + 1 < input.size() && !(input[i] == '*' && input[i + 1] == '/')) ++i;
      ++i;
    } else {
      output.push_back(c);
    }
  }
  return output;
}

nlohmann::json ReadJson(const char *path) {
  std::string text;
  if (!ReadFile(path, text)) return nlohmann::json::object();
  nlohmann::json root = nlohmann::json::parse(StripComments(text), nullptr, false);
  return root.is_object() ? root : nlohmann::json::object();
}

// A stored value as the ini text: tico may write strings, booleans or numbers.
bool ValueText(const nlohmann::json &root, const std::string &key, std::string &out) {
  const auto it = root.find(key);
  if (it == root.end()) return false;
  if (it->is_string()) out = it->get<std::string>();
  else if (it->is_boolean()) out = it->get<bool>() ? "true" : "false";
  else if (it->is_number_integer()) out = std::to_string(it->get<long long>());
  else if (it->is_number()) {
    char text[32];
    std::snprintf(text, sizeof(text), "%g", it->get<double>());
    out = text;
  } else return false;
  return true;
}

// The game's per-game file: its name without folder or extension.
std::string GameConfigPath(const char *rom) {
  if (!rom || !rom[0]) return {};
  std::string name = rom;
  const size_t slash = name.find_last_of("/\\");
  if (slash != std::string::npos) name = name.substr(slash + 1);
  const size_t dot = name.find_last_of('.');
  if (dot != std::string::npos) name = name.substr(0, dot);
  return name.empty() ? std::string() : kGameConfigDir + name + ".jsonc";
}

bool IsTrue(const char *key, bool fallback) {
  return prefs_get_bool(key, fallback);
}

// What the upstream launcher derives from its settings at launch.
void DeriveLaunchSettings() {
  const std::string build = prefs_get_string("Wrapper/CoreBuild", "4248");
  const bool disable_threaded = IsTrue("EmuCore/GS/DisableThreadedPresentation", false);
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
  if (IsTrue("Wrapper/LSFGEnabled", false)) {
    if (renderer != "14") prefs_set_bool("Wrapper/LSFGEnabled", false);
    // LSFG 2x must receive unique frames, not the repeated 30 fps ones
    else prefs_set_bool("EmuCore/GS/SkipDuplicateFrames", true);
  }
  prefs_set_bool("EmuCore/EnableCheats", true);
}

}  // namespace

extern "C" void tico_settings_apply(const char *rom) {
  const nlohmann::json definition = ReadJson(kSettingsPath);
  const nlohmann::json core = ReadJson(kCoreConfigPath);
  const std::string game_path = GameConfigPath(rom);
  const nlohmann::json game = game_path.empty() ? nlohmann::json::object()
                                                : ReadJson(game_path.c_str());
  if (!game.empty()) tico_log("per-game settings from %s\n", game_path.c_str());

  int applied = 0;
  for (const auto &tab : definition.value("tabs", nlohmann::json::array()))
    for (const auto &section : tab.value("sections", nlohmann::json::array()))
      for (const auto &option : section.value("options", nlohmann::json::array())) {
        const std::string key = option.value("key", "");
        if (key.empty()) continue;
        std::string value;
        if (!ValueText(game, key, value) && !ValueText(core, key, value) &&
            !ValueText(option, "default", value))
          continue;
        prefs_set_string(key.c_str(), value.c_str());
        ++applied;
      }
  tico_log("%d settings applied (core config %s)\n", applied,
           core.empty() ? "absent" : "read");
  DeriveLaunchSettings();
}
