// main.cpp
// Interactive REPL and batch (.sql script) runner for the minisql engine.
//
// Usage:
//   minisql [dbdir]                 interactive REPL, persisting to dbdir (default: ./data)
//   minisql [dbdir] -f script.sql   run all statements in script.sql, then exit
#include "../include/Catalog.hpp"
#include "../include/Executor.hpp"
#include "../include/Lexer.hpp"
#include "../include/Parser.hpp"
#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>

using namespace minisql;

static void printResult(const QueryResult& r) {
    if (r.isTabular) {
        std::vector<size_t> widths(r.columnNames.size());
        for (size_t i = 0; i < r.columnNames.size(); ++i) widths[i] = r.columnNames[i].size();
        for (auto& row : r.rows) {
            for (size_t i = 0; i < row.size() && i < widths.size(); ++i) {
                widths[i] = std::max(widths[i], row[i].size());
            }
        }

        auto printSeparator = [&]() {
            std::cout << "+";
            for (auto w : widths) std::cout << std::string(w + 2, '-') << "+";
            std::cout << "\n";
        };

        printSeparator();
        std::cout << "|";
        for (size_t i = 0; i < r.columnNames.size(); ++i) {
            std::cout << " " << std::left << std::setw(static_cast<int>(widths[i])) << r.columnNames[i] << " |";
        }
        std::cout << "\n";
        printSeparator();

        for (auto& row : r.rows) {
            std::cout << "|";
            for (size_t i = 0; i < row.size(); ++i) {
                std::cout << " " << std::left << std::setw(static_cast<int>(widths[i])) << row[i] << " |";
            }
            std::cout << "\n";
        }
        printSeparator();
        std::cout << r.message << "\n";
    } else {
        std::cout << r.message << "\n";
    }
}

// Runs each statement independently: a parse error or runtime error in one
// statement is reported and execution continues with the next statement,
// the same way sqlite3's CLI (and most SQL script runners) behave. Returns
// the number of statements that failed.
static int runStatementText(Executor& executor, const std::string& sqlText) {
    int failures = 0;
    Lexer lexer(sqlText);
    std::vector<Token> tokens;
    try {
        tokens = lexer.tokenize();
    } catch (const std::exception& e) {
        std::cerr << "Lex error: " << e.what() << "\n";
        return 1;
    }

    Parser parser(std::move(tokens));
    while (true) {
        parser.skipStatementSeparators();
        if (parser.isAtEnd()) break;

        Statement stmt;
        try {
            stmt = parser.parseOneStatement();
        } catch (const std::exception& e) {
            std::cerr << "Error: " << e.what() << "\n";
            parser.recoverToNextStatement();
            ++failures;
            continue;
        }

        try {
            QueryResult r = executor.execute(stmt);
            printResult(r);
        } catch (const std::exception& e) {
            std::cerr << "Error: " << e.what() << "\n";
            ++failures;
        }
    }
    return failures;
}

static void runScriptFile(Executor& executor, const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        std::cerr << "Cannot open script file: " << path << "\n";
        std::exit(1);
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    int failures = runStatementText(executor, buf.str());
    if (failures > 0) {
        std::cerr << failures << " statement(s) failed.\n";
        std::exit(1);
    }
}

static void printBanner() {
    std::cout << "minisql -- a small SQL RDBMS engine (C++17)\n";
    std::cout << "Type SQL statements terminated by ';'. Meta-commands: .tables .help .exit\n";
}

static void repl(Executor& executor) {
    printBanner();
    std::string buffer;
    std::string line;
    std::cout << "minisql> ";
    while (std::getline(std::cin, line)) {
        std::string trimmed = line;
        // trim leading/trailing whitespace for meta-command detection
        size_t a = trimmed.find_first_not_of(" \t\r\n");
        size_t b = trimmed.find_last_not_of(" \t\r\n");
        std::string stripped = (a == std::string::npos) ? "" : trimmed.substr(a, b - a + 1);

        if (buffer.empty() && !stripped.empty() && stripped[0] == '.') {
            if (stripped == ".exit" || stripped == ".quit") break;
            if (stripped == ".help") {
                std::cout <<
                    "Commands:\n"
                    "  CREATE TABLE name (col TYPE [PRIMARY KEY] [NOT NULL], ...);\n"
                    "  DROP TABLE [IF EXISTS] name;\n"
                    "  INSERT INTO name [(col,...)] VALUES (v,...), (v,...);\n"
                    "  SELECT col,... FROM name [JOIN name ON expr] [WHERE expr] [ORDER BY col [ASC|DESC]] [LIMIT n];\n"
                    "  UPDATE name SET col=expr, ... [WHERE expr];\n"
                    "  DELETE FROM name [WHERE expr];\n"
                    "  SHOW TABLES; DESCRIBE name;\n"
                    "Meta: .tables  .help  .exit\n";
            } else if (stripped == ".tables") {
                try {
                    Lexer lexer("SHOW TABLES;");
                    Parser parser(lexer.tokenize());
                    printResult(executor.execute(parser.parseSingleStatement()));
                } catch (const std::exception& e) {
                    std::cout << "Error: " << e.what() << "\n";
                }
            } else {
                std::cout << "Unknown meta-command: " << stripped << " (try .help)\n";
            }
            std::cout << "minisql> ";
            continue;
        }

        buffer += line;
        buffer += "\n";

        // Execute once we have at least one semicolon (naive statement-complete
        // heuristic; good enough since string literals in this grammar cannot
        // contain unescaped semicolons that would need special handling here,
        // and multi-statement buffers are supported via parseProgram()).
        if (buffer.find(';') != std::string::npos) {
            runStatementText(executor, buffer);
            buffer.clear();
            std::cout << "minisql> ";
        } else {
            std::cout << "     -> ";
        }
    }
    std::cout << "\nBye.\n";
}

int main(int argc, char** argv) {
    std::string dbDir = "./data";
    std::string scriptPath;

    std::vector<std::string> args(argv + 1, argv + argc);
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "-f" && i + 1 < args.size()) {
            scriptPath = args[i + 1];
            ++i;
        } else {
            dbDir = args[i];
        }
    }

    Catalog catalog;
    try {
        catalog.loadAll(dbDir);
    } catch (const std::exception& e) {
        std::cerr << "Warning: failed to load existing database at '" << dbDir << "': " << e.what() << "\n";
    }

    Executor executor(catalog, dbDir, /*persist=*/true);

    if (!scriptPath.empty()) {
        runScriptFile(executor, scriptPath);
        return 0;
    }

    repl(executor);
    return 0;
}
