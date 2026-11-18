"""Generate PPC sources. Build XenonRecomp first; requires Python 3.11+."""
import argparse
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import tomllib


def layout(root):
    config = root / "LostOdysseyRecompLib/config/LostOdysseyRecomp.toml"
    main = tomllib.loads(config.read_text(encoding="utf-8"))["main"]
    output = (config.parent / main["out_directory_path"]).resolve()
    if output != root / "LostOdysseyRecompLib/ppc":
        raise ValueError("Unexpected PPC output directory")
    return config, output


def generated_files(output):
    return [p for p in output.iterdir() if p.is_file() and p.suffix in {".cpp", ".h"}]


def require_outputs(output):
    if not any(output.glob("ppc_recomp.*.cpp")):
        raise ValueError("Missing generated PPC sources")
    for name in ("ppc_func_mapping.cpp", "ppc_config.h", "ppc_context.h", "ppc_recomp_shared.h"):
        if not (output / name).is_file():
            raise ValueError(f"Missing generated file: {name}")


# XenonRecomp emits PowerPC barriers as comments only. x86 keeps stores in order,
# but AArch64 reorders them like PowerPC, so publication through a plain store
# after lwsync (for example a job queue) can expose half-built objects. Each
# barrier becomes a C++ fence: acq_rel emits no instruction on x86 (compiler
# ordering only) and dmb on AArch64; sync is a full fence.
BARRIERS = {
    "\t// lwsync \n": "\tstd::atomic_thread_fence(std::memory_order_acq_rel);\n",
    "\t// eieio \n": "\tstd::atomic_thread_fence(std::memory_order_acq_rel);\n",
    "\t// sync \n": "\tstd::atomic_thread_fence(std::memory_order_seq_cst);\n",
}
SHARED_INCLUDE = '#include "ppc_recomp_shared.h"\n'


def insert_barriers(output):
    """Add the fences to generated sources; repeated runs change nothing."""
    inserted = 0
    for path in sorted(output.glob("ppc_recomp.*.cpp")):
        lines = path.read_text(encoding="utf-8").splitlines(keepends=True)
        result, changed = [], False
        for index, line in enumerate(lines):
            result.append(line)
            fence = BARRIERS.get(line)
            if fence and (index + 1 >= len(lines) or lines[index + 1] != fence):
                result.append(fence)
                changed = True
                inserted += 1
        if not changed:
            continue
        if "#include <atomic>\n" not in result:
            if SHARED_INCLUDE not in result:
                raise ValueError(f"Unexpected generated layout: {path.name}")
            result.insert(result.index(SHARED_INCLUDE) + 1, "#include <atomic>\n")
        path.write_text("".join(result), encoding="utf-8")
    return inserted


# The D3D runtime reads GPU-written memory through the cached physical view:
# MmGetPhysicalAddress inlined, then `addis rX,rY,-16384` (0xC0000000 + physical).
# The GPU writes 0xA0000000 + physical. Where the host maps those views onto the
# same pages they are one memory; the Switch 2 heap backend cannot alias them
# (kernel/guest_address_space_switch.cpp), so the occlusion query GetData
# (sub_823CF3F0) read zeros there: every query answered "0 pixels" and UE3 hid
# the skeletal meshes' base pass, leaving black silhouettes (2026-11-18). All
# 13 such sites (11 functions) are rewritten to the 0xA0000000 view, which is the
# same memory wherever the views alias. Only the line after that exact comment
# changes, so a run is idempotent.
CACHED_VIEW_COMMENT = re.compile(r"^\t// addis (r\d+),(r\d+),-16384 ?\n$")
CACHED_VIEW_LINE = re.compile(r"^\t(ctx\.)?(r\d+)\.s64 = (ctx\.)?(r\d+)\.s64 \+ -1073741824;\n$")
def retarget_cached_views(output):
    """Point the physical-to-cached-view conversions at the A view."""
    changed_sites = 0
    for path in sorted(output.glob("ppc_recomp.*.cpp")):
        lines = path.read_text(encoding="utf-8").splitlines(keepends=True)
        changed = False
        for index in range(len(lines) - 1):
            comment = CACHED_VIEW_COMMENT.match(lines[index])
            if not comment:
                continue
            line = CACHED_VIEW_LINE.match(lines[index + 1])
            if not line or line.group(2) != comment.group(1) or line.group(4) != comment.group(2):
                continue
            lines[index + 1] = lines[index + 1].replace("+ -1073741824;", "+ -1610612736; // A view (ppc_codegen.py)")
            changed = True
            changed_sites += 1
        if changed:
            path.write_text("".join(lines), encoding="utf-8")
    return changed_sites
def generate(root, executable):
    config, output = layout(root)
    output.mkdir(parents=True, exist_ok=True)
    # XenonRecomp can return zero without producing sources. Keep the previous
    # generated set until a new set has been produced successfully.
    with tempfile.TemporaryDirectory(prefix="ppc-codegen-", dir=output.parent) as temporary:
        backup = Path(temporary)
        moved = []
        try:
            for path in generated_files(output):
                path.rename(backup / path.name)
                moved.append(path.name)
        except BaseException:
            for name in moved:
                (backup / name).rename(output / name)
            raise
        try:
            subprocess.run([str(executable), str(config),
                            str(root / "tools/XenonRecomp/XenonUtils/ppc_context.h")],
                           cwd=root, check=True)
            require_outputs(output)
            insert_barriers(output)
            retarget_cached_views(output)
        except BaseException:
            for path in generated_files(output):
                path.unlink()
            for name in moved:
                (backup / name).rename(output / name)
            raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=["generate", "barriers", "views"],
                        help="barriers / views: only add fences / retarget the cached physical views in the existing sources")
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--executable", type=Path)
    args = parser.parse_args()
    root = args.root.resolve()
    executable = args.executable or root / "out/build/tools/XenonRecomp/XenonRecomp" / (
        "XenonRecomp.exe" if sys.platform == "win32" else "XenonRecomp")
    try:
        if args.command == "barriers":
            _, output = layout(root)
            require_outputs(output)
            print(f"PPC barriers: {insert_barriers(output)} fences added")
            return 0
        if args.command == "views":
            _, output = layout(root)
            require_outputs(output)
            print(f"PPC views: {retarget_cached_views(output)} cached-view sites retargeted")
            return 0
        generate(root, executable.resolve())
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        print(f"PPC generation failed: {error}", file=sys.stderr)
        return 1
    print("PPC generation: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
