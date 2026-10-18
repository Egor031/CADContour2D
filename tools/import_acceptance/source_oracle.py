"""Bounded independent source count/hash and binary64 controls for acceptance runs."""

import argparse
from fractions import Fraction
import hashlib
import json
from pathlib import Path
import struct
import sys


def binary64_bits(token):
    text = token.decode("ascii")
    mantissa, _, exponent = text.lower().partition("e")
    if not mantissa.strip("+-.0"):
        value = -0.0 if text.startswith("-") else 0.0
    else:
        # A bounded source token cannot represent finite nonzero binary64 with
        # an exponent beyond this range. Reject before constructing huge powers.
        if exponent:
            digits = exponent.lstrip("+-0")
            if len(digits) > 5 or abs(int(exponent)) > 66000:
                raise ValueError("control exponent outside finite binary64 range")
        value = float(Fraction(text))
    return struct.pack(">d", value).hex()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("--count", type=int, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.count <= 0:
        parser.error("count must be positive")
    ids = {0, 1, 2, 2725, 2726, 2727, 4095, 4096, args.count - 2, args.count - 1}
    ids.update(i * (args.count - 1) // 63 for i in range(64))
    ids = {i for i in ids if 0 <= i < args.count}
    digest = hashlib.sha256()
    records = []
    points = lines = size = 0
    with args.source.open("rb") as source:
        while raw := source.readline(65540):
            if len(raw) > 65539:
                raise ValueError("source line exceeds the bounded oracle input limit")
            digest.update(raw)
            size += len(raw)
            lines += 1
            content = raw.removeprefix(b"\xef\xbb\xbf") if lines == 1 else raw
            if not content.strip():
                continue
            if points in ids:
                tokens = content.split()
                if len(tokens) != 3:
                    raise ValueError("invalid control record")
                # Fraction parses the exact decimal rational. Its __float__ uses
                # integer rounding in CPython, independently of MSVC from_chars.
                bits = [binary64_bits(token) for token in tokens[:2]]
                records.append(
                    {"id": str(points), "physicalLine": str(lines),
                     "x": tokens[0].decode("ascii"), "y": tokens[1].decode("ascii"),
                     "xBits": bits[0], "yBits": bits[1]}
                )
            points += 1
    if points != args.count or len(records) != len(ids):
        raise ValueError("source count/control coverage mismatch")
    result = {
        "source": str(args.source.resolve()),
        "sourceBytes": str(size),
        "sourceLines": str(lines),
        "pointCount": str(points),
        "sha256": digest.hexdigest(),
        "python": sys.version,
        "conversion": "exact decimal Fraction -> CPython Fraction.__float__ -> binary64 bits",
        "records": records,
    }
    with args.output.open("x", encoding="utf-8") as output:
        json.dump(result, output, indent=2)
        output.write("\n")
    print(f"points={points} bytes={size} sha256={result['sha256']} controls={len(records)}")


if __name__ == "__main__":
    main()
