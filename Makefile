CXX ?= g++
CXXFLAGS ?= -O2 -std=c++17 -Wall -Wextra -Wpedantic
.PHONY: all test entropy-test transport-test csim hls rtl-test bitstream program hardware-test clean
all: build/fpga-zstd
build/fpga-zstd: host/fpga_zstd.cpp host/transport.hpp hls/protocol.hpp
	mkdir -p build
	$(CXX) $(CXXFLAGS) $< -o $@ -lzstd
build/entropy-test: tests/entropy.cpp hls/checksum.hpp hls/entropy.hpp hls/fse_tables.hpp hls/protocol.hpp
	mkdir -p build
	$(CXX) $(CXXFLAGS) -Wno-unknown-pragmas $< -o $@ -lzstd
entropy-test: build/entropy-test
	./build/entropy-test
build/transport-test: tests/transport.cpp host/transport.hpp hls/protocol.hpp
	mkdir -p build
	$(CXX) $(CXXFLAGS) $< -o $@
transport-test: build/transport-test
	./build/transport-test
test: all entropy-test transport-test
csim:
	bash scripts/build_hls.sh csim
hls:
	bash scripts/build_hls.sh
rtl-test:
	bash scripts/simulate_rtl.sh
bitstream:
	mkdir -p reports
	vivado -mode batch -nojournal -log reports/vivado.log -source scripts/build.tcl
program:
	bash scripts/program.sh
hardware-test: all
	python3 tests/hardware_roundtrip.py
	python3 tests/overwrite.py
clean:
	rm -f build/fpga-zstd build/entropy-test build/transport-test
