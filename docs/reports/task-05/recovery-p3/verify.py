"""Recovery P3 evidence runner; run from the repository root on Windows/MSVC."""
import argparse
import hashlib
import json
import subprocess
import time
import xml.etree.ElementTree as ET
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--env-cmd", default="build/task02-env.cmd")
parser.add_argument("--out", default="build/recovery-p3/verification")
args = parser.parse_args()
repo = Path.cwd()
env_cmd = (repo / args.env_cmd).resolve()
assert env_cmd.is_file(), "Provide a CMD wrapper that initializes vcvars64, Qt and CMake/Ninja."
out = (repo / args.out).resolve()
out.mkdir(parents=True, exist_ok=False)  # never overwrite an earlier failure
headless = """project_tests cache_tests persistence_tests asc_xyz_tests operation_tests
import_application_tests nonzero_z_confirmation_tests save_tests open_tests recovery_tests
dependency_smoke raster_mapping_tests density_storage_tests""".split()
targets = headless + ["gui_recovery_tests", "gui_import_tests", "gui_file_tests"]
summary = {"baseline": subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip(),
           "sourceSha256": {p: hashlib.sha256((repo / p).read_bytes()).hexdigest() for p in
                            ["tests/recovery_tests.cpp", "tests/recovery_test_access.h"]},
           "runs": [], "inventory": {}}


def save_summary():
    (out / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")


def run(stem, command, junit=False):
    log = out / (stem + ".log")
    full = ["cmd", "/d", "/c", str(env_cmd), *command]
    start = time.monotonic()
    print(stem, flush=True)
    with log.open("wb") as stream:
        result = subprocess.run(full, stdout=stream, stderr=subprocess.STDOUT)
    entry = {"name": stem, "command": full, "exit": result.returncode,
             "seconds": round(time.monotonic() - start, 3),
             "logSha256": hashlib.sha256(log.read_bytes()).hexdigest()}
    if junit:
        xml = out / (stem + ".xml")
        cases = list(ET.parse(xml).iter("testcase"))
        entry.update(cases=len(cases), failed=sum(c.find("failure") is not None for c in cases),
                     skipped=[c.attrib["name"] for c in cases if c.find("skipped") is not None],
                     xmlSha256=hashlib.sha256(xml.read_bytes()).hexdigest())
        entry["passed"] = entry["cases"] - entry["failed"] - len(entry["skipped"])
    summary["runs"].append(entry)
    save_summary()
    print(f"  exit={entry['exit']} " + (f"PASS={entry['passed']} FAIL={entry['failed']} "
          f"SKIP={len(entry['skipped'])}" if junit else ""), flush=True)
    return result.returncode


for config in ["debug", "release"]:
    if run("build-" + config, ["cmake", "--build", "--preset", "windows-" + config,
                              "--target", *targets, "--parallel", "4"]):
        raise SystemExit("Build failed; evidence retained, tests NOT RUN.")

# Discovery is deliberately serial, including across configurations.
for config in ["debug", "release"]:
    assert run("inventory-" + config, ["ctest", "--preset", "windows-" + config, "--show-only=json-v1"]) == 0
    tests = json.loads((out / ("inventory-" + config + ".log")).read_text(encoding="utf-8"))["tests"]
    names = [t["name"] for t in tests]
    assert len(names) == len(set(names)), "Duplicate CTest case names; do not run an ambiguous inventory."
    selected = {"headless": [], "recovery": [], "repeat": []}
    old_cases = {
        "FirstImportAutomaticallyPublishesAfterNativeCompletion",
        "NoRewriteForUnchangedPersistentContentOrUndoBackToRecovery",
        "PeriodicTimerSavesChangedAppliedState", "MatchingExplicitRecoveryRestoresClean",
        "ExplicitSaveReconcilesRecoveryWithoutDestroyingOldRecord",
        "ConflictSurvivesAutosaveRestartUndoAndFailedSaveAs",
        "ClosePreservesRecoveryAndConfirmedCloseDiscards", "CancelledAcceptanceKeepsPreviousProjectAndRecovery",
    }
    for test in tests:
        labels = next((p["value"] for p in test["properties"] if p["name"] == "LABELS"), [])
        if "production" not in labels:
            continue
        exe = Path(test["command"][0]).stem
        name = test["name"]
        if exe in headless:
            selected["headless"].append(name)
        if exe in ["recovery_tests", "gui_recovery_tests"]:
            selected["recovery"].append(name)
        if exe == "recovery_tests" and ("RecoveryScheduling." in name or "RecoveryFault." in name
                or name.rsplit(".", 1)[-1] in old_cases
                or name.endswith("InvalidRecordsAreReportedWithoutMutation/7")):
            selected["repeat"].append(name)
    assert len(selected["repeat"]) == 20
    for kind, cases in selected.items():
        assert cases and len(cases) == len(set(cases))
        (out / f"{kind}-{config}.txt").write_text("\n".join(cases) + "\n", encoding="utf-8")
    summary["inventory"][config] = {"totalUnique": len(names), **{k: len(v) for k, v in selected.items()}}
    save_summary()

for config in ["debug", "release"]:
    def ctest(kind, stem):
        return run(stem, ["ctest", "--preset", "windows-" + config, "-L", "production",
                   "--tests-from-file", str(out / f"{kind}-{config}.txt"), "--output-on-failure",
                   "--output-junit", str(out / (stem + ".xml")), "--parallel", "1"], junit=True)

    ctest("recovery", "recovery-" + config)
    for index in range(1, 21):
        ctest("repeat", f"repeat-{config}-{index:02}")  # all failures retained, no until-pass
    ctest("headless", "headless-" + config)

raise SystemExit(1 if any(r["exit"] for r in summary["runs"]) else 0)
