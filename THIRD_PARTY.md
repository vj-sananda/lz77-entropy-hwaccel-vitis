# Source provenance

- The board shell, constraints, driver/programming helpers, and host file
  publication behavior are adapted from the user-provided sibling project
  `vitis-file-compression-astra-high`.
- The Vitis L1 LZ77 and match-extension stages come from AMD/Xilinx
  Vitis_Libraries, commit `b2c657d254ead080de401ee33133702f1577424a`. Their license
  and copyright notices are preserved in `vendor/Vitis_Libraries`.
- The local entropy implementation follows RFC 8878 and the Zstandard format
  specification. FSE table construction, sequence ordering, and canonical
  Huffman ordering, and XXH64 checksum arithmetic were cross-checked against Zstandard 1.5.5 sources available
  in `../beezip-fpga-claude/zstd-1.5.5`. The accompanying upstream license is
  retained in `ZSTD_LICENSE`.
- libzstd is linked by the host and tests for frame inspection/decompression;
  the production host does not call its compression APIs.
