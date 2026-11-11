// Executor.hpp
// Executes parsed Statement AST nodes against a Catalog. Produces a
// QueryResult suitable for tabular display (SELECT/SHOW/DESCRIBE) or a
// plain status message (CREATE/DROP/INSERT/UPDATE/DELETE).
#pragma once

#include "AST.hpp"
#include "Catalog.hpp"
#include "Expression.hpp"
#include <deque>
#include <algorithm>
#include <sstream>

namespace minisql {

struct QueryResult {
    bool isTabular = false;
    std::vector<std::string> columnNames;
    std::vector<std::vector<std::string>> rows;
    std::string message;
    size_t rowsAffected = 0;
};

class Executor {
public:
    Executor(Catalog& catalog, std::string dbDir, bool persist)
        : catalog_(catalog), dbDir_(std::move(dbDir)), persist_(persist) {}

    QueryResult execute(const Statement& stmt) {
        switch (stmt.type) {
            case StmtType::CreateTable: return execCreateTable(stmt.createTable);
            case StmtType::DropTable:   return execDropTable(stmt.dropTable);
            case StmtType::Insert:      return execInsert(stmt.insert);
            case StmtType::Select:      return execSelect(stmt.select);
            case StmtType::Update:      return execUpdate(stmt.update);
            case StmtType::Delete:      return execDelete(stmt.del);
            case StmtType::ShowTables:  return execShowTables();
            case StmtType::DescribeTable: return execDescribe(stmt.describeTarget);
        }
        throw std::runtime_error("Unhandled statement type");
    }

private:
    Catalog& catalog_;
    std::string dbDir_;
    bool persist_;

    using JoinRow = std::vector<std::pair<std::string, const Row*>>;

    static Value convertToColumnType(const Value& v, DataType type) {
        if (v.isNull) return Value::makeNull(type);
        if (type == DataType::INT) {
            if (v.type == DataType::INT) return v;
            throw std::runtime_error("Expected INT value, got " + dataTypeToString(v.type));
        }
        if (type == DataType::FLOAT) {
            if (v.type == DataType::FLOAT) return v;
            if (v.type == DataType::INT) return Value::makeFloat(static_cast<double>(v.i));
            throw std::runtime_error("Expected FLOAT value, got " + dataTypeToString(v.type));
        }
        if (type == DataType::TEXT) {
            if (v.type == DataType::TEXT) return v;
            throw std::runtime_error("Expected TEXT value, got " + dataTypeToString(v.type));
        }
        if (type == DataType::BOOL) {
            if (v.type == DataType::BOOL) return v;
            throw std::runtime_error("Expected BOOL value, got " + dataTypeToString(v.type));
        }
        throw std::runtime_error("Unknown column type");
    }

    QueryResult execCreateTable(const CreateTableStmt& s) {
        QueryResult r;
        if (catalog_.hasTable(s.tableName)) {
            if (s.ifNotExists) {
                r.message = "Table '" + s.tableName + "' already exists, skipped.";
                return r;
            }
            throw std::runtime_error("Table '" + s.tableName + "' already exists");
        }
        Schema schema;
        for (auto& c : s.columns) {
            Column col;
            col.name = c.name;
            col.type = c.type;
            col.primaryKey = c.primaryKey;
            col.notNull = c.notNull;
            schema.columns.push_back(col);
        }
        catalog_.createTable(s.tableName, schema);
        if (persist_) catalog_.saveTable(dbDir_, s.tableName);
        r.message = "Table '" + s.tableName + "' created.";
        return r;
    }

    QueryResult execDropTable(const DropTableStmt& s) {
        QueryResult r;
        if (!catalog_.hasTable(s.tableName)) {
            if (s.ifExists) {
                r.message = "Table '" + s.tableName + "' does not exist, skipped.";
                return r;
            }
            throw std::runtime_error("Table '" + s.tableName + "' does not exist");
        }
        catalog_.dropTable(s.tableName);
        if (persist_) catalog_.removeTableFile(dbDir_, s.tableName);
        r.message = "Table '" + s.tableName + "' dropped.";
        return r;
    }

    QueryResult execInsert(const InsertStmt& s) {
        Table* table = catalog_.getTable(s.tableName);
        const Schema& schema = table->schema;

        // Determine which schema index each *provided* value position maps to.
        std::vector<int> targetIndices;
        if (s.columns.empty()) {
            for (size_t i = 0; i < schema.size(); ++i) targetIndices.push_back(static_cast<int>(i));
        } else {
            for (auto& colName : s.columns) {
                int idx = schema.indexOf(colName);
                if (idx < 0) throw std::runtime_error("Unknown column '" + colName + "' in INSERT");
                targetIndices.push_back(idx);
            }
        }

        EvalContext emptyCtx; // INSERT VALUES may not reference columns
        size_t inserted = 0;
        for (auto& valueRow : s.valueRows) {
            if (valueRow.size() != targetIndices.size()) {
                throw std::runtime_error("INSERT value count (" + std::to_string(valueRow.size()) +
                    ") does not match column count (" + std::to_string(targetIndices.size()) + ")");
            }
            Row row(schema.size());
            for (size_t i = 0; i < schema.size(); ++i) row[i] = Value::makeNull(schema.columns[i].type);

            for (size_t i = 0; i < valueRow.size(); ++i) {
                Value v = evalExpr(valueRow[i].get(), emptyCtx);
                int schemaIdx = targetIndices[i];
                row[schemaIdx] = convertToColumnType(v, schema.columns[schemaIdx].type);
            }
            table->insertRow(std::move(row));
            ++inserted;
        }

        if (persist_) catalog_.saveTable(dbDir_, s.tableName);
        QueryResult r;
        r.rowsAffected = inserted;
        r.message = std::to_string(inserted) + " row(s) inserted into '" + s.tableName + "'.";
        return r;
    }

        // If the WHERE clause contains (or AND-conjoins) an equality test
        // against the primary key column, use the B+Tree index for an O(log n)
        // point lookup instead of a full O(n) heap scan. Any remaining
        // conjuncts are still checked afterward by the normal WHERE filter,
        // so this is purely a scan-narrowing optimization, never a
        // correctness shortcut.
        static bool extractPKEquality(const Expr* e, const std::string& alias,
                                       const std::string& pkCol, Value& outVal) {
            if (!e) return false;
            if (e->kind == ExprKind::BinaryOp && e->binOp == BinOp::AND) {
                Value tmp;
                if (extractPKEquality(e->left.get(), alias, pkCol, tmp)) { outVal = tmp; return true; }
                if (extractPKEquality(e->right.get(), alias, pkCol, tmp)) { outVal = tmp; return true; }
                return false;
            }
            if (e->kind == ExprKind::BinaryOp && e->binOp == BinOp::EQ) {
                const Expr* colExpr = nullptr;
                const Expr* litExpr = nullptr;
                if (e->left->kind == ExprKind::Column && e->right->kind == ExprKind::Literal) {
                    colExpr = e->left.get(); litExpr = e->right.get();
                } else if (e->right->kind == ExprKind::Column && e->left->kind == ExprKind::Literal) {
                    colExpr = e->right.get(); litExpr = e->left.get();
                } else {
                    return false;
                }
                if (colExpr->columnName != pkCol) return false;
                if (!colExpr->columnTable.empty() && colExpr->columnTable != alias) return false;
                outVal = litExpr->literal;
                return true;
            }
            return false;
        }

    QueryResult execSelect(const SelectStmt& s) {
        std::string fromAlias = s.fromAlias.empty() ? s.fromTable : s.fromAlias;
        Table* fromTable = catalog_.getTable(s.fromTable);

        struct SourceMeta { std::string alias; Table* table; };
        std::vector<SourceMeta> sourcesMeta;
        sourcesMeta.push_back({fromAlias, fromTable});

        std::vector<size_t> baseSlots;
        bool usedIndexLookup = false;
        if (s.joins.empty() && fromTable->hasPrimaryKey() && s.whereClause) {
            const std::string& pkCol = fromTable->schema.columns[fromTable->primaryKeyColumnIndex()].name;
            Value pkVal;
            if (extractPKEquality(s.whereClause.get(), fromAlias, pkCol, pkVal)) {
                usedIndexLookup = true;
                long slot = fromTable->findByPrimaryKey(pkVal);
                if (slot >= 0) baseSlots.push_back(static_cast<size_t>(slot));
            }
        }
        if (!usedIndexLookup) baseSlots = fromTable->allRowSlots();

        std::vector<JoinRow> combined;
        for (size_t slot : baseSlots) {
            combined.push_back(JoinRow{{fromAlias, &fromTable->rowAt(slot)}});
        }

        std::deque<Row> nullRowPool; // stable storage for LEFT JOIN unmatched rows

        for (auto& j : s.joins) {
            Table* joinTable = catalog_.getTable(j.tableName);
            std::string alias = j.alias.empty() ? j.tableName : j.alias;
            sourcesMeta.push_back({alias, joinTable});

            std::vector<JoinRow> next;
            auto joinSlots = joinTable->allRowSlots();

            for (auto& cr : combined) {
                bool matched = false;
                for (size_t slot : joinSlots) {
                    JoinRow candidate = cr;
                    candidate.push_back({alias, &joinTable->rowAt(slot)});
                    EvalContext ctx = buildContext(sourcesMeta, candidate);
                    if (evalBool(j.onCondition.get(), ctx)) {
                        next.push_back(candidate);
                        matched = true;
                    }
                }
                if (!matched && j.isLeft) {
                    Row nullRow(joinTable->schema.size());
                    for (size_t i = 0; i < joinTable->schema.size(); ++i) {
                        nullRow[i] = Value::makeNull(joinTable->schema.columns[i].type);
                    }
                    nullRowPool.push_back(std::move(nullRow));
                    JoinRow candidate = cr;
                    candidate.push_back({alias, &nullRowPool.back()});
                    next.push_back(candidate);
                }
            }
            combined = std::move(next);
        }

        // WHERE filter
        std::vector<JoinRow> filtered;
        for (auto& cr : combined) {
            EvalContext ctx = buildContext(sourcesMeta, cr);
            if (evalBool(s.whereClause.get(), ctx)) filtered.push_back(cr);
        }

        // ORDER BY
        if (!s.orderBy.empty()) {
            std::stable_sort(filtered.begin(), filtered.end(), [&](const JoinRow& a, const JoinRow& b) {
                for (auto& ob : s.orderBy) {
                    EvalContext ca = buildContext(sourcesMeta, a);
                    EvalContext cb = buildContext(sourcesMeta, b);
                    auto [tbl, col] = splitQualified(ob.column);
                    Value va = ca.resolveColumn(tbl, col);
                    Value vb = cb.resolveColumn(tbl, col);
                    int c = va.compare(vb);
                    if (c != 0) return ob.descending ? (c > 0) : (c < 0);
                }
                return false;
            });
        }

        // LIMIT
        if (s.limit >= 0 && static_cast<size_t>(s.limit) < filtered.size()) {
            filtered.resize(static_cast<size_t>(s.limit));
        }

        // Projection: resolve column spec list into (header, resolver) pairs.
        bool multiSource = sourcesMeta.size() > 1;
        std::vector<std::pair<std::string, std::pair<std::string, std::string>>> projections; // header -> (table, col)

        auto addAllColumnsOf = [&](const SourceMeta& sm) {
            for (auto& col : sm.table->schema.columns) {
                std::string header = multiSource ? (sm.alias + "." + col.name) : col.name;
                projections.push_back({header, {sm.alias, col.name}});
            }
        };

        for (auto& spec : s.columns) {
            if (spec == "*") {
                for (auto& sm : sourcesMeta) addAllColumnsOf(sm);
            } else if (spec.size() > 2 && spec.substr(spec.size() - 2) == ".*") {
                std::string tbl = spec.substr(0, spec.size() - 2);
                bool found = false;
                for (auto& sm : sourcesMeta) {
                    if (sm.alias == tbl) { addAllColumnsOf(sm); found = true; break; }
                }
                if (!found) throw std::runtime_error("Unknown table or alias '" + tbl + "' in SELECT list");
            } else {
                auto [tbl, col] = splitQualified(spec);
                projections.push_back({spec, {tbl, col}});
            }
        }

        QueryResult result;
        result.isTabular = true;
        for (auto& p : projections) result.columnNames.push_back(p.first);

        for (auto& cr : filtered) {
            EvalContext ctx = buildContext(sourcesMeta, cr);
            std::vector<std::string> displayRow;
            for (auto& p : projections) {
                Value v = ctx.resolveColumn(p.second.first, p.second.second);
                displayRow.push_back(v.toDisplayString());
            }
            result.rows.push_back(std::move(displayRow));
        }
        result.rowsAffected = result.rows.size();
        result.message = std::to_string(result.rows.size()) + " row(s) returned.";
        return result;
    }

    QueryResult execUpdate(const UpdateStmt& s) {
        Table* table = catalog_.getTable(s.tableName);
        std::string alias = s.tableName;
        size_t affected = 0;

        for (size_t slot : table->allRowSlots()) {
            const Row& oldRow = table->rowAt(slot);
            EvalContext ctx;
            ctx.sources.push_back({alias, &table->schema, &oldRow});
            if (!evalBool(s.whereClause.get(), ctx)) continue;

            Row newRow = oldRow;
            for (auto& [colName, expr] : s.assignments) {
                int idx = table->schema.indexOf(colName);
                if (idx < 0) throw std::runtime_error("Unknown column '" + colName + "' in UPDATE");
                Value v = evalExpr(expr.get(), ctx); // evaluated against OLD row values
                newRow[idx] = convertToColumnType(v, table->schema.columns[idx].type);
            }
            table->updateRow(slot, std::move(newRow));
            ++affected;
        }

        if (persist_ && affected > 0) catalog_.saveTable(dbDir_, s.tableName);
        QueryResult r;
        r.rowsAffected = affected;
        r.message = std::to_string(affected) + " row(s) updated in '" + s.tableName + "'.";
        return r;
    }

    QueryResult execDelete(const DeleteStmt& s) {
        Table* table = catalog_.getTable(s.tableName);
        std::string alias = s.tableName;
        size_t affected = 0;

        for (size_t slot : table->allRowSlots()) {
            const Row& row = table->rowAt(slot);
            EvalContext ctx;
            ctx.sources.push_back({alias, &table->schema, &row});
            if (!evalBool(s.whereClause.get(), ctx)) continue;
            table->deleteRow(slot);
            ++affected;
        }

        if (persist_ && affected > 0) catalog_.saveTable(dbDir_, s.tableName);
        QueryResult r;
        r.rowsAffected = affected;
        r.message = std::to_string(affected) + " row(s) deleted from '" + s.tableName + "'.";
        return r;
    }

    QueryResult execShowTables() {
        QueryResult r;
        r.isTabular = true;
        r.columnNames = {"table_name"};
        for (auto& name : catalog_.listTableNames()) r.rows.push_back({name});
        r.message = std::to_string(r.rows.size()) + " table(s).";
        return r;
    }

    QueryResult execDescribe(const std::string& tableName) {
        Table* table = catalog_.getTable(tableName);
        QueryResult r;
        r.isTabular = true;
        r.columnNames = {"column", "type", "primary_key", "not_null"};
        for (auto& col : table->schema.columns) {
            r.rows.push_back({
                col.name,
                dataTypeToString(col.type),
                col.primaryKey ? "yes" : "no",
                col.notNull ? "yes" : "no",
            });
        }
        return r;
    }

    template <typename SourcesMeta>
    static EvalContext buildContext(const SourcesMeta& sourcesMeta, const JoinRow& parts) {
        EvalContext ctx;
        for (size_t i = 0; i < parts.size(); ++i) {
            ctx.sources.push_back({parts[i].first, &sourcesMeta[i].table->schema, parts[i].second});
        }
        return ctx;
    }

    static std::pair<std::string, std::string> splitQualified(const std::string& spec) {
        auto dot = spec.find('.');
        if (dot == std::string::npos) return {"", spec};
        return {spec.substr(0, dot), spec.substr(dot + 1)};
    }
};

} // namespace minisql
