#!/usr/bin/env python3
"""Write a binary file as a C array initializer fragment."""

import argparse
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, help="binary input file")
    parser.add_argument("output", type=Path, help="output header fragment")
    args = parser.parse_args()

    data = args.input.read_bytes()
    if not data:
        parser.error("input file must not be empty")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", encoding="ascii", newline="\n") as output:
        for offset in range(0, len(data), 12):
            chunk = data[offset : offset + 12]
            output.write("        ")
            output.write(", ".join(f"0x{byte:02x}" for byte in chunk))
            output.write(",\n")


if __name__ == "__main__":
    main()
