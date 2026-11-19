#pragma once
#include <algorithm>
#include <random>
#include <vector>
inline std::vector<std::vector<unsigned char>> test_cases(bool extensive) {
    std::mt19937 rng(42);
    std::vector<std::vector<unsigned char>> cases;
    const char text[]="FPGA accelerated Zstandard: hardware Huffman literals and FSE sequences.\n";
    for (unsigned size : {0,1,16,63,64,65,128,129,255,256,4096,32767,32768}) {
        std::vector<unsigned char> data(size);
        for (unsigned i=0;i<size;++i) data[i]=text[i%(sizeof(text)-1)];
        cases.push_back(data);
        for (auto& b:data) b=rng()&255;
        cases.push_back(data);
        std::fill(data.begin(),data.end(),255);
        cases.push_back(data);
    }
    for (unsigned alphabet : {2,8,32,128,256}) {
        std::vector<unsigned char> data(32768);
        for (auto& b:data) b=rng()%alphabet;
        cases.push_back(data);
        for (auto& b:data) b=255-b;
        cases.push_back(data);
    }
    for (unsigned maximum : {200,254}) {
        std::vector<unsigned char> data(32768);
        for (auto& byte:data) byte=maximum-31+rng()%32;
        cases.push_back(data);
    }
    if (extensive) {
        for (unsigned byte : {0,1,2,3,4,127,128,254})
            for (unsigned size : {129,32768}) cases.emplace_back(size,byte);
        for (unsigned test=0;test<64;++test) {
            std::vector<unsigned char> data(129+rng()%(32768-128));
            for (unsigned i=0;i<data.size();++i) {
                unsigned v=rng();
                if (test%4==0) data[i]=v%2;
                if (test%4==1) data[i]=v%8;
                if (test%4==2) data[i]=v%100<95 ? 'x' : v&255;
                if (test%4==3) data[i]=i>100 && v%4 ? data[i-97] : v&255;
            }
            cases.push_back(data);
        }
    }
    return cases;
}
