// Portable tests exercise the exact synthesizable entropy functions. libzstd
// appears only as an independent decoder; no software encoder is used.
#include "../hls/entropy.hpp"
#include <zstd.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>
static unsigned cases=0, huffman=0, fse=0, compressed=0, packed_weights=0;
static void check(const std::vector<unsigned char>& data, const std::vector<unsigned char>& literals,
                  const std::vector<entropy::Sequence>& seq) {
    unsigned char source[entropy::BLOCK]={}, lit[entropy::BLOCK]={}, out[entropy::CAP]={}, decoded[entropy::BLOCK];
    entropy::Sequence sequences[entropy::MAX_SEQ];
    std::copy(data.begin(),data.end(),source); std::copy(literals.begin(),literals.end(),lit);
    std::copy(seq.begin(),seq.end(),sequences);
    unsigned size=entropy::frame(source,data.size(),lit,literals.size(),sequences,seq.size(),out);
    size_t n=ZSTD_decompress(decoded,sizeof(decoded),out,size);
    if (ZSTD_isError(n) || n!=data.size() || !std::equal(data.begin(),data.end(),decoded)) {
        std::fprintf(stderr,"FAIL case=%u input=%zu literals=%zu seq=%zu size=%u error=%s\n",cases,data.size(),literals.size(),seq.size(),size,ZSTD_getErrorName(n));
        FILE* f=std::fopen("/tmp/entropy-failure.zst","wb"); std::fwrite(out,1,size,f); std::fclose(f); std::exit(1);
    }
    out[size-1]^=1;
    if (!ZSTD_isError(ZSTD_decompress(decoded,sizeof(decoded),out,size))) {
        std::fputs("FAIL: corrupted hardware checksum was accepted\n",stderr);std::exit(1);
    }
    out[size-1]^=1;
    unsigned header=data.size()<256 ? 6 : 7;
    if ((out[header]&6)==4) {
        ++compressed;
        if ((out[header+3]&3)==2) { ++huffman; if (out[header+8]<128) ++packed_weights; }
        if (!seq.empty()) ++fse;
    }
    ++cases;
}
int main() {
    std::mt19937 rng(42);
    for (unsigned n : {0,1,31,32,127,128,129,255,256,1023,1024,4095,4096,16383,16384,32767,32768}) {
        for (unsigned alphabet : {1,2,3,8,32,128,256}) {
            std::vector<unsigned char> data(n);
            for (auto& b:data) b=rng()%alphabet;
            check(data,data,{});
            for (auto& b:data) b=255-b;
            check(data,data,{});
        }
    }
    for (unsigned test=0; test<300; ++test) {
        std::vector<unsigned char> data,lit;
        std::vector<entropy::Sequence> seq;
        unsigned size=1024+rng()%(32768-1024);
        while (data.size()<size) {
            unsigned run=std::min(unsigned(size-data.size()),unsigned(rng()%100));
            if (data.empty() && !run) run=1;
            for (unsigned i=0;i<run;++i) {
                unsigned char b=rng() % (test%3==0 ? 256 : 8);
                data.push_back(b); lit.push_back(b);
            }
            unsigned match=std::min(unsigned(size-data.size()),unsigned(3+rng()%253));
            unsigned offset=1+rng()%data.size();
            if (match<3) {
                while (data.size()<size) { data.push_back(0); lit.push_back(0); }
                break;
            }
            seq.push_back({(unsigned short)run,(unsigned short)match,(unsigned short)offset});
            for (unsigned i=0;i<match;++i) data.push_back(data[data.size()-offset]);
        }
        check(data,lit,seq);
    }
    // Full-alphabet skew: forces FSE-compressed Huffman weights.
    for (unsigned shift=0;shift<8;++shift) {
        std::vector<unsigned char> data(32768);
        for (unsigned i=0;i<data.size();++i) data[i]=i<256 ? i : (rng()%100<90 ? (rng()%8+shift*31) : rng()%256);
        check(data,data,{});
    }
    // Odd/even weight counts take different two-state FSE initialization paths.
    for (unsigned maximum : {129,130,200,253,254,255}) {
        std::vector<unsigned char> data(32768);
        for (auto& byte:data) byte=maximum-31+rng()%32;
        check(data,data,{});
    }
    // Boundary LL codes and a maximum-density sequence stream exercise large
    // extra-bit fields, two-byte sequence counts, and output-buffer bounds.
    for (unsigned code=0; code<35; ++code) {
        unsigned run=LL_BASE[code];
        if (run+259>32768) continue;
        std::vector<unsigned char> data(4, 'a'),lit(1,'a');
        std::vector<entropy::Sequence> seq{{1,3,1}};
        for (unsigned i=0;i<run;++i) { unsigned char v=rng()&255; data.push_back(v);lit.push_back(v); }
        seq.push_back({(unsigned short)run,255,1});
        data.insert(data.end(),255,data.back());
        check(data,lit,seq);
    }
    {
        std::vector<unsigned char> data(32767,'a'),lit(1,'a');
        std::vector<entropy::Sequence> seq(10922,{0,3,1});
        seq.front().literals=1;
        check(data,lit,seq);
    }
    // Very deep Fibonacci tree must select raw literals safely.
    std::vector<unsigned char> deep;
    unsigned a=1,b=1;
    for (unsigned i=0;deep.size()+b<=32768;++i) { deep.insert(deep.end(),b,i); unsigned next=a+b; a=b;b=next; }
    check(deep,deep,{});
    if (!huffman || !fse || !packed_weights) return 2;
    std::printf("ENTROPY_PASS: %u cases, %u compressed blocks, %u Huffman, %u FSE sequences, %u FSE weight tables\n",cases,compressed,huffman,fse,packed_weights);
}
