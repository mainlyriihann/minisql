// AST.hpp
// Abstract syntax tree node definitions produced by the Parser and consumed
// by the Executor. Expressions (used in WHERE clauses) are a small tree of
// literals, column references, comparisons, and AND/OR/NOT.
#pragma once

#include "Types.hpp"
#include <string>
#include <vector>
#include <memory>

namespace minisql {

// --------------------------- Expressions (WHERE) --------------------------

enum class ExprKind { Literal, Column, BinaryOp, UnaryOp };
enum class BinOp { EQ, NEQ, LT, LTE, GT, GTE, AND, OR, ADD, SUB, MUL, DIV };
enum class UnOp { NOT, NEG };

struct Expr {
    ExprKind kind;

    // Literal
    Value literal;

    // Column reference. `table` is empty unless qualified (e.g. t1.col).
    std::string columnTable;
    std::string columnName;

    // BinaryOp
    BinOp binOp;
    std::unique_ptr<Expr> left;
    std::unique_ptr<Expr> right;

    // UnaryOp
    UnOp unOp;
    std::unique_ptr<Expr> operand;

    static std::unique_ptr<Expr> makeLiteral(Value v) {
        auto e = std::make_unique<Expr>();
        e->kind = ExprKind::Literal;
        e->literal = std::move(v);
        return e;
    }
    static std::unique_ptr<Expr> makeColumn(std::string table, std::string col) {
        auto e = std::make_unique<Expr>();
        e->kind = ExprKind::Column;
        e->columnTable = std::move(table);
        e->columnName = std::move(col);
        return e;
    }
    static std::unique_ptr<Expr> makeBinary(BinOp op, std::unique_ptr<Expr> l, std::unique_ptr<Expr> r) {
        auto e = std::make_unique<Expr>();
        e->kind = ExprKind::BinaryOp;
        e->binOp = op;
        e->left = std::move(l);
        e->right = std::move(r);
        return e;
    }
    static std::unique_ptr<Expr> makeUnary(UnOp op, std::unique_ptr<Expr> operand) {
        auto e = std::make_unique<Expr>();
        e->kind = ExprKind::UnaryOp;
        e->unOp = op;
        e->operand = std::move(operand);
        return e;
    }
};

// ------------------------------ Statements --------------------------------

struct ColumnDef {
    std::string name;
    DataType type;
    bool primaryKey = false;
    bool notNull = false;
};

struct CreateTableStmt {
    std::string tableName;
    std::vector<ColumnDef> columns;
    bool ifNotExists = false;
};

struct DropTableStmt {
    std::string tableName;
    bool ifExists = false;
};

struct InsertStmt {
    std::string tableName;
    std::vector<std::string> columns; // empty => all columns in schema order
    std::vector<std::vector<std::unique_ptr<Expr>>> valueRows; // supports multi-row INSERT
};

struct OrderByItem {
    std::string column;
    bool descending = false;
};

struct JoinClause {
    std::string tableName;
    std::string alias;
    bool isLeft = false; // false => INNER JOIN, true => LEFT [OUTER] JOIN
    std::unique_ptr<Expr> onCondition; // may be null for CROSS JOIN (not currently parsed, kept for completeness)
};

struct SelectStmt {
    std::vector<std::string> columns; // empty or {"*"} => all columns
    std::string fromTable;
    std::string fromAlias;
    std::vector<JoinClause> joins;
    std::unique_ptr<Expr> whereClause; // nullable
    std::vector<OrderByItem> orderBy;
    long limit = -1; // -1 => no limit
};

struct UpdateStmt {
    std::string tableName;
    std::vector<std::pair<std::string, std::unique_ptr<Expr>>> assignments;
    std::unique_ptr<Expr> whereClause; // nullable
};

struct DeleteStmt {
    std::string tableName;
    std::unique_ptr<Expr> whereClause; // nullable
};

enum class StmtType { CreateTable, DropTable, Insert, Select, Update, Delete, ShowTables, DescribeTable };

struct Statement {
    StmtType type;
    CreateTableStmt createTable;
    DropTableStmt dropTable;
    InsertStmt insert;
    SelectStmt select;
    UpdateStmt update;
    DeleteStmt del;
    std::string describeTarget;
};

} // namespace minisql
