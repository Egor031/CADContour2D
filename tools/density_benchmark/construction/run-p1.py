"""Targeted P1 matrix; existing prepare, worker, measurement and audit formats."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def load(path):
    return json.loads(Path(path).read_text(encoding="utf-8-sig"))


def save(path, value):
    with Path(path).open("x", encoding="utf-8", newline="\n") as file:
        json.dump(value, file, indent=2)
        file.write("\n")


def sha(path):
    with Path(path).open("rb") as file:
        return hashlib.file_digest(file, "sha256").hexdigest()


def main():
    cli = argparse.ArgumentParser()
    cli.add_argument("output", type=Path)
    cli.add_argument("evidence", type=Path)
    cli.add_argument("--baseline", type=Path, required=True)
    args = cli.parse_args()
    root = Path.cwd()
    output, evidence = args.output.resolve(), args.evidence.resolve()
    assert not output.exists() and not evidence.exists()
    output.mkdir()
    evidence.mkdir()
    (evidence / "raw").mkdir()
    (evidence / "tests").mkdir()
    (evidence / ".gitattributes").write_text("*.json -text\n*.csv -text\n*.xml -text\n", encoding="utf-8")
    exe = root / "build/windows-release/density_construction_benchmark.exe"
    prepare = root / "build/windows-release/density_construction_prepare.exe"
    baseline = args.baseline.resolve()
    label = "D: Hitachi HDS721050CLA362 SATA HDD NTFS; see disks.json / volume.json"
    subprocess.run([str(prepare), str(output / "prepared"), "--p1"], check=True)
    fixtures = load(output / "prepared/p1-inputs.json")["fixtures"]
    sources = {f["cachePath"]: sha(f["cachePath"]) for f in fixtures}
    regular = root / "build/task052-resource-42m/source.pc2dcache"
    sources[str(regular)] = sha(regular)
    # Recheck every RecordId/coordinate, produce fresh independent interval counts.
    subprocess.run([str(exe), "--cache", str(regular), "--regular-fixture", "--cell", "0.1",
                    "--tile-size", "256", "--group-tiles", "2", "--repeats", "2",
                    "--output-dir", str(output / "regular-reference"), "--storage-label", label], check=True)
    regular_job = load(output / "regular-reference/job-0.json")
    matrix = []
    for extent, cell in [(16383, 2), (16383, 1), (32767, 1), (131071, 1), (1048575, 1)]:
        matrix.append(dict(name=f"sparse-{extent}-c{cell}", cachePath=fixtures[0]["cachePath"],
                           extent=extent, cell=cell, tile=256, group=2, occupiedLimit=4,
                           candidates=["baseline-C1", "C1", "C2"] if extent == 16383 else ["C1"]))
    for fixture in fixtures[1:]:
        groups = int(fixture["occupiedFixtureGroups"])
        matrix.append(dict(name=fixture["name"], cachePath=fixture["cachePath"],
                           extent=int(groups**.5)*32, cell=1, tile=16, group=2,
                           occupiedLimit=groups, candidates=["baseline-C1", "C1", "C2"]))
    matrix.append(dict(name="42m-512", cachePath=str(regular), tile=256, group=2,
                       occupiedLimit=1000000, candidates=["baseline-C1", "C1", "C2"]))
    save(evidence / "matrix.json", matrix)
    save(evidence / "executables.json", {str(p): sha(p) for p in (exe, prepare, baseline)})
    rows = []
    for item in matrix:
        name = item["name"]
        if name == "42m-512":
            job = regular_job | {"name": name}
        else:
            extent, cell = item["extent"], item["cell"]
            n = (extent+cell-1)//cell
            job = dict(name=name, cachePath=item["cachePath"], grid=dict(bounds=[0, 0, extent, extent],
                cellMm=cell, width=str(n), height=str(n), convention="experimental-min-origin-y-up-half-open-clamped-max-v1"))
        job_path = output / (name + "-job.json")
        save(job_path, job)
        for repeat in range(4):
            candidates = item["candidates"]
            order = candidates[repeat % len(candidates):] + candidates[:repeat % len(candidates)]
            if repeat == 3:
                order = list(reversed(order))
            for candidate in order:
                trial = f"{name}-r{repeat}-{candidate}"
                print(trial, flush=True)
                executable = baseline if candidate == "baseline-C1" else exe
                cmd = [str(executable), "--worker", "C2" if candidate == "C2" else "C1", "--job", str(job_path),
                    "--output-dir", str(output / trial), "--tile-size", str(item["tile"]), "--group-tiles", str(item["group"]),
                    "--ram-mib", "512", "--temp-mib", "1024", "--output-mib", "4096", "--storage-label", label]
                if candidate != "baseline-C1":
                    cmd += ["--max-occupied-groups", str(item["occupiedLimit"])]
                subprocess.run(cmd, check=True)
                for suffix in ("json", "csv"):
                    (evidence / "raw" / (trial + "." + suffix)).write_bytes((output / trial / ("results." + suffix)).read_bytes())
                rows.append(dict(trial=trial, scenario=name, candidate=candidate, repetition=repeat,
                                 runClass="subsequent" if repeat else "first", workerDirectory=str(output / trial), job=str(job_path)))
    save(evidence / "trials.json", rows)
    refusals = []
    for name, executable, scenario, extra in [
        ("baseline-logical-guard", baseline, "sparse-1048575-c1", []),
        ("baseline-output-envelope", baseline, "sparse-32767-c1", []),
        ("occupied-limit", exe, "occupied-64", ["--max-occupied-groups", "1"]),
        ("boundary-work", exe, "sparse-1048575-c1", ["--max-boundary-checks", "1"]),
        ("tiny-ram", exe, "sparse-16383-c1", ["--ram-mib", "1"]),
        ("tiny-scratch", exe, "sparse-16383-c1", ["--temp-mib", "1"]),
        ("tiny-output", exe, "sparse-16383-c1", ["--output-mib", "1"]),
    ]:
        item = next(i for i in matrix if i["name"] == scenario)
        trial = "refused-" + name
        cmd = [str(executable), "--worker", "C1", "--job", str(output / (scenario + "-job.json")),
               "--output-dir", str(output / trial), "--tile-size", str(item["tile"]), "--group-tiles", str(item["group"])] + extra
        assert subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE).returncode == 1
        result = load(output / trial / "results.json")
        assert not result["success"] and not (output / trial / "complete.json").exists()
        assert not any((output / trial).rglob("*.events"))
        refused = [r for r in result["measurements"] if r["status"] != "ok"]
        assert (refused[0]["status"] if refused else result["status"]) == "resource_refused"
        save(evidence / "raw" / (trial + ".json"), result)
        refusals.append(dict(name=name, directory=str(output / trial), status="resource_refused"))
    save(evidence / "refusals.json", refusals)
    after = {path: sha(path) for path in sources}
    assert after == sources
    save(evidence / "source-hashes.json", {path: dict(before=h, after=after[path]) for path, h in sources.items()})


if __name__ == "__main__":
    main()
