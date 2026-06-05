//
// Shared test utilities — lex/parse/analyse from a source string.
//

#ifndef GG_HELPERS_H
#define GG_HELPERS_H

#include "../source/ImportResolver.h"
#include "../source/lexer/Lexer.h"
#include "../source/lexer/Token.h"
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
    std::ostringstream buffer;
    std::streambuf*    old;

    StderrCapture()  : old(std::cerr.rdbuf(buffer.rdbuf())) {}
    ~StderrCapture() { std::cerr.rdbuf(old); }

    std::string str() const { return buffer.str(); }
    bool        contains(const std::string& substr) const {
        return buffer.str().find(substr) != std::string::npos;
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

// Lex a source string and return the token list (used by lexer tests).
inline std::vector<Token> lexString(const std::string& source) {
    std::string path = detail::writeTempSource(source);
    std::vector<std::string> paths{ path };
    Lexer lexer(paths);
    lexer.lex();
    return lexer.tokens()[0];
}

// Lex + parse a source string WITHOUT resolving imports.
// Use this when testing the import syntax itself (ImportStmt nodes are preserved).
inline Program parseStringRaw(const std::string& source) {
    std::string path = detail::writeTempSource(source);
    std::vector<std::string> paths{ path };
    Lexer lexer(paths);
    lexer.lex();
    Parser parser;
    return parser.parse(lexer.tokens()[0]);
}

// Lex + parse + resolve imports for a source string and return the Program AST.
inline Program parseString(const std::string& source) {
    std::string path = detail::writeTempSource(source);
    ImportResolver resolver;
    return resolver.resolve(path);
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
    SemanticResult semanticResult = analyzer.analyze(ast);
    CodeGen codeGenerator;
    IRModule ir = codeGenerator.generate(ast, semanticResult.typeMap);
    std::ostringstream output;
    IRPrinter printer;
    printer.print(ir, output);
    return output.str();
}

// ---- AST unwrap helpers ----
// These call REQUIRE so a wrong-variant failure shows up cleanly.

template<typename T>
const T& asStmt(const Stmt& stmt) {
    REQUIRE(std::holds_alternative<T>(*stmt.node));
    return std::get<T>(*stmt.node);
}

template<typename T>
const T& asExpr(const Expr& expr) {
    REQUIRE(std::holds_alternative<T>(*expr.node));
    return std::get<T>(*expr.node);
}

#endif //GG_HELPERS_H
