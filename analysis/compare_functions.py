"""Read-only comparison of the original IDB and a Steam PE executable.

Dependencies can be installed locally with:
  python -m pip install --target analysis/vendor python-idb capstone pefile
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import sys

sys.path.insert(0, str(Path(__file__).parent / "vendor"))
import capstone
import idb
import pefile


def signature(code, address, image_range):
    """Mask relocated addresses and control-flow targets, retain other bytes."""
    dis = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    dis.detail = True
    mask = bytearray(b"\xff" * len(code))
    end = 0
    for ins in dis.disasm(code, address):
        offset = ins.address - address
        end = offset + ins.size
        control = ins.group(capstone.CS_GRP_CALL) or ins.group(capstone.CS_GRP_JUMP)
        for operand in ins.operands:
            if operand.type == capstone.x86.X86_OP_IMM:
                absolute = image_range[0] <= operand.imm < image_range[1]
                if (control or absolute) and ins.imm_size:
                    start = offset + ins.imm_offset
                    mask[start:start + ins.imm_size] = b"\0" * ins.imm_size
            elif operand.type == capstone.x86.X86_OP_MEM:
                if image_range[0] <= operand.mem.disp < image_range[1] and ins.disp_size:
                    start = offset + ins.disp_offset
                    mask[start:start + ins.disp_size] = b"\0" * ins.disp_size
    code, mask = code[:end], mask[:end]
    pattern = b"".join(re.escape(bytes([byte])) if keep else b"." for byte, keep in zip(code, mask))
    return code, bytes(mask), re.compile(pattern, re.DOTALL)


def disassemble(code, address, count=12):
    dis = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    return [f"{ins.address:08X}: {ins.mnemonic} {ins.op_str}".rstrip()
            for _, ins in zip(range(count), dis.disasm(code, address))]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("exe", type=Path)
    parser.add_argument("--idb", type=Path, default=Path("AlienShooter.exe.idb"))
    parser.add_argument("--addresses", type=Path, default=Path("asmp-dll/src/game/addresses.h"))
    parser.add_argument("--output", type=Path, default=Path("analysis/comparison.json"))
    args = parser.parse_args()
    pe = pefile.PE(str(args.exe))
    image_base = pe.OPTIONAL_HEADER.ImageBase
    sections = [(image_base + s.VirtualAddress, s.get_data())
                for s in pe.sections if s.Characteristics & 0x20000000]
    targets = [(name, int(addr, 16)) for name, addr in re.findall(
        r"^#define\s+(FUNC_\w+)\s+(0x[\da-fA-F]+)", args.addresses.read_text(), re.M)]
    rows = []
    with idb.from_file(str(args.idb)) as db:
        api = idb.IDAPython(db)
        segments = list(idb.analysis.Segments(db).segments.values())
        old_range = min(s.startEA for s in segments), max(s.endEA for s in segments)
        for define, old in targets:
            try:
                name = api.idc.GetFunctionName(old)
                end = api.idc.GetFunctionAttr(old, api.idc.FUNCATTR_END)
                if end <= old:
                    raise ValueError("No function bounds")
                length = min(end - old, 160)
                code = api.idc.GetManyBytes(old, length)
                code, mask, pattern = signature(code, old, old_range)
                if sum(bool(x) for x in mask) < 4:
                    raise ValueError("Too few fixed bytes")
                matches = []
                for base, data in sections:
                    for match in pattern.finditer(data):
                        matches.append(base + match.start())
                rows.append({"define": define, "name": name, "old": old,
                             "function_size": end - old, "signature_size": len(code),
                             "fixed_bytes": sum(bool(x) for x in mask), "matches": matches,
                             "delta": matches[0] - old if len(matches) == 1 else None,
                             "old_disassembly": disassemble(code, old),
                             "new_disassembly": disassemble(pe.get_data(matches[0] - image_base, len(code)), matches[0])
                             if len(matches) == 1 else [],
                             "signature_hex": " ".join(f"{b:02X}" if m else "??" for b,m in zip(code,mask))})
            except Exception as error:
                rows.append({"define": define, "old": old, "error": str(error)})
    report = {"exe": str(args.exe.resolve()), "sha256": hashlib.sha256(args.exe.read_bytes()).hexdigest(),
              "method": "Search executable sections for the first up to 160 instruction bytes; mask control-flow targets and absolute image addresses. Unique hits are static candidates, not runtime validation.",
              "rows": rows}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding="utf-8")
    unique = [r for r in rows if r.get("delta") is not None]
    print(f"Functions: {len(rows)}, unique candidates: {len(unique)}")
    for row in rows:
        hits = row.get("matches", [])
        if hits:
            locations = ', '.join(f'{x:08X}' for x in hits[:8])
            delta = f"{row['delta']:+#x}" if row.get("delta") is not None else "ambiguous"
            print(f"{row['define']}: {row['old']:08X} -> {locations} ({delta}), signature={row['signature_size']}")
    print(f"Saved {args.output}")


if __name__ == "__main__":
    main()
