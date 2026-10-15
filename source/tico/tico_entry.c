#include <switch.h>

#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "config.h"
#include "prefs.h"
#include "tico/tico_entry.h"
#include "tico/tico_settings.h"

/* The standalone NetherSX2_nx install, used when this build has no core */
#define STANDALONE_ROOT "/switch/nethersx2"
#define SAVES_ARGUMENT  "--saves="
#define STATES_ARGUMENT "--states="

static char display_title[256];

static bool is_file(const char *path) {
  struct stat st;
  return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static bool is_directory(const char *path) {
  struct stat st;
  return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* Creates a directory and any missing parents. */
static void make_path(const char *path) {
  char partial[1024];
  snprintf(partial, sizeof(partial), "%s", path);
  char *cursor = strstr(partial, ":/");
  cursor = cursor ? cursor + 2 : partial + 1;
  for (; *cursor; cursor++) {
    if (*cursor != '/') continue;
    *cursor = '\0';
    mkdir(partial, 0777);
    *cursor = '/';
  }
  mkdir(partial, 0777);
}

/* The core's FileSystem reads a colon as a URL scheme: SD paths go without
 * "sdmc:", and a trailing slash is dropped. */
static void core_path(char *out, size_t size, const char *path) {
  if (!strncmp(path, "sdmc:", 5)) path += 5;
  snprintf(out, size, "%s", path);
  size_t length = strlen(out);
  while (length > 1 && out[length - 1] == '/') out[--length] = '\0';
}

static bool copy_file(const char *from, const char *to) {
  FILE *input = fopen(from, "rb");
  if (!input) return false;
  char temporary[1100];
  snprintf(temporary, sizeof(temporary), "%s.tmp", to);
  FILE *output = fopen(temporary, "wb");
  if (!output) {
    fclose(input);
    return false;
  }
  static char buffer[256 * 1024];
  bool failed = false;
  size_t count;
  while ((count = fread(buffer, 1, sizeof(buffer), input)) > 0)
    if (fwrite(buffer, 1, count, output) != count) {
      failed = true;
      break;
    }
  if (ferror(input)) failed = true;
  fclose(input);
  if (fclose(output) != 0) failed = true;
  if (failed) {
    remove(temporary);
    return false;
  }
  remove(to);
  return rename(temporary, to) == 0;
}

static bool copy_tree(const char *from, const char *to) {
  DIR *dir = opendir(from);
  if (!dir) return false;
  make_path(to);
  bool ok = true;
  struct dirent *entry;
  while ((entry = readdir(dir))) {
    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
    char source[1024], target[1024];
    snprintf(source, sizeof(source), "%s/%s", from, entry->d_name);
    snprintf(target, sizeof(target), "%s/%s", to, entry->d_name);
    ok = (is_directory(source) ? copy_tree(source, target)
                               : copy_file(source, target)) && ok;
  }
  closedir(dir);
  return ok;
}

/* The core reads its shaders, GameIndex.yaml and fonts from RESOURCES_DIR;
 * they are copied out of romfs once per core build and NRO build. */
static void stage_resources(const char *build) {
  char source[64], marker[128], stamp[96] = "";
  snprintf(source, sizeof(source), "romfs:/res/%s", build);
  if (!is_directory(source)) return;
  snprintf(marker, sizeof(marker), "%s %s %s", build, __DATE__, __TIME__);
  FILE *file = fopen(RESOURCES_DIR "/.tico_build", "r");
  if (file) {
    if (!fgets(stamp, sizeof(stamp), file)) stamp[0] = '\0';
    fclose(file);
    stamp[strcspn(stamp, "\r\n")] = '\0';
  }
  if (!strcmp(stamp, marker) && is_file(RESOURCES_DIR "/GameIndex.yaml")) return;
  tico_log("staging resources for core %s\n", build);
  if (!copy_tree(source, RESOURCES_DIR)) return;
  file = fopen(RESOURCES_DIR "/.tico_build", "w");
  if (file) {
    fprintf(file, "%s\n", marker);
    fclose(file);
  }
}

/* The core build from the settings (4248 patched, 3668 classic): the copy in
 * this NRO's romfs, else the standalone install's extracted one. */
static void select_core(void) {
  /* a copy: setting prefs may move the stored string */
  char build[8] = "4248";
  if (!strcmp(prefs_get_string("Wrapper/CoreBuild", "4248"), "3668"))
    snprintf(build, sizeof(build), "3668");
  char bundled[64], standalone[128];
  snprintf(bundled, sizeof(bundled), "romfs:/cores/emucore_%s.so", build);
  snprintf(standalone, sizeof(standalone),
           STANDALONE_ROOT "/cores/libemucore_%s.so", build);
  const char *core = is_file(bundled) ? bundled : standalone;
  prefs_set_string("Wrapper/CoreSo", core);
  tico_log("core %s\n", core);
  stage_resources(build);
}

/* tico names a game on a USB drive usb://<volume-id>/<path>; the storage
 * bridge finds umsN:/<path> on whichever drive has it. */
static void resolve_rom(const char *argument, char *rom, size_t size) {
  if (!strncmp(argument, "usb://", 6)) {
    const char *rest = strchr(argument + 6, '/');
    snprintf(rom, size, "ums0:/%s", rest ? rest + 1 : "");
    return;
  }
  core_path(rom, size, argument);
}

static void set_folder(const char *key, const char *tico_folder,
                       const char *fallback) {
  char folder[1024];
  core_path(folder, sizeof(folder), tico_folder && tico_folder[0] ? tico_folder
                                                                  : fallback);
  make_path(folder);
  prefs_set_string(key, folder);
}

bool tico_prepare(int argc, char **argv) {
  make_path(DATA_ROOT);
  make_path(CACHE_DIR);
  make_path(CHEATS_DIR);

  const char *rom = NULL, *title = NULL, *saves = NULL, *states = NULL;
  for (int index = 1; index < argc; index++) {
    const char *argument = argv[index];
    if (!argument || !argument[0] || !strcmp(argument, "ticoSetup")) continue;
    if (!strncmp(argument, SAVES_ARGUMENT, sizeof(SAVES_ARGUMENT) - 1))
      saves = argument + sizeof(SAVES_ARGUMENT) - 1;
    else if (!strncmp(argument, STATES_ARGUMENT, sizeof(STATES_ARGUMENT) - 1))
      states = argument + sizeof(STATES_ARGUMENT) - 1;
    else if (!rom)
      rom = argument;
    else if (!title)
      title = argument;
  }
  for (int index = 0; index < argc; index++)
    tico_log("argv[%d] %s\n", index, argv[index] ? argv[index] : "(null)");

  /* Memory cards are the game's own saves: they follow the account */
  set_folder("Folders/MemoryCards", saves, "sdmc:/tico/saves/ps2");
  set_folder("Folders/Savestates", states, "sdmc:/tico/states/ps2");
  prefs_set_string("Folders/Bios", BIOS_DIR);
  prefs_set_string("Folders/Resources", RESOURCES_DIR);
  /* tico owns the return path; the standalone launcher's must not be used */
  prefs_remove("Wrapper/LauncherPath");
  prefs_remove("Wrapper/GameConfigPath");
  prefs_remove("Wrapper/GameCRCPath");
  tico_settings_apply(rom);
  /* Vulkan (14) or OpenGL; 13 is OpenGL through Zink, which the core only
   * knows as OpenGL (Wrapper/GLDriver picks the Mesa driver) */
  g_gs_renderer = prefs_get_int("EmuCore/GS/Renderer", GS_RENDERER) == GS_RENDERER_VK
                      ? GS_RENDERER_VK : GS_RENDERER_OGL;
  prefs_set_int("EmuCore/GS/Renderer", g_gs_renderer);
  if (g_gs_renderer != GS_RENDERER_VK) prefs_set_bool("Wrapper/LSFGEnabled", false);
  tico_log("renderer %d (%s)\n", g_gs_renderer,
           prefs_get_string("Wrapper/GLDriver", "nvc0"));
  select_core();

  if (!rom) {
    snprintf(display_title, sizeof(display_title), "NetherSX2");
    return false;
  }
  char path[1024];
  resolve_rom(rom, path, sizeof(path));
  prefs_set_bool("Wrapper/BootBIOS", false);
  prefs_set_string("EmuCore/DiscPath", path);
  if (title && title[0]) {
    snprintf(display_title, sizeof(display_title), "%s", title);
  } else {
    const char *name = strrchr(path, '/');
    snprintf(display_title, sizeof(display_title), "%s", name ? name + 1 : path);
    char *extension = strrchr(display_title, '.');
    if (extension) *extension = '\0';
  }
  tico_log("game %s (%s)\n", path, display_title);
  return true;
}

const char *tico_display_title(void) { return display_title; }

bool tico_queue_return(void) {
  if (!envHasNextLoad()) return false;
  const char *tico = is_file(TICO_LAUNCHER_PATH) ? TICO_LAUNCHER_PATH
                                                 : TICO_LAUNCHER_ROOT_PATH;
  if (!is_file(tico)) return false;
  static char arguments[256];
  snprintf(arguments, sizeof(arguments), "%s --resume", tico);
  return R_SUCCEEDED(envSetNextLoad(tico, arguments));
}

void tico_log(const char *format, ...) {
  static FILE *file;
  if (!file) {
    make_path(DATA_ROOT);
    file = fopen(TICO_LOG_PATH, "w");
    if (!file) return;
  }
  va_list arguments;
  va_start(arguments, format);
  vfprintf(file, format, arguments);
  va_end(arguments);
  fflush(file);
}
