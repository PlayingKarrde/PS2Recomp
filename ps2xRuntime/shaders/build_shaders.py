#!/usr/bin/env python3
"""Compile the HLSL shaders in this folder into C++ headers that hold the compiled bytes.

Why this exists. SDL_GPU (the graphics API the GPU renderer uses) does not read HLSL. It wants
compiled shaders in the format of the graphics driver it runs on: SPIR-V for Vulkan, DXIL for
Direct3D 12 and Metal Shading Language (MSL) source for Metal. We write each shader once in HLSL,
and this script compiles it three times. The results are committed to the repository as headers in
`compiled/`, so building the game or the tests never needs a shader compiler. Run this script only
when you change a `.hlsl` file, then commit the changed headers together with it.

What it does, for every `<name>.vert.hlsl` and `<name>.frag.hlsl` in this folder (or only the ones
named with `--only`):
  1. DXIL:   a `dxc` that can sign, `dxc -T vs_6_0|ps_6_0 -E main`. Direct3D 12 refuses unsigned DXIL.
             On Windows that is the Windows SDK build (it ships `dxil.dll`, which signs; it cannot
             write SPIR-V). On Linux it is Microsoft's DirectXShaderCompiler release, whose `dxc`
             finds `libdxil.so` beside its own library.
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

The output is byte-for-byte reproducible for one set of tools (no timestamps or paths), which is what
`--check` relies on. Another build of the same compiler gives other bytes for the same source, and
the headers in `compiled/` were not all made on one machine: every header records the versions of the
three tools that made it. So `--check` works header by header:
  - a header made by exactly the tools found now is compiled again and must match byte for byte;
  - a header made by other tools is listed as "built by another toolchain, not compared". That is not
    a failure, but it is also no proof that the header is up to date with its source: only its
    resource counts are compared with the `// @key N` lines of the source;
  - a shader with no header, and a header with no shader, are failures.
It exits with status 1 on any failure. Status 2 means a tool is missing or failed.

`--only NAME [NAME...]` compiles and writes just those shaders, named by source file
(`postfx_rim.frag.hlsl`; `postfx_rim.frag` and `postfx_rim_frag` are accepted too). Use it on a
machine whose tools differ from the ones that built the committed headers, so that the headers of
shaders you did not touch keep their bytes. You must name every shader whose own source or whose
included `.hlsli` changed: the script cannot tell for you.

Where the tools are looked for (an explicit argument wins, then these, then PATH):
  Windows  DXIL    the Windows SDK's dxc.exe (DEFAULT_WINDOWS_SDK_DXC below)
           SPIR-V  %VULKAN_SDK% or DEFAULT_VULKAN_SDK_ROOT below (Bin/dxc.exe, Bin/spirv-cross.exe)
  Linux    DXIL    ~/sdk/dxc-1.8.2502/bin/dxc   (Microsoft's release; signs through libdxil.so)
           SPIR-V  $VULKAN_SDK or ~/sdk/vulkan/1.4.357.0/x86_64 (bin/dxc, bin/spirv-cross)
On Linux each `dxc` finds its own `libdxcompiler.so` through its RPATH. LD_LIBRARY_PATH must not point
at either: both would then load the same library, the wrong one for one of them. The script takes
LD_LIBRARY_PATH out of the environment of the tools it runs for that reason.

Uses the Python standard library only.

Examples:
    python build_shaders.py                                   # rebuild every compiled/*.h
    python build_shaders.py --only postfx_rim.frag.hlsl       # rebuild one
    python build_shaders.py --check                           # verify the committed headers
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
# Linux (installed outside the repository; see the module docstring).
DEFAULT_LINUX_DXIL_DXC = os.path.join(os.path.expanduser("~"), "sdk", "dxc-1.8.2502", "bin", "dxc")
DEFAULT_LINUX_VULKAN_SDK_ROOT = os.path.join(os.path.expanduser("~"), "sdk", "vulkan", "1.4.357.0", "x86_64")

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
    roots = [os.environ.get("VULKAN_SDK"), DEFAULT_VULKAN_SDK_ROOT if os.name == "nt" else DEFAULT_LINUX_VULKAN_SDK_ROOT]
    out = []
    for r in roots:
        if r:
            out.append(os.path.join(r, "Bin", exe(tool)))
            out.append(os.path.join(r, "bin", exe(tool)))
    return out


def tool_env():
    """The environment the tools run in: ours without LD_LIBRARY_PATH (each dxc must find its own library, see the docstring)."""
    env = dict(os.environ)
    env.pop("LD_LIBRARY_PATH", None)
    return env


def run(cmd, cwd):
    proc = subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, errors="replace", env=tool_env())
    if proc.returncode != 0:
        raise ToolError(f"command failed ({proc.returncode}): {' '.join(cmd)}\n{proc.stdout}{proc.stderr}")
    return proc.stdout + proc.stderr


def tool_version(cmd):
    """First useful line of the tool's version output, flattened to one line."""
    proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace", env=tool_env())
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


def header_name(path):
    """compiled/<this> for a source path: smoke.vert.hlsl -> smoke_vert.h"""
    return os.path.basename(path)[:-len(".hlsl")].replace(".", "_") + ".h"


def select_only(sources, names):
    """The sources named by --only (file name, with or without .hlsl, or the header's stem). Unknown names are an error."""
    by_key = {}
    for path, stage in sources:
        base = os.path.basename(path)
        stem = base[:-len(".hlsl")]
        for key in (base, stem, stem.replace(".", "_"), stem.replace(".", "_") + ".h"):
            by_key[key] = (path, stage)
    chosen = []
    for n in names:
        if n not in by_key:
            raise ToolError(f"--only {n}: no such shader (expected a name like {os.path.basename(sources[0][0])})")
        if by_key[n] not in chosen:
            chosen.append(by_key[n])
    return chosen


def header_tools(path):
    """The three tool version strings a committed header records, as header_text wrote them (None for a line that is missing)."""
    found = {"dxc_dxil": None, "dxc_spirv": None, "spirv_cross": None}
    labels = {"DXIL": "dxc_dxil", "SPIR-V": "dxc_spirv", "MSL": "spirv_cross"}
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for _ in range(12):
            line = f.readline()
            m = re.match(r"^//\s+(DXIL|SPIR-V|MSL)\s*:\s(.*)$", line.rstrip("\r\n"))
            if m:
                value = m.group(2)
                if m.group(1) == "MSL":
                    value = re.sub(r" \(entry point renamed by spirv-cross to \"\w+\"\)$", "", value)
                found[labels[m.group(1)]] = value
    return found


def header_counts(path):
    """The resource counts a committed header holds (the four '    N, // samplers' lines)."""
    counts = {}
    names = {"samplers": "samplers", "storage textures": "storage_textures", "storage buffers": "storage_buffers", "uniform buffers": "uniform_buffers"}
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            m = re.match(r"^\s+(\d+), // (samplers|storage textures|storage buffers|uniform buffers)\s*$", line)
            if m:
                counts[names[m.group(2)]] = int(m.group(1))
    return counts


def build_all(shader_dir, out_dir, tools, versions, sources=None):
    """Write the header of every source (or of `sources`) into out_dir; returns the list of header file names written."""
    os.makedirs(out_dir, exist_ok=True)
    written = []
    for path, stage in (discover(shader_dir) if sources is None else sources):
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
                    help="verify the committed headers: recompile the ones this machine's tools built and compare; "
                         "list the ones another toolchain built; exit 1 if one differs, is missing or has no shader")
    ap.add_argument("--only", nargs="+", metavar="NAME",
                    help="compile and write only these shaders (source names, e.g. postfx_rim.frag.hlsl). "
                         "Name every shader whose source or included .hlsli changed")
    args = ap.parse_args()

    shader_dir = os.path.abspath(args.shader_dir)
    out_dir = os.path.abspath(args.out_dir) if args.out_dir else os.path.join(shader_dir, "compiled")

    try:
        tools = {
            "dxc_dxil": find_tool(args.dxc_dxil, [DEFAULT_WINDOWS_SDK_DXC if os.name == "nt" else DEFAULT_LINUX_DXIL_DXC], "dxc"),
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

        if args.only:
            sources = select_only(sources, args.only)

        if not args.check:
            build_all(shader_dir, out_dir, tools, versions, sources)
            return 0

        # --check, header by header: compared byte for byte when this machine's tools are the ones that built it.
        problems, same_tools, other_tools = [], [], []
        for path, stage in sources:
            name = header_name(path)
            committed = os.path.join(out_dir, name)
            if not os.path.isfile(committed):
                problems.append(f"missing: {committed}")
                continue
            if header_tools(committed) == versions:
                same_tools.append((path, stage))
            else:
                other_tools.append(name)
                if header_counts(committed) != parse_counts(path):
                    problems.append(f"resource counts differ from the source's: {committed}")
        with tempfile.TemporaryDirectory(prefix="ps2x_shader_check_") as tmp:
            for name in sorted(build_all(shader_dir, tmp, tools, versions, same_tools)):
                if read_normalised(os.path.join(out_dir, name)) != read_normalised(os.path.join(tmp, name)):
                    problems.append(f"differs: {os.path.join(out_dir, name)}")
        if os.path.isdir(out_dir) and not args.only:
            expected = {header_name(path) for path, _ in sources}
            for name in sorted(os.listdir(out_dir)):
                if name.endswith(".h") and name not in expected:
                    problems.append(f"stale (no matching .hlsl): {os.path.join(out_dir, name)}")
        if other_tools:
            print(f"built by another toolchain, not compared ({len(other_tools)}): " + ", ".join(sorted(other_tools)))
        if problems:
            print("CHECK FAILED:\n  " + "\n  ".join(problems))
            return 1
        print(f"CHECK OK: {len(same_tools)} header(s) match a fresh build; {len(other_tools)} built by another toolchain, not compared")
        return 0
    except ToolError as e:
        print(f"error: {e}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
