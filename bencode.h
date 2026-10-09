#pragma once
#include <string>
#include <vector>
#include <stdint.h>
#include <stddef.h>

struct BNode {
    enum Type { NONE, INT, STR, LIST, DICT };
    Type type = NONE;
    int64_t i = 0;
    std::string s;
    std::vector<BNode> l;
    std::vector<std::string> keys;
    std::vector<BNode> vals;
    size_t begin = 0, end = 0;   // диапазон байт в исходных данных

    const BNode* find(const char* key) const;
};

bool bdecode(const std::string& data, BNode& out);