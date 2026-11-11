// Catalog.hpp
// Owns all Tables in the database and handles database-directory persistence:
//   <dbDir>/catalog.meta   -- one table name per line
//   <dbDir>/<name>.tbl     -- per-table binary data (see Table.hpp)
#pragma once

#include "Table.hpp"
#include <unordered_map>
#include <memory>
#include <fstream>
#include <filesystem>
#include <stdexcept>

namespace minisql {

namespace fs = std::filesystem;

class Catalog {
public:
    bool hasTable(const std::string& name) const { return tables_.count(name) != 0; }

    Table* getTable(const std::string& name) {
        auto it = tables_.find(name);
        if (it == tables_.end()) throw std::runtime_error("Table '" + name + "' does not exist");
        return it->second.get();
    }

    void createTable(const std::string& name, Schema schema) {
        if (hasTable(name)) throw std::runtime_error("Table '" + name + "' already exists");
        int pkCount = 0;
        for (auto& c : schema.columns) if (c.primaryKey) ++pkCount;
        if (pkCount > 1) throw std::runtime_error("Only one PRIMARY KEY column is supported per table");
        tables_[name] = std::make_unique<Table>(name, std::move(schema));
    }

    void dropTable(const std::string& name) {
        if (!hasTable(name)) throw std::runtime_error("Table '" + name + "' does not exist");
        tables_.erase(name);
    }

    std::vector<std::string> listTableNames() const {
        std::vector<std::string> names;
        for (auto& [name, table] : tables_) names.push_back(name);
        std::sort(names.begin(), names.end());
        return names;
    }

    // ------------------------------- persistence ------------------------- //
    void saveAll(const std::string& dbDir) const {
        fs::create_directories(dbDir);
        std::ofstream meta(dbDir + "/catalog.meta", std::ios::trunc);
        for (auto& [name, table] : tables_) {
            meta << name << "\n";
            table->saveToFile(dbDir + "/" + name + ".tbl");
        }
    }

    void saveTable(const std::string& dbDir, const std::string& name) const {
        fs::create_directories(dbDir);
        auto it = tables_.find(name);
        if (it == tables_.end()) return;
        it->second->saveToFile(dbDir + "/" + name + ".tbl");
        rewriteMeta(dbDir);
    }

    void removeTableFile(const std::string& dbDir, const std::string& name) const {
        std::error_code ec;
        fs::remove(dbDir + "/" + name + ".tbl", ec);
        rewriteMeta(dbDir);
    }

    void loadAll(const std::string& dbDir) {
        tables_.clear();
        std::ifstream meta(dbDir + "/catalog.meta");
        if (!meta) return; // no existing database: start empty
        std::string name;
        while (std::getline(meta, name)) {
            if (name.empty()) continue;
            std::string path = dbDir + "/" + name + ".tbl";
            if (fs::exists(path)) {
                tables_[name] = std::make_unique<Table>(Table::loadFromFile(path, name));
            }
        }
    }

private:
    std::unordered_map<std::string, std::unique_ptr<Table>> tables_;

    void rewriteMeta(const std::string& dbDir) const {
        std::ofstream meta(dbDir + "/catalog.meta", std::ios::trunc);
        for (auto& [name, table] : tables_) meta << name << "\n";
    }
};

} // namespace minisql
