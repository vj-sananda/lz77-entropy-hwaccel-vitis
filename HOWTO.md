# Build and use

Use the Xilinx 2026.1 tools on PATH (`v++`, `vitis-run`, `vivado`), a C++17
compiler, Python 3, Verilator, `libzstd-dev`, and the `zstd` command-line utility.
The target is the YPCB-00338-1P1 with `xc7k480tffg1156-2`; install its board files
from `../fpga-board-ypcb_00338_1p1` in the Vivado board repository if necessary.
The supplied physical constraints are for that board.

## Software tests and hardware build

```bash
make
make test
make transport-test # bounded retry and fatal-error handling; no FPGA required
make csim       # C simulation only; includes complete AXI transaction model
make hls        # reruns C simulation, then synthesizes and packages the HLS IP
make rtl-test   # needs make hls to have completed
make bitstream  # needs packaged HLS IP; runs synthesis, implementation, timing gate
```

`make hls` fetches the Vitis L1 source at commit
`b2c657d254ead080de401ee33133702f1577424a` if absent. The supplied workspace
already contains that source, copied from the reference project. The script
checks the revision and prepares a BRAM-only include tree in
`build/vitis_include`. It does not use the full Vitis Zstd entropy encoder.

Vivado uses two jobs by default to limit memory use. Set `FPGA_BUILD_JOBS=4`
for a dedicated machine with sufficient RAM.

FSE ROMs are checked in and do not require generation during normal builds.
To regenerate them, run `python3 scripts/generate_fse_tables.py`. The generator
has no third-party Python dependencies.

C simulation and HLS synthesis logs are in `reports/hls_*.log`. Verilator logs
are `reports/verilator_build.log` and `reports/rtl_roundtrip.log`. Implementation
reports include `timing_summary.rpt`, `bus_skew.rpt`, `utilization.rpt`, and
`drc.rpt`. A failing timing gate does not publish a new top-level bitstream.

## Driver and programming

If the existing reference accelerator works on this machine, its loaded XDMA
driver also supports this design. On a new machine build the pinned driver:

```bash
bash scripts/build_driver.sh
```

This uses AMD/Xilinx dma_ip_drivers commit
`a918c5196a09f95756af244a607d543d9dce5bfb`. Secure Boot needs a locally enrolled
signing key; `host_setup.sh` uses the same Ubuntu MOK paths as the reference
project when Secure Boot is enabled. Adapt those paths for a different host.

Close clients using the card, then run:

```bash
make program
```

The helper uses `sudo -n` to detach the enumerated Xilinx endpoint, programs
volatile FPGA SRAM through JTAG, rescans PCIe, loads XDMA if needed, and grants
the invoking user access to the device nodes. It refuses to detach a card with
open XDMA devices and requires a single matching card/JTAG target. It preserves
the board's boot flash. Programming replaces whatever design is currently in
SRAM, and the boot image may return after a power cycle.

## File compression and regression

```bash
./build/fpga-zstd --verify input.txt archive.zst
zstd -d -c archive.zst > recovered.txt
cmp input.txt recovered.txt
./build/fpga-zstd -f --verify input.txt archive.zst
make hardware-test
```

An advisory exclusive lock on H2C prevents concurrent cooperating compressors.
Do not run unrelated DMA clients concurrently. If identity verification fails,
load this project's `build/accelerator.bit`; the LZ-only reference image is
intentionally incompatible. After a partial DMA, killed process, timeout, or
protocol mismatch, reprogram before retrying.

The utility supports regular input files, preserves inputs, and publishes the
output through a temporary file in the destination directory. Existing files
require `-f`; without it, final publication also protects against a destination
appearing during compression. There is no stdin/stdout or decompression mode.
The reported elapsed time includes optional software verification and file
publication; it is not a measurement of raw entropy-core throughput.

Large transfers may print `DMA framing retries recovered: N`. The pinned XDMA
driver inserts an extra packet-end marker when a write crosses its descriptor
ring boundary. The utility retries a fully drained framing rejection once;
this counter records successful recoveries. A persistent error reports the
FPGA status, zero-based block index, and input byte offset. `make hardware-test`
includes a 16 MiB stream to exercise this behavior beyond the original small-file
tests.
