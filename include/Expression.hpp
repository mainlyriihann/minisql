// Expression.hpp
// Evaluates AST Expr nodes (from WHERE / SET / ON clauses) against an
// EvalContext: an ordered list of (alias, Schema*, Row*) sources, supporting
// both single-table and multi-table (JOIN) evaluation with qualified
// (table.col) or unqualified (col) column references.
#pragma once

#include "AST.hpp"
#include "Types.hpp"
#include <vector>
#include <string>
#include <stdexcept>
#include <tuple>

namespace minisql {

struct EvalSource {
    std::string alias;      // table name or AS alias
    const Schema* schema;
    const Row* row;
};

struct EvalContext {
    std::vector<EvalSource> sources;

    Value resolveColumn(const std::string& table, const std::string& col) const {
        if (!table.empty()) {
            for (const auto& src : sources) {
                if (src.alias == table) {
                    int idx = src.schema->indexOf(col);
                    if (idx < 0) throw std::runtime_error("Unknown column '" + table + "." + col + "'");
                    return (*src.row)[idx];
                }
            }
            throw std::runtime_error("Unknown table or alias '" + table + "'");
        }
        // Unqualified: search all sources, error on ambiguity.
        const Value* found = nullptr;
        std::string foundIn;
        for (const auto& src : sources) {
            int idx = src.schema->indexOf(col);
            if (idx >= 0) {
                if (found != nullptr) {
                    throw std::runtime_error("Ambiguous column reference '" + col + "' (found in both '" +
                        foundIn + "' and '" + src.alias + "')");
                }
                found = &(*src.row)[idx];
                foundIn = src.alias;
            }
        }
        if (!found) throw std::runtime_error("Unknown column '" + col + "'");
        return *found;
    }
};

inline Value evalExpr(const Expr* e, const EvalContext& ctx) {
    switch (e->kind) {
        case ExprKind::Literal:
            return e->literal;

        case ExprKind::Column:
            return ctx.resolveColumn(e->columnTable, e->columnName);

        case ExprKind::UnaryOp: {
            Value v = evalExpr(e->operand.get(), ctx);
            if (e->unOp == UnOp::NOT) {
                if (v.isNull) return Value::makeNull(DataType::BOOL);
                bool b = (v.type == DataType::BOOL) ? v.b : (v.asNumeric() != 0.0);
                return Value::makeBool(!b);
            } else { // NEG
                if (v.isNull) return Value::makeNull(v.type);
                if (v.type == DataType::FLOAT) return Value::makeFloat(-v.d);
                return Value::makeInt(-v.i);
            }
        }

        case ExprKind::BinaryOp: {
            const Expr* le = e->left.get();
            const Expr* re = e->right.get();

            if (e->binOp == BinOp::AND || e->binOp == BinOp::OR) {
                Value lv = evalExpr(le, ctx);
                bool lb = lv.isNull ? false : (lv.type == DataType::BOOL ? lv.b : lv.asNumeric() != 0.0);
                if (e->binOp == BinOp::AND && !lb) return Value::makeBool(false);
                if (e->binOp == BinOp::OR && lb) return Value::makeBool(true);
                Value rv = evalExpr(re, ctx);
                bool rb = rv.isNull ? false : (rv.type == DataType::BOOL ? rv.b : rv.asNumeric() != 0.0);
                return Value::makeBool(rb);
            }

            Value lv = evalExpr(le, ctx);
            Value rv = evalExpr(re, ctx);

            bool isComparison =
                e->binOp == BinOp::EQ || e->binOp == BinOp::NEQ || e->binOp == BinOp::LT ||
                e->binOp == BinOp::LTE || e->binOp == BinOp::GT || e->binOp == BinOp::GTE;

            // SQL three-valued logic: any comparison against NULL yields
            // NULL/UNKNOWN, not a concrete TRUE or FALSE. This matters for
            // correctness of NOT(...): NOT UNKNOWN is still UNKNOWN (and thus
            // excluded from WHERE results), not TRUE. Arithmetic operators
            // handle their own NULL propagation below (result type FLOAT/INT,
            // not BOOL).
            if (isComparison && (lv.isNull || rv.isNull)) return Value::makeNull(DataType::BOOL);

            switch (e->binOp) {
                case BinOp::EQ:  return Value::makeBool(lv.compare(rv) == 0);
                case BinOp::NEQ: return Value::makeBool(lv.compare(rv) != 0);
                case BinOp::LT:  return Value::makeBool(lv.compare(rv) < 0);
                case BinOp::LTE: return Value::makeBool(lv.compare(rv) <= 0);
                case BinOp::GT:  return Value::makeBool(lv.compare(rv) > 0);
                case BinOp::GTE: return Value::makeBool(lv.compare(rv) >= 0);

                case BinOp::ADD: case BinOp::SUB: case BinOp::MUL: case BinOp::DIV: {
                    if (lv.isNull || rv.isNull) return Value::makeNull(DataType::FLOAT);
                    bool useFloat = (lv.type == DataType::FLOAT || rv.type == DataType::FLOAT);
                    double a = lv.asNumeric(), b = rv.asNumeric();
                    double result;
                    switch (e->binOp) {
                        case BinOp::ADD: result = a + b; break;
                        case BinOp::SUB: result = a - b; break;
                        case BinOp::MUL: result = a * b; break;
                        default:
                            if (b == 0.0) throw std::runtime_error("Division by zero");
                            result = a / b;
                            break;
                    }
                    if (useFloat) return Value::makeFloat(result);
                    return Value::makeInt(static_cast<int64_t>(result));
                }
                default:
                    throw std::runtime_error("Unhandled binary operator");
            }
        }
    }
    throw std::runtime_error("Unhandled expression kind");
}

// Convenience: evaluate a boolean expression (WHERE/ON clause), treating
// NULL as false, per standard SQL three-valued-logic semantics.
inline bool evalBool(const Expr* e, const EvalContext& ctx) {
    if (e == nullptr) return true;
    Value v = evalExpr(e, ctx);
    if (v.isNull) return false;
    if (v.type == DataType::BOOL) return v.b;
    return v.asNumeric() != 0.0;
}

} // namespace minisql
