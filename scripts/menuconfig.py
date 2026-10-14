#!/usr/bin/env python3
"""Configure the boot options supported by the CreateOS build."""

import curses
import os
from pathlib import Path
import sys
import tempfile


ROOT = Path(__file__).resolve().parent.parent
DEFAULT_CONFIG = ROOT / "configs" / "default.config"
CONFIG_FILE = ROOT / ".config"
OPTIONS = {
    "CONFIG_CONSOLE": ("tty1", "ttyS0"),
    "CONFIG_LOGLEVEL": ("quiet", "warn", "info", "debug"),
    "CONFIG_GRUB_TIMEOUT": ("0", "3", "5"),
}
DESCRIPTIONS = {
    "CONFIG_CONSOLE": "Boot console",
    "CONFIG_LOGLEVEL": "Kernel log verbosity",
    "CONFIG_GRUB_TIMEOUT": "GRUB menu timeout (seconds)",
}


def load_config(path):
    values = {key: choices[0] for key, choices in OPTIONS.items()}
    for config_path in (DEFAULT_CONFIG, path):
        if not config_path.exists():
            continue
        for line_number, line in enumerate(config_path.read_text().splitlines(), 1):
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            if "=" not in line:
                raise ValueError(f"{config_path}:{line_number}: expected KEY=VALUE")
            key, value = (part.strip() for part in line.split("=", 1))
            if key not in OPTIONS:
                raise ValueError(f"{config_path}:{line_number}: unknown option {key}")
            if value not in OPTIONS[key]:
                raise ValueError(f"{config_path}:{line_number}: invalid value for {key}")
            values[key] = value
    return values


def save_config(path, values):
    content = "".join(f"{key}={values[key]}\n" for key in OPTIONS)
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=".config-", dir=path.parent, text=True)
    try:
        with os.fdopen(fd, "w") as output:
            output.write(content)
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary, path)
    except BaseException:
        try:
            os.unlink(temporary)
        except FileNotFoundError:
            pass
        raise


def export_makefile(path):
    values = load_config(CONFIG_FILE)
    content = "".join(f"{key} := {values[key]}\n" for key in OPTIONS)
    output = Path(path)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(content)


def draw_menu(screen, values, selection, message):
    screen.erase()
    height, width = screen.getmaxyx()
    screen.addnstr(0, 0, "CreateOS kernel and boot configuration", width - 1,
                   curses.A_BOLD)
    screen.addnstr(2, 0, "Select an option with ↑/↓ or j/k; Enter changes it.",
                   width - 1)
    screen.addnstr(3, 0, "S saves .config and Q exits without saving.",
                   width - 1)
    for index, key in enumerate(OPTIONS):
        row = 6 + index * 2
        if row >= height - 2:
            break
        label = f"{DESCRIPTIONS[key]:32} {values[key]}"
        screen.addnstr(row, 2, label, width - 3,
                       curses.A_REVERSE if index == selection else curses.A_NORMAL)
    if message:
        screen.addnstr(height - 2, 0, message, width - 1)
    screen.refresh()


def run_menu(screen):
    curses.curs_set(0)
    screen.keypad(True)
    values = load_config(CONFIG_FILE)
    keys = list(OPTIONS)
    selection = 0
    message = "Changes are applied to the next ISO build."
    while True:
        draw_menu(screen, values, selection, message)
        key = screen.getch()
        if key in (curses.KEY_UP, ord("k")):
            selection = (selection - 1) % len(keys)
        elif key in (curses.KEY_DOWN, ord("j")):
            selection = (selection + 1) % len(keys)
        elif key in (curses.KEY_ENTER, 10, 13, ord(" ")):
            option = keys[selection]
            choices = OPTIONS[option]
            values[option] = choices[(choices.index(values[option]) + 1) %
                                     len(choices)]
        elif key in (ord("s"), ord("S")):
            save_config(CONFIG_FILE, values)
            message = f"Saved {CONFIG_FILE}"
        elif key in (ord("q"), ord("Q"), 27):
            return


def main(argv):
    if len(argv) == 3 and argv[1] == "--export":
        export_makefile(argv[2])
        return 0
    if len(argv) != 1:
        print("Usage: menuconfig.py [--export MAKEFILE]", file=sys.stderr)
        return 2
    try:
        curses.wrapper(run_menu)
    except (curses.error, OSError, ValueError) as error:
        print(f"menuconfig: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
