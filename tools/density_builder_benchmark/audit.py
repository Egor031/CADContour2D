"""Validate full-pipeline evidence; this does not replace the C++ spatial oracle."""

import json
import sys


def audit(path):
    with open(path, encoding="utf-8") as stream:
        report = json.load(stream)
    assert report["schema"] == "density-builder-06.3A-v1"
    rows = report["rows"]
    assert len(rows) == 9
    assert report["status"] == "PASS"
    profiles = {"compact": 1, "irregular64": 64, "irregular256": 256,
                "sparse64": 64, "fixed-area-16": 16, "fixed-area-128": 128}
    for row in rows:
        assert row["status"] == "PASS", row
        assert row["cellMm"] == 1 and row["tileSide"] == 256 and row["groupSide"] == 512
        count = int(row["N"])
        assert count >= 4096
        stats = row["statistics"]
        for key in ("scanned", "eventsWritten", "eventsRead", "accumulated", "tileTotal", "densityTotal"):
            assert int(stats[key]) == count, (row["profile"], key, stats[key], count)
        assert int(stats["scanAllCalls"]) == 1 and int(stats["spatialQueries"]) == 0
        assert int(stats["occupiedGroups"]) == int(stats["visitedGroups"]) == profiles[row["profile"]]
        assert int(stats["eventBytesWritten"]) == int(stats["eventBytesRead"]) == 4 * count
        verify = row["verification"]
        assert verify["exactSpatialCounts"] is True and int(verify["total"]) == count
        assert int(verify["physicalTilesChecked"]) == int(stats["tiles"]) == int(stats["densityPhysicalTiles"])
        assert int(stats["densitySubmissions"]) == int(stats["tiles"])
        assert 0 < int(verify["nonzeroCellsChecked"]) <= count
        assert int(stats["sourcePointPages"]) == (count + 2725) // 2726
        assert int(row["sourceBytes"]) >= int(stats["sourcePointPages"]) * 65536 + 8192
        assert int(stats["sourceReadBytes"]) == (int(stats["sourcePointPages"]) + int(stats["sourceIndexPages"])) * 65536 + 8192
        assert int(stats["densityFileBytes"]) >= 384 + 80 * int(stats["tiles"])
        assert int(stats["densityOpenOperations"]) == 5
        assert int(stats["densityReadOperations"]) > 0 and int(stats["densityWriteOperations"]) > 0
        assert row["wallMs"] >= 0 and row["processCpuMs"] >= 0 and row["verificationMs"] >= 0
        policy = row["policy"]
        assert 0 < int(stats["sourceBufferReserveBytes"]) <= int(policy["readerBytes"])
        for usage, limit in (("workingPeakBytes", "workingBytes"), ("metadataPeakBytes", "metadataBytes"),
                             ("scratchPeakBytes", "scratchBytes"), ("combinedDiskPeakBytes", "combinedDiskBytes"),
                             ("filesPeak", "files"), ("handlesPeak", "handles"), ("workUnits", "workUnits"),
                             ("densityFileBytes", "outputBytes")):
            assert 0 <= int(stats[usage]) <= int(policy[limit]), (usage, stats[usage], policy[limit])
    representative = [r for r in rows if r["profile"] == "irregular64" and int(r["eventCapacity"]) == 65536]
    assert sorted(r["repetition"] for r in representative) == [1, 2, 3]
    assert all(r["runClass"] == "representative-repeat" for r in representative)
    for row in representative[1:]:
        for key in ("N", "sourceBytes", "width", "height", "verification"):
            assert row[key] == representative[0][key]
    compact = next(r for r in rows if r["profile"] == "compact")
    assert int(compact["verification"]["nonzeroCellsChecked"]) == 3
    sparse = next(r for r in rows if r["profile"] == "sparse64")
    assert int(sparse["verification"]["implicitZeroTilesChecked"]) == 1
    small = next(r for r in rows if r["profile"] == "irregular64" and int(r["eventCapacity"]) == 1024)
    base = representative[0]
    assert int(sparse["width"]) > int(base["width"]) and int(sparse["height"]) > int(base["height"])
    for key in ("occupiedGroups", "visitedGroups", "tiles", "eventBytesWritten", "eventBytesRead"):
        assert sparse["statistics"][key] == base["statistics"][key]
    assert small["verification"] == base["verification"]
    assert small["N"] == base["N"] and small["sourceBytes"] == base["sourceBytes"]
    fixed = [r for r in rows if r["profile"].startswith("fixed-area-")]
    assert len(fixed) == 2 and fixed[0]["width"] == fixed[1]["width"] and fixed[0]["height"] == fixed[1]["height"]
    assert int(fixed[0]["statistics"]["occupiedGroups"]) < int(fixed[1]["statistics"]["occupiedGroups"])
    assert all(report["pairedChecks"].values())
    print(f"PASS: {path}: {len(rows)} builds, exact spatial checks, resources and paired comparisons")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("Usage: audit.py <evidence.json>")
    audit(sys.argv[1])
