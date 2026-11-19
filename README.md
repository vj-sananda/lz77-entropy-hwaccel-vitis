# FPGA Zstandard with hardware entropy coding

`fpga-zstd` compresses files on the YPCB-00338-1P1 PCIe FPGA card
(`xc7k480tffg1156-2`). The FPGA performs **LZ77 matching, Huffman literal
coding, FSE sequence coding, XXH64 content checksums, and Zstandard frame assembly**. The host transfers
blocks and writes the returned frames; it never calls a software compressor.

This extends the design in `../vitis-file-compression-astra-high`, retaining its
Vitis L1 match finder, PCIe Gen2 x8 XDMA shell, 128-bit AXI streams, 50 MHz core,
board constraints, and atomic output publication. The entropy encoder is local
synthesizable C++, rather than the full Vitis Zstd encoder whose failures are
recorded in the reference project.

```bash
make                         # build host utility
make test                    # portable entropy/decoder tests; no FPGA required
make hls                     # Vitis 2026.1 C simulation, synthesis, IP packaging
make rtl-test                # Verilator tests of synthesized RTL
make bitstream               # Vivado implementation; timing checks must pass
make program                 # volatile JTAG programming and PCIe rescan
./build/fpga-zstd --verify input.txt input.txt.zst
zstd -d -c input.txt.zst > recovered.txt
cmp input.txt recovered.txt
make hardware-test           # PCIe, CLI decode, and overwrite regressions
```

See [HOWTO.md](HOWTO.md) for prerequisites and driver setup,
[ARCHITECTURE.md](ARCHITECTURE.md) for the hardware and wire format, and
[VALIDATION.md](VALIDATION.md) for actual verification results and remaining
limits. Build artifacts and reports are local to this directory.

## Utility

```text
fpga-zstd [-f] [--verify] [--device /dev/xdma0] INPUT [OUTPUT.zst]
```

The default output is `INPUT.zst`. `-f` atomically replaces an existing output
only after processing succeeds; aliases of the input are rejected. `--verify`
uses libzstd to decode every returned frame and compare the bytes with the
original input before publishing the file. Files are processed with bounded
memory, one independent frame per block of up to 32 KiB. Empty files produce a
valid empty frame. Standard Zstd decoders support the concatenated output.

The entropy design has a distinct FPGA identity and request/response version.
The host rejects the reference project's LZ-only bitstream before DMA.
The host retries a complete, zero-payload framing-error response once to recover
from XDMA descriptor-ring packet splits. It reports the number recovered.
Persistent errors, invalid responses, and short DMA transfers stop the operation.
There is no CPU compression fallback. After an interrupted DMA operation,
reprogram the FPGA to clear queued stream data.

## Compression choices and limits

- Huffman trees are generated from each block's literal histogram in hardware,
  with up to 11-bit codes. Four reverse bitstreams encode the literals.
- Huffman weights use direct nibble packing for small alphabets and a fixed,
  explicitly described FSE distribution for larger alphabets.
- Literal lengths, match lengths, and offsets use Zstd's predefined FSE tables.
  This is real hardware entropy coding; adaptive sequence table selection and
  repeat-offset optimization are future improvements.
- A single-symbol literal section uses Zstd RLE. Trees deeper than 11 bits or
  unprofitable Huffman encodings use raw literals. If the complete compressed
  block is larger, hardware selects a raw block. All choices produce standard
  Zstd data and require no host encoding.
- Frames include their content size and an XXH64 content checksum computed in
  hardware. `--verify` additionally compares every decompressed byte with the
  original input before publishing the output.
- There is no match history across frames. The initial design processes one
  block at a time and prioritizes correctness and bounded BRAM use. No speedup
  over CPU Zstd is claimed.
