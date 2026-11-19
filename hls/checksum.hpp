#pragma once
#include <cstdint>
#include "protocol.hpp"
// XXH64, seed zero. Zstandard stores the low 32 bits of this hash of the
// uncompressed frame content. All arithmetic intentionally wraps at 64 bits.
namespace checksum {
constexpr uint64_t P1=11400714785074694791ull, P2=14029467366897019727ull;
constexpr uint64_t P3=1609587929392839161ull, P4=9650029242287828579ull;
constexpr uint64_t P5=2870177450012600261ull;
inline uint64_t rotate(uint64_t x,unsigned n) { return (x<<n)|(x>>(64-n)); }
// A 64-bit multiply spans several Kintex-7 DSPs. Explicit internal registers
// prevent HLS from putting their complete cascade in one 50 MHz cycle.
static uint64_t multiply(uint64_t a,uint64_t b) {
#pragma HLS INLINE off
#pragma HLS PIPELINE II=1
    uint64_t product=a*b;
#pragma HLS BIND_OP variable=product op=mul impl=dsp latency=4
    return product;
}
inline uint64_t round(uint64_t acc,uint64_t word) { return multiply(rotate(acc+multiply(word,P2),31),P1); }
inline uint64_t merge(uint64_t acc,uint64_t v) { return multiply(acc^round(0,v),P1)+P4; }
static uint64_t load(const unsigned char source[protocol::BLOCK],unsigned pos,unsigned n) {
#pragma HLS INLINE
    uint64_t value=0;
    for (unsigned i=0;i<n;++i) {
#pragma HLS PIPELINE II=1
        value|=uint64_t(source[pos+i])<<(8*i);
    }
    return value;
}
static uint32_t xxh64(const unsigned char source[protocol::BLOCK],unsigned size) {
#pragma HLS INLINE off
    uint64_t hash=P5;
    unsigned pos=0;
    if (size>=32) {
        uint64_t v1=P1+P2,v2=P2,v3=0,v4=0-P1;
        for (;pos+32<=size;pos+=32) {
            v1=round(v1,load(source,pos,8));
            v2=round(v2,load(source,pos+8,8));
            v3=round(v3,load(source,pos+16,8));
            v4=round(v4,load(source,pos+24,8));
        }
        hash=rotate(v1,1)+rotate(v2,7)+rotate(v3,12)+rotate(v4,18);
        hash=merge(hash,v1);hash=merge(hash,v2);hash=merge(hash,v3);hash=merge(hash,v4);
    }
    hash+=size;
    for (;pos+8<=size;pos+=8) hash=multiply(rotate(hash^round(0,load(source,pos,8)),27),P1)+P4;
    if (pos+4<=size) {
        hash=multiply(rotate(hash^multiply(load(source,pos,4),P1),23),P2)+P3;
        pos+=4;
    }
    for (;pos<size;++pos) hash=multiply(rotate(hash^multiply(uint64_t(source[pos]),P5),11),P1);
    hash^=hash>>33;hash=multiply(hash,P2);hash^=hash>>29;hash=multiply(hash,P3);hash^=hash>>32;
    return uint32_t(hash);
}
}
