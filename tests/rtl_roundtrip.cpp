// Cycle-level verification of generated RTL: no CPU compression path.
#include "Vzstd_accel.h"
#include <verilated.h>
#include <zstd.h>
#include "../hls/protocol.hpp"
#include "cases.hpp"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
using Word=std::array<uint32_t,4>;
static void check(bool ok,const char* message) {
    if (!ok) { std::fprintf(stderr,"RTL FAIL: %s\n",message); std::exit(1); }
}
int main(int argc,char** argv) {
    Verilated::commandArgs(argc,argv);
    Vzstd_accel dut;
    std::mt19937 random(12345);
    auto cases=test_cases(false);
    unsigned valid_count=cases.size();
    for (unsigned i=0;i<6;++i) cases.emplace_back(1024,'x');
    std::vector<Word> requests;
    std::vector<bool> lasts;
    std::vector<uint16_t> keeps;
    for (unsigned tag=0;tag<cases.size();++tag) {
        unsigned bad=tag>=valid_count ? tag-valid_count+1 : 0;
        requests.push_back({protocol::REQUEST_MAGIC+(bad==1),bad==2 ? 32769 : uint32_t(cases[tag].size()),tag,bad==3 ? 1u : 0u});
        lasts.push_back(false); keeps.push_back(65535);
        for (unsigned pos=0;pos<protocol::BLOCK;pos+=16) {
            Word word{};
            for (unsigned k=0;k<16;++k)
                if (pos+k<cases[tag].size()) word[k/4]|=uint32_t(cases[tag][pos+k])<<(8*(k%4));
            requests.push_back(word);
            lasts.push_back(pos==protocol::BLOCK-16 || (bad==5 && pos==0));
            keeps.push_back(bad==4 && pos==32 ? 0 : 65535);
        }
    }
    dut.ap_clk=0;dut.ap_rst_n=0;dut.ap_start=0;
    dut.input_r_TVALID=0;dut.output_r_TREADY=0;
    for (unsigned cycle=0;cycle<20;++cycle) {
        dut.ap_clk=0;dut.eval();dut.ap_clk=1;dut.eval();
    }
    dut.ap_rst_n=1;dut.ap_start=1;
    size_t sent=0,received=0;
    unsigned tag=0,huffman=0,compressed=0;
    bool holding=false,stalled=false;
    Word previous{};
    bool previous_last=false;
    std::vector<unsigned char> response;
    constexpr unsigned RESPONSE_WORDS=protocol::CAPACITY/16+1;
    for (uint64_t cycle=0;cycle<100000000;++cycle) {
        dut.ap_clk=0;
        if (!holding && sent<requests.size() && random()%5!=0) holding=true;
        dut.input_r_TVALID=holding;
        dut.input_r_TKEEP=holding ? keeps[sent] : 65535;
        dut.input_r_TSTRB=65535;
        dut.input_r_TLAST=holding && lasts[sent];
        if (holding) for (unsigned k=0;k<4;++k) dut.input_r_TDATA[k]=requests[sent][k];
        dut.output_r_TREADY=random()%7!=0 && cycle%8192>=512;
        dut.eval();
        bool tx=dut.input_r_TVALID && dut.input_r_TREADY;
        if (stalled) {
            check(dut.output_r_TVALID && bool(dut.output_r_TLAST)==previous_last,"output changed under backpressure");
            for (unsigned k=0;k<4;++k) check(dut.output_r_TDATA[k]==previous[k],"data changed under backpressure");
        }
        stalled=dut.output_r_TVALID && !dut.output_r_TREADY;
        if (stalled) {
            for (unsigned k=0;k<4;++k) previous[k]=dut.output_r_TDATA[k];
            previous_last=dut.output_r_TLAST;
        }
        if (dut.output_r_TVALID) check(dut.output_r_TKEEP==65535 && dut.output_r_TSTRB==65535,"output byte enables");
        if (dut.output_r_TVALID && dut.output_r_TREADY) {
            check(bool(dut.output_r_TLAST)==(received%RESPONSE_WORDS==RESPONSE_WORDS-1),"TLAST");
            for (unsigned k=0;k<16;++k) response.push_back(dut.output_r_TDATA[k/4]>>(8*(k%4)));
            ++received;
            if (received%RESPONSE_WORDS==0) {
                uint32_t header[4];std::memcpy(header,response.data(),16);
                unsigned bad=tag>=valid_count ? tag-valid_count+1 : 0;
                bool error=bad && bad!=6;
                check(header[0]==protocol::RESPONSE_MAGIC && header[2]==tag,"response header");
                check(error ? header[3]!=0 && header[1]==0 : header[3]==0 && header[1]>=13 && header[1]<=cases[tag].size()+14,"status/length");
                if (!error) {
                    unsigned char decoded[32768];
                    auto frame=response.data()+16;
                    size_t n=ZSTD_decompress(decoded,sizeof(decoded),frame,header[1]);
                    check(!ZSTD_isError(n),ZSTD_getErrorName(n));
                    check(n==cases[tag].size() && (!n || !std::memcmp(decoded,cases[tag].data(),n)),"decompressed bytes");
                    unsigned fh=n<256 ? 6 : 7;
                    if ((frame[fh]&6)==4) { ++compressed;if ((frame[fh+3]&3)==2) ++huffman; }
                }
                for (unsigned i=16+header[1];i<response.size();++i) check(!response[i],"nonzero padding");
                std::printf("RTL PASS tag=%u input=%zu compressed=%u status=%u cycle=%llu\n",tag,cases[tag].size(),header[1],header[3],(unsigned long long)cycle);
                std::fflush(stdout);response.clear();++tag;
            }
        }
        dut.ap_clk=1;dut.eval();
        if (tx) { ++sent;holding=false; }
        if (tag==cases.size()) {
            check(sent==requests.size(),"unconsumed requests");
            check(huffman>0 && compressed>0,"entropy coverage");
            dut.final();
            std::printf("RTL_ROUNDTRIP_PASS: %u cases, %u compressed blocks, %u Huffman\n",tag,compressed,huffman);
            return 0;
        }
    }
    std::fprintf(stderr,"Timeout: sent=%zu/%zu received=%zu tag=%u\n",sent,requests.size(),received,tag);
    return 1;
}
