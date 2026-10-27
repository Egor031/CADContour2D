"""P1 extension of the existing exact stream / JSON-CSV audit, no raster allocation."""
import argparse
from collections import Counter
import csv
import importlib.util
import json
from pathlib import Path
import statistics
import struct
import sys
import xml.etree.ElementTree as ET

sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location("construction_audit", Path(__file__).with_name("audit-results.py"))
base = importlib.util.module_from_spec(spec)
spec.loader.exec_module(base)


def sparse_reference(item):
    expected = Counter()
    if item["name"].startswith("sparse-"):
        step = item["cell"]
        n = (item["extent"]+step-1)//step
        for cluster in range(4):
            for y in range(256):
                for x in range(256):
                    # Exact integer fixture intervals; independent of binary-search mapping.
                    cx = min(((cluster % 2)*16128+x)//step, n-1)
                    cy = min(((cluster // 2)*16128+y)//step, n-1)
                    expected[cx, cy] += 1
    else:
        groups = int(item["name"].split("-")[-1])
        axis = int(groups**.5)
        for group in range(groups):
            expected[(group % axis)*32, (group // axis)*32] = 262144//groups
    return expected


def exact_sparse(encoded, side, expected):
    seen = set()
    for tile in encoded:
        x, y, w, h = struct.unpack("<QQII", tile[8:32])
        for i, (value,) in enumerate(struct.iter_unpack("<Q", tile[32:])):
            key = x*side+i % w, y*side+i//w
            assert value == expected.get(key, 0), (key, value, expected.get(key, 0))
            if value:
                seen.add(key)
    # Every stored zero and every nonzero are checked; all omitted tiles are zero.
    assert seen == expected.keys()


def main():
    cli = argparse.ArgumentParser()
    cli.add_argument("evidence", type=Path)
    args = cli.parse_args()
    evidence = args.evidence.resolve()
    assert not (evidence / "audit.json").exists()
    matrix = {item["name"]: item for item in base.load(evidence / "matrix.json")}
    trials = base.load(evidence / "trials.json")
    executables = base.load(evidence / "executables.json")
    for path, digest in executables.items():
        assert base.sha(path) == digest
    root = Path.cwd()
    for item in base.load(evidence / "baseline.json")["sources"]:
        assert base.sha(root / "build/density-p1-baseline-source" / item["path"]) == item["baselineSha256"]
        assert base.sha(root / item["path"]) == item["currentSha256"]
    for path, hashes in base.load(evidence / "source-hashes.json").items():
        assert base.sha(path) == hashes["before"] == hashes["after"]
        if "task052-resource-42m" in path:
            previous = base.load(evidence.parent / "density06c/audit.json")["sourceHashes"]
            assert hashes["before"] == next(h for p, h in previous.items() if "task052-resource-42m" in p)
    refusals = base.load(evidence / "refusals.json")
    assert len(refusals) == 7
    for refusal in refusals:
        worker = Path(refusal["directory"])
        report = base.load(evidence / "raw" / ("refused-"+refusal["name"]+".json"))
        assert not report["success"] and not (worker / "complete.json").exists()
        assert not any(worker.rglob("*.events"))
        failed = [r for r in report["measurements"] if r["status"] != "ok"]
        assert (failed[0]["status"] if failed else report["status"]) == "resource_refused"
    canonical, rows, checks = {}, [], 0
    expected_by_name = {name: sparse_reference(item) for name, item in matrix.items() if name != "42m-512"}
    for trial in trials:
        item = matrix[trial["scenario"]]
        report = base.load(evidence / "raw" / (trial["trial"] + ".json"))
        assert report["success"] and report["environment"]["buildConfiguration"] == "Release"
        assert report["environment"]["toolExecutableSha256"] in executables.values()
        with (evidence / "raw" / (trial["trial"] + ".csv")).open(encoding="utf-8", newline="") as file:
            csv_rows = list(csv.DictReader(file))
        assert len(csv_rows) == len(report["measurements"]) == 2
        for row, flat in zip(report["measurements"], csv_rows):
            assert row["status"] == "ok"
            for key in ("details", "fileIo", "processIo"):
                assert row[key] == json.loads(flat[key])
            for key in ("elapsedMs", "totalElapsedMs", "verificationMs"):
                assert row[key] == float(flat[key])
            for key in ("dataset", "candidate", "phase", "status"):
                assert row[key] == flat[key]
        row = report["measurements"][1]
        row["openPlusBuildMs"] = report["measurements"][0]["elapsedMs"] + row["elapsedMs"]
        assert row["correctness"] is True
        d = row["details"]
        job = base.load(trial["job"])
        worker = Path(trial["workerDirectory"])
        stream, controls = base.stream(worker / "counts.partial", d, job.get("expectedCounts"))
        if trial["scenario"] != "42m-512":
            exact_sparse(stream, item["tile"], expected_by_name[trial["scenario"]])
        if trial["scenario"] in canonical:
            assert canonical[trial["scenario"]] == stream
        else:
            canonical[trial["scenario"]] = stream
        assert base.load(worker / "complete.json") == d["fingerprint"]
        assert not any((worker / "scratch").rglob("*.events"))
        assert int(d["seenPoints"]) == int(d["fingerprint"]["sum"])
        assert int(d["scratchFinalBytes"]) == 0
        if trial["candidate"] != "baseline-C1":
            assert int(d["scratchAllocatedFinalBytes"]) == 0
            occupied = int(d["occupiedGroups"])
            assert int(d["visitedGroups"]) == (int(d["groups"]) if trial["candidate"] == "C2" else occupied)
        if trial["candidate"] != "C2":
            assert int(d["spoolIo"]["readBytes"]) == int(d["spoolIo"]["writeBytes"]) == int(d["scratchLogicalPeakBytes"]) == 4*int(d["seenPoints"])
            assert int(d["cacheReaderScans"]["returnedRecords"]) == int(d["seenPoints"])
            assert int(d["queries"]) == 0
        else:
            assert int(d["queries"]) == int(d["groups"])
            assert int(d["cacheReaderScans"]["returnedRecords"])-int(d["boundaryNeighbourRecordsRejected"]) == int(d["seenPoints"])
        rows.append(row | trial)
        checks += 1
    preparation = base.load(evidence / "raw/regular-reference.json")
    assert preparation["success"]
    with (evidence / "raw/regular-reference.csv").open(encoding="utf-8", newline="") as file:
        preparation_csv = list(csv.DictReader(file))
    assert len(preparation["measurements"]) == len(preparation_csv) == 8
    large_digest = next(r["details"]["fingerprint"]["sha256"] for r in rows if r["scenario"] == "42m-512")
    for row, flat in zip(preparation["measurements"], preparation_csv):
        assert row["status"] == "ok"
        for key in ("details", "fileIo", "processIo"):
            assert row[key] == json.loads(flat[key])
        for key in ("elapsedMs", "totalElapsedMs", "verificationMs"):
            assert row[key] == float(flat[key])
        if row["phase"] == "build_counts":
            assert row["correctness"] and row["details"]["fingerprint"]["oracleAvailable"]
            assert row["details"]["fingerprint"]["sha256"] == large_digest
    summary = []
    for name, item in matrix.items():
        for candidate in item["candidates"]:
            relevant = [r for r in rows if r["scenario"] == name and r["candidate"] == candidate]
            warm = [r["elapsedMs"] for r in relevant if r["runClass"] == "subsequent"]
            assert len(relevant) == 4 and len(warm) == 3
            d = relevant[0]["details"]
            summary.append(dict(scenario=name, candidate=candidate, firstMs=relevant[0]["elapsedMs"],
                medianMs=statistics.median(warm), minMs=min(warm), maxMs=max(warm),
                medianOpenPlusBuildMs=statistics.median(r["openPlusBuildMs"] for r in relevant if r["runClass"] == "subsequent"),
                peakWorkingSetBytes=max(int(r["processPeakWorkingSetBytes"]) for r in relevant), details=d))
    tests = {}
    for config in ("debug", "release"):
        for prefix, expected in (("core", 36), ("formats", 13)):
            path = evidence / "tests" / f"{prefix}-{config}.xml"
            suites = list(ET.parse(path).getroot().iter("testsuite"))
            assert suites and sum(int(s.attrib["tests"]) for s in suites) == expected
            assert all(int(s.attrib.get(k, 0)) == 0 for s in suites for k in ("failures", "errors", "skipped"))
            tests[path.name] = expected
    manifest = [dict(path=str(p.relative_to(evidence)), sha256=base.sha(p), bytes=p.stat().st_size)
                for p in sorted(evidence.rglob("*")) if p.is_file() and p.name != "audit.json"]
    result = dict(status="PASS", exactOracleOutputs=checks, preparationJsonCsvRows=8,
                  resourceRefusals=len(refusals), summary=summary, tests=tests, evidence=manifest)
    with (evidence / "audit.json").open("x", encoding="utf-8", newline="\n") as file:
        json.dump(result, file, indent=2)
        file.write("\n")
    print(f"PASS: {checks} exact oracle outputs; JSON/CSV, sources, digests, sparse traversal, scratch, tests")


if __name__ == "__main__":
    main()
