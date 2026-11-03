# faust_build.py by Arev Imer
#
# Compiles FAUST programs for the NEMESIS module with the architecture files
# in this folder (replaces the old faust2nemesis script).
#
# As PlatformIO pre-script every src/*.dsp of the project is compiled into
# lib/faust_programs/<name>.{h,cpp}; programs are only regenerated when the
# .dsp, the architecture or the options changed:
#
#   extra_scripts = pre:../../faust/faust_build.py
#   custom_faust = /path/to/faust          ; optional, default $FAUST or faust on PATH
#   custom_faust_options = -double         ; optional extra compiler options
#   custom_faust_arch = path/to/faust      ; optional, for projects outside this
#                                          ; repository (default: searched upwards)
#   custom_faust_programs =                ; optional, more programs (paths or
#       ../../programs/juno.dsp            ; globs relative to the project) in
#       ../../programs/*.dsp               ; this order, before src/*.dsp
#
# For many programs in one firmware, board_build.ldscript =
# ../lib/nemesis/teensy41_flash.ld runs them from flash instead of RAM1.
#
# The generated lib/faust_programs/nemesis_programs.h creates one instance of
# every program and lists them for nemesis::begin(nemesis_programs).
#
# Stand-alone:
#
#   python3 faust_build.py program.dsp [more.dsp ...] [-o outdir] [-- faust options]
#
# Per program options in the .dsp file:
#
#   declare options "[midi:on][nvoices:8]";
#
# A polyphonic program can define 'effect = ...;' next to 'process': the voices
# are mixed and run through the effect once (e.g. a chorus), as in FAUST's
# faust2xx -effect auto.

import glob
import os
import re
import shutil
import subprocess
import sys
import tempfile

PROGRAM_LIST = "nemesis_programs"
ARCH_CPP = "nemesis_arch.cpp"
EFFECT_ARCH = "nemesis_effect_arch.cpp"
EFFECT_MARKER = "// <<NEMESIS_EFFECT_CLASS>>"
ARCH_H = "nemesis_arch.h"
PLACEHOLDER = "NemesisAudio"
OUT_DIR = os.path.join("lib", "faust_programs")


class FaustError(Exception):
    pass


def find_arch_dir(start):
    path = os.path.abspath(start)
    while True:
        for candidate in (path, os.path.join(path, "faust"), os.path.join(path, "software", "faust")):
            if os.path.isfile(os.path.join(candidate, ARCH_CPP)):
                return candidate
        parent = os.path.dirname(path)
        if parent == path:
            raise FaustError("could not find %s above %s" % (ARCH_CPP, start))
        path = parent


def faust_version(faust):
    try:
        out = subprocess.run([faust, "--version"], capture_output=True, text=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError) as e:
        raise FaustError("could not run '%s' (%s), install FAUST or set custom_faust" % (faust, e))
    match = re.search(r"FAUST Version ([\d.]+)", out)
    return match.group(1) if match else "unknown"


def strip_comments(source):
    out = []
    i, n, in_string = 0, len(source), False
    while i < n:
        c = source[i]
        if in_string:
            out.append(c)
            if c == "\\" and i + 1 < n:
                out.append(source[i + 1])
                i += 1
            elif c == '"':
                in_string = False
        elif c == '"':
            in_string = True
            out.append(c)
        elif source.startswith("//", i):
            end = source.find("\n", i)
            i = n if end < 0 else end
            continue
        elif source.startswith("/*", i):
            end = source.find("*/", i + 2)
            i = n if end < 0 else end + 2
            continue
        else:
            out.append(c)
        i += 1
    return "".join(out)


def program_options(dsp_path):
    with open(dsp_path, encoding="utf-8") as f:
        source = strip_comments(f.read())
    options = {}
    for declared in re.findall(r'declare\s+options\s+"([^"]*)"', source):
        for key, value in re.findall(r"\[\s*(\w+)\s*:\s*([^\]]*?)\s*\]", declared):
            options[key] = value
    midi = options.get("midi", "off") == "on"
    nvoices = int(options.get("nvoices", "0") or 0)
    return midi, nvoices


def has_effect(dsp_path):
    with open(dsp_path, encoding="utf-8") as f:
        return re.search(r"^\s*effect\s*=", strip_comments(f.read()), re.M) is not None


def compile_effect(faust, faust_options, arch_dir, dsp_path, tmp):
    """The 'effect' of a polyphonic program as class NemesisAudio_effect."""
    out = os.path.join(tmp, "effect.cpp")
    cmd = [faust] + list(faust_options) + [
        "-lang", "cpp", "-mem", "-pn", "effect", "-cn", PLACEHOLDER + "_effect",
        "-A", arch_dir, "-a", os.path.join(arch_dir, EFFECT_ARCH),
        "-I", os.path.dirname(os.path.abspath(dsp_path)), dsp_path, "-o", out]
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        raise FaustError("faust failed on the effect of %s:\n%s%s" % (dsp_path, result.stdout, result.stderr))
    with open(out, encoding="utf-8") as f:
        code = f.read()
    outputs = re.search(r"getNumOutputs\(\)\s*\{\s*return (\d+);", code)
    if not outputs:
        raise FaustError("could not read the output count of the effect of %s" % dsp_path)
    return ("#define NEMESIS_EFFECT 1\n#define NEMESIS_NUM_OUTPUTS %s\n" % outputs.group(1)) + code


def arch_inputs(arch_dir):
    files = []
    for root, _, names in os.walk(arch_dir):
        files += [os.path.join(root, n) for n in names if n.endswith((".cpp", ".h", ".py"))]
    return files


def generate(faust, version, dsp_path, out_dir, arch_dir, faust_options, define_globals, extra_inputs=()):
    name = os.path.splitext(os.path.basename(dsp_path))[0]
    if not re.match(r"^[A-Za-z_]\w*$", name):
        raise FaustError("'%s': the file name is used as C++ class name, use letters, digits and _" % dsp_path)

    midi, nvoices = program_options(dsp_path)
    defines = []
    if midi:
        defines.append("#define MIDICTRL 1")
    if nvoices > 0:
        defines.append("#define NVOICES %d" % nvoices)
    if define_globals:
        defines.append("#define NEMESIS_FAUST_GLOBALS 1")
    effect = nvoices > 0 and has_effect(dsp_path)

    signature = "// generated by faust_build.py with FAUST %s, options: %s %s%s\n" % (
        version, " ".join(faust_options), " ".join(d.split(" ", 1)[1] for d in defines),
        " effect" if effect else "")
    out_cpp = os.path.join(out_dir, name + ".cpp")
    out_h = os.path.join(out_dir, name + ".h")

    inputs = [dsp_path] + arch_inputs(arch_dir) + list(extra_inputs)
    if os.path.isfile(out_cpp) and os.path.isfile(out_h):
        with open(out_cpp, encoding="utf-8") as f:
            up_to_date = f.readline() == signature
        newest_input = max(os.path.getmtime(p) for p in inputs)
        if up_to_date and min(os.path.getmtime(out_cpp), os.path.getmtime(out_h)) >= newest_input:
            return name, False

    with tempfile.TemporaryDirectory() as tmp:
        tmp_cpp = os.path.join(tmp, name + ".cpp")
        cmd = [faust] + list(faust_options) + [
            "-lang", "cpp", "-i", "-mem", "-uim",
            "-cn", PLACEHOLDER + "_dsp",
            "-A", arch_dir, "-a", os.path.join(arch_dir, ARCH_CPP),
            "-I", os.path.dirname(os.path.abspath(dsp_path)),
            dsp_path, "-o", tmp_cpp]
        result = subprocess.run(cmd, capture_output=True, text=True)
        if result.returncode != 0:
            raise FaustError("faust failed on %s:\n%s%s" % (dsp_path, result.stdout, result.stderr))
        with open(tmp_cpp, encoding="utf-8") as f:
            cpp = f.read()
        if effect:
            cpp = cpp.replace(EFFECT_MARKER, compile_effect(faust, faust_options, arch_dir, dsp_path, tmp))

    with open(os.path.join(arch_dir, ARCH_H), encoding="utf-8") as f:
        header = f.read()

    os.makedirs(out_dir, exist_ok=True)
    with open(out_cpp, "w", encoding="utf-8") as f:
        f.write(signature + "\n".join(defines) + "\n\n" + cpp.replace(PLACEHOLDER, name))
    with open(out_h, "w", encoding="utf-8") as f:
        f.write(signature + header.replace(PLACEHOLDER, name))
    return name, True


def build(dsp_files, out_dir, faust, faust_options, arch_dir, extra_inputs=(), log=print):
    """Compile the programs in the given order."""
    version = faust_version(faust)
    names = [os.path.splitext(os.path.basename(p))[0] for p in dsp_files]
    duplicates = sorted(set(n for n in names if names.count(n) > 1))
    if duplicates:
        raise FaustError("program names must be unique: %s" % ", ".join(duplicates))
    midi_programs = [p for p in dsp_files if program_options(p)[0]]
    for dsp_path in dsp_files:
        define_globals = bool(midi_programs) and dsp_path == midi_programs[0]
        name, regenerated = generate(faust, version, dsp_path, out_dir, arch_dir,
                                     faust_options, define_globals, extra_inputs)
        log("FAUST %s: %s %s" % (version, name, "compiled" if regenerated else "up to date"))
    write_program_list(out_dir, names)
    return names


def write_program_list(out_dir, names):
    """nemesis_programs.h: one instance per program and the list for nemesis::begin()."""
    lines = ["// generated by faust_build.py: the programs of this firmware",
             "// include it once (in main.cpp) and call nemesis::begin(nemesis_programs)",
             "#pragma once", "#include <nemesis_program.h>"]
    lines += ['#include "%s.h"' % n for n in names]
    lines += ["static %s program_%s;" % (n, n) for n in names]
    lines.append("nemesis::Program *nemesis_programs[] = {%s};" % ", ".join("&program_%s" % n for n in names))
    text = "\n".join(lines) + "\n"
    path = os.path.join(out_dir, PROGRAM_LIST + ".h")
    os.makedirs(out_dir, exist_ok=True)
    if not os.path.isfile(path) or open(path, encoding="utf-8").read() != text:
        with open(path, "w", encoding="utf-8") as f:
            f.write(text)


def remove_stale(out_dir, names):
    if not os.path.isdir(out_dir):
        return
    for entry in os.listdir(out_dir):
        stem, ext = os.path.splitext(entry)
        if ext in (".cpp", ".h") and stem not in names and stem != PROGRAM_LIST:
            os.remove(os.path.join(out_dir, entry))


def run_platformio(env):
    project_dir = env.subst("$PROJECT_DIR")
    src_dir = env.subst("$PROJECT_SRC_DIR")
    out_dir = os.path.join(project_dir, OUT_DIR)
    faust = env.GetProjectOption("custom_faust", "") or os.environ.get("FAUST") or shutil.which("faust") or "faust"
    faust_options = env.GetProjectOption("custom_faust_options", "").split()

    arch_option = env.GetProjectOption("custom_faust_arch", "")

    try:
        arch_dir = os.path.join(project_dir, arch_option) if arch_option else find_arch_dir(project_dir)
        if not os.path.isfile(os.path.join(arch_dir, ARCH_CPP)):
            raise FaustError("%s not found in %s (custom_faust_arch)" % (ARCH_CPP, arch_dir))
        dsp_files = []
        for pattern in env.GetProjectOption("custom_faust_programs", "").split():
            matches = sorted(glob.glob(os.path.join(project_dir, pattern)))
            if not matches:
                raise FaustError("custom_faust_programs: nothing matches %s" % pattern)
            dsp_files += [os.path.normpath(m) for m in matches]
        dsp_files += sorted(os.path.join(src_dir, f) for f in os.listdir(src_dir) if f.endswith(".dsp"))
        if not dsp_files:
            raise FaustError("no FAUST programs: add src/*.dsp or custom_faust_programs")
        lib_dirs = set([src_dir] + [os.path.dirname(p) for p in dsp_files])
        libs = [os.path.join(d, f) for d in lib_dirs for f in os.listdir(d) if f.endswith(".lib")]
        names = build(dsp_files, out_dir, faust, faust_options, arch_dir, libs)
        remove_stale(out_dir, names)
        poly = [os.path.splitext(os.path.basename(p))[0] for p in dsp_files if program_options(p)[1] > 0]
    except FaustError as e:
        sys.stderr.write("\n*** %s\n\n" % e)
        env.Exit(1)

    # poly-dsp.h uses dynamic_cast while the Teensy core builds with -fno-rtti,
    # which is added after pre-scripts run. RTTI is enabled for all programs as
    # soon as one is polyphonic: the inline FAUST header code is shared between
    # them and the linker keeps one copy, which mixed with and without RTTI
    # crashed in __dynamic_cast.
    poly_sources = set(os.path.join(out_dir, name + ".cpp") for name in names) if poly else set()

    def rtti_middleware(env, node):
        if node.srcnode().get_abspath() not in poly_sources:
            return node
        flags = [f for f in env["CXXFLAGS"] if f != "-fno-rtti"] + ["-frtti"]
        return env.Object(node, CXXFLAGS=flags)

    if poly_sources:
        env.AddBuildMiddleware(rtti_middleware)


def main(argv):
    if "--" in argv:
        split = argv.index("--")
        argv, faust_options = argv[:split], argv[split + 1:]
    else:
        faust_options = []
    out_dir = "."
    if "-o" in argv:
        i = argv.index("-o")
        out_dir = argv[i + 1]
        argv = argv[:i] + argv[i + 2:]
    if not argv:
        print(__doc__ or "usage: faust_build.py program.dsp [-o outdir] [-- faust options]")
        return 1
    faust = os.environ.get("FAUST") or shutil.which("faust") or "faust"
    arch_dir = os.path.dirname(os.path.abspath(__file__))
    try:
        build(argv, out_dir, faust, faust_options, arch_dir)
    except FaustError as e:
        sys.stderr.write("%s\n" % e)
        return 1
    return 0


try:
    Import("env")  # noqa: F821 - provided by PlatformIO/SCons
except NameError:
    sys.exit(main(sys.argv[1:]))
else:
    run_platformio(env)  # noqa: F821
