// Table.hpp
// A Table owns its Schema, its row storage (a vector<Row> acting as a heap
// file), and a B+Tree index on the primary key column (if any) mapping
// key -> row storage slot. Deleted rows are tombstoned (marked invalid)
// rather than physically removed, to keep row ids (used by the index)
// stable; slots can be reused by future inserts (simple free-list).
#pragma once

#include "Types.hpp"
#include "Row.hpp"
#include "BPlusTree.hpp"
#include <vector>
#include <memory>
#include <fstream>
#include <filesystem>
#include <stdexcept>

namespace minisql {

class Table {
public:
    std::string name;
    Schema schema;

    Table() = default;
    Table(std::string tableName, Schema s) : name(std::move(tableName)), schema(std::move(s)) {
        int pk = schema.primaryKeyIndex();
        if (pk >= 0) {
            pkIndex_ = std::make_unique<BPlusTree>();
            hasPk_ = true;
            pkColumnIndex_ = pk;
        }
    }

    bool hasPrimaryKey() const { return hasPk_; }
    int primaryKeyColumnIndex() const { return pkColumnIndex_; }

    size_t rowCount() const { return liveCount_; }

    // Validate + insert a new row. Throws std::runtime_error on constraint
    // violation (type mismatch, NOT NULL violation, duplicate primary key).
    void insertRow(Row row) {
        validateRow(row, /*forInsert=*/true);

        size_t slot;
        if (!freeList_.empty()) {
            slot = freeList_.back();
            freeList_.pop_back();
            rows_[slot] = std::move(row);
            valid_[slot] = true;
        } else {
            slot = rows_.size();
            rows_.push_back(std::move(row));
            valid_.push_back(true);
        }
        ++liveCount_;

        if (hasPk_) {
            try {
                pkIndex_->insert(rows_[slot][pkColumnIndex_], slot);
            } catch (...) {
                // roll back the row insertion if index insert fails (duplicate key)
                valid_[slot] = false;
                --liveCount_;
                freeList_.push_back(slot);
                throw;
            }
        }
    }

    // Returns row slot indices for all live rows (heap scan order).
    std::vector<size_t> allRowSlots() const {
        std::vector<size_t> result;
        result.reserve(liveCount_);
        for (size_t i = 0; i < rows_.size(); ++i) {
            if (valid_[i]) result.push_back(i);
        }
        return result;
    }

    const Row& rowAt(size_t slot) const { return rows_[slot]; }
    bool isLive(size_t slot) const { return slot < valid_.size() && valid_[slot]; }

    // Point lookup by primary key. Returns slot or -1 if not found / no PK.
    long findByPrimaryKey(const Value& key) const {
        if (!hasPk_) return -1;
        size_t slot;
        if (pkIndex_->find(key, slot)) return static_cast<long>(slot);
        return -1;
    }

    BPlusTree* primaryKeyIndex() const { return pkIndex_.get(); }

    // Delete a row at slot (tombstone it, remove from index).
    void deleteRow(size_t slot) {
        if (!valid_[slot]) return;
        if (hasPk_) pkIndex_->remove(rows_[slot][pkColumnIndex_]);
        valid_[slot] = false;
        --liveCount_;
        freeList_.push_back(slot);
    }

    // Replace the row at slot with newRow (used by UPDATE). If the primary
    // key column changes, the index is updated accordingly.
    void updateRow(size_t slot, Row newRow) {
        validateRow(newRow, /*forInsert=*/false, /*skipSlot=*/slot);

        if (hasPk_) {
            const Value& oldKey = rows_[slot][pkColumnIndex_];
            const Value& newKey = newRow[pkColumnIndex_];
            if (oldKey.compare(newKey) != 0) {
                pkIndex_->remove(oldKey);
                pkIndex_->insert(newKey, slot);
            }
        }
        rows_[slot] = std::move(newRow);
    }

    // ------------------------------------------------------------------- //
    // Persistence: <tablename>.tbl binary file
    //   Header: uint32 magic, uint32 numColumns
    //   Per column: uint32 nameLen + name, uint8 type, uint8 pk, uint8 notNull
    //   Then: uint32 rowCount, followed by that many serialized rows
    //   (tombstoned rows are not written; slots are rebuilt densely).
    // ------------------------------------------------------------------- //
    static constexpr uint32_t MAGIC = 0x4D53514C; // "MSQL"

    void saveToFile(const std::string& path) const {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("Cannot open file for writing: " + path);

        out.write(reinterpret_cast<const char*>(&MAGIC), sizeof(MAGIC));
        uint32_t numCols = static_cast<uint32_t>(schema.columns.size());
        out.write(reinterpret_cast<const char*>(&numCols), sizeof(numCols));

        for (const auto& col : schema.columns) {
            uint32_t nameLen = static_cast<uint32_t>(col.name.size());
            out.write(reinterpret_cast<const char*>(&nameLen), sizeof(nameLen));
            out.write(col.name.data(), nameLen);
            uint8_t typeTag = static_cast<uint8_t>(col.type);
            out.write(reinterpret_cast<const char*>(&typeTag), sizeof(typeTag));
            uint8_t pkFlag = col.primaryKey ? 1 : 0;
            out.write(reinterpret_cast<const char*>(&pkFlag), sizeof(pkFlag));
            uint8_t nnFlag = col.notNull ? 1 : 0;
            out.write(reinterpret_cast<const char*>(&nnFlag), sizeof(nnFlag));
        }

        uint32_t rowCount = static_cast<uint32_t>(liveCount_);
        out.write(reinterpret_cast<const char*>(&rowCount), sizeof(rowCount));
        for (size_t i = 0; i < rows_.size(); ++i) {
            if (valid_[i]) writeRow(out, rows_[i]);
        }
    }

    static Table loadFromFile(const std::string& path, const std::string& tableName) {
        std::ifstream in(path, std::ios::binary);
        if (!in) throw std::runtime_error("Cannot open file for reading: " + path);

        uint32_t magic = 0;
        in.read(reinterpret_cast<char*>(&magic), sizeof(magic));
        if (magic != MAGIC) throw std::runtime_error("Corrupt table file (bad magic): " + path);

        uint32_t numCols = 0;
        in.read(reinterpret_cast<char*>(&numCols), sizeof(numCols));

        Schema schema;
        for (uint32_t c = 0; c < numCols; ++c) {
            uint32_t nameLen = 0;
            in.read(reinterpret_cast<char*>(&nameLen), sizeof(nameLen));
            std::string name(nameLen, '\0');
            if (nameLen > 0) in.read(&name[0], nameLen);
            uint8_t typeTag = 0, pkFlag = 0, nnFlag = 0;
            in.read(reinterpret_cast<char*>(&typeTag), sizeof(typeTag));
            in.read(reinterpret_cast<char*>(&pkFlag), sizeof(pkFlag));
            in.read(reinterpret_cast<char*>(&nnFlag), sizeof(nnFlag));

            Column col;
            col.name = name;
            col.type = static_cast<DataType>(typeTag);
            col.primaryKey = (pkFlag != 0);
            col.notNull = (nnFlag != 0);
            schema.columns.push_back(col);
        }

        Table table(tableName, schema);

        uint32_t rowCount = 0;
        in.read(reinterpret_cast<char*>(&rowCount), sizeof(rowCount));
        for (uint32_t r = 0; r < rowCount; ++r) {
            Row row = readRow(in);
            table.insertRow(std::move(row)); // rebuilds index as it loads
        }
        return table;
    }

private:
    std::vector<Row> rows_;
    std::vector<bool> valid_;
    std::vector<size_t> freeList_;
    size_t liveCount_ = 0;

    bool hasPk_ = false;
    int pkColumnIndex_ = -1;
    std::unique_ptr<BPlusTree> pkIndex_;

    // Type-check + NOT NULL check a row against the schema. `skipSlot` is
    // used during UPDATE to allow the row to "conflict" with its own
    // pre-update primary key value.
    void validateRow(const Row& row, bool /*forInsert*/, long skipSlot = -1) const {
        (void)skipSlot;
        if (row.size() != schema.columns.size()) {
            throw std::runtime_error("Column count mismatch: expected " +
                std::to_string(schema.columns.size()) + ", got " + std::to_string(row.size()));
        }
        for (size_t i = 0; i < row.size(); ++i) {
            const Column& col = schema.columns[i];
            const Value& v = row[i];
            if (v.isNull) {
                if (col.notNull || col.primaryKey) {
                    throw std::runtime_error("NULL not allowed in column '" + col.name + "'");
                }
                continue;
            }
            bool typeOk =
                (col.type == DataType::INT && v.type == DataType::INT) ||
                (col.type == DataType::FLOAT && (v.type == DataType::FLOAT || v.type == DataType::INT)) ||
                (col.type == DataType::TEXT && v.type == DataType::TEXT) ||
                (col.type == DataType::BOOL && v.type == DataType::BOOL);
            if (!typeOk) {
                throw std::runtime_error("Type mismatch in column '" + col.name + "': expected " +
                    dataTypeToString(col.type) + ", got " + dataTypeToString(v.type));
            }
        }
    }
};

} // namespace minisql
