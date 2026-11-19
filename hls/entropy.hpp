#pragma once
#include <cstdint>
#include "protocol.hpp"
#include "fse_tables.hpp"
#include "checksum.hpp"

namespace entropy {
using Byte = unsigned char;
constexpr unsigned BLOCK = protocol::BLOCK;
constexpr unsigned CAP = protocol::CAPACITY;
constexpr unsigned MAX_SEQ = BLOCK / 3 + 1;
struct Sequence { unsigned short literals, match, offset; };

// Forward byte storage of a reverse-decoded little-endian bitstream. Each call
// emits at most 16 bits; no shift can reach the 64-bit accumulator width.
struct Bits {
    uint64_t value = 0;
    unsigned used = 0, pos;
    explicit Bits(unsigned start) : pos(start) {}
    void add(Byte out[CAP], unsigned v, unsigned n) {
#pragma HLS INLINE
        value |= (uint64_t(v) & ((uint64_t(1) << n) - 1)) << used;
        used += n;
        for (unsigned k = 0; k < 3; ++k) {
            if (used >= 8) {
                out[pos++] = Byte(value);
                value >>= 8;
                used -= 8;
            }
        }
    }
    unsigned close(Byte out[CAP]) {
#pragma HLS INLINE
        add(out, 1, 1); // Zstd reverse-bitstream end marker
        if (used) out[pos++] = Byte(value);
        return pos;
    }
};
inline void le(Byte out[CAP], unsigned pos, uint64_t value, unsigned bytes) {
    for (unsigned k = 0; k < bytes; ++k) {
#pragma HLS PIPELINE II=1
        out[pos+k] = Byte(value >> (8*k));
    }
}
inline unsigned highbit(unsigned value) {
#pragma HLS INLINE
    unsigned bit = 0;
    for (unsigned k = 1; k < 16; ++k) {
#pragma HLS UNROLL
        if (value >= (1u<<k)) bit = k;
    }
    return bit;
}
template<unsigned N>
void fse(Bits& bits, Byte out[CAP], unsigned& state, unsigned symbol,
         const FseSymbol (&symbols)[N], const unsigned short* states) {
#pragma HLS INLINE
    FseSymbol s = symbols[symbol];
    unsigned n = (state + s.delta_bits) >> 16;
    bits.add(out, state, n);
    state = states[int(state >> n) + s.delta_find];
}

// Ordinary Huffman tree, with bounded 11-bit codes. Unusually deep trees use
// raw literals. Tree/table construction and all bit packing execute in RTL.
static bool huffman_tree(const unsigned freq[256], Byte lengths[256],
                         unsigned short codes[256], Byte weights[256],
                         unsigned& maximum) {
#pragma HLS INLINE off
    unsigned counts[512];
    unsigned short left[512], right[512];
    Byte depth[512];
#pragma HLS BIND_STORAGE variable=counts type=ram_2p impl=bram
#pragma HLS BIND_STORAGE variable=left type=ram_2p impl=bram
#pragma HLS BIND_STORAGE variable=right type=ram_2p impl=bram
#pragma HLS BIND_STORAGE variable=depth type=ram_2p impl=bram
    unsigned symbols = 0;
    maximum = 0;
    for (unsigned i=0; i<512; ++i) {
#pragma HLS PIPELINE II=1
        counts[i] = i<256 ? freq[i] : 0;
        depth[i] = 0;
        if (i<256 && freq[i]) { ++symbols; maximum=i; }
    }
    if (symbols<2) return false;
    unsigned root = 256;
    for (unsigned step=0; step<symbols-1; ++step) {
        unsigned a=0,b=0,fa=0xffffffff,fb=0xffffffff;
        for (unsigned i=0; i<256+step; ++i) {
#pragma HLS PIPELINE II=1
            unsigned f=counts[i];
            if (f && f<fa) { b=a; fb=fa; a=i; fa=f; }
            else if (f && f<fb) { b=i; fb=f; }
        }
        root=256+step;
        counts[a]=0; counts[b]=0; counts[root]=fa+fb;
        left[root]=a; right[root]=b;
    }
    for (int i=int(root); i>=256; --i) {
        depth[left[i]]=depth[i]+1;
        depth[right[i]]=depth[i]+1;
    }
    unsigned ranks[12] = {}, next[12] = {};
#pragma HLS ARRAY_PARTITION variable=ranks complete
#pragma HLS ARRAY_PARTITION variable=next complete
    unsigned maxbits=0;
    bool valid=true;
    for (unsigned i=0; i<256; ++i) {
        unsigned n=depth[i];
        lengths[i]=n;
        if (n>11) valid=false;
        else if (n) ++ranks[n];
        if (n>maxbits) maxbits=n;
    }
    if (!valid) return false;
    unsigned start=0;
    for (int n=11; n>0; --n) { next[n]=start; start=(start+ranks[n])>>1; }
    for (unsigned i=0; i<256; ++i) {
        unsigned n=lengths[i];
        codes[i]=n ? next[n]++ : 0;
        weights[i]=n ? maxbits+1-n : 0;
    }
    return true;
}

static unsigned weight_header(Byte out[CAP], const Byte weights[256], unsigned maximum) {
#pragma HLS INLINE off
    if (maximum<=128) {
        out[0]=127+maximum;
        for (unsigned i=0; i<maximum; i+=2)
            out[1+i/2]=(weights[i]<<4) | (i+1<maximum ? weights[i+1] : 0);
        return 1+(maximum+1)/2;
    }
    for (unsigned i=0; i<sizeof(WT_HEADER); ++i) out[1+i]=WT_HEADER[i];
    Bits bits(1+sizeof(WT_HEADER));
    unsigned i=maximum, s1, s2;
    if (i&1) {
        s1=WT_SYMBOL[weights[--i]].initial;
        s2=WT_SYMBOL[weights[--i]].initial;
        fse(bits,out,s1,weights[--i],WT_SYMBOL,WT_STATE);
    } else {
        s2=WT_SYMBOL[weights[--i]].initial;
        s1=WT_SYMBOL[weights[--i]].initial;
    }
    while (i) {
        fse(bits,out,s2,weights[--i],WT_SYMBOL,WT_STATE);
        fse(bits,out,s1,weights[--i],WT_SYMBOL,WT_STATE);
    }
    bits.add(out,s2,5); bits.add(out,s1,5);
    unsigned end=bits.close(out);
    if (end-1>=128) return 0;
    out[0]=end-1;
    return end;
}

static unsigned literal_section(const Byte literals[BLOCK], unsigned n,
                                Byte out[CAP], Byte scratch[CAP]) {
#pragma HLS INLINE off
    unsigned freq[256] = {};
#pragma HLS BIND_STORAGE variable=freq type=ram_2p impl=bram
    for (unsigned i=0; i<n; ++i) ++freq[literals[i]];
    unsigned raw_header = n<32 ? 1 : n<4096 ? 2 : 3;
    unsigned flag = raw_header==1 ? 0 : raw_header==2 ? 4 : 12;
    unsigned raw_value = flag | (n << (raw_header==1 ? 3 : 4));
    if (n && freq[literals[0]]==n) {
        le(out,0,raw_value|1,raw_header); out[raw_header]=literals[0];
        return raw_header+1;
    }
    if (n>=128) {
        Byte lengths[256], weights[256];
        unsigned short codes[256];
        unsigned maximum=0;
        if (huffman_tree(freq,lengths,codes,weights,maximum)) {
            unsigned table=weight_header(scratch,weights,maximum);
            if (table) {
                unsigned pos=table+6;
                unsigned segment=(n+3)/4;
                for (unsigned stream=0; stream<4; ++stream) {
                    unsigned begin=stream*segment;
                    unsigned end=stream==3 ? n : begin+segment;
                    Bits bits(pos);
                    for (unsigned i=end; i>begin; --i)
                        bits.add(scratch,codes[literals[i-1]],lengths[literals[i-1]]);
                    unsigned next=bits.close(scratch);
                    if (stream<3) le(scratch,table+2*stream,next-pos,2);
                    pos=next;
                }
                if (pos+5<n+raw_header) {
                    // Compressed literals, 4 streams, 18-bit size fields.
                    le(out,0,14 | (uint64_t(n)<<4) | (uint64_t(pos)<<22),5);
                    for (unsigned i=0; i<pos; ++i) {
#pragma HLS PIPELINE II=1
                        out[5+i]=scratch[i];
                    }
                    return 5+pos;
                }
            }
        }
    }
    le(out,0,raw_value,raw_header);
    for (unsigned i=0; i<n; ++i) {
#pragma HLS PIPELINE II=1
        out[raw_header+i]=literals[i];
    }
    return raw_header+n;
}

static unsigned sequences(const Sequence seq[MAX_SEQ], unsigned count,
                          Byte out[CAP], unsigned pos) {
#pragma HLS INLINE off
    if (count<128) out[pos++]=count;
    else { out[pos++]=128+(count>>8); out[pos++]=count; }
    if (!count) return pos;
    out[pos++]=0; // predefined FSE tables for LL, OF, ML
    Bits bits(pos);
    unsigned sl=0,sm=0,so=0;
    for (unsigned i=count; i>0; --i) {
        Sequence s=seq[i-1];
        unsigned ll=0,ml=0,of=highbit(unsigned(s.offset)+3);
        for (unsigned c=1; c<36; ++c) {
#pragma HLS UNROLL
            if (s.literals>=LL_BASE[c]) ll=c;
        }
        for (unsigned c=1; c<53; ++c) {
#pragma HLS UNROLL
            if (unsigned(s.match)-3>=ML_BASE[c]) ml=c;
        }
        if (i==count) {
            sl=LL_SYMBOL[ll].initial; sm=ML_SYMBOL[ml].initial; so=OF_SYMBOL[of].initial;
        } else {
            fse(bits,out,so,of,OF_SYMBOL,OF_STATE);
            fse(bits,out,sm,ml,ML_SYMBOL,ML_STATE);
            fse(bits,out,sl,ll,LL_SYMBOL,LL_STATE);
        }
        bits.add(out,unsigned(s.literals)-LL_BASE[ll],LL_BITS[ll]);
        bits.add(out,unsigned(s.match)-3-ML_BASE[ml],ML_BITS[ml]);
        bits.add(out,unsigned(s.offset)+3,of);
    }
    bits.add(out,sm,6); bits.add(out,so,5); bits.add(out,sl,6);
    return bits.close(out);
}

static unsigned frame(const Byte source[BLOCK], unsigned size,
                      const Byte literals[BLOCK], unsigned literal_count,
                      const Sequence seq[MAX_SEQ], unsigned count, Byte out[CAP]) {
#pragma HLS INLINE off
    Byte payload[CAP], scratch[CAP];
#pragma HLS BIND_STORAGE variable=payload type=ram_2p impl=bram
#pragma HLS BIND_STORAGE variable=scratch type=ram_2p impl=bram
    unsigned n=literal_section(literals,literal_count,payload,scratch);
    n=sequences(seq,count,payload,n);
    bool compressed=n<size;
    unsigned content=compressed ? n : size;
    le(out,0,0xfd2fb528,4);
    out[4]=size<256 ? 0x24 : 0x64; // single segment, checksum present
    unsigned header=size<256 ? 6 : 7;
    le(out,5,size<256 ? size : size-256,header-5);
    le(out,header,(content<<3) | (compressed ? 5 : 1),3); // last block
    for (unsigned i=0; i<content; ++i) {
#pragma HLS PIPELINE II=1
        out[header+3+i]=compressed ? payload[i] : source[i];
    }
    le(out,header+3+content,checksum::xxh64(source,size),4);
    return header+7+content;
}
} // namespace entropy
