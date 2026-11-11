// Row.hpp
// A Row is simply a vector of Values, ordered according to a Schema.
// Provides binary serialization used by the on-disk table storage format.
#pragma once

#include "Types.hpp"
#include <vector>
#include <fstream>
#include <cstring>

namespace minisql {

using Row = std::vector<Value>;

// Writes a single Value in a fixed binary layout:
//   [uint8 isNull][uint8 type] then payload:
//     INT   -> int64
//     FLOAT -> double
//     BOOL  -> uint8
//     TEXT  -> uint32 length + bytes
inline void writeValue(std::ostream& out, const Value& v) {
    uint8_t nullFlag = v.isNull ? 1 : 0;
    out.write(reinterpret_cast<const char*>(&nullFlag), sizeof(nullFlag));
    uint8_t typeTag = static_cast<uint8_t>(v.type);
    out.write(reinterpret_cast<const char*>(&typeTag), sizeof(typeTag));
    if (v.isNull) return;

    switch (v.type) {
        case DataType::INT:
            out.write(reinterpret_cast<const char*>(&v.i), sizeof(v.i));
            break;
        case DataType::FLOAT:
            out.write(reinterpret_cast<const char*>(&v.d), sizeof(v.d));
            break;
        case DataType::BOOL: {
            uint8_t bb = v.b ? 1 : 0;
            out.write(reinterpret_cast<const char*>(&bb), sizeof(bb));
            break;
        }
        case DataType::TEXT: {
            uint32_t len = static_cast<uint32_t>(v.s.size());
            out.write(reinterpret_cast<const char*>(&len), sizeof(len));
            out.write(v.s.data(), len);
            break;
        }
    }
}

inline Value readValue(std::istream& in) {
    uint8_t nullFlag = 0, typeTag = 0;
    in.read(reinterpret_cast<char*>(&nullFlag), sizeof(nullFlag));
    in.read(reinterpret_cast<char*>(&typeTag), sizeof(typeTag));
    DataType type = static_cast<DataType>(typeTag);

    Value v;
    v.type = type;
    if (nullFlag) {
        v.isNull = true;
        return v;
    }

    switch (type) {
        case DataType::INT:
            in.read(reinterpret_cast<char*>(&v.i), sizeof(v.i));
            break;
        case DataType::FLOAT:
            in.read(reinterpret_cast<char*>(&v.d), sizeof(v.d));
            break;
        case DataType::BOOL: {
            uint8_t bb = 0;
            in.read(reinterpret_cast<char*>(&bb), sizeof(bb));
            v.b = (bb != 0);
            break;
        }
        case DataType::TEXT: {
            uint32_t len = 0;
            in.read(reinterpret_cast<char*>(&len), sizeof(len));
            v.s.resize(len);
            if (len > 0) in.read(&v.s[0], len);
            break;
        }
    }
    return v;
}

inline void writeRow(std::ostream& out, const Row& row) {
    uint32_t n = static_cast<uint32_t>(row.size());
    out.write(reinterpret_cast<const char*>(&n), sizeof(n));
    for (const auto& v : row) writeValue(out, v);
}

inline Row readRow(std::istream& in) {
    uint32_t n = 0;
    in.read(reinterpret_cast<char*>(&n), sizeof(n));
    Row row;
    row.reserve(n);
    for (uint32_t i = 0; i < n; ++i) row.push_back(readValue(in));
    return row;
}

} // namespace minisql
