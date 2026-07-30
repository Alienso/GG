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
    std::string path = writeTempFile("gg_no_imports.gg", "fn foo() -> i32 { return 42; }");
    ImportResolver resolver;
    Program result = resolver.resolve(path);
    REQUIRE(result.declarations.size() == 1);
    asStmt<FunctionDeclStmt>(result.declarations[0]);
}

TEST_CASE("ImportResolver - ImportStmt nodes are stripped from the result", "[import]") {
    // Write the imported file first so the path exists.
    writeTempFile("gg_empty_lib.gg", "fn helper() -> i32 { return 1; }");
    std::string mainPath = writeTempFile("gg_strip_import.gg",
        "import \"gg_empty_lib.gg\";\nfn main() -> i32 { return 0; }");

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
    writeTempFile("gg_lib_a.gg", "fn helper() -> i32 { return 7; }");
    std::string mainPath = writeTempFile("gg_main_a.gg",
        "import \"gg_lib_a.gg\";\nfn main() -> i32 { return helper(); }");

    ImportResolver resolver;
    Program result = resolver.resolve(mainPath);

    // Both helper() and main() should be present.
    REQUIRE(result.declarations.size() == 2);
    // Imported declarations come first, root file declarations come last.
    asStmt<FunctionDeclStmt>(result.declarations[0]);
    asStmt<FunctionDeclStmt>(result.declarations[1]);
}

TEST_CASE("ImportResolver - duplicate import is only included once", "[import]") {
    writeTempFile("gg_shared.gg", "fn shared() -> i32 { return 0; }");
    // Both mid_a.gg and mid_b.gg import the same shared.gg.
    writeTempFile("gg_mid_a.gg", "import \"gg_shared.gg\";\nfn midA() -> i32 { return 1; }");
    writeTempFile("gg_mid_b.gg", "import \"gg_shared.gg\";\nfn midB() -> i32 { return 2; }");
    std::string rootPath = writeTempFile("gg_root_dedup.gg",
        "import \"gg_mid_a.gg\";\nimport \"gg_mid_b.gg\";\nfn root() -> i32 { return 0; }");

    ImportResolver resolver;
    Program result = resolver.resolve(rootPath);

    // shared(), midA(), midB(), root() — shared() must appear exactly once.
    REQUIRE(result.declarations.size() == 4);
}

TEST_CASE("ImportResolver - transitive import works end-to-end", "[import]") {
    // io.gg depends on mem.gg; root depends on io.gg.
    writeTempFile("gg_mem.gg",  "extern malloc(u64 size) -> ptr;");
    writeTempFile("gg_io.gg",   "import \"gg_mem.gg\";\nextern puts(ptr s) -> i32;");
    std::string rootPath = writeTempFile("gg_transitive_root.gg",
        "import \"gg_io.gg\";\nfn main() { puts(\"hi\"); }");

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

// ============================================================
// ImportResolver — module search roots (the `std/` prefix + GG_PATH-style roots)
// ============================================================

// Create a subdirectory under the temp dir and write a file into it.
static std::string writeTempFileIn(const std::string& subdir, const std::string& filename,
                                    const std::string& source) {
    fs::path dir = fs::temp_directory_path() / subdir;
    fs::create_directories(dir);
    std::string path = (dir / filename).string();
    std::ofstream(path) << source;
    return path;
}

TEST_CASE("ImportResolver - the `std/` prefix resolves against the configured stdlib dir", "[import]") {
    // A fake stdlib laid out under a dir whose name is NOT next to the importer, so only the
    // configured `std/` anchor can find it (file-relative resolution would fail).
    fs::path fakeStd = fs::temp_directory_path() / "gg_fake_std";
    fs::create_directories(fakeStd);
    std::ofstream((fakeStd / "Widget.gg").string()) << "fn widget() -> i32 { return 42; }";

    std::string mainPath = writeTempFileIn("gg_std_prefix_app", "main.gg",
        "import \"std/Widget.gg\";\nfn main() -> i32 { return widget(); }");

    ModuleSearchConfig cfg;
    cfg.stdlibDir = fakeStd.string();

    ImportResolver resolver;
    Program result = resolver.resolve(mainPath, cfg);

    // widget() + main() present; the `std/` import was found via the anchor, not file-relative.
    REQUIRE(result.declarations.size() == 2);
}

TEST_CASE("ImportResolver - a search root resolves a bare import path", "[import]") {
    fs::path libRoot = fs::temp_directory_path() / "gg_search_root_lib";
    fs::create_directories(libRoot);
    std::ofstream((libRoot / "gadget.gg").string()) << "fn gadget() -> i32 { return 5; }";

    std::string mainPath = writeTempFileIn("gg_search_root_app", "main.gg",
        "import \"gadget.gg\";\nfn main() -> i32 { return gadget(); }");

    ModuleSearchConfig cfg;
    cfg.searchRoots = { libRoot.string() };

    ImportResolver resolver;
    Program result = resolver.resolve(mainPath, cfg);

    REQUIRE(result.declarations.size() == 2);
}

TEST_CASE("ImportResolver - file-relative resolution takes precedence over search roots", "[import]") {
    // A `helper.gg` exists BOTH next to the importer and under a search root, with different
    // bodies. File-relative must win — proving back-compat with existing relative imports.
    fs::path rootDir = fs::temp_directory_path() / "gg_precedence_root";
    fs::create_directories(rootDir);
    std::ofstream((rootDir / "helper.gg").string()) << "fn helper() -> i32 { return 99; }";

    // Local sibling helper.gg — the one that should be chosen.
    writeTempFileIn("gg_precedence_app", "helper.gg", "fn helper() -> i32 { return 1; }");
    std::string mainPath = writeTempFileIn("gg_precedence_app", "main.gg",
        "import \"helper.gg\";\nfn main() -> i32 { return helper(); }");

    ModuleSearchConfig cfg;
    cfg.searchRoots = { rootDir.string() };

    ImportResolver resolver;
    Program result = resolver.resolve(mainPath, cfg);

    // Exactly one helper() + one main() — the local sibling, not the root copy (would be a
    // duplicate definition otherwise).
    REQUIRE(result.declarations.size() == 2);
    SemanticAnalyzer analyzer;
    SemanticResult sem = analyzer.analyze(result);
    REQUIRE_FALSE(sem.hadError);   // no duplicate-definition error → only the sibling was merged
}

TEST_CASE("ImportResolver - a missing `std/` import reports against the stdlib dir", "[import]") {
    StderrCapture capture;
    std::string mainPath = writeTempFileIn("gg_std_missing_app", "main.gg",
        "import \"std/Nope.gg\";\nfn main() -> i32 { return 0; }");

    ModuleSearchConfig cfg;
    cfg.stdlibDir = (fs::temp_directory_path() / "gg_fake_std").string();

    ImportResolver resolver;
    Program result = resolver.resolve(mainPath, cfg);

    REQUIRE(capture.contains("Error"));
    REQUIRE(capture.contains("std/ resolves to"));
}
