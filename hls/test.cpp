#include <ap_axi_sdata.h>
#include <hls_stream.h>
#include <zstd.h>
#include "protocol.hpp"
#include "../tests/cases.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
using Axis=ap_axiu<128,0,0,0>;
void zstd_accel(hls::stream<Axis>&,hls::stream<Axis>&);
static void require(bool ok,const char* message) {
    if (!ok) { std::fprintf(stderr,"CSIM FAIL: %s\n",message); std::abort(); }
}
int main() {
    auto cases=test_cases(true);
    unsigned huffman=0, compressed=0;
    // Invalid requests followed by valid requests test draining and recovery.
    for (unsigned tag=0;tag<cases.size()+6;++tag) {
        unsigned bad=tag>=cases.size() ? tag-cases.size()+1 : 0;
        auto data=bad ? std::vector<unsigned char>(1024,'x') : cases[tag];
        hls::stream<Axis> in,out;
        Axis a; a.data=0;a.keep=-1;a.strb=-1;a.last=0;
        a.data.range(31,0)=protocol::REQUEST_MAGIC+(bad==1);
        a.data.range(63,32)=bad==2 ? 32769 : data.size();
        a.data.range(95,64)=tag;
        a.data.range(127,96)=bad==3;
        in.write(a);
        for (unsigned i=0;i<protocol::BLOCK;i+=16) {
            a.data=0;
            for (unsigned k=0;k<16;++k) if (i+k<data.size()) a.data.range(8*k+7,8*k)=data[i+k];
            a.last=i==protocol::BLOCK-16;
            a.keep=(bad==4 && i==32) ? 0 : 65535;
            if (bad==5 && i==0) a.last=1;
            in.write(a);
        }
        zstd_accel(in,out);
        a=out.read();
        require(a.data.range(31,0)==protocol::RESPONSE_MAGIC && a.data.range(95,64)==tag,"header");
        require(a.keep==65535 && a.strb==65535 && !a.last,"header sidebands");
        unsigned n=a.data.range(63,32), status=a.data.range(127,96);
        bool error=bad && bad!=6;
        require(error ? status!=0 && n==0 : status==0 && n>=13 && n<=data.size()+14,"status/length");
        std::vector<unsigned char> frame;
        for (unsigned i=0;i<protocol::CAPACITY;i+=16) {
            a=out.read();
            require(a.keep==65535 && a.strb==65535 && bool(a.last)==(i==protocol::CAPACITY-16),"payload sidebands");
            for (unsigned k=0;k<16;++k) {
                unsigned byte=a.data.range(8*k+7,8*k);
                if (i+k<n) frame.push_back(byte); else require(byte==0,"padding");
            }
        }
        if (!error) {
            unsigned char decoded[32768];
            size_t size=ZSTD_decompress(decoded,sizeof(decoded),frame.data(),frame.size());
            require(!ZSTD_isError(size),ZSTD_getErrorName(size));
            require(size==data.size() && (!size || std::memcmp(decoded,data.data(),size)==0),"round trip");
            unsigned header=data.size()<256 ? 6 : 7;
            if ((frame[header]&6)==4) { ++compressed; if ((frame[header+3]&3)==2) ++huffman; }
        }
        require(in.empty() && out.empty(),"undrained streams");
        std::printf("CSIM PASS tag=%u input=%zu compressed=%u status=%u\n",tag,data.size(),n,status);
        std::fflush(stdout);
    }
    require(huffman>0 && compressed>0,"entropy coverage");
    std::printf("CSIM_ROUNDTRIP_PASS: %zu cases; %u compressed blocks, %u Huffman\n",cases.size()+6,compressed,huffman);
}
