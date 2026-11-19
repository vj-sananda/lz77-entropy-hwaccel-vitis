// Vitis L1 LZ77 + local synthesizable Zstandard Huffman/FSE/frame encoder.
#define DISABLE_DEPENDENCE
#define AVOID_STATIC_MODE
#include <ap_axi_sdata.h>
#include "zstd_compress_internal.hpp"
#include "entropy.hpp"
using Axis = ap_axiu<128,0,0,0>;
using Byte = entropy::Byte;
using Token = IntVectorStream_dt<32,1>;
using InputByte = IntVectorStream_dt<8,1>;

static void feed(const Byte source[protocol::BLOCK], unsigned size,
                 hls::stream<InputByte>& bytes) {
    InputByte b;
    for (unsigned i=0; i<size; ++i) {
#pragma HLS PIPELINE II=1
        b.strobe=1; b.data[0]=source[i]; bytes.write(b);
    }
    b.strobe=0; b.data[0]=0;
    bytes.write(b); bytes.write(b); // block end, file end
}
static void collect(hls::stream<Token>& tokens, Byte literals[protocol::BLOCK],
                    entropy::Sequence seq[entropy::MAX_SEQ], unsigned size,
                    unsigned& nl, unsigned& ns, unsigned& status) {
    nl=0; ns=0;
    unsigned run=0, consumed=0;
    for (auto t=tokens.read(); t.strobe; t=tokens.read()) {
        unsigned match=t.data[0].range(15,8);
        unsigned offset=unsigned(t.data[0].range(31,16))+1;
        if (match) {
            if (match<3 || offset>consumed || consumed+match>size || ns>=entropy::MAX_SEQ)
                status=3;
            else { seq[ns++]={static_cast<unsigned short>(run),static_cast<unsigned short>(match),static_cast<unsigned short>(offset)}; }
            consumed+=match; run=0;
        } else {
            if (nl<protocol::BLOCK) literals[nl++]=t.data[0].range(7,0);
            else status=3;
            ++run; ++consumed;
        }
    }
    tokens.read();
    if (consumed!=size) status=3;
}
static void match(const Byte source[protocol::BLOCK], unsigned size,
                  Byte literals[protocol::BLOCK], entropy::Sequence seq[entropy::MAX_SEQ],
                  unsigned& nl, unsigned& ns, unsigned& status) {
#pragma HLS DATAFLOW
    hls::stream<InputByte> bytes("bytes");
    hls::stream<Token> matches("matches"), boosted("boosted");
#pragma HLS STREAM variable=bytes depth=32
#pragma HLS STREAM variable=matches depth=32
#pragma HLS STREAM variable=boosted depth=32
    feed(source,size,bytes);
    xf::compression::lzCompress<protocol::BLOCK,uint32_t,6,3,protocol::BLOCK>(bytes,matches);
    xf::compression::lzBooster<255>(matches,boosted);
    collect(boosted,literals,seq,size,nl,ns,status);
}
void zstd_accel(hls::stream<Axis>& input, hls::stream<Axis>& output) {
#pragma HLS INTERFACE axis port=input
#pragma HLS INTERFACE axis port=output
#pragma HLS INTERFACE ap_ctrl_hs port=return
    Byte source[protocol::BLOCK], literals[protocol::BLOCK], result[protocol::CAPACITY];
    entropy::Sequence seq[entropy::MAX_SEQ];
#pragma HLS BIND_STORAGE variable=source type=ram_2p impl=bram
#pragma HLS BIND_STORAGE variable=literals type=ram_2p impl=bram
#pragma HLS BIND_STORAGE variable=result type=ram_2p impl=bram
#pragma HLS BIND_STORAGE variable=seq type=ram_2p impl=bram
    Axis a=input.read();
    unsigned size=a.data.range(63,32), tag=a.data.range(95,64);
    unsigned status=0;
    if (a.data.range(31,0)!=protocol::REQUEST_MAGIC || size>protocol::BLOCK || a.data.range(127,96)!=0) status=1;
    if (a.keep!=65535 || a.strb!=65535 || a.last) status=2;
    for (unsigned i=0; i<protocol::BLOCK; i+=16) {
        a=input.read();
        if (a.keep!=65535 || a.strb!=65535 || bool(a.last)!=(i==protocol::BLOCK-16)) status=2;
        for (unsigned j=0; j<16; ++j) {
#pragma HLS PIPELINE II=1
            source[i+j]=a.data.range(8*j+7,8*j);
        }
    }
    unsigned nl=0,ns=0,length=0;
    if (!status) {
        if (size>128) match(source,size,literals,seq,nl,ns,status);
        else {
            nl=size;
            for (unsigned i=0; i<size; ++i) {
#pragma HLS PIPELINE II=1
                literals[i]=source[i];
            }
        }
        if (!status) length=entropy::frame(source,size,literals,nl,seq,ns,result);
    }
    a.data=0; a.keep=-1; a.strb=-1; a.last=0;
    a.data.range(31,0)=protocol::RESPONSE_MAGIC;
    a.data.range(63,32)=length;
    a.data.range(95,64)=tag;
    a.data.range(127,96)=status;
    output.write(a);
    for (unsigned i=0; i<protocol::CAPACITY; i+=16) {
        a.data=0;
        for (unsigned j=0; j<16; ++j) {
#pragma HLS PIPELINE II=1
            if (i+j<length) a.data.range(8*j+7,8*j)=result[i+j];
        }
        a.last=i==protocol::CAPACITY-16;
        output.write(a);
    }
}
