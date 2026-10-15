#!/usr/bin/env python3
"""Generates the module's settings.json and lang/en.json from the option
tables of the upstream launcher (launcher/source/main.cpp), so tico's
settings screen offers what NetherSX2_nx's launcher does.

Option keys are the nethersx2.ini keys: the host copies every value tico
stores in nethersx2.jsonc into its prefs under the same name.

  gen_tico_settings.py [--launcher main.cpp] [--module tico/module]
"""
import argparse
import json
import os
import re

# tico tabs: (tab id, title, [(launcher table, section id, section title)])
LAYOUT = [
    ("video", "Video", [
        ("S_graphics", "graphics", "Graphics"),
        ("S_enhance", "enhancements", "Enhancements"),
        ("S_framegen", "framegen", "Frame Generation (Vulkan)"),
    ]),
    ("audio", "Audio", [("S_audio", "audio", "Audio")]),
    ("system", "System", [
        ("S_emu", "emulation", "Emulation"),
        ("S_framerate", "framerate", "Frame Rate Control"),
        ("S_network", "network", "Network (experimental)"),
    ]),
    ("controls", "Controls", [("S_controller", "controller", "Controller")]),
    ("advanced", "Advanced", [
        ("S_advanced", "cpu", "CPU"),
        ("S_gamefixes", "gamefixes", "Game Fixes"),
    ]),
]

# Read only when a game starts (the rest the core applies at once): the
# renderer and core, the CPU and VU setup, game fixes, audio output, network.
RESTART_KEYS = {
    "EmuCore/GS/Renderer", "EmuCore/GS/DisableThreadedPresentation",
    "EmuCore/Speedhacks/vuThread", "EmuCore/EnableGameFixes", "EmuCore/EnablePatches",
    "Wrapper/CoreBuild", "Wrapper/FastmemMode", "Wrapper/FastBoot",
    "Wrapper/SystemLanguage", "Wrapper/ControllerCount", "Wrapper/Network",
    "Wrapper/NetDNS", "Wrapper/LSFGFlowScale", "Wrapper/LSFGPerformance",
}
RESTART_PREFIXES = ("EmuCore/CPU/", "EmuCore/Gamefixes/", "SPU2/Output/")

# The module's own options, after the launcher's: keys without a section
# are the menu's, not nethersx2.ini's.
OWN_SECTIONS = {
    "system": [{
        "id": "performance",
        "title": "Performance",
        "options": [{
            # tico-dolphin's Boost mode (source/tico/tico_clocks.c)
            "key": "Wrapper/BoostMode",
            "label": "settings_nethersx2_boost_mode",
            "text": "Boost mode (CPU 1785 MHz, GPU 768 MHz)",
            "type": "bool",
            "default": "false",
            "restart": True,
        }],
    }, {
        "id": "interface",
        "title": "Interface",
        "options": [{
            "key": "nethersx2_menu_theme",
            "label": "settings_nethersx2_menu_theme",
            "text": "In-game menu theme",
            "type": "enum",
            "default": "system",
            "choices": [
                {"label": "Console theme", "value": "system"},
                {"label": "Light", "value": "light"},
                {"label": "Dark", "value": "dark"},
            ],
        }],
    }],
}

# Other languages' strings for the module's own options and sections (the
# launcher's options are English only); choice labels go through
# settings_nethersx2_value_<label> keys, as tico_config.cpp looks them up.
OWN_TRANSLATIONS = {
    "settings_nethersx2_boost_mode": {
        "pt": "Modo boost (CPU 1785 MHz, GPU 768 MHz)",
        "es": "Modo boost (CPU 1785 MHz, GPU 768 MHz)",
        "fr": "Mode boost (CPU 1785 MHz, GPU 768 MHz)",
        "de": "Boost-Modus (CPU 1785 MHz, GPU 768 MHz)",
        "ja": "ブーストモード（CPU 1785 MHz、GPU 768 MHz）",
        "ru": "Режим ускорения (CPU 1785 МГц, GPU 768 МГц)",
        "zh": "加速模式（CPU 1785 MHz，GPU 768 MHz）",
    },
    "settings_nethersx2_menu_theme": {
        "pt": "Tema do menu do jogo",
        "es": "Tema del menú del juego",
        "fr": "Thème du menu en jeu",
        "de": "Design des Spielmenüs",
        "ja": "ゲーム内メニューのテーマ",
        "ru": "Тема игрового меню",
        "zh": "游戏内菜单主题",
    },
    "settings_nethersx2_section_performance": {
        "pt": "Desempenho", "es": "Rendimiento", "fr": "Performances",
        "de": "Leistung", "ja": "パフォーマンス", "ru": "Производительность", "zh": "性能",
    },
    "settings_nethersx2_section_interface": {
        "pt": "Interface", "es": "Interfaz", "fr": "Interface",
        "de": "Oberfläche", "ja": "インターフェース", "ru": "Интерфейс", "zh": "界面",
    },
    "settings_nethersx2_value_console_theme": {
        "pt": "Tema do console", "es": "Tema de la consola", "fr": "Thème de la console",
        "de": "Konsolendesign", "ja": "本体のテーマ", "ru": "Тема консоли", "zh": "主机主题",
    },
    "settings_nethersx2_value_light": {
        "pt": "Claro", "es": "Claro", "fr": "Clair", "de": "Hell", "ja": "ライト",
        "ru": "Светлая", "zh": "浅色",
    },
    "settings_nethersx2_value_dark": {
        "pt": "Escuro", "es": "Oscuro", "fr": "Sombre", "de": "Dunkel", "ja": "ダーク",
        "ru": "Тёмная", "zh": "深色",
    },
}

STRING = r'"((?:[^"\\]|\\.)*)"'


def unquote(text):
    return json.loads(f'"{text}"')


def parse_choices(source):
    choices = {}
    for name, body in re.findall(r"static const Choice (\w+)\[\]\s*=\s*\{(.*?)\};",
                                 source, re.S):
        choices[name] = [(unquote(l), unquote(v)) for l, v in
                         re.findall(r"\{\s*" + STRING + r"\s*,\s*" + STRING + r"\s*\}", body)]
    return choices


def split_arguments(text):
    arguments, depth, current, quoted = [], 0, "", False
    for i, c in enumerate(text):
        if c == '"' and (i == 0 or text[i - 1] != "\\"):
            quoted = not quoted
        if not quoted and c == "(":
            depth += 1
        if not quoted and c == ")":
            depth -= 1
        if not quoted and depth == 0 and c == ",":
            arguments.append(current.strip())
            current = ""
            continue
        current += c
    if current.strip():
        arguments.append(current.strip())
    return arguments


def parse_tables(source):
    tables = {}
    for name, body in re.findall(r"static const Opt (\w+)\[\]\s*=\s*\{(.*?)\n\};", source, re.S):
        entries = []
        for macro, arguments in re.findall(r"(O_\w+)\s*\((.*?)\)\s*,?\s*\n", body + "\n"):
            entries.append((macro, split_arguments(arguments)))
        tables[name] = entries
    return tables


def text(argument):
    return unquote(argument.strip()[1:-1])


def range_choices(lo, hi, step, multiplier=1, suffix=""):
    # tico has no range type: an enum of the steps, at most ~30 of them
    step *= max(1, -(-(hi - lo) // (30 * step)))
    values = list(range(lo, hi + 1, step))
    if values[-1] != hi:
        values.append(hi)
    choices = []
    for v in values:
        stored = f"{v / multiplier:g}" if multiplier != 1 else str(v)
        choices.append({"label": f"{v}{suffix}", "value": stored})
    return choices


def option(macro, args, choices, strings, prefix):
    label = text(args[0])
    if macro in ("O_SUB", "O_STATUS", "O_STATUSK", "O_HOTKEY"):
        return None
    key = text(args[1])
    label_key = f"{prefix}_{re.sub(r'[^a-z0-9]+', '_', key.lower()).strip('_')}"
    strings[label_key] = label.strip()
    entry = {"key": key, "label": label_key}
    gate = None
    if macro in ("O_CHOICE", "O_CHOICEG"):
        entry["type"] = "enum"
        entry["default"] = text(args[3])
        values = choices[args[2]]
        if [v for _, v in values] == ["false", "true"]:
            entry["type"] = "bool"
        else:
            entry["choices"] = [{"label": l, "value": v} for l, v in values]
        if macro == "O_CHOICEG":
            gate = (text(args[4]), text(args[5]))
    elif macro in ("O_RANGE", "O_RANGEG"):
        lo, hi, step = (int(a) for a in args[2:5])
        entry.update(type="enum", default=text(args[5]),
                     choices=range_choices(lo, hi, step))
        if macro == "O_RANGEG":
            gate = (text(args[6]), text(args[7]))
    elif macro in ("O_SCALED_RANGE", "O_SCALED_RANGEG"):
        lo, hi, step = (int(a) for a in args[2:5])
        multiplier, suffix = int(args[6]), text(args[7])
        entry.update(type="enum", default=text(args[5]),
                     choices=range_choices(lo, hi, step, multiplier, suffix))
        if macro == "O_SCALED_RANGEG":
            gate = (text(args[8]), text(args[9]))
    elif macro in ("O_TEXT", "O_TEXTG"):
        entry.update(type="string", default=text(args[2]), max_length=64)
        if macro == "O_TEXTG":
            gate = (text(args[3]), text(args[4]))
    else:
        raise SystemExit(f"unknown option macro {macro}")
    # The launcher hides a gated option while its gate has the "off" value;
    # tico shows one while another has a value, which fits two-valued gates.
    if gate:
        on = {"false": "true", "true": "false"}.get(gate[1])
        if on:
            entry["depends_on"] = {"key": gate[0], "value": on}
    if entry["type"] == "enum":
        values = [c["value"] for c in entry["choices"]]
        if entry["default"] not in values:
            entry["default"] = min(values, key=lambda v: abs(float(v) - float(entry["default"]))) \
                if all(re.fullmatch(r"-?[\d.]+", v) for v in values + [entry["default"]]) \
                else values[0]
    if key in RESTART_KEYS or key.startswith(RESTART_PREFIXES):
        entry["restart"] = True
    return entry


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--launcher", default=os.path.join(root, "launcher/source/main.cpp"))
    parser.add_argument("--module", default=os.path.join(root, "tico/module"))
    args = parser.parse_args()
    with open(args.launcher, encoding="utf-8") as f:
        source = f.read()
    choices, tables = parse_choices(source), parse_tables(source)
    prefix = "settings_nethersx2"
    strings = {}
    tabs = []
    for tab_id, tab_title, sections in LAYOUT:
        tab_key = f"{prefix}_tab_{tab_id}"
        strings[tab_key] = tab_title
        tab = {"name": tab_key, "sections": []}
        for table, section_id, section_title in sections:
            section_key = f"{prefix}_section_{section_id}"
            strings[section_key] = section_title
            options = [o for o in (option(m, a, choices, strings, prefix)
                                   for m, a in tables[table]) if o]
            tab["sections"].append({"title": section_key, "options": options})
        for section in OWN_SECTIONS.get(tab_id, []):
            section_key = f"{prefix}_section_{section['id']}"
            strings[section_key] = section["title"]
            for own in section["options"]:
                strings[own["label"]] = own.pop("text")
                for choice in own.get("choices", []):
                    value_key = "settings_nethersx2_value_" + re.sub(
                        r"[^a-z0-9]+", "_", choice["label"].lower()).strip("_")
                    strings[value_key] = choice["label"]
            tab["sections"].append({"title": section_key, "options": section["options"]})
        tabs.append(tab)
    settings = {
        "core_id": "nethersx2",
        "display_name": "NetherSX2",
        "config_file": "nethersx2.jsonc",
        "slugs": ["ps2"],
        "bool_true_value": "true",
        "bool_false_value": "false",
        "tabs": tabs,
    }
    os.makedirs(os.path.join(args.module, "lang"), exist_ok=True)
    with open(os.path.join(args.module, "settings.json"), "w", encoding="utf-8", newline="\n") as f:
        json.dump(settings, f, indent=2, ensure_ascii=False)
        f.write("\n")
    with open(os.path.join(args.module, "lang", "en.json"), "w", encoding="utf-8", newline="\n") as f:
        json.dump(strings, f, indent=2, ensure_ascii=False)
        f.write("\n")
    languages = sorted({lang for texts in OWN_TRANSLATIONS.values() for lang in texts})
    for lang in languages:
        own = {key: texts[lang] for key, texts in OWN_TRANSLATIONS.items() if lang in texts}
        with open(os.path.join(args.module, "lang", f"{lang}.json"), "w", encoding="utf-8",
                  newline="\n") as f:
            json.dump(own, f, indent=2, ensure_ascii=False)
            f.write("\n")
    count = sum(len(s["options"]) for t in tabs for s in t["sections"])
    print(f"{count} options in {len(tabs)} tabs")


if __name__ == "__main__":
    main()
