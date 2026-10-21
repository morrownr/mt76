#!/usr/bin/env python3
"""Stage and run the offline, actual-core MLO KUnit fixture (never host modules)."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import time


ROOT = Path(__file__).resolve().parents[2]
FIXTURE = ROOT / "tests/mt7925/kunit/mlo_activation.c"
VALIDATOR_CASES = ROOT / "tests/mt7925/kunit/mlo_validator_cases.h"
COMPLETION_CASES = ROOT / "tests/mt7925/kunit/mlo_completion_cases.h"
CONFIG = """CONFIG_KUNIT=y
CONFIG_NET=y
CONFIG_INET=y
CONFIG_WIRELESS=y
CONFIG_WLAN=y
CONFIG_CFG80211=y
CONFIG_MAC80211=y
CONFIG_MAC80211_KUNIT_TEST=y
CONFIG_MAC80211_RC_MINSTREL=y
CONFIG_DEBUG_KERNEL=y
CONFIG_PROVE_LOCKING=y
"""


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def replace_once(text, old, new, label):
    if text.count(old) != 1:
        raise ValueError(f"reviewed source anchor changed: {label}")
    return text.replace(old, new, 1)


def materialize(source, overlay, relative, transform):
    original = source / relative
    target = overlay / relative
    text = transform(original.read_text())
    # Only detach this test-owned link, never modify the original source.
    target.unlink()
    target.write_text(text)
    return {"path": relative, "original_sha256": digest(original),
            "overlay_sha256": digest(target)}


def driver_sections(source=ROOT):
    """Unchanged current function bodies, not a copied transition model."""
    text = (source / "mt7925/mlo/steering.c").read_text()
    starts = ["static u16 mt7925_mlo_secure_links(",
              "struct mt7925_mlo_steer_counters {",
              "static void mt7925_mlo_steer_read_counters(",
              "static void mt7925_mlo_steer_switch_validate("]
    sections = []
    for start in starts:
        if text.count(start) != 1:
            raise ValueError(f"driver section changed: {start}")
        begin = text.index(start)
        end = text.index("\n}", begin) + 2
        if start.startswith("struct "):
            end += 1  # closing semicolon
        sections.append(text[begin:end])
    names = ["MT7925_MLO_STEER_SWITCH_POLL_MS", "MT7925_MLO_STEER_SWITCH_POLL_COUNT",
        "MT7925_MLO_STEER_SETTLE_MS", "MT7925_MLO_STEER_VERIFY_SAMPLE_MS",
        "MT7925_MLO_STEER_VERIFY_SAMPLES", "MT7925_MLO_STEER_VERIFY_MIN_PKTS",
        "MT7925_MLO_STEER_PROGRESS_RECENT_MS", "MT7925_MLO_STEER_BULK_TX_THRESHOLD",
        "MT7925_MLO_STEER_DEAD_TX_THRESHOLD", "MT7925_MLO_STEER_CANDIDATE_COOLDOWN"]
    constants = []
    for name in names:
        matches = [line for line in text.splitlines() if line.startswith(f"#define {name}\t")]
        if len(matches) != 1:
            raise ValueError(f"driver constant changed: {name}")
        constants += matches
    return "\n".join(constants + sections) + "\n"


def snapshot_prototype(destination):
    """Freeze actual layout/helper dependencies, including untracked headers."""
    pending = [ROOT / p for p in ("mt7925/mt7925.h", "mt7925/mlo_pair_transition.h",
        "mt7925/mlo_secure_shrink.h", "mt7925/mlo_steer_validate.h", "mt7925/main.c",
        "mt7925/mlo/completion.c", "mt7925/mlo/steering.c", "mt7925/mlo/pairs.h")]
    hashes = {}
    while pending:
        original = pending.pop().resolve()
        if ROOT not in original.parents:
            raise ValueError(f"prototype include escapes worktree: {original}")
        relative = original.relative_to(ROOT)
        if str(relative) in hashes:
            continue
        target = destination / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(original, target)
        hashes[str(relative)] = digest(target)
        # main.c is retained for provenance/body extraction, not compiled here.
        if original.suffix == ".h":
            for include in re.findall(r'^#include\s+"([^"]+)"', target.read_text(), re.M):
                pending.append(original.parent / include)
    return hashes


def stage(kernel, output, strict_completion=False, completion_only=False):
    kernel, output = kernel.resolve(), output.resolve()
    if output == kernel or kernel in output.parents:
        raise ValueError("output must not be inside the kernel source")
    if output.exists() and any(output.iterdir()):
        raise ValueError("output is nonempty; preserving existing evidence")
    if not (kernel / "net/mac80211/link.c").is_file():
        raise ValueError("kernel source is missing net/mac80211/link.c")
    if not FIXTURE.is_file():
        raise ValueError("KUnit fixture is not implemented")
    for name in ("mlo_activation.c", "mlo_validator_cases.h", "mlo_validator_body.inc",
                 "mlo_completion_cases.h", "mlo_completion_body.inc"):
        generated = kernel / "net/mac80211/tests" / name
        if generated.exists() or generated.is_symlink():
            raise ValueError(f"generated source collision: {generated}")
    tracked = subprocess.check_output(
        ["git", "-C", str(kernel), "ls-files", "-z"], text=False).split(b"\0")
    output.mkdir(parents=True, exist_ok=True)
    prototype_snapshot = output / "prototype_inputs"
    prototype_hashes = snapshot_prototype(prototype_snapshot)
    overlay = output / "source"
    overlay.mkdir()
    # Point at current reviewed files, including tracked dirty changes, not at
    # upstream HEAD. O= keeps generated objects out of both source trees.
    for name in tracked:
        if not name:
            continue
        relative = Path(os.fsdecode(name))
        if relative.is_absolute() or ".." in relative.parts:
            raise ValueError("unsafe tracked source path")
        original = kernel / relative
        if not original.exists():
            continue
        target = overlay / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.symlink_to(original)

    def iface(text):
        text = replace_once(text,
            "\tieee80211_set_active_links(&sdata->vif, sdata->desired_active_links);",
            "\t{\n\t\tint ret = ieee80211_set_active_links(&sdata->vif, sdata->desired_active_links);\n"
            "\t\textern void mlo_kunit_outer_result(struct ieee80211_vif *, int);\n"
            "\t\tmlo_kunit_outer_result(&sdata->vif, ret);\n\t}", "outer result")
        return text + """

/* Test-owned overlay only. No production module uses this constructor. */
void ieee80211_mlo_test_sdata_init(struct ieee80211_local *local,
                                 struct ieee80211_sub_if_data *sdata);
void ieee80211_mlo_test_sdata_init(struct ieee80211_local *local,
                                 struct ieee80211_sub_if_data *sdata)
{
    sdata->wdev.wiphy = local->hw.wiphy;
    ieee80211_sdata_init(local, sdata);
    ieee80211_setup_sdata(sdata, NL80211_IFTYPE_STATION);
    ieee80211_set_default_queues(sdata);
    ieee80211_init_frag_cache(&sdata->frags);
    wiphy_delayed_work_init(&sdata->dec_tailroom_needed_wk,
                           ieee80211_delayed_tailroom_dec);
}
"""

    def wrappers(text):
        return replace_once(text, "\told_links &= sdata->vif.active_links;",
            "\t{\n\t\textern void mlo_kunit_sta_callsite(struct ieee80211_vif *, u16, u16);\n"
            "\t\tmlo_kunit_sta_callsite(&sdata->vif, old_links, new_links);\n\t}\n"
            "\told_links &= sdata->vif.active_links;", "STA callsite")

    # Kbuild derives srctree from realpath(lastword MAKEFILE_LIST). Detach the
    # top Makefile too, otherwise its symlink points Kbuild back at the native
    # archived build (which must never be cleaned or rebuilt by this fixture).
    changes = [materialize(kernel, overlay, "Makefile", lambda text: text),
               materialize(kernel, overlay, "tools/testing/kunit/kunit.py", lambda text: text),
               materialize(kernel, overlay, "net/mac80211/iface.c", iface),
               materialize(kernel, overlay, "net/mac80211/driver-ops.c", wrappers),
               materialize(kernel, overlay, "net/mac80211/Makefile",
                   lambda text: text + "\nmac80211-$(CONFIG_MAC80211_KUNIT_TEST) += tests/mlo_activation.o\n"
                       + f"CFLAGS_tests/mlo_activation.o += -I{prototype_snapshot} -I{prototype_snapshot / 'mt7925'} -I{prototype_snapshot / 'mt7925/mlo'}\n"
                       + ('' if completion_only else
                          'CFLAGS_tests/mlo_activation.o += -DMLO_KUNIT_VALIDATOR_INCLUDE=\\\"mlo_validator_cases.h\\\"\n')
                       + 'CFLAGS_tests/mlo_activation.o += -DMLO_KUNIT_COMPLETION_INCLUDE=\\\"mlo_completion_cases.h\\\"\n'
                       + ('CFLAGS_tests/mlo_activation.o += -DMLO_KUNIT_STRICT_COMPLETION\n'
                          if strict_completion else ''))]
    destination = overlay / "net/mac80211/tests/mlo_activation.c"
    shutil.copyfile(FIXTURE, destination)
    changes.append({"path": "net/mac80211/tests/mlo_activation.c",
                    "original_sha256": digest(FIXTURE), "overlay_sha256": digest(destination)})
    validator = overlay / "net/mac80211/tests/mlo_validator_body.inc"
    validator.write_text(driver_sections(prototype_snapshot))
    cases = overlay / "net/mac80211/tests/mlo_validator_cases.h"
    shutil.copyfile(VALIDATOR_CASES, cases)
    completion = overlay / "net/mac80211/tests/mlo_completion_body.inc"
    shutil.copyfile(prototype_snapshot / "mt7925/mlo/completion.c", completion)
    completion_cases = overlay / "net/mac80211/tests/mlo_completion_cases.h"
    shutil.copyfile(COMPLETION_CASES, completion_cases)
    changes += [{"path": "net/mac80211/tests/mlo_validator_body.inc",
                 "source": "unchanged sections of current mt7925/mlo/steering.c",
                 "overlay_sha256": digest(validator)},
                {"path": "net/mac80211/tests/mlo_validator_cases.h",
                 "original_sha256": digest(VALIDATOR_CASES), "overlay_sha256": digest(cases)},
                {"path": "net/mac80211/tests/mlo_completion_body.inc",
                 "source": "verbatim current production mt7925/mlo/completion.c",
                 "original_sha256": digest(prototype_snapshot / "mt7925/mlo/completion.c"),
                 "overlay_sha256": digest(completion)},
                {"path": "net/mac80211/tests/mlo_completion_cases.h",
                 "original_sha256": digest(COMPLETION_CASES),
                 "overlay_sha256": digest(completion_cases)}]
    config = output / "kunitconfig"
    config.write_text(CONFIG)
    identity = {"kernel_source": str(kernel), "prototype": str(ROOT),
        "kernel_head": subprocess.check_output(["git", "-C", str(kernel), "rev-parse", "HEAD"], text=True).strip(),
        "prototype_head": subprocess.check_output(["git", "-C", str(ROOT), "rev-parse", "HEAD"], text=True).strip(),
        "prototype_tracked_diff_sha256": hashlib.sha256(subprocess.check_output(
            ["git", "-C", str(ROOT), "diff", "--binary"])).hexdigest(),
        "source_changes": changes,
        "prototype_input_sha256": prototype_hashes,
        "runner_sha256": digest(Path(__file__)),
        "real_core_paths": {p: digest(kernel / p) for p in (
            "net/mac80211/link.c", "net/mac80211/key.c", "net/wireless/core.c")},
        "config_sha256": digest(config), "created_wall_ns": time.time_ns(),
        "coverage": ("actual core/wrappers/keys/wiphy queue plus production completion; explicit driver callback doubles"
                     if completion_only else
                     "actual core/wrappers/keys/wiphy queue plus unchanged driver validator bodies; explicit callback/request/metric/time doubles"),
        "completion_only": completion_only,
        "strict_completion": strict_completion,
        "hardware_test": False, "firmware_test": False}
    (output / "source_identity.json").write_text(json.dumps(identity, indent=2) + "\n")
    return overlay


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel-source", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--stage-only", action="store_true")
    parser.add_argument("--strict-completion", action="store_true",
        help="assert required completion contract (preserved RED run predates the fix)")
    parser.add_argument("--completion-only", action="store_true",
        help="run actual-core and production-completion suites without the separate steering-validator fixture")
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--timeout", type=int, default=120)
    args = parser.parse_args(argv)
    if args.jobs < 1 or args.jobs > 8 or args.timeout < 1:
        parser.error("jobs must be 1..8 and timeout positive")
    try:
        overlay = stage(args.kernel_source, args.output, args.strict_completion,
                        args.completion_only)
    except (ValueError, OSError, subprocess.CalledProcessError) as exc:
        parser.error(str(exc))
    output = args.output.resolve()
    if args.stage_only:
        print(json.dumps({"staged": str(overlay), "hardware_test": False}))
        return 0
    temporary = output / "tmp"
    temporary.mkdir()
    env = os.environ.copy()
    env["TMPDIR"] = str(temporary)
    command = ["python3", "tools/testing/kunit/kunit.py", "run",
        "--arch=um", f"--build_dir={output / 'build'}",
        f"--kunitconfig={output / 'kunitconfig'}", f"--jobs={args.jobs}",
        f"--timeout={args.timeout}", "mlo_activation*"]
    (output / "command.json").write_text(json.dumps(command, indent=2) + "\n")
    with (output / "run.log").open("w") as log:
        process = subprocess.Popen(command, cwd=overlay, env=env,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        for line in process.stdout:
            log.write(line)
            log.flush()
            print(line, end="", flush=True)
        result = process.wait()
    (output / "result.json").write_text(json.dumps({"returncode": result,
        "hardware_test": False, "firmware_test": False,
        "run_log_sha256": digest(output / "run.log")}, indent=2) + "\n")
    return result


if __name__ == "__main__":
    raise SystemExit(main())
