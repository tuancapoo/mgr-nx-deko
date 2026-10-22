"""Audit MGR's XDK 21173 reNut hooks against its generated PowerPC listings.

Reads only this new project's generated sources and the explicitly supplied image.
--write records the evidence and creates the runtime binary-version guard header.
No SA2 addresses are inferred by adding a fixed displacement.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BASE = 0x82000000
# (entry, role, instructions that establish the interface/layout)
MAP = [
    (0x826B2418, "Direct3D_CreateDevice", ["li r3,24704", "cmpwi cr6,r29,2", "stw r30,24392(r31)"]),
    (0x826C0C48, "InitializePresentationParameters", ["stw r3,15232(r29)", "stw r5,15236(r29)", "addi r3,r29,13948"]),
    (0x826C0278, "InitializeApiState", ["lwz r25,14000(r3)", "stw r23,13088(r31)", "stw r10,13084(r31)"]),
    (0x826C0930, "XenonGpu_Initialize", ["std r11,11008(r3)", "stw r3,11016(r31)", "stw r3,17136(r31)"]),
    (0x826C0EC8, "HardwareDeviceSetup_reference", ["addi r3,r3,15348", "addi r3,r31,15376", "stfs f13,21996(r31)"]),
    (0x826B5688, "KickOff", ["lwz r11,13600(r31)", "bl 0x826b5348", "lwz r3,48(r31)"]),
    (0x826B5348, "KickOffSegment", ["lwz r30,15332(r3)", "stw r10,48(r31)", "bl 0x826b45a8"]),
    (0x826B58C0, "BeginRingBig", ["rlwinm r30,r4,2,0,29", "bl 0x826b5688", "bl 0x826b45a8"]),
    (0x826B4078, "RingBufferDeviceAllocate", ["lwz r11,0(r5)", "stw r11,0(r29)", "lwz r11,15320(r3)"]),
    (0x826B44C0, "AllocateInlineData", ["rlwinm r11,r4,2,0,29", "lwz r3,13920(r31)", "bl 0x826b4078"]),
    (0x826B45A8, "NewSegment", ["stw r11,48(r31)", "stw r10,56(r31)", "stw r11,52(r31)"]),
    (0x826B5A08, "SetRingBufferParameters", ["bl 0x826b5938", "stw r30,15308(r31)"]),
    (0x826B42A8, "BlockOnFence", ["lwz r10,11036(r3)", "lwz r11,11024(r3)", "bl 0x826b1c88"]),
    (0x826B1C88, "CBlocker_Check", ["lwz r29,0(r3)", "lwz r11,11024(r29)", "lwz r11,11016(r29)"]),
    (0x826B57C8, "IsFencePending", ["lwz r11,11036(r31)", "bl 0x826b5688", "lwz r11,11024(r31)"]),
    (0x826B59B0, "InsertCallback", ["mr r30,r4", "mr r29,r5", "mr r28,r6", "bl 0x826b4d38"]),
    (0x826B3050, "SynchronizeToPresentationInterval", ["lwz r11,14000(r31)", "ori r11,r11,16", "stb r11,11069(r31)"]),
    (0x826B3510, "Swap_shared", ["lwz r11,16968(r31)", "stw r30,11056(r31)", "addi r26,r31,15408"]),
    (0x826BC928, "DrawVertices_shared", ["mr r23,r5", "bl 0x826cb580", "ori r11,r11,8705"]),
    (0x826BCD40, "DrawIndexedVertices_shared", ["bl 0x826cb580", "lwz r24,12812(r31)"]),
    (0x826BC468, "BeginVertices", ["mullw r9,r5,r6", "stb r10,12904(r3)", "stw r11,13844(r31)"]),
    (0x826BC260, "ClearF", ["lwz r7,12816(r3)", "lwz r11,12832(r3)", "bl 0x826bbbe0"]),
    (0x826BB0D0, "Resolve", ["lwz r30,260(r1)", "lwz r29,252(r1)", "bl 0x826b9f78"]),
    (0x826ACD00, "XGRegisterVertexShader", ["li r5,872", "stw r30,32(r31)", "lwz r11,872(r31)"]),
    (0x826AC3D8, "XGRegisterPixelShader", ["li r10,7", "stw r4,24(r3)"]),
    (0x826AC938, "CreateVertexDeclaration", ["cmplwi cr6,r9,255", "mulli r11,r11,12", "bl 0x826ac848"]),
    (0x826AAF80, "D3DResource_Destroy", ["clrlwi r10,r11,28", "bl 0x826b42a8", "bl 0x82715a90"]),
    (0x826B5FC8, "WindowScissor", ["mr r25,r4", "mr r24,r5", "mr r23,r6", "mr r22,r7", "ori r11,r11,8320"]),
    (0x826B2FC0, "EngineSwapProgress_reference", ["lwz r11,16968(r3)", "lwz r11,16976(r3)", "stw r11,8(r4)"]),
]


def listings(folder: Path) -> dict[int, list[str]]:
    found = {}
    for path in folder.glob("*_recomp.*.cpp"):
        text = path.read_text(encoding="utf-8", errors="replace")
        for match in re.finditer(r"^DEFINE_REX_FUNC\(sub_([0-9A-F]+)\) \{(.*?)(?=^DEFINE_REX_FUNC|\Z)", text, re.M | re.S):
            va = int(match[1], 16)
            if 0x82690000 <= va < 0x826D0000:
                found[va] = re.findall(r"^\s*// ([a-z][a-z0-9.]* .+|blr|bctrl|bctr|nop)$", match[2], re.M)
    return found


def fnv32(data: bytes) -> int:
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xFFFFFFFF
    return value


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gen", type=Path, default=ROOT / "app/generated/default")
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--write", action="store_true")
    parser.add_argument("--dump", type=lambda x: int(x, 0))
    args = parser.parse_args()
    bodies = listings(args.gen)
    if args.dump is not None:
        print("\n".join(bodies[args.dump]))
        return
    image = args.image.read_bytes()
    result = []
    for va, role, witnesses in MAP:
        body = bodies.get(va, [])
        missing = [instruction for instruction in witnesses if instruction not in body]
        if missing:
            raise SystemExit(f"{role} {va:08X}: instruction witnesses missing: {missing}")
        size = min(64, max(4, len(body) * 4))
        code = image[va - BASE:va - BASE + size]
        if len(code) != size:
            raise SystemExit(f"{role}: outside supplied decompressed image")
        result.append(dict(address=f"0x{va:08X}", role=role, witnesses=witnesses,
                           guard_bytes=size, fnv32=fnv32(code)))
    if args.write:
        report = ROOT / "docs/mgr-renut-hook-map.json"
        report.write_text(json.dumps(dict(image_sha256=hashlib.sha256(image).hexdigest(), hooks=result), indent=2) + "\n")
        header = ROOT / "app/src/renut_engine/gpu/d3d/mgr_hook_map.h"
        lines = ["// Generated by tools/mgr_renut_hooks.py from MGR's original image.", "#pragma once", "#include <cstdint>",
                 "namespace renut::gpu::mgr {", "struct GuestCodeGuard { uint32_t address, bytes, fnv32; const char* name; };",
                 "inline constexpr GuestCodeGuard kGuestCodeGuards[] = {"]
        for item in result:
            lines.append(f'  {{{item["address"]}u, {item["guard_bytes"]}u, 0x{item["fnv32"]:08X}u, "{item["role"]}"}},')
        lines += ["};", "}  // namespace renut::gpu::mgr", ""]
        header.write_text("\n".join(lines))
    print(f"PASS: {len(result)} MGR interfaces have PowerPC instruction witnesses; image SHA256 {hashlib.sha256(image).hexdigest()}")


if __name__ == "__main__":
    main()
