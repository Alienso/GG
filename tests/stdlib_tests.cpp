#include <catch2/catch_test_macros.hpp>
#include "helpers.h"

#include <algorithm>
#include <string>

// GG_SOURCE_DIR is injected by CMake so we can locate the stdlib/ directory
// regardless of where the build or working directory happens to be.
static std::string stdlibPath(const std::string& filename) {
    return std::string(GG_SOURCE_DIR) + "/stdlib/" + filename;
}

static std::string stdlibIR(const std::string& filename) {
    return codegenFile(stdlibPath(filename));
}

// ============================================================
// stdlib/io.gg
// ============================================================

TEST_CASE("stdlib/io.gg - semantic analysis passes", "[stdlib][io]") {
    ImportResolver resolver;
    Program program = resolver.resolve(stdlibPath("io.gg"));
    SemanticAnalyzer analyzer;
    SemanticResult result = analyzer.analyze(program);
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("stdlib/io.gg - IR declares puts and putchar", "[stdlib][io]") {
    std::string ir = stdlibIR("io.gg");
    REQUIRE(ir.find("declare i32 @puts(ptr)") != std::string::npos);
    REQUIRE(ir.find("declare i32 @putchar(i32)") != std::string::npos);
}

TEST_CASE("stdlib/io.gg - IR declares getchar with no parameters", "[stdlib][io]") {
    std::string ir = stdlibIR("io.gg");
    REQUIRE(ir.find("declare i32 @getchar()") != std::string::npos);
}

// ============================================================
// stdlib/mem.gg
// ============================================================

TEST_CASE("stdlib/mem.gg - semantic analysis passes", "[stdlib][mem]") {
    ImportResolver resolver;
    Program program = resolver.resolve(stdlibPath("mem.gg"));
    SemanticAnalyzer analyzer;
    SemanticResult result = analyzer.analyze(program);
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("stdlib/mem.gg - IR declares malloc and free", "[stdlib][mem]") {
    std::string ir = stdlibIR("mem.gg");
    REQUIRE(ir.find("declare ptr @malloc(i64)") != std::string::npos);
    REQUIRE(ir.find("declare void @free(ptr)") != std::string::npos);
}

TEST_CASE("stdlib/mem.gg - IR declares memcpy and memset", "[stdlib][mem]") {
    std::string ir = stdlibIR("mem.gg");
    REQUIRE(ir.find("declare ptr @memcpy(ptr, ptr, i64)") != std::string::npos);
    REQUIRE(ir.find("declare ptr @memset(ptr, i32, i64)") != std::string::npos);
}

// ============================================================
// stdlib/string.gg
// ============================================================

TEST_CASE("stdlib/string.gg - semantic analysis passes", "[stdlib][string]") {
    ImportResolver resolver;
    Program program = resolver.resolve(stdlibPath("string.gg"));
    SemanticAnalyzer analyzer;
    SemanticResult result = analyzer.analyze(program);
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("stdlib/string.gg - IR declares strlen and strcmp", "[stdlib][string]") {
    std::string ir = stdlibIR("string.gg");
    REQUIRE(ir.find("declare i64 @strlen(ptr)") != std::string::npos);
    REQUIRE(ir.find("declare i32 @strcmp(ptr, ptr)") != std::string::npos);
}

TEST_CASE("stdlib/string.gg - IR declares strcpy and strdup", "[stdlib][string]") {
    std::string ir = stdlibIR("string.gg");
    REQUIRE(ir.find("declare ptr @strcpy(ptr, ptr)") != std::string::npos);
    REQUIRE(ir.find("declare ptr @strdup(ptr)") != std::string::npos);
}

// ============================================================
// stdlib/math.gg
// ============================================================

TEST_CASE("stdlib/math.gg - semantic analysis passes", "[stdlib][math]") {
    ImportResolver resolver;
    Program program = resolver.resolve(stdlibPath("math.gg"));
    SemanticAnalyzer analyzer;
    SemanticResult result = analyzer.analyze(program);
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("stdlib/math.gg - IR declares sqrt and pow", "[stdlib][math]") {
    std::string ir = stdlibIR("math.gg");
    REQUIRE(ir.find("declare double @sqrt(double)") != std::string::npos);
    REQUIRE(ir.find("declare double @pow(double, double)") != std::string::npos);
}

TEST_CASE("stdlib/math.gg - IR declares trig and log functions", "[stdlib][math]") {
    std::string ir = stdlibIR("math.gg");
    REQUIRE(ir.find("declare double @sin(double)") != std::string::npos);
    REQUIRE(ir.find("declare double @cos(double)") != std::string::npos);
    REQUIRE(ir.find("declare double @log(double)") != std::string::npos);
}

// ============================================================
// stdlib/process.gg
// ============================================================

TEST_CASE("stdlib/process.gg - semantic analysis passes", "[stdlib][process]") {
    ImportResolver resolver;
    Program program = resolver.resolve(stdlibPath("process.gg"));
    SemanticAnalyzer analyzer;
    SemanticResult result = analyzer.analyze(program);
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("stdlib/process.gg - IR declares exit and abort", "[stdlib][process]") {
    std::string ir = stdlibIR("process.gg");
    REQUIRE(ir.find("declare void @exit(i32)") != std::string::npos);
    REQUIRE(ir.find("declare void @abort()") != std::string::npos);
}

TEST_CASE("stdlib/process.gg - IR declares getenv and system", "[stdlib][process]") {
    std::string ir = stdlibIR("process.gg");
    REQUIRE(ir.find("declare ptr @getenv(ptr)") != std::string::npos);
    REQUIRE(ir.find("declare i32 @system(ptr)") != std::string::npos);
}

// ============================================================
// Cross-module: importing a stdlib module from a program
// ============================================================

TEST_CASE("stdlib import - io.gg is importable from a program", "[stdlib][integration]") {
    // Build an import path using the absolute location of io.gg.
    // fs::path treats an absolute RHS as a full replacement, so the resolver
    // will find the file even though the importing temp file lives elsewhere.
    std::string absolutePath = stdlibPath("io.gg");
    // Normalise separators for the GG string literal (fs::path handles both on Windows).
    std::replace(absolutePath.begin(), absolutePath.end(), '\\', '/');

    std::string source =
        "import \"" + absolutePath + "\";\n"
        "fn main() { puts(\"hello\"); }";

    StderrCapture capture;
    std::string ir = codegenString(source);

    REQUIRE(capture.str().empty());
    REQUIRE(ir.find("declare i32 @puts(ptr)") != std::string::npos);
    REQUIRE(ir.find("define void @main") != std::string::npos);
}

TEST_CASE("stdlib import - mem.gg is importable from a program", "[stdlib][integration]") {
    std::string absolutePath = stdlibPath("mem.gg");
    std::replace(absolutePath.begin(), absolutePath.end(), '\\', '/');

    std::string source =
        "import \"" + absolutePath + "\";\n"
        "fn main() { ptr p = malloc(64); free(p); }";

    StderrCapture capture;
    std::string ir = codegenString(source);

    // The integer literal 64 triggers an i32→u64 narrowing warning — that is expected.
    // What we care about is that no hard error was emitted and the IR looks correct.
    REQUIRE_FALSE(capture.contains("Error:"));
    REQUIRE(ir.find("declare ptr @malloc(i64)") != std::string::npos);
    REQUIRE(ir.find("declare void @free(ptr)") != std::string::npos);
}
