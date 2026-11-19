#!/usr/bin/env python3
"""Generate main/cloudzao_endpoints.local.c from an official Metalio firmware image.

The open-source tree ships main/cloudzao_endpoints.c with every service address
blank, so 百问, checklist, weather, push, app store and WorkBuddy cannot reach
the official cloud. The official images on burner.cloudzao.com are full builds.
This script reads the endpoint constants out of one and writes them into a
local, git-ignored source file; main/CMakeLists.txt compiles that file instead
of the blank one when it exists. No value is stored in the repository.

    python3 tools/official/extract_endpoints.py <official-image.bin>

The image may be a full 16 MB flash image (the app is read from ota_0 at
0x80000) or a bare app image. Mapping works by declaration order: the linker
lays the non-empty constants out contiguously, in reverse declaration order,
ending at the app-store URL. Names left blank by the official build are listed
in OFFICIAL_BLANK. Every assigned value is checked against its variable name,
so a layout change in a future release fails loudly instead of writing a
wrong address.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TEMPLATE = ROOT / "main/cloudzao_endpoints.c"
OUTPUT = ROOT / "main/cloudzao_endpoints.local.c"

# Blank in the official domestic build as well (verified on 2.0.81).
OFFICIAL_BLANK = {
    "yunxin_binding_url",
    "cloudzao_book_tools_url",
    "cloudzao_path_sinicloud_token",
    "cloudzao_path_asr_transcribe",
    "cloudzao_path_asr_audio_records",
    "cloudzao_path_library_sync",
    "cloudzao_path_device_location_wifi",
}

# (substring of the variable name, check on the value)
CHECKS = [
    ("_url", lambda v: re.match(r"^(https?|wss?)://", v)),
    ("_host", lambda v: re.match(r"^https?://[^/]+(/[a-z]+)?$", v)),
    ("checklist_items_all", lambda v: v.endswith("/items/all")),
    ("checklist_item_prefix", lambda v: v.endswith("/items/")),
    ("complete_suffix", lambda v: v.startswith("/") and "complete" in v),
    ("weather", lambda v: "weather" in v),
    ("settings_tts", lambda v: v.endswith("/tts")),
    ("push_resources_next", lambda v: v.endswith("/next")),
    ("push_resources", lambda v: "push-resources" in v),
    ("vision_upload", lambda v: "vision" in v or v.startswith("http")),
    ("device_logs", lambda v: v.endswith("/logs")),
    ("workbuddy_authorization_sessions", lambda v: v.endswith("authorization-sessions")),
    ("workbuddy_authorization", lambda v: v.endswith("/authorization")),
    ("workbuddy_text_turn", lambda v: v.endswith("/turn/text")),
    ("workbuddy_asr_stream", lambda v: "/asr/stream" in v),
    ("workbuddy_local_messages", lambda v: v.endswith("/message")),
    ("workbuddy_local_status", lambda v: v.endswith("/localassistant")),
    ("workbuddy_local_reply", lambda v: v.endswith("/reply")),
    ("app_key", lambda v: re.fullmatch(r"[0-9a-f]{32}", v)),
    ("token", lambda v: re.fullmatch(r"[\x21-\x7e]+", v)),
]


def declarations():
    """Variable names in the order main/cloudzao_endpoints.c defines them (domestic branch)."""
    text = TEMPLATE.read_text()
    text = re.sub(r"#if CONFIG_BOARD_TYPE_METALIO_E_INK4_PLUS_GLOBAL.*?#else", "", text, flags=re.S)
    return re.findall(r'^const char (\w+)\[\] = "";', text, flags=re.M)


def app_image(blob):
    if len(blob) >= 0x580000 and blob[0x80000] == 0xE9:
        return blob[0x80000:0x580000]
    if blob[:1] == b"\xE9":
        return blob
    sys.exit("not an ESP firmware image")


def strings_below(app, anchor_end):
    """Collect NUL-terminated strings packed directly below anchor_end, highest first."""
    found, pos = [], anchor_end
    while True:
        end = pos - 1
        while end > 0 and app[end - 1] == 0 and pos - end < 8:
            end -= 1                     # alignment padding
        if app[end] != 0:
            break
        start = end
        while start > 0 and 0x20 <= app[start - 1] < 0x7F:
            start -= 1
        if start == end:
            break
        found.append(app[start:end].decode())
        pos = start
    return found


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    app = app_image(Path(sys.argv[1]).read_bytes())
    anchors = [m for m in re.finditer(rb"https://[a-z0-9.]+/[a-z0-9/]*/device/app-store\0", app)]
    if len(anchors) != 1:
        sys.exit(f"expected one app-store URL, found {len(anchors)}")
    anchor = anchors[0]
    values = [anchor.group()[:-1].decode()] + strings_below(app, anchor.start())

    names = [n for n in declarations()]
    filled = [n for n in names if n not in OFFICIAL_BLANK]
    if len(values) < len(filled):
        sys.exit(f"found {len(values)} strings, need {len(filled)}")
    mapping = dict(zip(filled, values))
    # A path is packed right after other data; drop any printable bytes that
    # belong to the preceding word (e.g. the tail of a pointer).
    for name, value in mapping.items():
        if name.startswith("cloudzao_path_") and not value.startswith("/") and "/" in value:
            mapping[name] = value[value.index("/"):]

    for name, value in mapping.items():
        if name.startswith("cloudzao_path_") and not value.startswith("/"):
            sys.exit(f"layout changed: {name} is not a path")
        for key, check in CHECKS:
            if key in name:
                if not check(value):
                    sys.exit(f"layout changed: {name} does not look right")
                break
        else:
            sys.exit(f"no check for {name}")

    # Fill only the domestic build; the international branch keeps its blanks.
    text = TEMPLATE.read_text()
    head, sep, rest = text.partition("#if CONFIG_BOARD_TYPE_METALIO_E_INK4_PLUS_GLOBAL")
    global_part, sep2, tail = rest.partition("#else")
    if not sep or not sep2:
        sys.exit("template layout changed")
    def fill(part):
        for name, value in mapping.items():
            part = re.sub(rf'^(\s*const char {name}\[\] = )"";', lambda m: m.group(1) + '"' + value + '";',
                          part, flags=re.M)
        return part
    text = fill(head) + sep + global_part + sep2 + fill(tail)
    header = ("// Generated by tools/official/extract_endpoints.py from an official image.\n"
              "// Local only: git-ignored, never commit.\n")
    OUTPUT.write_text(header + text)
    print(f"wrote {OUTPUT.relative_to(ROOT)}: {len(mapping)} endpoints, "
          f"{len(OFFICIAL_BLANK)} blank as in the official build")


if __name__ == "__main__":
    main()
