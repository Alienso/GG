#include <catch2/catch_test_macros.hpp>
#include "helpers.h"

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

// Write a named file into the system temp directory.
// Returns the absolute path to the file.
static std::string writeTempFile(const std::string& filename, const std::string& source) {
    std::string path = (fs::temp_directory_path() / filename).string();
    std::ofstream(path) << source;
    return path;
}

// ============================================================
// Parser — ImportStmt node
// ============================================================

TEST_CASE("Parser - import statement produces ImportStmt", "[parser][import]") {
    // Use raw parse so the ImportStmt node is not stripped by the resolver.
    Program program = parseStringRaw("import \"stdlib/io.gg\";");
    REQUIRE(program.declarations.size() == 1);
    const auto& importStmt = asStmt<ImportStmt>(program.declarations[0]);
    REQUIRE(importStmt.path.type == TokenType::STRING);
    // The lexer strips the surrounding quotes — lexeme is the raw string content.
    REQUIRE(importStmt.path.lexeme == "stdlib/io.gg");
}

TEST_CASE("Parser - import keyword token is stored", "[parser][import]") {
    // Use raw parse so the ImportStmt node is not stripped by the resolver.
    Program program = parseStringRaw("import \"foo.gg\";");
    const auto& importStmt = asStmt<ImportStmt>(program.declarations[0]);
    REQUIRE(importStmt.keyword.type == TokenType::IMPORT);
}

// ============================================================
// ImportResolver — single file (no imports)
// ============================================================

TEST_CASE("ImportResolver - file with no imports returns its declarations", "[import]") {
    std::string path = writeTempFile("gg_no_imports.gg", "i32 foo() { return 42; }");
    ImportResolver resolver;
    Program result = resolver.resolve(path);
    REQUIRE(result.declarations.size() == 1);
    asStmt<FunctionDeclStmt>(result.declarations[0]);
}

TEST_CASE("ImportResolver - ImportStmt nodes are stripped from the result", "[import]") {
    // Write the imported file first so the path exists.
    writeTempFile("gg_empty_lib.gg", "i32 helper() { return 1; }");
    std::string mainPath = writeTempFile("gg_strip_import.gg",
        "import \"gg_empty_lib.gg\";\ni32 main() { return 0; }");

    ImportResolver resolver;
    Program result = resolver.resolve(mainPath);

    // The result should contain helper() + main(), but no ImportStmt.
    for (const Stmt& declaration : result.declarations)
        REQUIRE_FALSE(std::holds_alternative<ImportStmt>(*declaration.node));
}

// ============================================================
// ImportResolver — actual import merging
// ============================================================

TEST_CASE("ImportResolver - imported declarations are available", "[import]") {
    writeTempFile("gg_lib_a.gg", "i32 helper() { return 7; }");
    std::string mainPath = writeTempFile("gg_main_a.gg",
        "import \"gg_lib_a.gg\";\ni32 main() { return helper(); }");

    ImportResolver resolver;
    Program result = resolver.resolve(mainPath);

    // Both helper() and main() should be present.
    REQUIRE(result.declarations.size() == 2);
    // Imported declarations come first, root file declarations come last.
    asStmt<FunctionDeclStmt>(result.declarations[0]);
    asStmt<FunctionDeclStmt>(result.declarations[1]);
}

TEST_CASE("ImportResolver - duplicate import is only included once", "[import]") {
    writeTempFile("gg_shared.gg", "i32 shared() { return 0; }");
    // Both mid_a.gg and mid_b.gg import the same shared.gg.
    writeTempFile("gg_mid_a.gg", "import \"gg_shared.gg\";\ni32 midA() { return 1; }");
    writeTempFile("gg_mid_b.gg", "import \"gg_shared.gg\";\ni32 midB() { return 2; }");
    std::string rootPath = writeTempFile("gg_root_dedup.gg",
        "import \"gg_mid_a.gg\";\nimport \"gg_mid_b.gg\";\ni32 root() { return 0; }");

    ImportResolver resolver;
    Program result = resolver.resolve(rootPath);

    // shared(), midA(), midB(), root() — shared() must appear exactly once.
    REQUIRE(result.declarations.size() == 4);
}

TEST_CASE("ImportResolver - transitive import works end-to-end", "[import]") {
    // io.gg depends on mem.gg; root depends on io.gg.
    writeTempFile("gg_mem.gg",  "extern ptr malloc(u64 size);");
    writeTempFile("gg_io.gg",   "import \"gg_mem.gg\";\nextern i32 puts(ptr s);");
    std::string rootPath = writeTempFile("gg_transitive_root.gg",
        "import \"gg_io.gg\";\nvoid main() { puts(\"hi\"); }");

    // malloc and puts should both be in scope — no errors expected.
    ImportResolver resolver;
    Program program = resolver.resolve(rootPath);
    SemanticAnalyzer analyzer;
    SemanticResult result = analyzer.analyze(program);
    REQUIRE_FALSE(result.hadError);
}

// ============================================================
// ImportResolver — error handling
// ============================================================

TEST_CASE("ImportResolver - missing file produces empty program", "[import]") {
    StderrCapture capture;
    std::string path = writeTempFile("gg_bad_import.gg",
        "import \"does_not_exist_xyz.gg\";");

    ImportResolver resolver;
    Program result = resolver.resolve(path);

    // The missing import produces an error on stderr.
    REQUIRE(capture.contains("Error"));
    // No declarations from the missing file.
    REQUIRE(result.declarations.empty());
}
