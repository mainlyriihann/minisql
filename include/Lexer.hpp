// Lexer.hpp
// Tokenizer for the SQL subset supported by minisql.
#pragma once

#include <string>
#include <vector>
#include <cctype>
#include <stdexcept>
#include <unordered_set>

namespace minisql {

enum class TokenType {
    Identifier, Keyword, IntLiteral, FloatLiteral, StringLiteral,
    Star, Comma, Semicolon, LParen, RParen, Dot,
    Eq, Neq, Lt, Lte, Gt, Gte,
    Plus, Minus, Slash,
    End
};

struct Token {
    TokenType type;
    std::string text; // original text (for identifiers/keywords: as written; keywords normalized upper in `text`)
    int64_t intVal = 0;
    double floatVal = 0.0;
    int line = 1;
};

inline const std::unordered_set<std::string>& keywordSet() {
    static const std::unordered_set<std::string> kws = {
        "CREATE", "TABLE", "DROP", "IF", "NOT", "EXISTS", "PRIMARY", "KEY", "NULL",
        "INSERT", "INTO", "VALUES", "SELECT", "FROM", "WHERE", "AND", "OR",
        "ORDER", "BY", "ASC", "DESC", "LIMIT", "UPDATE", "SET", "DELETE",
        "JOIN", "INNER", "LEFT", "ON", "AS", "SHOW", "TABLES", "DESCRIBE", "DESC_STMT",
        "INT", "INTEGER", "FLOAT", "DOUBLE", "REAL", "TEXT", "VARCHAR", "STRING", "CHAR",
        "BOOL", "BOOLEAN", "TRUE", "FALSE"
    };
    return kws;
}

class Lexer {
public:
    explicit Lexer(std::string src) : src_(std::move(src)) {}

    std::vector<Token> tokenize() {
        std::vector<Token> tokens;
        while (true) {
            skipWhitespaceAndComments();
            if (pos_ >= src_.size()) {
                tokens.push_back({TokenType::End, "", 0, 0, line_});
                break;
            }
            char c = src_[pos_];

            if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
                tokens.push_back(lexIdentifierOrKeyword());
            } else if (std::isdigit(static_cast<unsigned char>(c))) {
                tokens.push_back(lexNumber());
            } else if (c == '\'') {
                tokens.push_back(lexString());
            } else {
                tokens.push_back(lexSymbol());
            }
        }
        return tokens;
    }

private:
    std::string src_;
    size_t pos_ = 0;
    int line_ = 1;

    void skipWhitespaceAndComments() {
        while (pos_ < src_.size()) {
            char c = src_[pos_];
            if (c == '\n') { ++line_; ++pos_; }
            else if (std::isspace(static_cast<unsigned char>(c))) { ++pos_; }
            else if (c == '-' && pos_ + 1 < src_.size() && src_[pos_ + 1] == '-') {
                while (pos_ < src_.size() && src_[pos_] != '\n') ++pos_;
            } else {
                break;
            }
        }
    }

    Token lexIdentifierOrKeyword() {
        size_t start = pos_;
        while (pos_ < src_.size() && (std::isalnum(static_cast<unsigned char>(src_[pos_])) || src_[pos_] == '_')) {
            ++pos_;
        }
        std::string text = src_.substr(start, pos_ - start);
        std::string upper;
        for (char c : text) upper += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

        Token tok;
        tok.line = line_;
        if (keywordSet().count(upper)) {
            tok.type = TokenType::Keyword;
            tok.text = upper;
        } else {
            tok.type = TokenType::Identifier;
            tok.text = text;
        }
        return tok;
    }

    Token lexNumber() {
        size_t start = pos_;
        bool isFloat = false;
        while (pos_ < src_.size() && std::isdigit(static_cast<unsigned char>(src_[pos_]))) ++pos_;
        if (pos_ < src_.size() && src_[pos_] == '.') {
            // Only treat as float if followed by a digit (avoid eating "5." + "col" weirdly,
            // though standard SQL numeric literals always have digits after the dot).
            if (pos_ + 1 < src_.size() && std::isdigit(static_cast<unsigned char>(src_[pos_ + 1]))) {
                isFloat = true;
                ++pos_;
                while (pos_ < src_.size() && std::isdigit(static_cast<unsigned char>(src_[pos_]))) ++pos_;
            }
        }
        std::string text = src_.substr(start, pos_ - start);
        Token tok;
        tok.line = line_;
        if (isFloat) {
            tok.type = TokenType::FloatLiteral;
            tok.floatVal = std::stod(text);
        } else {
            tok.type = TokenType::IntLiteral;
            tok.intVal = std::stoll(text);
        }
        tok.text = text;
        return tok;
    }

    Token lexString() {
        ++pos_; // skip opening quote
        std::string value;
        while (pos_ < src_.size() && src_[pos_] != '\'') {
            if (src_[pos_] == '\\' && pos_ + 1 < src_.size()) {
                // simple escape handling: \' \\ \n \t
                char next = src_[pos_ + 1];
                switch (next) {
                    case '\'': value += '\''; break;
                    case '\\': value += '\\'; break;
                    case 'n': value += '\n'; break;
                    case 't': value += '\t'; break;
                    default: value += next; break;
                }
                pos_ += 2;
            } else if (src_[pos_] == '\'' + 0 && pos_ + 1 < src_.size() && src_[pos_ + 1] == '\'') {
                // SQL-standard doubled single quote '' -> '
                value += '\'';
                pos_ += 2;
            } else {
                value += src_[pos_];
                ++pos_;
            }
        }
        if (pos_ >= src_.size()) throw std::runtime_error("Unterminated string literal at line " + std::to_string(line_));
        ++pos_; // skip closing quote
        Token tok;
        tok.type = TokenType::StringLiteral;
        tok.text = value;
        tok.line = line_;
        return tok;
    }

    Token lexSymbol() {
        Token tok;
        tok.line = line_;
        char c = src_[pos_];
        auto two = [&](char second) {
            return pos_ + 1 < src_.size() && src_[pos_ + 1] == second;
        };

        switch (c) {
            case '*': tok.type = TokenType::Star; tok.text = "*"; ++pos_; break;
            case ',': tok.type = TokenType::Comma; tok.text = ","; ++pos_; break;
            case ';': tok.type = TokenType::Semicolon; tok.text = ";"; ++pos_; break;
            case '(': tok.type = TokenType::LParen; tok.text = "("; ++pos_; break;
            case ')': tok.type = TokenType::RParen; tok.text = ")"; ++pos_; break;
            case '.': tok.type = TokenType::Dot; tok.text = "."; ++pos_; break;
            case '+': tok.type = TokenType::Plus; tok.text = "+"; ++pos_; break;
            case '-': tok.type = TokenType::Minus; tok.text = "-"; ++pos_; break;
            case '/': tok.type = TokenType::Slash; tok.text = "/"; ++pos_; break;
            case '=': tok.type = TokenType::Eq; tok.text = "="; ++pos_; break;
            case '!':
                if (two('=')) { tok.type = TokenType::Neq; tok.text = "!="; pos_ += 2; }
                else throw std::runtime_error("Unexpected character '!' at line " + std::to_string(line_));
                break;
            case '<':
                if (two('=')) { tok.type = TokenType::Lte; tok.text = "<="; pos_ += 2; }
                else if (two('>')) { tok.type = TokenType::Neq; tok.text = "<>"; pos_ += 2; }
                else { tok.type = TokenType::Lt; tok.text = "<"; ++pos_; }
                break;
            case '>':
                if (two('=')) { tok.type = TokenType::Gte; tok.text = ">="; pos_ += 2; }
                else { tok.type = TokenType::Gt; tok.text = ">"; ++pos_; }
                break;
            default:
                throw std::runtime_error(std::string("Unexpected character '") + c + "' at line " + std::to_string(line_));
        }
        return tok;
    }
};

} // namespace minisql
