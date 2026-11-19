#!/usr/bin/env python3
"""End-to-end PCIe compression, independent zstd CLI decode, and exact comparison."""
import hashlib
import json
from pathlib import Path
import random
import re
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parent.parent
REPORT = ROOT / 'reports/hardware_roundtrip.json'
REPORT.parent.mkdir(parents=True, exist_ok=True)
random_bytes = random.Random(42)
text = (b'FPGA accelerated Zstandard compression with AMD Vitis L1.\n'
        b'The PCIe card compresses these bytes; software verifies lossless recovery.\n')
cases = [('empty', b'')]
for size in (1, 16, 63, 64, 65, 128, 129, 255, 256, 4096, 32767, 32768, 32769, 65536):
    cases.extend([(f'text_{size}', (text * (size // len(text) + 1))[:size]),
                  (f'random_{size}', random_bytes.randbytes(size)),
                  (f'repeated_{size}', b'A' * size)])
cases += [('readme', (ROOT / 'README.md').read_bytes()),
          ('library_source', (ROOT / 'vendor/Vitis_Libraries/data_compression/L1/include/hw/zstd_compress.hpp').read_bytes()),
          ('text_1MiB', (text * (1048576 // len(text) + 1))[:1048576]),
          ('random_1MiB', random_bytes.randbytes(1048576))]
# Hundreds of requests cross XDMA's 2048-descriptor ring with the normal
# multi-page buffers, exposing the extra H2C TLAST inserted at ring wrap.
cases.append(('descriptor_wrap_16MiB', (text * (16777216 // len(text) + 1))[:16777216]))
for byte in (0, 1, 3, 4, 127, 128, 255):
    cases.append((f'single_symbol_{byte}', bytes([byte]) * 32768))
for alphabet in (2, 8, 32):
    cases.append((f'alphabet_{alphabet}', bytes(random_bytes.randrange(alphabet) for _ in range(65536))))
for maximum in (200, 254):
    cases.append((f'even_maximum_{maximum}',
                  bytes(maximum-31+random_bytes.randrange(32) for _ in range(65536))))

def inspect_frames(encoded):
    """Inspect the accelerator's one-block, single-segment frames."""
    counts = dict(frames=0, raw_blocks=0, huffman_sections=0, rle_sections=0,
                  fse_sequences=0)
    pos = 0
    while pos < len(encoded):
        assert encoded[pos:pos+4] == bytes.fromhex('28b52ffd')
        descriptor = encoded[pos+4]
        assert descriptor in (0x24, 0x64)
        header = pos + (6 if descriptor == 0x24 else 7)
        block = int.from_bytes(encoded[header:header+3], 'little')
        assert block & 1
        kind, size = (block >> 1) & 3, block >> 3
        begin = header+3
        end = begin+size
        assert end <= len(encoded)
        if kind == 0:
            counts['raw_blocks'] += 1
        else:
            assert kind == 2
            literal_type = encoded[begin] & 3
            fmt = (encoded[begin] >> 2) & 3
            if literal_type in (0, 1):
                hsize = 1 if fmt in (0, 2) else 2 if fmt == 1 else 3
                value = int.from_bytes(encoded[begin:begin+hsize], 'little')
                regenerated = value >> (3 if hsize == 1 else 4)
                sequence_start = begin+hsize+(regenerated if literal_type == 0 else 1)
                counts['rle_sections'] += literal_type == 1
            else:
                assert literal_type == 2 and fmt == 3
                value = int.from_bytes(encoded[begin:begin+5], 'little')
                sequence_start = begin+5+(value >> 22)
                counts['huffman_sections'] += 1
            assert sequence_start < end
            first = encoded[sequence_start]
            sequences = first if first < 128 else ((first-128) << 8) + encoded[sequence_start+1]
            if sequences:
                modes = encoded[sequence_start+(1 if first < 128 else 2)]
                assert modes == 0, 'Expected predefined FSE tables'
            counts['fse_sequences'] += sequences
        counts['frames'] += 1
        pos = end+4  # hardware XXH64 content checksum
    assert pos == len(encoded)
    return counts

records = []
with tempfile.TemporaryDirectory(prefix='fpga-zstd-test-') as folder:
    folder = Path(folder)
    for name, data in cases:
        original = folder / name
        compressed = folder / (name + '.zst')
        original.write_bytes(data)
        start = time.monotonic()
        run = subprocess.run([str(ROOT / 'build/fpga-zstd'), '--verify', str(original), str(compressed)],
                             capture_output=True, text=True, timeout=120)
        if run.returncode:
            raise RuntimeError(f'{name}: compressor failed\n{run.stdout}{run.stderr}')
        decoded = subprocess.run(['zstd', '-q', '-d', '-c', str(compressed)],
                                 check=True, capture_output=True, timeout=30).stdout
        assert decoded == data, name
        retried = re.search(r'DMA framing retries recovered: (\d+)', run.stdout)
        record = dict(name=name, input_bytes=len(data), compressed_bytes=compressed.stat().st_size,
                      sha256=hashlib.sha256(decoded).hexdigest(),
                      compressed_sha256=hashlib.sha256(compressed.read_bytes()).hexdigest(),
                      seconds=time.monotonic() - start,
                      framing_retries=int(retried.group(1)) if retried else 0,
                      result='PASS', utility_output=run.stdout,
                      entropy=inspect_frames(compressed.read_bytes()))
        records.append(record)
        REPORT.write_text(json.dumps(records, indent=2) + '\n')
        print(f'PASS {name}: {len(data)} -> {compressed.stat().st_size} bytes; '
              f"{record['framing_retries']} framing retries", flush=True)
        if name == 'library_source':
            demo = ROOT / 'reports/demo'
            demo.mkdir(exist_ok=True)
            (demo / 'original.hpp').write_bytes(data)
            (demo / 'original.hpp.zst').write_bytes(compressed.read_bytes())
            (demo / 'recovered.hpp').write_bytes(decoded)
    # Failed invocations must not overwrite an existing destination.
    preserved = compressed.read_bytes()
    run = subprocess.run([str(ROOT / 'build/fpga-zstd'), str(original), str(compressed)], capture_output=True)
    assert run.returncode != 0 and compressed.read_bytes() == preserved
assert sum(r['entropy']['huffman_sections'] for r in records) > 0
assert sum(r['entropy']['fse_sequences'] for r in records) > 0
print(f'HARDWARE_ROUNDTRIP_PASS: {len(records)} cases; report {REPORT}')
