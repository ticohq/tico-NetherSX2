// tico's in-game menu for the NetherSX2 host, after tico-drasticDS's
// tico_menu.cpp. The overlay draws on the core's presenting thread; the menu
// runs on the host's input thread, carrying out what the overlay returns with
// the game held, and takes the overlay's lock around every call into it.

#include <switch.h>

#include <zlib.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <vector>

#include <stb_image.h>

extern "C" {
#include "config.h"
#include "prefs.h"
#include "CheatManager.h"
#include "tico/tico_entry.h"
#include "tico/tico_menu.h"
#include "tico/tico_settings.h"
#include "tico/overlay_hook.h"
#include "aaudio.h"
#ifdef USE_VULKAN
#include "hooks/vk.h"
#endif
}

#include "tico/overlay/imgui_overlay.h"
#include "tico/overlay/overlay_ui.h"
#include "tico/overlay/tico_config.h"
#include "tico/overlay/translation_manager.h"

namespace OverlayUI = SwitchFrontend::OverlayUI;
namespace ImGuiOverlay = SwitchFrontend::ImGuiOverlay;
namespace TicoConfig = SwitchFrontend::TicoConfig;

namespace {

// The overlay's slots 1..5 are the core's first five, 0..4 (NetherSX2 counts
// its slots from 0); its auto slot is the core's slot 10, past the 0..9 the
// core's own menus offer.
constexpr int kOverlaySlots = OverlayUI::kAutoStateSlot;
constexpr int kAutoCoreSlot = 10;
constexpr u64 kAutoSaveTimeoutMs = 5000;
constexpr u64 kRepeatDelayMs = 400;
constexpr u64 kRepeatRateMs = 80;
// frames the game shows, once the core names it, before the auto save is
// offered: past the boot logos' first frames
constexpr int kResumeOfferFrames = 60;
constexpr u64 kToggleCombo = HidNpadButton_Plus | HidNpadButton_Minus;
constexpr const char* kTicoCheatsDir = "sdmc:/tico/cheats/ps2/";

std::unique_lock<std::recursive_mutex> Lock() {
    return std::unique_lock<std::recursive_mutex>(ImGuiOverlay::Mutex());
}

std::string Tr(const char* key) {
    SwitchFrontend::OverlayTranslation::TranslationManager::Instance().Init();
    return SwitchFrontend::OverlayTranslation::tr(key);
}

template <typename T>
std::string TrFormat(const char* key, T value) {
    const std::string format = Tr(key);
    char text[256];
    std::snprintf(text, sizeof(text), format.c_str(), value);
    return text;
}

u64 NowMs() {
    return armTicksToNs(armGetSystemTick()) / 1000000;
}

int CoreSlot(int overlay_slot) {
    return overlay_slot == OverlayUI::kAutoStateSlot ? kAutoCoreSlot : overlay_slot - 1;
}

// ---------------------------------------------------------------------------
// The picture a PCSX2 save state keeps: Screenshot.png inside the state's zip.

uint32_t U16(const unsigned char* p) { return p[0] | (p[1] << 8); }
uint32_t U32(const unsigned char* p) { return U16(p) | (U16(p + 2) << 16); }

bool ReadZipEntry(const std::string& path, const char* wanted, std::vector<unsigned char>& out) {
    FILE* file = std::fopen(path.c_str(), "rb");
    if (!file)
        return false;
    bool ok = false;
    std::vector<unsigned char> tail, directory, packed;
    do {
        if (std::fseek(file, 0, SEEK_END) != 0)
            break;
        const long size = std::ftell(file);
        const long tail_size = size < 65557 ? size : 65557;
        if (tail_size < 22)
            break;
        tail.resize(static_cast<size_t>(tail_size));
        std::fseek(file, size - tail_size, SEEK_SET);
        if (std::fread(tail.data(), 1, tail.size(), file) != tail.size())
            break;
        long end = -1;
        for (long i = tail_size - 22; i >= 0; i--)
            if (U32(&tail[static_cast<size_t>(i)]) == 0x06054b50) {
                end = i;
                break;
            }
        if (end < 0)
            break;
        const uint32_t directory_size = U32(&tail[static_cast<size_t>(end) + 12]);
        const uint32_t directory_offset = U32(&tail[static_cast<size_t>(end) + 16]);
        if (directory_size > (1u << 20))
            break;
        directory.resize(directory_size);
        std::fseek(file, static_cast<long>(directory_offset), SEEK_SET);
        if (std::fread(directory.data(), 1, directory.size(), file) != directory.size())
            break;
        for (size_t at = 0; at + 46 <= directory.size();) {
            const unsigned char* entry = &directory[at];
            if (U32(entry) != 0x02014b50)
                break;
            const uint32_t method = U16(entry + 10);
            const uint32_t packed_size = U32(entry + 20);
            const uint32_t size_out = U32(entry + 24);
            const uint32_t name_length = U16(entry + 28);
            const uint32_t extra_length = U16(entry + 30);
            const uint32_t comment_length = U16(entry + 32);
            const uint32_t local = U32(entry + 42);
            const bool match = at + 46 + name_length <= directory.size() &&
                               name_length == std::strlen(wanted) &&
                               std::memcmp(entry + 46, wanted, name_length) == 0;
            at += 46 + name_length + extra_length + comment_length;
            if (!match)
                continue;
            if (size_out > (16u << 20) || packed_size > (16u << 20))
                break;
            unsigned char header[30];
            std::fseek(file, static_cast<long>(local), SEEK_SET);
            if (std::fread(header, 1, sizeof(header), file) != sizeof(header) ||
                U32(header) != 0x04034b50)
                break;
            std::fseek(file, static_cast<long>(local + 30 + U16(header + 26) + U16(header + 28)),
                       SEEK_SET);
            packed.resize(packed_size);
            if (std::fread(packed.data(), 1, packed.size(), file) != packed.size())
                break;
            if (method == 0) {
                out = packed;
                ok = true;
            } else if (method == 8) {
                out.resize(size_out);
                z_stream stream{};
                if (inflateInit2(&stream, -MAX_WBITS) == Z_OK) {
                    stream.next_in = packed.data();
                    stream.avail_in = static_cast<uInt>(packed.size());
                    stream.next_out = out.data();
                    stream.avail_out = static_cast<uInt>(out.size());
                    ok = inflate(&stream, Z_FINISH) == Z_STREAM_END;
                    inflateEnd(&stream);
                }
            }
            break;
        }
    } while (false);
    std::fclose(file);
    return ok;
}

// ---------------------------------------------------------------------------
// Input

bool PromptKeyboard(const char* header, const std::string& initial, std::size_t max_length,
                    std::string& output) {
    SwkbdConfig keyboard;
    if (R_FAILED(swkbdCreate(&keyboard, 0)))
        return false;
    swkbdConfigMakePresetDefault(&keyboard);
    swkbdConfigSetHeaderText(&keyboard, header);
    if (!initial.empty())
        swkbdConfigSetInitialText(&keyboard, initial.c_str());
    swkbdConfigSetStringLenMax(&keyboard, static_cast<u32>(max_length));
    std::vector<char> text(max_length + 1);
    const Result result = swkbdShow(&keyboard, text.data(), text.size());
    swkbdClose(&keyboard);
    if (R_FAILED(result))
        return false;
    output = text.data();
    return true;
}

// Edge-triggered directions, repeating while one is held.
struct NavRepeat {
    u64 button = 0;
    u64 at = 0;

    u64 Apply(u64 held, u64 pressed) {
        constexpr u64 kDirections = HidNpadButton_AnyUp | HidNpadButton_AnyDown |
                                    HidNpadButton_AnyLeft | HidNpadButton_AnyRight;
        const u64 now = NowMs();
        if (pressed & kDirections) {
            button = pressed & kDirections;
            at = now + kRepeatDelayMs;
            return pressed;
        }
        if (!button || !(held & button)) {
            button = 0;
            return pressed;
        }
        if (now >= at) {
            at = now + kRepeatRateMs;
            return pressed | button;
        }
        return pressed;
    }
};

std::string ControllerName(HidNpadIdType id) {
    const u32 style = hidGetNpadStyleSet(id);
    if (style & HidNpadStyleTag_NpadHandheld)
        return "Handheld";
    if (style & HidNpadStyleTag_NpadFullKey)
        return "Pro Controller";
    if (style & HidNpadStyleTag_NpadJoyDual)
        return "Joy-Con (L/R)";
    if (style & HidNpadStyleTag_NpadJoyLeft)
        return "Joy-Con (L)";
    if (style & HidNpadStyleTag_NpadJoyRight)
        return "Joy-Con (R)";
    return style ? "Controller" : "";
}

// ---------------------------------------------------------------------------

struct Menu {
    TicoMenuCore core{};
    bool open = false;
    bool exit_requested = false;
    bool restart_requested = false;
    NavRepeat repeat;
    std::array<unsigned long long, kOverlaySlots> slot_pictures{};

    std::mutex game_mutex;
    std::string serial;
    uint32_t crc = 0;
    bool game_known = false;
    bool cheats_imported = false;
    bool resume_offered = false;
    // frames presented when the core named the game
    int frames_at_game = -1;
    // the values of the options read only at launch (Use Restart to apply)
    std::vector<std::pair<std::string, std::string>> launch_values;

    NxCheatList cheats{};
    std::string cheat_path;

    bool GameKnown(std::string* serial_out = nullptr, uint32_t* crc_out = nullptr) {
        std::lock_guard<std::mutex> lock(game_mutex);
        if (serial_out)
            *serial_out = serial;
        if (crc_out)
            *crc_out = crc;
        return game_known;
    }

    // The core's own pause stops its frames, and the menu with them: instead
    // the presenting thread stays in the present while the menu is open
    // (tico/overlay_hook.h), which stalls the core within a frame or two.
    void Hold(bool hold) {
        __atomic_store_n(&tico_overlay_hold, hold ? 1 : 0, __ATOMIC_RELEASE);
        aaudio_set_muted(hold);
    }

    void Open() {
        if (open)
            return;
        open = true;
        repeat = {};
        if (core.release_inputs)
            core.release_inputs();
        {
            auto lock = Lock();
            ImGuiOverlay::SetVisible(true);
        }
        Hold(true);
    }

    void Close() {
        if (!open)
            return;
        open = false;
        Hold(false);
        auto lock = Lock();
        ImGuiOverlay::SetVisible(false);
    }

    // -----------------------------------------------------------------------
    // Save states: <Folders/Savestates>/<serial> (<CRC>).<slot>.p2s

    std::string StatePath(int core_slot) {
        std::string game_serial;
        uint32_t game_crc = 0;
        if (!GameKnown(&game_serial, &game_crc))
            return {};
        char name[160];
        std::snprintf(name, sizeof(name), "/%s (%08X).%02d.p2s", game_serial.c_str(),
                      static_cast<unsigned>(game_crc), core_slot);
        return std::string(prefs_get_string("Folders/Savestates", "")) + name;
    }

    bool SlotOccupied(int core_slot) {
        const std::string path = StatePath(core_slot);
        struct stat status;
        return !path.empty() && stat(path.c_str(), &status) == 0 && status.st_size > 0;
    }

    // Runs while the overlay draws (its lock held).
    OverlayUI::SlotPreview SlotPreview(int overlay_slot) {
        OverlayUI::SlotPreview preview;
        if (overlay_slot < 1 || overlay_slot > kOverlaySlots)
            return preview;
        unsigned long long& picture = slot_pictures[static_cast<size_t>(overlay_slot - 1)];
        ImGuiOverlay::DestroyTexture(picture); // the slot may have been saved again
        picture = 0;
        const std::string path = StatePath(CoreSlot(overlay_slot));
        struct stat status;
        if (path.empty() || stat(path.c_str(), &status) != 0)
            return preview;
        char when[32];
        std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M", std::localtime(&status.st_mtime));
        preview.saved_at = when;
        std::vector<unsigned char> png;
        if (!ReadZipEntry(path, "Screenshot.png", png))
            return preview;
        int width = 0, height = 0, channels = 0;
        unsigned char* rgba = stbi_load_from_memory(png.data(), static_cast<int>(png.size()),
                                                    &width, &height, &channels, 4);
        if (!rgba)
            return preview;
        picture = ImGuiOverlay::CreateTexture(rgba, width, height);
        stbi_image_free(rgba);
        preview.texture = picture;
        // PS2 frames are shown at 4:3 whatever their stored size
        preview.aspect = 4.0f / 3.0f;
        return preview;
    }

    // The core saves and loads on its own thread, which runs again once the
    // menu closes: the state is the frame the menu froze.
    void SaveState(int overlay_slot) {
        const bool known = GameKnown();
        if (known && core.save_state)
            core.save_state(CoreSlot(overlay_slot));
        auto lock = Lock();
        OverlayUI::ShowToast(
            TrFormat(known ? "nethersx2_saving_state" : "nethersx2_save_failed", overlay_slot));
    }

    void LoadState(int overlay_slot) {
        const int core_slot = CoreSlot(overlay_slot);
        const bool present = SlotOccupied(core_slot);
        if (present && core.load_state)
            core.load_state(core_slot);
        auto lock = Lock();
        if (present && core_slot == kAutoCoreSlot)
            OverlayUI::ShowToast(Tr("emulator_auto_loaded"));
        else
            OverlayUI::ShowToast(
                TrFormat(present ? "nethersx2_state_loaded" : "nethersx2_load_failed", overlay_slot));
    }

    void AutoSave() {
        if (!GameKnown() || !core.save_state)
            return;
        // the core saves on its own thread, which must be running
        Close();
        const std::string path = StatePath(kAutoCoreSlot);
        struct stat before {};
        const bool existed = stat(path.c_str(), &before) == 0;
        core.save_state(kAutoCoreSlot);
        const u64 deadline = NowMs() + kAutoSaveTimeoutMs;
        off_t last_size = -1;
        while (NowMs() < deadline) {
            svcSleepThread(50000000ULL);
            struct stat after {};
            if (stat(path.c_str(), &after) != 0 || after.st_size <= 0)
                continue;
            const bool newer = !existed || after.st_mtime != before.st_mtime ||
                               after.st_size != before.st_size;
            // written once its size holds still between two looks
            if (newer && after.st_size == last_size) {
                tico_log("menu: auto save written (%lld bytes)\n",
                         static_cast<long long>(after.st_size));
                return;
            }
            last_size = after.st_size;
        }
        tico_log("menu: auto save not confirmed within %llu ms\n",
                 static_cast<unsigned long long>(kAutoSaveTimeoutMs));
    }

    // A game with an auto save offers to continue from it, as tico's General >
    // Continue Last Game says, unless this launch is a Restart.
    void OfferResume() {
        resume_offered = true;
        if (tico_was_restarted() || !SlotOccupied(kAutoCoreSlot))
            return;
        const std::string mode = TicoConfig::ResumeOnLaunch();
        tico_log("menu: auto save found, Continue Last Game is %s\n", mode.c_str());
        if (mode == "never")
            return;
        if (mode == "always") {
            LoadState(OverlayUI::kAutoStateSlot);
            return;
        }
        Open();
        auto lock = Lock();
        OverlayUI::ShowResumePrompt();
    }

    // -----------------------------------------------------------------------
    // Cheats: the core reads <cheats>/<CRC>.pnach; tico's own file for the
    // game, sdmc:/tico/cheats/ps2/<game>.pnach, is brought in with every
    // cheat off, as tico's other emulators start them.

    void ImportTicoCheats() {
        cheats_imported = true;
        uint32_t game_crc = 0;
        if (!GameKnown(nullptr, &game_crc) || !game_crc)
            return;
        char target[128];
        std::snprintf(target, sizeof(target), CHEATS_DIR "/%08X.pnach",
                      static_cast<unsigned>(game_crc));
        cheat_path = target;
        std::string name = tico_rom_path();
        const size_t slash = name.find_last_of('/');
        if (slash != std::string::npos)
            name = name.substr(slash + 1);
        const size_t dot = name.find_last_of('.');
        if (dot != std::string::npos)
            name = name.substr(0, dot);
        if (name.empty())
            return;
        const std::string source = kTicoCheatsDir + name + ".pnach";
        FILE* input = std::fopen(source.c_str(), "rb");
        if (!input)
            return;
        FILE* output = std::fopen(target, "wb");
        if (output) {
            char buffer[8192];
            size_t count;
            while ((count = std::fread(buffer, 1, sizeof(buffer), input)) > 0)
                std::fwrite(buffer, 1, count, output);
            std::fclose(output);
        }
        std::fclose(input);
        if (!output)
            return;
        NxCheatList list{};
        if (nx_cheat_load(target, &list))
            for (size_t i = 0; i < list.count; i++)
                if (list.entries[i].enabled_count)
                    nx_cheat_set_enabled(target, i, 0);
        if (core.reload_patches)
            core.reload_patches();
        tico_log("menu: %zu cheats from %s\n", list.count, source.c_str());
    }

    std::vector<OverlayUI::CheatMenuEntry> CheatEntries() {
        std::vector<OverlayUI::CheatMenuEntry> entries;
        cheats = {};
        if (cheat_path.empty() || !nx_cheat_load(cheat_path.c_str(), &cheats))
            return entries;
        for (size_t i = 0; i < cheats.count; i++) {
            OverlayUI::CheatMenuEntry entry;
            entry.name = cheats.entries[i].name;
            entry.enabled = cheats.entries[i].enabled_count > 0;
            entry.source_index = static_cast<int>(i);
            entries.push_back(std::move(entry));
        }
        return entries;
    }

    bool ToggleCheat(int index) {
        if (index < 0 || static_cast<size_t>(index) >= cheats.count)
            return false;
        const bool enable = cheats.entries[index].enabled_count == 0;
        if (!nx_cheat_set_enabled(cheat_path.c_str(), static_cast<size_t>(index), enable))
            return false;
        cheats.entries[index].enabled_count = enable ? cheats.entries[index].patch_count : 0;
        if (core.reload_patches)
            core.reload_patches();
        return true;
    }

    // -----------------------------------------------------------------------

    // The values of the options read only at launch, kept while the game runs:
    // the core applies some of them at once (a renderer change freezes it).
    void RememberLaunchValues() {
        launch_values.clear();
        for (const TicoConfig::OptionCategory& category : TicoConfig::GetCategories())
            for (std::size_t i = 0; i < category.option_count; i++) {
                const TicoConfig::OptionDef& option = category.options[i];
                if (option.needs_restart && std::strchr(option.key, '/'))
                    launch_values.emplace_back(option.key, prefs_get_string(option.key, ""));
            }
        launch_values.emplace_back("Wrapper/GLDriver", prefs_get_string("Wrapper/GLDriver", ""));
    }

    void ApplySettings() {
        // per-game settings may have been saved or deleted: the values that
        // now apply go to the prefs the core and host read
        TicoConfig::ApplyToPrefs();
        tico_settings_derive();
        for (const auto& [key, value] : launch_values)
            prefs_set_string(key.c_str(), value.c_str());
        if (core.apply_settings)
            core.apply_settings();
#ifdef USE_VULKAN
        const bool lsfg = prefs_get_bool("Wrapper/LSFGEnabled", false);
        if (lsfg != (vk_lsfg_is_enabled() != 0)) {
            if (vk_lsfg_is_available())
                vk_lsfg_request_enabled(lsfg);
            else {
                auto lock = Lock();
                OverlayUI::ShowToast(Tr("nethersx2_lsfg_next_launch"),
                                     OverlayUI::ToastCorner::TopRight);
            }
        }
#endif
    }

    void EditText() {
        const TicoConfig::OptionDef* option;
        {
            auto lock = Lock();
            option = OverlayUI::ConsumeTextEditOption();
        }
        if (!option)
            return;
        std::string value;
        const std::size_t length = option->max_length > 0 ? option->max_length : 64;
        const std::string header = Tr(option->label_key ? option->label_key : option->fallback);
        if (!PromptKeyboard(header.c_str(), TicoConfig::GetOptionValue(*option), length, value))
            return;
        auto lock = Lock();
        TicoConfig::SetOptionValue(*option, value);
        OverlayUI::NotifyOptionEdited(*option);
    }

    void ControllerOrder() {
        HidLaControllerSupportArg arg;
        hidLaCreateControllerSupportArg(&arg);
        arg.hdr.player_count_min = 1;
        arg.hdr.player_count_max = 2;
        HidLaControllerSupportResultInfo result{};
        hidLaShowControllerSupport(&result, &arg);
    }

    void RunAction(OverlayUI::Action action) {
        using OverlayUI::Action;
        if (action != Action::None)
            tico_log("menu: action %d\n", static_cast<int>(action));
        switch (action) {
        case Action::None:
            return;
        case Action::Resume:
            Close();
            return;
        case Action::Exit:
            exit_requested = true;
            Close();
            return;
        case Action::Reset:
            if (core.reset)
                core.reset();
            Close();
            return;
        case Action::Restart:
            restart_requested = true;
            exit_requested = true;
            Close();
            return;
        case Action::EditText:
            EditText();
            return;
        case Action::ControllerOrder:
            ControllerOrder();
            return;
        default:
            break;
        }
        if (OverlayUI::IsSaveStateAction(action)) {
            SaveState(OverlayUI::GetStateSlotForAction(action));
            Close();
        } else if (OverlayUI::IsLoadStateAction(action)) {
            LoadState(OverlayUI::GetStateSlotForAction(action));
            Close();
        }
    }

    bool Update(u64 held, u64 pressed) {
        const bool toggle = (held & kToggleCombo) == kToggleCombo && (pressed & kToggleCombo);
        if (!open) {
            if (!toggle || !GameKnown())
                return false;
            Open();
            return true;
        }
        OverlayUI::Action action;
        bool settings_changed;
        {
            auto lock = Lock();
            action = ImGuiOverlay::ConsumeAction();
            settings_changed = OverlayUI::ConsumeSettingsChanged();
        }
        RunAction(action);
        if (settings_changed)
            ApplySettings();
        if (!open)
            return true;
        if (toggle) {
            Close();
            return true;
        }
        HidTouchScreenState touch{};
        auto lock = Lock();
        if (hidGetTouchScreenStates(&touch, 1)) {
            ImGuiOverlay::FeedTouch({
                .down = touch.count > 0,
                .x = touch.count > 0 ? static_cast<float>(touch.touches[0].x) : 0.0f,
                .y = touch.count > 0 ? static_cast<float>(touch.touches[0].y) : 0.0f,
            });
        }
        pressed = repeat.Apply(held, pressed);
        ImGuiOverlay::FeedNav({
            .up = (pressed & HidNpadButton_AnyUp) != 0,
            .down = (pressed & HidNpadButton_AnyDown) != 0,
            .left = (pressed & HidNpadButton_AnyLeft) != 0,
            .right = (pressed & HidNpadButton_AnyRight) != 0,
            .accept = (pressed & HidNpadButton_A) != 0,
            .cancel = (pressed & HidNpadButton_B) != 0,
        });
        return true;
    }

    void Tick() {
        if (!GameKnown() || !core.presented_frames)
            return;
        if (frames_at_game < 0)
            frames_at_game = core.presented_frames();
        if (!cheats_imported)
            ImportTicoCheats();
        if (!resume_offered && core.presented_frames() - frames_at_game >= kResumeOfferFrames)
            OfferResume();
    }
};

Menu* s_menu = nullptr;

} // namespace

extern "C" {

void tico_menu_init(const TicoMenuCore* core, bool vulkan) {
    if (s_menu || !core)
        return;
    s_menu = new Menu();
    s_menu->core = *core;
    s_menu->RememberLaunchValues();
    Menu* menu = s_menu;
    auto lock = Lock();
    OverlayUI::ReloadSettings();
    OverlayUI::SetGameTitle(tico_display_title());
    OverlayUI::SetSlotOccupiedCallback([menu](int slot) {
        return slot >= 1 && slot <= kOverlaySlots && menu->SlotOccupied(CoreSlot(slot));
    });
    OverlayUI::SetSlotPreviewCallback([menu](int slot) { return menu->SlotPreview(slot); });
    OverlayUI::SetCheatCallbacks([menu] { return menu->CheatEntries(); },
                                 [menu](int index) { return menu->ToggleCheat(index); });
    OverlayUI::PlayerCallbacks players;
    players.ports = [] {
        return std::vector<std::string>{ControllerName(HidNpadIdType_Handheld).empty()
                                            ? ControllerName(HidNpadIdType_No1)
                                            : ControllerName(HidNpadIdType_Handheld),
                                        ControllerName(HidNpadIdType_No2)};
    };
    players.note = [] { return std::string(); };
    OverlayUI::SetPlayerCallbacks(players);
    ImGuiOverlay::Init(vulkan);
    tico_log("menu: ready (%s)\n", vulkan ? "Vulkan" : "OpenGL");
}

void tico_menu_shutdown(void) {
    if (!s_menu)
        return;
    {
        auto lock = Lock();
        ImGuiOverlay::Shutdown();
        OverlayUI::SetSlotOccupiedCallback(nullptr);
        OverlayUI::SetSlotPreviewCallback(nullptr);
        OverlayUI::SetCheatCallbacks(nullptr, nullptr);
        OverlayUI::SetPlayerCallbacks({});
    }
    delete s_menu;
    s_menu = nullptr;
}

void tico_menu_game_changed(const char* serial, uint32_t crc) {
    if (!s_menu)
        return;
    std::lock_guard<std::mutex> lock(s_menu->game_mutex);
    s_menu->serial = serial ? serial : "";
    s_menu->crc = crc;
    s_menu->game_known = crc != 0;
}

bool tico_menu_update(u64 held, u64 pressed) {
    return s_menu && s_menu->Update(held, pressed);
}

void tico_menu_tick(void) {
    if (s_menu)
        s_menu->Tick();
}

bool tico_menu_is_open(void) {
    return s_menu && s_menu->open;
}

bool tico_menu_take_exit_request(void) {
    if (!s_menu || !s_menu->exit_requested)
        return false;
    s_menu->exit_requested = false;
    return true;
}

bool tico_menu_take_restart_request(void) {
    if (!s_menu || !s_menu->restart_requested)
        return false;
    s_menu->restart_requested = false;
    return true;
}

void tico_menu_auto_save(void) {
    if (s_menu)
        s_menu->AutoSave();
}

} // extern "C"
