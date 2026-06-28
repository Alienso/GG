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
#include "../source/CompilerOptions.h"
#include "../source/CompileError.h"

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

// Default compiler options used by test helpers.
// allowRawPtr = true so all existing ptr/ptr<T> tests pass without change.
// A test that wants to verify the restriction should pass CompilerOptions{} explicitly.
inline CompilerOptions defaultTestOptions() {
    CompilerOptions opts;
    opts.allowRawPtr = true;
    return opts;
}

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
// If a parse/lex error occurs (CompileError), prints the error to stderr and returns an empty Program.
inline Program parseStringRaw(const std::string& source) {
    std::string path = detail::writeTempSource(source);
    std::vector<std::string> paths{ path };
    Lexer lexer(paths);
    lexer.lex();
    Parser parser;
    try {
        return parser.parse(lexer.tokens()[0]);
    } catch (const CompileError& e) {
        std::cerr << e.what() << '\n';
        return Program{};
    }
}

// Lex + parse + resolve imports for a source string and return the Program AST.
// If a parse/lex error occurs (CompileError), prints the error to stderr and returns an empty Program.
inline Program parseString(const std::string& source) {
    std::string path = detail::writeTempSource(source);
    ImportResolver resolver;
    try {
        return resolver.resolve(path);
    } catch (const CompileError& e) {
        std::cerr << e.what() << '\n';
        return Program{};
    }
}

// Lex + parse + analyse a source string and return the SemanticResult.
// options defaults to defaultTestOptions() (allowRawPtr=true) so existing tests
// that use ptr/ptr<T> continue to pass without modification.
inline SemanticResult analyzeString(const std::string& source,
                                    CompilerOptions options = defaultTestOptions()) {
    Program ast = parseString(source);
    SemanticAnalyzer analyzer;
    try {
        return analyzer.analyze(ast, "", options);
    } catch (const CompileError& e) {
        std::cerr << e.what() << '\n';
        return SemanticResult{true, {}, {}, {}};
    }
}

// Lex + parse + analyse + codegen a source string and return the IR text.
// options defaults to defaultTestOptions() (allowRawPtr=true).
inline std::string codegenString(const std::string& source,
                                 CompilerOptions options = defaultTestOptions()) {
    Program ast = parseString(source);
    SemanticAnalyzer analyzer;
    // Let CompileError propagate — a semantic error in codegenString is a test failure
    SemanticResult semanticResult = analyzer.analyze(ast, "", options);
    CodeGen codeGenerator;
    IRModule ir = codeGenerator.generate(ast, semanticResult, options);
    std::ostringstream output;
    IRPrinter printer;
    printer.print(ir, output);
    return output.str();
}

// Resolve + analyse + codegen a .gg file on disk and return the IR text.
// Used by stdlib tests that operate on actual source files rather than inline strings.
// options defaults to defaultTestOptions() (allowRawPtr=true).
inline std::string codegenFile(const std::string& path,
                                CompilerOptions options = defaultTestOptions()) {
    ImportResolver resolver;
    Program ast = resolver.resolve(path);
    SemanticAnalyzer analyzer;
    SemanticResult semanticResult = analyzer.analyze(ast, path, options);
    CodeGen codeGenerator;
    IRModule ir = codeGenerator.generate(ast, semanticResult, options);
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
