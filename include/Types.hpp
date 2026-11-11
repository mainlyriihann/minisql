// Types.hpp
// Core data types used throughout the RDBMS: SQL value types, typed Value
// (a tagged union over int64/double/string/bool/null), Column metadata, and
// Schema (ordered list of columns + primary key info).
#pragma once

#include <string>
#include <vector>
#include <variant>
#include <stdexcept>
#include <sstream>
#include <cstdint>
#include <optional>
#include <iomanip>

namespace minisql {

// ---------------------------------------------------------------------------
// Column data types supported by the engine.
// ---------------------------------------------------------------------------
enum class DataType { INT, FLOAT, TEXT, BOOL };

inline std::string dataTypeToString(DataType t) {
    switch (t) {
        case DataType::INT: return "INT";
        case DataType::FLOAT: return "FLOAT";
        case DataType::TEXT: return "TEXT";
        case DataType::BOOL: return "BOOL";
    }
    return "UNKNOWN";
}

inline DataType dataTypeFromString(const std::string& s) {
    std::string up;
    for (char c : s) up += static_cast<char>(::toupper(static_cast<unsigned char>(c)));
    if (up == "INT" || up == "INTEGER") return DataType::INT;
    if (up == "FLOAT" || up == "DOUBLE" || up == "REAL") return DataType::FLOAT;
    if (up == "TEXT" || up == "VARCHAR" || up == "STRING" || up == "CHAR") return DataType::TEXT;
    if (up == "BOOL" || up == "BOOLEAN") return DataType::BOOL;
    throw std::runtime_error("Unknown data type: " + s);
}

// ---------------------------------------------------------------------------
// A runtime value. NULL is represented by isNull==true regardless of `type`.
// ---------------------------------------------------------------------------
class Value {
public:
    DataType type = DataType::INT;
    bool isNull = false;
    int64_t i = 0;
    double d = 0.0;
    std::string s;
    bool b = false;

    Value() = default;

    static Value makeNull(DataType t) {
        Value v;
        v.type = t;
        v.isNull = true;
        return v;
    }
    static Value makeInt(int64_t val) {
        Value v; v.type = DataType::INT; v.i = val; return v;
    }
    static Value makeFloat(double val) {
        Value v; v.type = DataType::FLOAT; v.d = val; return v;
    }
    static Value makeText(std::string val) {
        Value v; v.type = DataType::TEXT; v.s = std::move(val); return v;
    }
    static Value makeBool(bool val) {
        Value v; v.type = DataType::BOOL; v.b = val; return v;
    }

    // Numeric value regardless of INT/FLOAT storage (for comparisons/arith).
    double asNumeric() const {
        if (type == DataType::INT) return static_cast<double>(i);
        if (type == DataType::FLOAT) return d;
        if (type == DataType::BOOL) return b ? 1.0 : 0.0;
        throw std::runtime_error("Cannot convert TEXT to numeric");
    }

    std::string toDisplayString() const {
        if (isNull) return "NULL";
        switch (type) {
            case DataType::INT: return std::to_string(i);
            case DataType::FLOAT: {
                std::ostringstream oss;
                oss << std::defaultfloat << std::setprecision(10) << d;
                return oss.str();
            }
            case DataType::TEXT: return s;
            case DataType::BOOL: return b ? "true" : "false";
        }
        return "";
    }

    // Ordering used by ORDER BY / index keys / comparisons.
    // NULLs sort last regardless of direction handling done by caller.
    int compare(const Value& other) const {
        if (isNull && other.isNull) return 0;
        if (isNull) return 1;
        if (other.isNull) return -1;

        if (type == DataType::TEXT || other.type == DataType::TEXT) {
            const std::string& a = (type == DataType::TEXT) ? s : toDisplayString();
            const std::string& bb = (other.type == DataType::TEXT) ? other.s : other.toDisplayString();
            if (a < bb) return -1;
            if (a > bb) return 1;
            return 0;
        }
        double av = asNumeric();
        double bv = other.asNumeric();
        if (av < bv) return -1;
        if (av > bv) return 1;
        return 0;
    }

    bool operator==(const Value& other) const { return compare(other) == 0; }
    bool operator<(const Value& other) const { return compare(other) < 0; }
};

// ---------------------------------------------------------------------------
// Column metadata
// ---------------------------------------------------------------------------
struct Column {
    std::string name;
    DataType type;
    bool primaryKey = false;
    bool notNull = false;
};

// ---------------------------------------------------------------------------
// Schema: ordered columns for a table, with quick name->index lookup.
// ---------------------------------------------------------------------------
class Schema {
public:
    std::vector<Column> columns;

    int indexOf(const std::string& name) const {
        for (size_t i = 0; i < columns.size(); ++i) {
            if (columns[i].name == name) return static_cast<int>(i);
        }
        return -1;
    }

    int primaryKeyIndex() const {
        for (size_t i = 0; i < columns.size(); ++i) {
            if (columns[i].primaryKey) return static_cast<int>(i);
        }
        return -1;
    }

    size_t size() const { return columns.size(); }
};

} // namespace minisql
