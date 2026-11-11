// Parser.hpp
// Recursive-descent parser for the minisql SQL subset. Turns a token stream
// (from Lexer) into a list of Statement AST nodes (one per `;`-terminated
// statement). Expression grammar (WHERE / SET values), lowest to highest
// precedence:
//   OR
//   AND
//   NOT (unary)
//   comparison: = != < <= > >=
//   additive: + -
//   multiplicative: * /
//   unary: - (numeric negation)
//   primary: literal | column-ref | ( expr )
#pragma once

#include "AST.hpp"
#include "Lexer.hpp"
#include <vector>
#include <stdexcept>
#include <memory>

namespace minisql {

class Parser {
public:
    explicit Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

    std::vector<Statement> parseProgram() {
        std::vector<Statement> stmts;
        while (!check(TokenType::End)) {
            // allow stray semicolons between statements
            while (match(TokenType::Semicolon)) {}
            if (check(TokenType::End)) break;
            stmts.push_back(parseStatement());
            while (match(TokenType::Semicolon)) {}
        }
        return stmts;
    }

    Statement parseSingleStatement() {
        while (match(TokenType::Semicolon)) {}
        Statement s = parseStatement();
        while (match(TokenType::Semicolon)) {}
        expect(TokenType::End, "end of statement");
        return s;
    }

    // ---- incremental, error-recoverable interface (used by script runner) ----
    bool isAtEnd() const { return check(TokenType::End); }

    void skipStatementSeparators() { while (match(TokenType::Semicolon)) {} }

    // Parses exactly one statement (leading separators must already be skipped
    // by the caller via skipStatementSeparators()).
    Statement parseOneStatement() { return parseStatement(); }

    // On a parse error, call this to discard tokens up to and including the
    // next ';' (or End), so parsing of subsequent statements can continue.
    void recoverToNextStatement() {
        while (!check(TokenType::Semicolon) && !check(TokenType::End)) advance();
        if (check(TokenType::Semicolon)) advance();
    }

private:
    std::vector<Token> tokens_;
    size_t pos_ = 0;

    // ---- token stream helpers ----
    const Token& peek(int offset = 0) const { return tokens_[std::min(pos_ + offset, tokens_.size() - 1)]; }
    const Token& advance() { const Token& t = tokens_[pos_]; if (pos_ + 1 < tokens_.size()) ++pos_; return t; }
    bool check(TokenType t) const { return peek().type == t; }
    bool checkKeyword(const std::string& kw) const { return peek().type == TokenType::Keyword && peek().text == kw; }

    bool match(TokenType t) {
        if (check(t)) { advance(); return true; }
        return false;
    }
    bool matchKeyword(const std::string& kw) {
        if (checkKeyword(kw)) { advance(); return true; }
        return false;
    }
    const Token& expect(TokenType t, const std::string& what) {
        if (!check(t)) error("Expected " + what + " but found '" + peek().text + "'");
        return advance();
    }
    const Token& expectKeyword(const std::string& kw) {
        if (!checkKeyword(kw)) error("Expected keyword " + kw + " but found '" + peek().text + "'");
        return advance();
    }
    [[noreturn]] void error(const std::string& msg) {
        throw std::runtime_error("Parse error (line " + std::to_string(peek().line) + "): " + msg);
    }

    // ---- statement dispatch ----
    Statement parseStatement() {
        if (checkKeyword("CREATE")) return parseCreateTable();
        if (checkKeyword("DROP")) return parseDropTable();
        if (checkKeyword("INSERT")) return parseInsert();
        if (checkKeyword("SELECT")) return parseSelect();
        if (checkKeyword("UPDATE")) return parseUpdate();
        if (checkKeyword("DELETE")) return parseDelete();
        if (checkKeyword("SHOW")) return parseShowTables();
        if (checkKeyword("DESCRIBE")) return parseDescribe();
        error("Unrecognized statement starting with '" + peek().text + "'");
    }

    std::string parseIdentifierName() {
        if (check(TokenType::Identifier)) return advance().text;
        // allow keywords-as-identifiers in a pinch? No -- keep it strict for clarity.
        error("Expected identifier but found '" + peek().text + "'");
    }

    // CREATE TABLE [IF NOT EXISTS] name ( col type [PRIMARY KEY] [NOT NULL] (, ...)* )
    Statement parseCreateTable() {
        expectKeyword("CREATE");
        expectKeyword("TABLE");
        Statement s;
        s.type = StmtType::CreateTable;
        if (matchKeyword("IF")) {
            expectKeyword("NOT");
            expectKeyword("EXISTS");
            s.createTable.ifNotExists = true;
        }
        s.createTable.tableName = parseIdentifierName();
        expect(TokenType::LParen, "'('");

        do {
            ColumnDef col;
            col.name = parseIdentifierName();
            col.type = parseTypeName();
            while (checkKeyword("PRIMARY") || checkKeyword("NOT") || checkKeyword("NULL")) {
                if (matchKeyword("PRIMARY")) {
                    expectKeyword("KEY");
                    col.primaryKey = true;
                    col.notNull = true; // primary key implies not null
                } else if (matchKeyword("NOT")) {
                    expectKeyword("NULL");
                    col.notNull = true;
                } else if (matchKeyword("NULL")) {
                    // explicit NULL: no-op, column already nullable by default
                }
            }
            s.createTable.columns.push_back(col);
        } while (match(TokenType::Comma));

        expect(TokenType::RParen, "')'");
        return s;
    }

    DataType parseTypeName() {
        if (!check(TokenType::Keyword)) error("Expected a type name but found '" + peek().text + "'");
        std::string t = advance().text;
        return dataTypeFromString(t);
    }

    // DROP TABLE [IF EXISTS] name
    Statement parseDropTable() {
        expectKeyword("DROP");
        expectKeyword("TABLE");
        Statement s;
        s.type = StmtType::DropTable;
        if (matchKeyword("IF")) {
            expectKeyword("EXISTS");
            s.dropTable.ifExists = true;
        }
        s.dropTable.tableName = parseIdentifierName();
        return s;
    }

    // INSERT INTO name [(col, ...)] VALUES (expr, ...), (expr, ...)*
    Statement parseInsert() {
        expectKeyword("INSERT");
        expectKeyword("INTO");
        Statement s;
        s.type = StmtType::Insert;
        s.insert.tableName = parseIdentifierName();

        if (match(TokenType::LParen)) {
            do {
                s.insert.columns.push_back(parseIdentifierName());
            } while (match(TokenType::Comma));
            expect(TokenType::RParen, "')'");
        }

        expectKeyword("VALUES");
        do {
            expect(TokenType::LParen, "'('");
            std::vector<std::unique_ptr<Expr>> row;
            do {
                row.push_back(parseExpr());
            } while (match(TokenType::Comma));
            expect(TokenType::RParen, "')'");
            s.insert.valueRows.push_back(std::move(row));
        } while (match(TokenType::Comma));

        return s;
    }

    // SELECT col,col|* FROM name [alias] [JOIN name [alias] ON expr]*
    //   [WHERE expr] [ORDER BY col [ASC|DESC] (, ...)*] [LIMIT n]
    Statement parseSelect() {
        expectKeyword("SELECT");
        Statement s;
        s.type = StmtType::Select;

        if (match(TokenType::Star)) {
            s.select.columns.push_back("*");
        } else {
            do {
                s.select.columns.push_back(parseColumnRefText());
            } while (match(TokenType::Comma));
        }

        expectKeyword("FROM");
        s.select.fromTable = parseIdentifierName();
        s.select.fromAlias = maybeParseAlias();

        while (checkKeyword("JOIN") || checkKeyword("INNER") || checkKeyword("LEFT")) {
            JoinClause jc;
            if (matchKeyword("INNER")) { expectKeyword("JOIN"); }
            else if (matchKeyword("LEFT")) { expectKeyword("JOIN"); jc.isLeft = true; }
            else { expectKeyword("JOIN"); }
            jc.tableName = parseIdentifierName();
            jc.alias = maybeParseAlias();
            expectKeyword("ON");
            jc.onCondition = parseExpr();
            s.select.joins.push_back(std::move(jc));
        }

        if (matchKeyword("WHERE")) {
            s.select.whereClause = parseExpr();
        }

        if (matchKeyword("ORDER")) {
            expectKeyword("BY");
            do {
                OrderByItem item;
                item.column = parseColumnRefText();
                if (matchKeyword("DESC")) item.descending = true;
                else matchKeyword("ASC");
                s.select.orderBy.push_back(item);
            } while (match(TokenType::Comma));
        }

        if (matchKeyword("LIMIT")) {
            const Token& t = expect(TokenType::IntLiteral, "integer after LIMIT");
            s.select.limit = t.intVal;
        }

        return s;
    }

    std::string maybeParseAlias() {
        if (matchKeyword("AS")) return parseIdentifierName();
        if (check(TokenType::Identifier)) return advance().text;
        return "";
    }

    // Returns "table.col" or "col" as plain text (used for SELECT list / ORDER BY,
    // where we just need a name to resolve later, not a full expression).
    std::string parseColumnRefText() {
        if (check(TokenType::Star)) { advance(); return "*"; }
        std::string first = parseIdentifierName();
        if (match(TokenType::Dot)) {
            if (check(TokenType::Star)) { advance(); return first + ".*"; }
            std::string second = parseIdentifierName();
            return first + "." + second;
        }
        return first;
    }

    // UPDATE name SET col = expr (, col = expr)* [WHERE expr]
    Statement parseUpdate() {
        expectKeyword("UPDATE");
        Statement s;
        s.type = StmtType::Update;
        s.update.tableName = parseIdentifierName();
        expectKeyword("SET");
        do {
            std::string col = parseIdentifierName();
            expect(TokenType::Eq, "'='");
            auto expr = parseExpr();
            s.update.assignments.emplace_back(col, std::move(expr));
        } while (match(TokenType::Comma));

        if (matchKeyword("WHERE")) {
            s.update.whereClause = parseExpr();
        }
        return s;
    }

    // DELETE FROM name [WHERE expr]
    Statement parseDelete() {
        expectKeyword("DELETE");
        expectKeyword("FROM");
        Statement s;
        s.type = StmtType::Delete;
        s.del.tableName = parseIdentifierName();
        if (matchKeyword("WHERE")) {
            s.del.whereClause = parseExpr();
        }
        return s;
    }

    Statement parseShowTables() {
        expectKeyword("SHOW");
        expectKeyword("TABLES");
        Statement s;
        s.type = StmtType::ShowTables;
        return s;
    }

    Statement parseDescribe() {
        expectKeyword("DESCRIBE");
        Statement s;
        s.type = StmtType::DescribeTable;
        s.describeTarget = parseIdentifierName();
        return s;
    }

    // ---- expression parsing (precedence climbing) ----
    std::unique_ptr<Expr> parseExpr() { return parseOr(); }

    std::unique_ptr<Expr> parseOr() {
        auto left = parseAnd();
        while (matchKeyword("OR")) {
            auto right = parseAnd();
            left = Expr::makeBinary(BinOp::OR, std::move(left), std::move(right));
        }
        return left;
    }

    std::unique_ptr<Expr> parseAnd() {
        auto left = parseNot();
        while (matchKeyword("AND")) {
            auto right = parseNot();
            left = Expr::makeBinary(BinOp::AND, std::move(left), std::move(right));
        }
        return left;
    }

    std::unique_ptr<Expr> parseNot() {
        if (matchKeyword("NOT")) {
            auto operand = parseNot();
            return Expr::makeUnary(UnOp::NOT, std::move(operand));
        }
        return parseComparison();
    }

    std::unique_ptr<Expr> parseComparison() {
        auto left = parseAdditive();
        if (check(TokenType::Eq) || check(TokenType::Neq) || check(TokenType::Lt) ||
            check(TokenType::Lte) || check(TokenType::Gt) || check(TokenType::Gte)) {
            BinOp op;
            switch (advance().type) {
                case TokenType::Eq: op = BinOp::EQ; break;
                case TokenType::Neq: op = BinOp::NEQ; break;
                case TokenType::Lt: op = BinOp::LT; break;
                case TokenType::Lte: op = BinOp::LTE; break;
                case TokenType::Gt: op = BinOp::GT; break;
                default: op = BinOp::GTE; break;
            }
            auto right = parseAdditive();
            return Expr::makeBinary(op, std::move(left), std::move(right));
        }
        return left;
    }

    std::unique_ptr<Expr> parseAdditive() {
        auto left = parseMultiplicative();
        while (check(TokenType::Plus) || check(TokenType::Minus)) {
            BinOp op = check(TokenType::Plus) ? BinOp::ADD : BinOp::SUB;
            advance();
            auto right = parseMultiplicative();
            left = Expr::makeBinary(op, std::move(left), std::move(right));
        }
        return left;
    }

    std::unique_ptr<Expr> parseMultiplicative() {
        auto left = parseUnary();
        while (check(TokenType::Star) || check(TokenType::Slash)) {
            BinOp op = check(TokenType::Star) ? BinOp::MUL : BinOp::DIV;
            advance();
            auto right = parseUnary();
            left = Expr::makeBinary(op, std::move(left), std::move(right));
        }
        return left;
    }

    std::unique_ptr<Expr> parseUnary() {
        if (match(TokenType::Minus)) {
            auto operand = parseUnary();
            return Expr::makeUnary(UnOp::NEG, std::move(operand));
        }
        return parsePrimary();
    }

    std::unique_ptr<Expr> parsePrimary() {
        if (match(TokenType::LParen)) {
            auto e = parseExpr();
            expect(TokenType::RParen, "')'");
            return e;
        }
        if (check(TokenType::IntLiteral)) {
            int64_t v = advance().intVal;
            return Expr::makeLiteral(Value::makeInt(v));
        }
        if (check(TokenType::FloatLiteral)) {
            double v = advance().floatVal;
            return Expr::makeLiteral(Value::makeFloat(v));
        }
        if (check(TokenType::StringLiteral)) {
            std::string v = advance().text;
            return Expr::makeLiteral(Value::makeText(v));
        }
        if (checkKeyword("TRUE")) { advance(); return Expr::makeLiteral(Value::makeBool(true)); }
        if (checkKeyword("FALSE")) { advance(); return Expr::makeLiteral(Value::makeBool(false)); }
        if (checkKeyword("NULL")) { advance(); return Expr::makeLiteral(Value::makeNull(DataType::INT)); }
        if (check(TokenType::Identifier)) {
            std::string first = advance().text;
            if (match(TokenType::Dot)) {
                std::string second = parseIdentifierName();
                return Expr::makeColumn(first, second);
            }
            return Expr::makeColumn("", first);
        }
        error("Unexpected token in expression: '" + peek().text + "'");
    }
};

} // namespace minisql
