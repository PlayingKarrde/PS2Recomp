#!/usr/bin/env python3
"""Compile the HLSL shaders in this folder into C++ headers that hold the compiled bytes.

Why this exists. SDL_GPU (the graphics API the GPU renderer uses) does not read HLSL. It wants
compiled shaders in the format of the graphics driver it runs on: SPIR-V for Vulkan, DXIL for
Direct3D 12 and Metal Shading Language (MSL) source for Metal. We write each shader once in HLSL,
and this script compiles it three times. The results are committed to the repository as headers in
`compiled/`, so building the game or the tests never needs a shader compiler. Run this script only
when you change a `.hlsl` file, then commit the changed headers together with it.

What it does, for every `<name>.vert.hlsl` and `<name>.frag.hlsl` in this folder:
  1. DXIL:   Windows SDK `dxc -T vs_6_0|ps_6_0 -E main`. The Windows SDK build ships `dxil.dll`, which
             signs the result; Direct3D 12 refuses unsigned DXIL. That build cannot write SPIR-V.
  2. SPIR-V: Vulkan SDK `dxc -spirv -fspv-target-env=vulkan1.0 -T ... -E main`.
  3. MSL:    Vulkan SDK `spirv-cross --msl --msl-version 20100` applied to the SPIR-V from step 2.
             spirv-cross renames the entry point `main` to `main0`; the script reads the real name
             from the generated source and stores it, because SDL_CreateGPUShader must be told it.
  4. Writes `compiled/<name>_<stage>.h` (for example `smoke_vert.h`) holding the three byte arrays,
     the MSL entry-point name, the resource counts and the versions of the tools used.

Each shader declares its SDL_GPU resource counts (SDL_CreateGPUShader needs them and cannot read
them from the bytecode) in a comment block near the top of its `.hlsl` file:

    // @samplers 1
    // @storage_textures 0
    // @storage_buffers 0
    // @uniform_buffers 0

The register spaces to use in the HLSL are fixed by SDL_gpu.h (see the comment on
SDL_CreateGPUShader): vertex shaders use space0 for textures/storage and space1 for uniform
buffers, fragment shaders use space2 and space3. dxc turns "spaceN" into SPIR-V descriptor set N, so
the same source satisfies DXIL and SPIR-V.

The output is byte-for-byte reproducible (no timestamps or paths), which is what `--check` relies on:
it recompiles everything into a temporary folder and exits with status 1 if any committed header
differs, is missing, or has no matching shader. Status 2 means a tool is missing or failed.

Uses the Python standard library only.

Examples:
    python build_shaders.py            # rebuild compiled/*.h
    python build_shaders.py --check    # verify the committed headers are up to date
"""

import argparse
import glob
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))

# stage suffix in the file name -> (dxc target profile, MSL keyword, C++ enum name)
STAGES = {
    "vert": ("vs_6_0", "vertex", "Vertex"),
    "frag": ("ps_6_0", "fragment", "Fragment"),
}

COUNT_KEYS = ("samplers", "storage_textures", "storage_buffers", "uniform_buffers")

# Defaults named in the task brief; used only when the file exists, otherwise PATH is searched.
DEFAULT_WINDOWS_SDK_DXC = r"C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\dxc.exe"
DEFAULT_VULKAN_SDK_ROOT = r"C:\VulkanSDK\1.4.363.0"

MSL_VERSION = "20100"  # MSL 2.1, the oldest version SDL's Metal backend targets (macOS 10.14)


class ToolError(Exception):
    pass


def exe(name):
    return name + (".exe" if os.name == "nt" else "")


def find_tool(explicit, candidates, path_name):
    """Explicit argument, then the first existing candidate, then PATH."""
    if explicit:
        if not os.path.isfile(explicit):
            raise ToolError(f"{explicit}: not found")
        return explicit
    for c in candidates:
        if c and os.path.isfile(c):
            return c
    found = shutil.which(path_name)
    if found:
        return found
    raise ToolError(f"{path_name} not found (searched {', '.join(c for c in candidates if c)} and PATH); pass it explicitly")


def vulkan_sdk_candidates(tool):
    roots = [os.environ.get("VULKAN_SDK"), DEFAULT_VULKAN_SDK_ROOT]
    out = []
    for r in roots:
        if r:
            out.append(os.path.join(r, "Bin", exe(tool)))
            out.append(os.path.join(r, "bin", exe(tool)))
    return out


def run(cmd, cwd):
    proc = subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, errors="replace")
    if proc.returncode != 0:
        raise ToolError(f"command failed ({proc.returncode}): {' '.join(cmd)}\n{proc.stdout}{proc.stderr}")
    return proc.stdout + proc.stderr


def tool_version(cmd):
    """First useful line of the tool's version output, flattened to one line."""
    proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace")
    lines = [ln.strip() for ln in proc.stdout.splitlines() if ln.strip()]
    if not lines:
        raise ToolError(f"{' '.join(cmd)}: no version output")
    return " ".join(lines)


def parse_counts(path):
    counts = {}
    pattern = re.compile(r"^\s*//\s*@(\w+)\s+(\d+)\s*$")
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            m = pattern.match(line)
            if m:
                if m.group(1) not in COUNT_KEYS:
                    raise ToolError(f"{path}: unknown resource key @{m.group(1)}")
                counts[m.group(1)] = int(m.group(2))
    missing = [k for k in COUNT_KEYS if k not in counts]
    if missing:
        raise ToolError(f"{path}: missing '// @key N' lines for: {', '.join(missing)}")
    return counts


def hex_array(name, data):
    lines = [f"inline constexpr unsigned char {name}[{len(data)}] = {{"]
    for i in range(0, len(data), 12):
        chunk = data[i:i + 12]
        lines.append("    " + ", ".join(f"0x{b:02x}" for b in chunk) + ",")
    lines.append("};")
    return "\n".join(lines)


def check_dxil_signed(data, label):
    # A DXIL container is "DXBC" + a 16 byte digest. dxc fills the digest in when dxil.dll signs it.
    if data[:4] != b"DXBC":
        raise ToolError(f"{label}: not a DXIL container")
    if data[4:20] == bytes(16):
        raise ToolError(f"{label}: DXIL is unsigned (dxil.dll was not found next to dxc); D3D12 would reject it")


def find_msl_entry(msl_text, keyword, label):
    m = re.search(r"^" + keyword + r"\s+\w+\s+(\w+)\s*\(", msl_text, re.M)
    if not m:
        raise ToolError(f"{label}: no '{keyword}' entry point found in the MSL")
    return m.group(1)


def compile_shader(src, stage, tools, versions, work):
    """Compile one shader. Returns (spirv, dxil, msl_bytes, msl_entry, commands)."""
    profile, msl_keyword, _ = STAGES[stage]
    base = os.path.basename(src)  # e.g. smoke.vert.hlsl
    stem = base[:-len(".hlsl")]
    name = stem.replace(".", "_")
    # Work in a private folder with short relative names so no absolute path can leak into the output.
    shutil.copyfile(src, os.path.join(work, base))
    for inc in glob.glob(os.path.join(os.path.dirname(src), "*.hlsli")):
        shutil.copyfile(inc, os.path.join(work, os.path.basename(inc)))

    dxil_cmd = [tools["dxc_dxil"], "-T", profile, "-E", "main", "-Fo", name + ".dxil", base]
    spv_cmd = [tools["dxc_spirv"], "-spirv", "-fspv-target-env=vulkan1.0", "-T", profile, "-E", "main",
               "-Fo", name + ".spv", base]
    msl_cmd = [tools["spirv_cross"], "--msl", "--msl-version", MSL_VERSION, name + ".spv", "--output", name + ".msl"]

    run(dxil_cmd, work)
    run(spv_cmd, work)
    if tools.get("spirv_val"):
        run([tools["spirv_val"], "--target-env", "vulkan1.0", name + ".spv"], work)
    run(msl_cmd, work)

    with open(os.path.join(work, name + ".dxil"), "rb") as f:
        dxil = f.read()
    with open(os.path.join(work, name + ".spv"), "rb") as f:
        spirv = f.read()
    with open(os.path.join(work, name + ".msl"), "rb") as f:
        # spirv-cross writes text, so on Windows the lines end in CRLF. Store LF so the blob is the same everywhere.
        msl = f.read().replace(b"\r\n", b"\n")
    check_dxil_signed(dxil, base)
    if spirv[:4] != bytes([0x03, 0x02, 0x23, 0x07]):
        raise ToolError(f"{base}: SPIR-V magic number missing")
    entry = find_msl_entry(msl.decode("utf-8"), msl_keyword, base)

    shown = []
    for cmd in (dxil_cmd, spv_cmd, msl_cmd):
        shown.append(" ".join([os.path.basename(cmd[0]).replace(".exe", "")] + cmd[1:]))
    return spirv, dxil, msl, entry, shown


def header_text(src_base, stage, name, counts, blobs, entry, commands, versions):
    spirv, dxil, msl = blobs
    _, _, stage_enum = STAGES[stage]
    lines = [
        f"// Generated by ps2xRuntime/shaders/build_shaders.py from {src_base}. Do not edit; edit the .hlsl and rerun the script.",
        "//",
        "// Tools:",
        f"//   DXIL   : {versions['dxc_dxil']}",
        f"//   SPIR-V : {versions['dxc_spirv']}",
        f"//   MSL    : {versions['spirv_cross']} (entry point renamed by spirv-cross to \"{entry}\")",
        "// Commands (run in a folder holding only the source):",
    ]
    lines += [f"//   {c}" for c in commands]
    lines += [
        "",
        "#pragma once",
        "",
        '#include "gpu/gpu_shaders.h"',
        "",
        "namespace ps2x::gpu::shaders",
        "{",
        hex_array(f"{name}_spirv", spirv),
        "",
        hex_array(f"{name}_dxil", dxil),
        "",
        hex_array(f"{name}_msl", msl),
        "",
        f"inline constexpr CompiledShader {name} = {{",
        f'    "{src_base}",',
        f"    ShaderStage::{stage_enum},",
        f"    {{{name}_spirv, sizeof({name}_spirv)}},",
        f"    {{{name}_dxil, sizeof({name}_dxil)}},",
        f"    {{{name}_msl, sizeof({name}_msl)}},",
        f'    "{entry}",',
        f"    {counts['samplers']}, // samplers",
        f"    {counts['storage_textures']}, // storage textures",
        f"    {counts['storage_buffers']}, // storage buffers",
        f"    {counts['uniform_buffers']}, // uniform buffers",
        "};",
        "} // namespace ps2x::gpu::shaders",
        "",
    ]
    return "\n".join(lines)


def discover(shader_dir):
    found = []
    for stage in STAGES:
        for path in sorted(glob.glob(os.path.join(shader_dir, f"*.{stage}.hlsl"))):
            found.append((path, stage))
    return found


def build_all(shader_dir, out_dir, tools, versions):
    """Write every header into out_dir; returns the list of header file names written."""
    os.makedirs(out_dir, exist_ok=True)
    written = []
    for path, stage in discover(shader_dir):
        base = os.path.basename(path)
        stem = base[:-len(".hlsl")]
        name = stem.replace(".", "_")
        counts = parse_counts(path)
        with tempfile.TemporaryDirectory(prefix="ps2x_shader_") as work:
            spirv, dxil, msl, entry, commands = compile_shader(path, stage, tools, versions, work)
        text = header_text(base, stage, name, counts, (spirv, dxil, msl), entry, commands, versions)
        out_name = name + ".h"
        with open(os.path.join(out_dir, out_name), "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        written.append(out_name)
        print(f"{base}: SPIR-V {len(spirv)} B, DXIL {len(dxil)} B, MSL {len(msl)} B, MSL entry {entry} -> {out_name}")
    return written


def read_normalised(path):
    with open(path, "rb") as f:
        return f.read().replace(b"\r\n", b"\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dxc-dxil", help="dxc that writes signed DXIL (Windows SDK build, with dxil.dll beside it)")
    ap.add_argument("--dxc-spirv", help="dxc built with SPIR-V code generation (Vulkan SDK)")
    ap.add_argument("--spirv-cross", help="spirv-cross (Vulkan SDK), used to make MSL")
    ap.add_argument("--shader-dir", default=HERE, help="folder with the .hlsl sources (default: this folder)")
    ap.add_argument("--out-dir", default=None, help="where the headers go (default: <shader-dir>/compiled)")
    ap.add_argument("--check", action="store_true",
                    help="recompile into a temporary folder; exit 1 if a committed header differs")
    args = ap.parse_args()

    shader_dir = os.path.abspath(args.shader_dir)
    out_dir = os.path.abspath(args.out_dir) if args.out_dir else os.path.join(shader_dir, "compiled")

    try:
        tools = {
            "dxc_dxil": find_tool(args.dxc_dxil, [DEFAULT_WINDOWS_SDK_DXC], "dxc"),
            "dxc_spirv": find_tool(args.dxc_spirv, vulkan_sdk_candidates("dxc"), "dxc"),
            "spirv_cross": find_tool(args.spirv_cross, vulkan_sdk_candidates("spirv-cross"), "spirv-cross"),
        }
        spirv_val = None
        for c in vulkan_sdk_candidates("spirv-val"):
            if os.path.isfile(c):
                spirv_val = c
                break
        tools["spirv_val"] = spirv_val or shutil.which("spirv-val")
        versions = {
            "dxc_dxil": tool_version([tools["dxc_dxil"], "--version"]),
            "dxc_spirv": tool_version([tools["dxc_spirv"], "--version"]),
            "spirv_cross": tool_version([tools["spirv_cross"], "--revision"]),
        }
        sources = discover(shader_dir)
        if not sources:
            raise ToolError(f"no *.vert.hlsl or *.frag.hlsl files in {shader_dir}")

        if not args.check:
            build_all(shader_dir, out_dir, tools, versions)
            return 0

        with tempfile.TemporaryDirectory(prefix="ps2x_shader_check_") as tmp:
            written = set(build_all(shader_dir, tmp, tools, versions))
            problems = []
            for name in sorted(written):
                committed = os.path.join(out_dir, name)
                if not os.path.isfile(committed):
                    problems.append(f"missing: {committed}")
                elif read_normalised(committed) != read_normalised(os.path.join(tmp, name)):
                    problems.append(f"differs: {committed}")
            if os.path.isdir(out_dir):
                for name in sorted(os.listdir(out_dir)):
                    if name.endswith(".h") and name not in written:
                        problems.append(f"stale (no matching .hlsl): {os.path.join(out_dir, name)}")
            if problems:
                print("CHECK FAILED:\n  " + "\n  ".join(problems))
                return 1
            print(f"CHECK OK: {len(written)} header(s) match a fresh build")
            return 0
    except ToolError as e:
        print(f"error: {e}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
