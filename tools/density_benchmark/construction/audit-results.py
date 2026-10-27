"""Independent JSON/CSV, exact tile stream, oracle and source preservation audit."""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import statistics
import struct
import xml.etree.ElementTree as ET


def load(path):
    return json.loads(Path(path).read_text(encoding="utf-8-sig"))


def sha(path):
    with Path(path).open("rb") as file:
        return hashlib.file_digest(file, "sha256").hexdigest()


def stream(path, details, expected):
    grid = details["grid"]
    width, height, side = int(grid["width"]), int(grid["height"]), details["tileSide"]
    tiles = {}
    total = nonzero = maximum = 0
    controls = {(0, 0): 0, (width // 2, height // 2): 0, (width - 1, height - 1): 0}
    actual = [0] * (width * height) if expected is not None else None
    with path.open("rb") as file:
        while header := file.read(32):
            assert len(header) == 32 and header[:8] == b"D06AIN01"
            x, y, w, h = struct.unpack("<QQII", header[8:])
            assert w == min(side, width - x * side) and h == min(side, height - y * side)
            payload = file.read(w * h * 8)
            assert len(payload) == w * h * 8 and (y, x) not in tiles
            values = [v[0] for v in struct.iter_unpack("<Q", payload)]
            assert sum(values) > 0
            total += sum(values)
            nonzero += sum(v != 0 for v in values)
            maximum = max(maximum, max(values))
            for (cx, cy) in controls:
                if x * side <= cx < x * side + w and y * side <= cy < y * side + h:
                    controls[cx, cy] = values[(cy - y * side) * w + cx - x * side]
            if actual is not None:
                for dy in range(h):
                    start = (y * side + dy) * width + x * side
                    actual[start:start + w] = values[dy * w:(dy + 1) * w]
            tiles[y, x] = header + payload
    spec = dict(name="raster-counts", width=str(width), height=str(height), tileSide=str(side),
                seed="0", countBits=64, inputSchema="density-input-06.0A-v1")
    digest = hashlib.sha256(json.dumps(spec, sort_keys=True, separators=(",", ":")).encode())
    canonical = [tiles[key] for key in sorted(tiles)]
    for encoded in canonical:
        digest.update(encoded)
    fp = details["fingerprint"]
    assert digest.hexdigest() == fp["sha256"]
    assert total == int(fp["sum"]) and maximum == int(fp["maximum"])
    assert nonzero == int(fp["nonzeroCells"]) and len(tiles) == int(fp["storedTiles"])
    assert path.stat().st_size == int(fp["outputBytes"])
    if expected is not None:
        assert actual == list(map(int, expected))
    return canonical, [{"x": x, "y": y, "count": str(n)} for (x, y), n in controls.items()]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("matrix_directory", type=Path)
    parser.add_argument("evidence_directory", type=Path)
    parser.add_argument("--executable", type=Path)
    args = parser.parse_args()
    evidence = args.evidence_directory.resolve()
    assert not (evidence / "audit.json").exists(), "audit must be new"
    sources, output_hashes, comparisons, table, cross_group = {}, [], 0, [], {}
    executable = args.executable or args.matrix_directory.resolve().parent / "windows-release" / "density_construction_benchmark.exe"
    executable_hash = sha(executable)
    for item in load(evidence / "matrix.json"):
        name = item["name"]
        report = load(evidence / "raw" / (name + ".json"))
        assert report["success"] is True and report["environment"]["buildConfiguration"] == "Release"
        assert report["environment"]["toolExecutableSha256"] == executable_hash
        for source in report["sources"]:
            path = source["path"]
            if path not in sources:
                sources[path] = sha(path)
            assert sources[path] == source["sha256Before"] == source["sha256After"]
        with (evidence / "raw" / (name + ".csv")).open(newline="", encoding="utf-8") as file:
            csv_rows = list(csv.DictReader(file))
        rows = report["measurements"]
        assert len(csv_rows) == len(rows)
        canonical, pairs = {}, {}
        for row, csv_row in zip(rows, csv_rows):
            assert row["status"] == "ok"
            for key in ("dataset", "candidate", "phase", "repetition", "runClass", "status"):
                assert str(row[key]) == csv_row[key]
            for key in ("elapsedMs", "verificationMs", "totalElapsedMs"):
                assert float(csv_row[key]) == row[key] and row[key] >= 0
            for key in ("details", "fileIo", "processIo"):
                assert json.loads(csv_row[key]) == row[key]
            if row["phase"] == "open":
                assert row["correctness"] is None and csv_row["correctness"] == ""
                continue
            assert row["correctness"] is True and csv_row["correctness"] == "true"
            details = row["details"]
            worker = Path(row["workerDirectory"])
            job_id = worker.name.split("-")[1]
            job = load(worker.parent / ("job-" + job_id + ".json"))
            encoded, controls = stream(worker / "counts.partial", details, job.get("expectedCounts"))
            assert load(worker / "complete.json") == details["fingerprint"]
            assert not any((worker / "scratch").rglob("*.events"))
            assert int(details["seenPoints"]) == int(details["fingerprint"]["sum"])
            if row["candidate"] == "C1":
                assert int(details["scratchLogicalPeakBytes"]) == 4 * int(details["seenPoints"])
                assert int(details["spoolIo"]["readBytes"]) == int(details["spoolIo"]["writeBytes"]) == int(details["scratchLogicalPeakBytes"])
                assert int(details["cacheReaderScans"]["returnedRecords"]) == int(details["seenPoints"])
            else:
                assert int(details["queries"]) == int(details["groups"])
                assert int(details["scratchLogicalPeakBytes"]) == 0
                assert int(details["cacheReaderScans"]["returnedRecords"]) - int(details["boundaryNeighbourRecordsRejected"]) == int(details["seenPoints"])
            assert int(details["scratchFinalBytes"]) == 0
            dataset = row["dataset"]
            identity = (job["cachePath"], json.dumps(details["grid"], sort_keys=True), details["tileSide"])
            if identity in cross_group:
                assert cross_group[identity] == details["fingerprint"]["sha256"]
            else:
                cross_group[identity] = details["fingerprint"]["sha256"]
            if dataset in canonical:
                assert encoded == canonical[dataset]
                comparisons += 1
            else:
                canonical[dataset] = encoded
            pair_key = dataset, row["repetition"]
            parameters = (details["grid"], details["tileSide"], details["groupTiles"])
            if pair_key in pairs:
                assert pairs[pair_key] == parameters
            else:
                pairs[pair_key] = parameters
            output_hashes.append(dict(run=name, dataset=dataset, candidate=row["candidate"], repetition=row["repetition"],
                                      sha256=details["fingerprint"]["sha256"], controls=controls))
        for dataset in canonical:
            for candidate in ("C1", "C2"):
                relevant = [r for r in rows if r["dataset"] == dataset and r["candidate"] == candidate and r["phase"] == "build_counts"]
                warm = [r for r in relevant if r["runClass"] == "subsequent"]
                assert len(relevant) == 4 and len(warm) == 3
                d = warm[0]["details"]
                table.append(dict(run=name, dataset=dataset, candidate=candidate,
                                  firstMs=relevant[0]["elapsedMs"], medianMs=statistics.median(r["elapsedMs"] for r in warm),
                                  minimumMs=min(r["elapsedMs"] for r in warm), maximumMs=max(r["elapsedMs"] for r in warm),
                                  peakWorkingSetBytes=max(int(r["processPeakWorkingSetBytes"]) for r in relevant),
                                  cacheLogicalBytes=int(d["cacheReaderScans"]["readBytes"]), pointPages=int(d["cacheReaderScans"]["pointPages"]),
                                  indexPages=int(d["cacheReaderScans"]["indexPages"]), seeks=int(d["cacheReaderScans"]["seekOperations"]),
                                  scratchBytes=int(d["scratchLogicalPeakBytes"]), spoolOpens=int(d["spoolFileOpens"]), queries=int(d["queries"]),
                                  points=int(d["seenPoints"])))
    test_counts = {}
    for filename, expected_count in (("core-debug.xml", 34), ("core-release.xml", 34),
                                     ("formats-debug.xml", 13), ("formats-release.xml", 13)):
        suites = list(ET.parse(evidence / "tests" / filename).getroot().iter("testsuite"))
        assert suites and all(int(s.attrib.get("failures", 0)) == 0 and int(s.attrib.get("errors", 0)) == 0
                              and int(s.attrib.get("skipped", 0)) == 0 for s in suites)
        count = sum(int(s.attrib["tests"]) for s in suites)
        assert count == expected_count
        test_counts[filename] = count
    manifest = [{"path": str(p.relative_to(evidence)), "bytes": p.stat().st_size, "sha256": sha(p)}
                for p in sorted(evidence.rglob("*")) if p.is_file() and p.suffix in (".json", ".csv", ".xml")]
    result = dict(status="PASS", executableSha256=executable_hash, sourceHashes=sources, exactStreamComparisons=comparisons,
                  outputs=output_hashes, summary=table, testCounts=test_counts, evidence=manifest)
    (evidence / "audit.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(f"PASS: {len(output_hashes)} outputs, {comparisons} exact comparisons, {len(sources)} preserved sources")


if __name__ == "__main__":
    main()
