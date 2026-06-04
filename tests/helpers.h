//
// Shared test utilities — lex/parse/analyse from a source string.
//

#pragma once

#include "../source/lexer/Lexer.h"
#include "../source/parser/Parser.h"
#include "../source/semantic/SemanticAnalyzer.h"
#include "../source/codegen/CodeGen.h"
#include "../source/codegen/IRPrinter.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

// ---- RAII stderr capture ----
// Redirects std::cerr for the duration of its lifetime.
// Restores automatically (even on test failure / exception).

struct StderrCapture {
    std::ostringstream buf;
    std::streambuf*    old;

    StderrCapture()  : old(std::cerr.rdbuf(buf.rdbuf())) {}
    ~StderrCapture() { std::cerr.rdbuf(old); }

    std::string str() const { return buf.str(); }
    bool        contains(const std::string& substr) const {
        return buf.str().find(substr) != std::string::npos;
    }
};

// ---- Pipeline helpers ----

namespace detail {
    inline std::string writeTempSource(const std::string& source) {
        auto path = (std::filesystem::temp_directory_path() / "gg_test.gg").string();
        std::ofstream(path) << source;
        return path;
    }
}

// Lex a source string and return all tokens (including the trailing EOF).
inline std::vector<Token> lexString(const std::string& source) {
    std::string path = detail::writeTempSource(source);
    std::vector<std::string> paths{ path };
    Lexer lexer(paths);
    lexer.lex();
    return lexer.tokens()[0];
}

// Lex + parse a source string and return the Program AST.
inline Program parseString(const std::string& source) {
    auto tokens = lexString(source);
    Parser parser;
    return parser.parse(tokens);
}

// Lex + parse + analyse a source string and return the SemanticResult.
inline SemanticResult analyzeString(const std::string& source) {
    Program ast = parseString(source);
    SemanticAnalyzer analyzer;
    return analyzer.analyze(ast);
}

// Lex + parse + analyse + codegen a source string and return the IR text.
inline std::string codegenString(const std::string& source) {
    Program ast = parseString(source);
    SemanticAnalyzer analyzer;
    SemanticResult sem = analyzer.analyze(ast);
    CodeGen cg;
    IRModule ir = cg.generate(ast, sem.typeMap);
    std::ostringstream oss;
    IRPrinter printer;
    printer.print(ir, oss);
    return oss.str();
}

// ---- AST unwrap helpers ----
// These call REQUIRE so a wrong-variant failure shows up cleanly.

template<typename T>
const T& asStmt(const Stmt& s) {
    REQUIRE(std::holds_alternative<T>(*s.node));
    return std::get<T>(*s.node);
}

template<typename T>
const T& asExpr(const Expr& e) {
    REQUIRE(std::holds_alternative<T>(*e.node));
    return std::get<T>(*e.node);
}
