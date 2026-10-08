"""Export read-only static evidence for the analyzed Steam build.

This is an analysis profile, not addresses validated for DLL injection.
Run from the repository root after installing analysis/vendor dependencies.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import sys

sys.path.insert(0, str(Path(__file__).parent / "vendor"))
import capstone
import idb
import pefile

EXPECTED_SHA256 = "4dd960458d6fffcc9d00e9e7ba492739fb6d530d4c0b302f1c6baa8b55d9b142"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("exe", type=Path)
    parser.add_argument("--output", type=Path, default=Path("analysis/steam-hooks.json"))
    args = parser.parse_args()
    raw = args.exe.read_bytes()
    digest = hashlib.sha256(raw).hexdigest()
    if digest != EXPECTED_SHA256:
        raise SystemExit("Different EXE: this static profile must be re-established for its hash.")
    pe = pefile.PE(data=raw)
    image_base = pe.OPTIONAL_HEADER.ImageBase
    dis = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)

    def read(va, size):
        return pe.get_data(va - image_base, size)

    def refs(value):
        return [image_base + pe.get_rva_from_offset(match.start())
                for match in re.finditer(re.escape(struct.pack("<I", value)), raw)]

    def is_code(va):
        return any(s.Characteristics & 0x20000000
                   and image_base + s.VirtualAddress <= va
                   < image_base + s.VirtualAddress + s.Misc_VirtualSize
                   for s in pe.sections)

    def rtti(name, slot_count):
        marker = (".?AV" + name + "@@\0").encode("ascii")
        position = raw.index(marker)
        descriptor = image_base + pe.get_rva_from_offset(position) - 8
        found = []
        for ref in refs(descriptor):
            col = ref - 12
            values = struct.unpack("<5I", read(col, 20))
            if values[:3] != (0, 0, 0):
                continue
            for reference in refs(col):
                table = reference + 4
                slots = struct.unpack("<" + "I" * slot_count, read(table, slot_count * 4))
                if all(is_code(value) for value in slots):
                    found.append({"class": name, "type_descriptor": descriptor,
                                  "complete_object_locator": col, "vtable": table,
                                  "slots": list(slots)})
        if len(found) != 1:
            raise ValueError(f"Expected one primary vtable for {name}; found {len(found)}")
        return found[0]

    tables = {name: rtti(name, size) for name, size in
              (("MAP", 9), ("MAP_STEAM", 9), ("MAN", 8), ("UNIT", 8), ("SPRITE", 8))}
    rows = []
    candidates = [
        ("GAME_PTR_PTR", 0x490738, 0x522C20, "WinMain stores the constructed MAP_STEAM object here at 0x4342DD."),
        ("RENDER_PTR_PTR", 0x4B2C64, 0x502AD4, "Menu positioning and coordinate attack use renderer dimensions and buffer fields through this pointer."),
        ("TIME_MS_PTR", 0x490604, 0x522C64, "The matching frame-time helper loads/stores timeGetTime results here."),
        ("GAME_VTBL (base MAP)", 0x47A308, tables["MAP"]["vtable"], "RTTI MAP primary vtable; base tick slot is an empty return-zero function."),
        ("Game runtime vtable (MAP_STEAM)", 0x47A2A4, tables["MAP_STEAM"]["vtable"], "RTTI MAP_STEAM; constructor 0x4404B0 writes this vtable."),
        ("ENT_PLAYER_VTBL", 0x47AB00, tables["MAN"]["vtable"], "RTTI MAN primary vtable; slot 1 is the matching player-action dispatcher."),
        ("FUNC_WINMAIN", 0x404F00, 0x434260, "Allocates 0x22C8 bytes, calls MAP_STEAM constructor, stores game pointer, then enters the loop."),
        ("FUNC_GAME_CONSTRUCTOR", 0x405060, 0x4404B0, "Calls base initializer with five arguments and sets the MAP_STEAM vtable."),
        ("FUNC_GAME_INIT", 0x408FF0, 0x435180, "Base initialization called by both corresponding derived constructors; RTTI corroborates class hierarchy."),
        ("FUNC_GAME_TICK", 0x405190, tables["MAP_STEAM"]["slots"][3], "Runtime vtable slot 3; same early-return helper and keyboard switch range 0x47..0x7E."),
        ("Game frame-time helper", 0x40F210, 0x43FCD0, "timeGetTime timing loop, delta calculation and 0x47 clamp; called at the beginning of tick."),
        ("FUNC_GAME_WNDPROC", 0x40B930, tables["MAP"]["slots"][1], "Base and derived vtable slot 1; same game flag and window-message dispatch."),
        ("FUNC_GAME_LOAD_MAP", 0x40C960, tables["MAP"]["slots"][6], "Same slot 6 in both MAP and MAP_STEAM. Candidate primarily identified by vtable correspondence; body needs further comparison."),
        ("FUNC_GAME_CREATE_ENTITY", 0x405BD0, tables["MAP_STEAM"]["slots"][8], "Derived slot 8; VID class switch and MAN constructor arm, accepts six arguments."),
        ("FUNC_GAME_GET_ARMY_PLAYER", 0x40E620, 0x43B5F0, "Four-army index mask, army array +0x244 and player pointer +0x10; the full 23-byte accessor is used as the diagnostic in-memory signature."),
        ("FUNC_ENT_PLAYER_ACTION", 0x454800, tables["MAN"]["slots"][1], "Same action switch bounds 0x25..0x82 and ammo cases 0x5C/0x5D; default delegates to UNIT slot 1."),
        ("FUNC_ENT_UNIT_ACTION (additional)", 0x447500, tables["UNIT"]["slots"][1], "RTTI UNIT slot 1 and the direct default-dispatch target from MAN::action."),
        ("FUNC_ENT_PLAYER_SET_ARMED_WEAPON", 0x455070, 0x434BA0, "Same VID child check <=0x14, slot 10 to 0 conversion, item lookup slot+0x104 and weapon-child update."),
        ("FUNC_ENTITY_SET_ANIM", 0x442EE0, 0x46B970, "Animation limit 0..16, matching child propagation/frame calculation and ChangeAnimation error message."),
    ]
    with idb.from_file("AlienShooter.exe.idb") as db:
        api = idb.IDAPython(db)
        for name, old, new, evidence in candidates:
            row = {"name": name, "old": old, "steam": new, "evidence": evidence,
                   "status": "static candidate; no runtime validation"}
            if is_code(new):
                try:
                    end = api.idc.GetFunctionAttr(old, api.idc.FUNCATTR_END)
                    old_code = api.idc.GetManyBytes(old, min(end - old, 500))
                    row["old_disassembly"] = [f"{i.address:08X}: {i.mnemonic} {i.op_str}".rstrip()
                                              for _, i in zip(range(65), dis.disasm(old_code, old))]
                except (KeyError, ValueError):
                    pass
                row["steam_disassembly"] = [f"{i.address:08X}: {i.mnemonic} {i.op_str}".rstrip()
                                            for _, i in zip(range(65), dis.disasm(read(new, 500), new))]
            rows.append(row)
        action_cases = []
        for action in (0x25, 0x5C, 0x5D, 0x82):
            old_index = api.idc.GetManyBytes(0x455010 + action - 0x25, 1)[0]
            old_target = struct.unpack("<I", api.idc.GetManyBytes(0x454FF0 + old_index * 4, 4))[0]
            new_index = read(0x434B0C + action - 0x25, 1)[0]
            new_target = struct.unpack("<I", read(0x434AEC + new_index * 4, 4))[0]
            action_cases.append({"action": action, "old_target": old_target, "steam_target": new_target,
                                 "old_disassembly": [f"{i.address:08X}: {i.mnemonic} {i.op_str}".rstrip()
                                                     for _, i in zip(range(30), dis.disasm(api.idc.GetManyBytes(old_target, 150), old_target))],
                                 "steam_disassembly": [f"{i.address:08X}: {i.mnemonic} {i.op_str}".rstrip()
                                                       for _, i in zip(range(30), dis.disasm(read(new_target, 150), new_target))]})
    imports = {entry.dll.decode("ascii"): [imp.name.decode("ascii") if imp.name else imp.ordinal for imp in entry.imports]
               for entry in pe.DIRECTORY_ENTRY_IMPORT if b"d3d" in entry.dll.lower()}
    report = {"exe": str(args.exe.resolve()), "sha256": digest, "rtti": tables,
              "candidates": rows, "player_action_cases": action_cases, "graphics_imports": imports,
              "warning": "Do not inject the existing DLL or mechanically substitute these addresses: object layouts, menu signature and graphics API differ."}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding="utf-8")
    for row in rows:
        print(f"{row['name']}: {row['old']:08X} -> {row['steam']:08X}")
    print("Graphics imports:", imports)
    print("Saved", args.output)


if __name__ == "__main__":
    main()
