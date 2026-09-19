#!/usr/bin/env python3
"""Patch configuration literals in a verified minimal ASR image, preserving code.

This recovery tool is for a device image whose exact source is unavailable.
It does not compile firmware. Keep the original image and generated audit JSON.
"""
import argparse
import functools
import hashlib
import ipaddress
import json
import operator
from pathlib import Path
import re
import struct


def layout(data):
    if len(data) < 24 or data[0] != 0xE9 or data[23] != 1:
        raise ValueError("Expected an ESP image with an appended SHA256 digest")
    if struct.unpack_from('<H', data, 12)[0] != 9:
        raise ValueError("Expected an ESP32-S3 image")
    offset, segments = 24, []
    for _ in range(data[1]):
        address, length = struct.unpack_from('<II', data, offset)
        offset += 8
        if offset + length > len(data):
            raise ValueError("Truncated image segment")
        segments.append((address, offset, offset + length))
        offset += length
    checksum_offset = offset | 15
    end = checksum_offset + 33
    if end > len(data):
        raise ValueError("Missing checksum or digest")
    checksum = functools.reduce(operator.xor,
        (byte for _, start, stop in segments for byte in data[start:stop]), 0xEF)
    if checksum != data[checksum_offset]:
        raise ValueError("Image checksum does not match")
    if hashlib.sha256(data[:checksum_offset + 1]).digest() != data[checksum_offset + 1:end]:
        raise ValueError("Image digest does not match")
    if any(byte != 0xFF for byte in data[end:]):
        raise ValueError("Unexpected image trailer; signed images are not supported")
    return segments, checksum_offset, end


def patch(data, *, expected_sha256, old_host, new_host, old_version, new_version, token,
          board='sticks3'):
    if board not in ('sticks3', 'cardputer_adv'):
        raise ValueError("Unsupported recovery board")
    segments, checksum_offset, end = layout(data)
    original = bytes(data[:end])
    if hashlib.sha256(original).hexdigest() != expected_sha256:
        raise ValueError("Source image SHA256 is not the approved backup")
    for host in (old_host, new_host):
        ipaddress.IPv4Address(host)
    if not re.fullmatch(r'[A-Za-z0-9_-]{16,64}', token):
        raise ValueError("Unsupported token format")
    for version in (old_version, new_version):
        if not re.fullmatch(r'\d+\.\d+\.\d+', version):
            raise ValueError("Expected a three-part numeric version")
    if tuple(map(int, new_version.split('.'))) <= tuple(map(int, old_version.split('.'))):
        raise ValueError("The recovery version must increase")
    address, start, stop = segments[0]
    if not 0x3C000000 <= address < 0x3E000000:
        raise ValueError("Expected read-only flash data as the first segment")
    rodata = original[start:stop]
    if ('vibe_stick_' + board).encode() + b'\0' not in rodata or b'sticks3_min\0' not in rodata:
        raise ValueError("Image does not match the selected board's minimal ASR runtime")
    result = bytearray(original)
    changes = []

    def replace(offset, previous, replacement, kind):
        if len(replacement) > len(previous):
            raise ValueError(f"{kind} cannot exceed its existing storage")
        assert original[offset:offset + len(previous)] == previous
        result[offset:offset + len(previous)] = replacement.ljust(len(previous), b'\0')
        changes.append({'kind': kind, 'offset': offset, 'length': len(previous)})

    old = old_host.encode() + b'\0'
    if rodata.count(old) != 1:
        raise ValueError("Expected exactly one old bridge address")
    replace(start + rodata.index(old), old, new_host.encode() + b'\0', 'bridge_host')
    version = old_version.encode() + b'\0'
    positions = [m.start() for m in re.finditer(re.escape(version), rodata)]
    if len(positions) != 2 or rodata[16:48].split(b'\0')[0] != old_version.encode():
        raise ValueError("Expected matching application metadata and HTTP version literals")
    for position in positions:
        replace(start + position, version, new_version.encode() + b'\0', 'version')
    candidates = list(re.finditer(rb'(?<![\x20-\x7e])([0-9a-fA-F]{64})\0X-Vibe-Stick-Token\0', rodata))
    if len(candidates) != 1:
        raise ValueError("Cannot unambiguously locate the existing bridge token")
    match = candidates[0]
    replace(start + match.start(1), match.group(1) + b'\0', token.encode() + b'\0', 'bridge_token')
    result[checksum_offset] = functools.reduce(operator.xor,
        (byte for _, begin, finish in segments for byte in result[begin:finish]), 0xEF)
    result[checksum_offset + 1:end] = hashlib.sha256(result[:checksum_offset + 1]).digest()
    layout(result)
    for _, begin, finish in segments[1:]:
        assert result[begin:finish] == original[begin:finish], "Code or RAM segment changed"
    return bytes(result), {
        'method': 'configuration-only binary recovery; not a source rebuild',
        'board': board,
        'source_sha256': expected_sha256,
        'file_sha256': hashlib.sha256(result).hexdigest(),
        'validation_sha256': bytes(result[checksum_offset + 1:end]).hex(),
        'original_version': old_version, 'version': new_version,
        'old_host': old_host, 'host': new_host, 'size': len(result),
        'non_rodata_segments_unchanged': True, 'changes': changes,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--expected-sha256', required=True)
    parser.add_argument('--board', choices=('sticks3', 'cardputer_adv'), default='sticks3')
    parser.add_argument('--old-host', required=True)
    parser.add_argument('--new-host', required=True)
    parser.add_argument('--old-version', required=True)
    parser.add_argument('--new-version', required=True)
    parser.add_argument('--token-file', type=Path, required=True,
                        help='Private file containing only the bridge token')
    args = parser.parse_args()
    if args.output.exists() or args.output.with_suffix('.audit.json').exists():
        raise SystemExit('Output already exists; preserve previous recovery artifacts')
    image, audit = patch(args.source.read_bytes(), expected_sha256=args.expected_sha256,
        old_host=args.old_host, new_host=args.new_host, old_version=args.old_version,
        new_version=args.new_version, token=args.token_file.read_text().strip(), board=args.board)
    args.output.write_bytes(image)
    args.output.with_suffix('.audit.json').write_text(json.dumps(audit, indent=2) + '\n')
    print(json.dumps(audit, indent=2))


if __name__ == '__main__':
    main()
