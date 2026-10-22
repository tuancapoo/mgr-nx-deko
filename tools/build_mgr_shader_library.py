"""Build the MGR shader library with native Windows tools, preserving previous builds.

Run from the Windows compiler environment (tools/win/env.ps1). Output must be new.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import shutil
import subprocess


def main():
    root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("inputs", type=Path, nargs="+")
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--jobs", type=int, default=6)
    parser.add_argument("--base", type=Path, help="Reuse compiled shaders and tools from a previous build")
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    for directory in ("in", "hlsl", "spirv", "bin", "tmp", "in_ok"):
        (out / directory).mkdir()
    xenos = root / "shaders/XenosRecomp"
    sdk = root / "sdk"
    cxx = root.parent / "_tools/llvm/bin/clang++.exe"
    dxc = root.parent / "_tools/dxc/bin/x64/dxc.exe"

    def run(command, log):
        with log.open("w", encoding="utf-8") as stream:
            try:
                return subprocess.run([str(a) for a in command], stdout=stream,
                                      stderr=subprocess.STDOUT, timeout=300).returncode == 0
            except subprocess.TimeoutExpired:
                stream.write("\nshader tool timed out after 300 seconds\n")
                return False

    common_flags = [cxx, "-std=c++20", "-O2", "-w", "-DNOMINMAX"]
    translator = out / "bin/xenos_hlsl.exe"
    packer = out / "bin/me_pack_shaders.exe"
    commands = [
        (common_flags + ["-fms-extensions", "-include", xenos / "pch_min.h",
         "-I" + str(root / "shaders"), "-I" + str(xenos),
         "-I" + str(sdk / "thirdparty/fmt/include"), "-I" + str(sdk / "thirdparty/xxHash"),
         "-DFMT_HEADER_ONLY", "-DXXH_INLINE_ALL", "-DMASSEFFECT_RECOMP",
         root / "shaders/tools/masseffect_hlsl.cpp", xenos / "shader_recompiler.cpp",
         "-o", translator], out / "build_translator.log"),
        (common_flags + ["-I" + str(sdk / "thirdparty/xxHash"),
         "-I" + str(root / "app/src/me/native"), root / "shaders/tools/me_pack_shaders.cpp",
         root / "app/src/me/native/sa2_shader_library.cpp", "-o", packer], out / "build_packer.log"),
    ]
    reused = set()
    if args.base:
        base = args.base.resolve()
        for directory, pattern in (("bin", "*.exe"), ("in_ok", "*.bin"), ("spirv", "*.spv")):
            for source in (base / directory).glob(pattern):
                shutil.copy2(source, out / directory / source.name)
                if directory == "in_ok":
                    reused.add(source.name)
        if not translator.is_file() or not packer.is_file():
            raise RuntimeError("base build has no compiled translator or packer")
    else:
        with ThreadPoolExecutor(max_workers=2) as pool:
            if not all(pool.map(lambda item: run(*item), commands)):
                raise RuntimeError(f"tool build failed; see {out}/build_*.log")
    for directory in args.inputs:
        for source in sorted(directory.glob("*.bin")):
            name = source.name
            if name.startswith("v_"):
                name = "vs_i" + name[2:]
            elif name.startswith("p_"):
                name = "ps_i" + name[2:]
            elif not name.startswith(("vs_", "ps_")):
                continue
            if name in reused:
                if source.read_bytes() != (out / "in_ok" / name).read_bytes():
                    raise RuntimeError(f"different containers share the name {name}")
                continue
            shutil.copy2(source, out / "in" / name)
    sources = sorted((out / "in").glob("*.bin"))
    print(f"containers={len(sources)} reused={len(reused)}", flush=True)

    def translate(source):
        temp = out / "tmp" / source.stem
        (temp / "in").mkdir(parents=True)
        shutil.copy2(source, temp / "in" / source.name)
        log = temp / "translate.log"
        ok = run([translator, temp / "in", temp / "out", xenos / "shader_common.h"], log)
        hlsl = temp / "out" / (source.stem + ".hlsl")
        if ok and hlsl.exists():
            shutil.copy2(hlsl, out / "hlsl" / hlsl.name)
            return source
        return None

    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        translated = [source for source in pool.map(translate, sources) if source]
    failed_translation = [p.name for p in sources if p not in translated]
    (out / "translate_failed.txt").write_text("\n".join(failed_translation), encoding="utf-8")
    print(f"translated={len(translated)} failed={len(failed_translation)}", flush=True)

    def compile_shader(source):
        pixel = source.name.startswith("ps_")
        spv = out / "spirv" / (source.stem + ".spv")
        cmd = [dxc, "-spirv", "-T", "ps_6_6" if pixel else "vs_6_6", "-E", "main",
               "-HV", "2021", "-fspv-target-env=vulkan1.2", "-fvk-use-dx-layout",
               "-Werror=parameter-usage"]
        if not pixel:
            cmd.append("-fvk-invert-y")
        cmd += ["-Fo", spv, out / "hlsl" / (source.stem + ".hlsl")]
        if run(cmd, out / "spirv" / (source.stem + ".log")) and spv.exists():
            shutil.copy2(source, out / "in_ok" / source.name)
            return source
        return None

    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        compiled = [source for source in pool.map(compile_shader, translated) if source]
    print(f"compiled={len(compiled)} failed={len(translated) - len(compiled)}", flush=True)
    if not compiled or not run([packer, out / "in_ok", out / "spirv", out / "sa2_shaders.mesp"], out / "pack.log"):
        raise RuntimeError(f"packing failed; see {out}/pack.log")
    print((out / "pack.log").read_text(), flush=True)
    print(f"package={out / 'sa2_shaders.mesp'}", flush=True)


if __name__ == "__main__":
    main()
