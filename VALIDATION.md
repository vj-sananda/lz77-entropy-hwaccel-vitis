# Validation

Results are from this directory's implementation, not copied results from the
LZ-only reference. The independent decoder is system libzstd 1.5.5.

| Check | Result |
| --- | --- |
| Host build (`make`) | PASS, C++17 with warnings enabled |
| Portable entropy tests (`make test`) | PASS, 588 cases |
| Address/undefined-behavior sanitizers | PASS, 588 entropy cases and 16 transport cases |
| Host transport recovery | PASS, 16 retry/fatal-error cases |
| Full accelerator C simulation | PASS, 137 checksummed transactions |
| HLS synthesis and IP packaging | PASS, including hardware checksums |
| Generated RTL with AXI backpressure | PASS, 57 checksummed transactions |
| Routed timing and bitstream | PASS, 50 MHz core; setup +0.010 ns, hold +0.009 ns |
| PCIe file round trips | PASS, 60 files / 645 frames; independent CLI decode |
| User archive (`beezip.tar`) | PASS, 220,856,320 bytes / 6,740 frames; independent CLI decode |
| Overwrite and failure preservation | PASS, all seven regression groups |

Portable tests pass input and explicitly constructed sequences to the exact
synthesizable entropy functions and decode the resulting frames with libzstd.
They cover size boundaries, empty input, single-symbol input, sparse and full
alphabets, inverted high-byte alphabets, randomized match distances/lengths,
full-alphabet skew, long literal-run code boundaries, 10,922 sequences in one
block, and a Huffman tree deeper than the supported limit. Coverage
includes 479 compressed blocks, 314 Huffman sections, 335 blocks with FSE
sequences, and 58 FSE-compressed Huffman weight tables.

Full C simulation uses the actual Vitis LZ77 stages and AXI packet wrapper.
It passed 137 transactions: 131 valid input cases, five invalid requests, and
one valid recovery request following those errors. The valid outputs include
107 compressed blocks and 28 Huffman literal sections. Invalid cases cover
signature, size, reserved fields, byte enables, and TLAST. Every output is
independently decompressed, and response sidebands/padding are checked. The
retained report is `reports/hls_csim.log`.

The host executable imports only `ZSTD_decompress`, `ZSTD_getFrameContentSize`,
`ZSTD_getErrorName`, and `ZSTD_isError` from libzstd, confirmed with `nm -D`.
There are no compression API calls in the production host. FSE ROM regeneration
is deterministic.

Reproduce the sanitizer test with:

```bash
g++ -O1 -g -std=c++17 -Wno-unknown-pragmas \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  tests/entropy.cpp -o build/entropy-sanitize -lzstd
ASAN_OPTIONS=detect_leaks=1 ./build/entropy-sanitize
```

The initial architecture is serialized and uses predefined sequence tables.
Neither functional tests nor HLS estimates demonstrate a performance advantage
over CPU Zstd. Frames include a hardware XXH64 content checksum; `--verify` additionally
compares hardware output against original input.

HLS estimates: 37,523 LUTs, 18,869 FFs, 284 BRAM18s, 40 DSPs, and
16.148 ns logic delay against a 20 ns target with 2.5 ns uncertainty. These
estimates cover the accelerator IP, excluding the PCIe shell.

The synthesized RTL passed 57 transactions over 12,757,238 cycles with
randomized input gaps, randomized output backpressure, and 512-cycle output
stalls. The test asserts output stability while stalled, checks byte enables,
TLAST and padding, decodes every valid frame, and verifies recovery after five
invalid requests. Coverage includes 27 compressed blocks and eight Huffman
sections. See `reports/rtl_roundtrip.log`. The simulator runs in the generated
RTL directory so HLS's relative ROM initialization paths resolve correctly.

The host was also tested against the still-loaded LZ-only reference image: it
rejected the FPGA identity before DMA and did not publish an output file.

Hardware XXH64 checksums passed the portable, C-simulation, and RTL tests.
All 588 portable cases also verify that a deliberately corrupted checksum is
rejected by libzstd.

The first placement attempt exposed a 26.385 ns checksum path. The final
checksum source explicitly uses four-cycle DSP multipliers; the revised
design passed all portable, C-simulation, and RTL checks again. The diagnostic
placement report is retained under `reports/initial_timing/`.

Final Vivado implementation passed setup, hold, pulse-width, and bus-skew
checks. The complete design, including PCIe, uses 35,241 LUTs (11.80%),
37,717 registers (6.32%), 178 block-RAM tiles (18.64%), and 40 DSPs (2.08%).
The timing-gated build publishes `build/accelerator.bit` and
`build/accelerator.xsa`; see `reports/timing_summary.rpt`,
`reports/bus_skew.rpt`, and `reports/utilization.rpt`.

The bitstream was programmed into volatile FPGA SRAM on November 13, 2026.
JTAG reported DONE=1 and CRC_ERROR=0; PCIe enumerated at 5 GT/s, x8. The
board remains loaded with this accelerator. Reports are `reports/program.log`
and `reports/program_run.log`.

Physical PCIe testing passed all 60 files, covering empty and tiny inputs,
32 KiB boundaries, multiple frames, text, random data, all single-symbol cases
listed in the test, small alphabets, high-byte alphabets, and a 16 MiB
descriptor-ring regression. All 645 frames
passed host verification and independent `zstd -d` decoding. Inspection found
59 raw blocks, 508 Huffman literal sections, 21 RLE literal sections, and
182,425 FSE-encoded sequences. Two framing rejections were recovered. See `reports/hardware_test.log` and
`reports/hardware_roundtrip.json` for exact inputs, hashes, sizes, and timings.
The seven overwrite-regression groups also passed, including preservation
after device-open and output-write failures and protection of input aliases.

Representative physical-board results:

| Input | Input bytes | Zstd bytes | Verified end-to-end MiB/s |
| --- | ---: | ---: | ---: |
| Vendored Zstd C++ header | 44,712 | 14,422 | 2.94 |
| Repeated text | 1,048,576 | 28,847 | 5.63 |
| Random bytes | 1,048,576 | 1,049,024 | 2.75 |

These are single-run observations including DMA, optional software decoding,
and file publication, not isolated core throughput measurements. The header
example is retained as `reports/demo/original.hpp`, `original.hpp.zst`, and
`recovered.hpp`. Its verified size reduction is 67.74%.

`reports/source_sha256.json` records implementation, test, and build-source
hashes. `reports/artifact_sha256.json` records the host executable, packaged
HLS IP, published bitstream, and hardware-platform hashes.

## Large-file transport correction

The user's `beezip.tar` exposed a coverage gap in the original 59-file suite:
that suite sent only 133 frames and did not exercise an H2C descriptor-ring
split. Reproductions failed with status 2 at zero-based block 226 (offset
7,405,568) and then block 227 (offset 7,438,336), rather than consistently
failing on one input block.

The pinned XDMA driver's `transfer_init()` caps a descriptor chain at the
2048-entry ring boundary and sets `XDMA_DESC_EOP` on its last descriptor,
even when more descriptors remain in the same userspace write. This inserts
an early TLAST. The FPGA correctly rejects the request, drains its fixed
32784 bytes, and returns status 2 with a zero-length, zero-padded response.

`host/transport.hpp` now validates that complete rejection and resends the
same block once. It preserves the request tag and input/output positions.
Short DMA, wrong signatures/tags, nonzero error payloads, invalid frame
lengths, other statuses, and a second framing rejection remain fatal. The
FPGA and kernel driver were not changed. Errors now identify the block,
byte offset, and FPGA status; successful invocations report recovered retries.

All 16 targeted transport tests passed, including ASan/UBSan. The extended
hardware suite passed 60 files and all seven overwrite regression groups;
its 16 MiB case alone exercises 512 requests. The original reports and
manifests are preserved in `reports/before_transport_fix/`.

The exact user command completed successfully and published `junk.zst`:
220,856,320 input bytes became 103,177,241 bytes across 6,740 frames, with
26 framing retries recovered. End-to-end time with `--verify` was 55.908 s
(3.77 MiB/s). A separate `zstd -d` run was compared byte-for-byte with the
entire original archive and passed. See `reports/beezip_roundtrip.log` and
`reports/beezip_roundtrip.json` for the result and SHA-256 hashes.
