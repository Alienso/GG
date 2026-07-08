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

// Analyze a stdlib file with the default test options (allowRawPtr = true, so the
// String class — which owns a raw `ptr` buffer — is accepted, just as compile.ps1
// compiles the stdlib with --unsafe-ptr).
static SemanticResult analyzeStdlib(const std::string& filename) {
    ImportResolver resolver;
    Program program = resolver.resolve(stdlibPath(filename));
    SemanticAnalyzer analyzer;
    return analyzer.analyze(program, "", defaultTestOptions());
}

// ============================================================
// stdlib/crt/cio.gg — <stdio.h> bindings
// ============================================================

TEST_CASE("stdlib/crt/cio.gg - semantic analysis passes", "[stdlib][io]") {
    REQUIRE_FALSE(analyzeStdlib("crt/cio.gg").hadError);
}

TEST_CASE("stdlib/crt/cio.gg - IR declares puts and putchar", "[stdlib][io]") {
    std::string ir = stdlibIR("crt/cio.gg");
    REQUIRE(ir.find("declare i32 @puts(ptr)") != std::string::npos);
    REQUIRE(ir.find("declare i32 @putchar(i32)") != std::string::npos);
}

TEST_CASE("stdlib/crt/cio.gg - IR declares getchar with no parameters", "[stdlib][io]") {
    std::string ir = stdlibIR("crt/cio.gg");
    REQUIRE(ir.find("declare i32 @getchar()") != std::string::npos);
}

// ============================================================
// stdlib/crt/cmem.gg — <stdlib.h>/<string.h> memory bindings
// ============================================================

TEST_CASE("stdlib/crt/cmem.gg - semantic analysis passes", "[stdlib][mem]") {
    REQUIRE_FALSE(analyzeStdlib("crt/cmem.gg").hadError);
}

TEST_CASE("stdlib/crt/cmem.gg - IR declares malloc and free", "[stdlib][mem]") {
    std::string ir = stdlibIR("crt/cmem.gg");
    REQUIRE(ir.find("declare ptr @malloc(i64)") != std::string::npos);
    REQUIRE(ir.find("declare void @free(ptr)") != std::string::npos);
}

TEST_CASE("stdlib/crt/cmem.gg - IR declares memcpy and memset", "[stdlib][mem]") {
    std::string ir = stdlibIR("crt/cmem.gg");
    REQUIRE(ir.find("declare ptr @memcpy(ptr, ptr, i64)") != std::string::npos);
    REQUIRE(ir.find("declare ptr @memset(ptr, i32, i64)") != std::string::npos);
}

// ============================================================
// stdlib/crt/cstring.gg — <string.h> C string bindings
// ============================================================

TEST_CASE("stdlib/crt/cstring.gg - semantic analysis passes", "[stdlib][cstring]") {
    REQUIRE_FALSE(analyzeStdlib("crt/cstring.gg").hadError);
}

TEST_CASE("stdlib/crt/cstring.gg - IR declares strlen and strcmp", "[stdlib][cstring]") {
    std::string ir = stdlibIR("crt/cstring.gg");
    REQUIRE(ir.find("declare i64 @strlen(ptr)") != std::string::npos);
    REQUIRE(ir.find("declare i32 @strcmp(ptr, ptr)") != std::string::npos);
}

TEST_CASE("stdlib/crt/cstring.gg - IR declares strcpy and strdup", "[stdlib][cstring]") {
    std::string ir = stdlibIR("crt/cstring.gg");
    REQUIRE(ir.find("declare ptr @strcpy(ptr, ptr)") != std::string::npos);
    REQUIRE(ir.find("declare ptr @strdup(ptr)") != std::string::npos);
}

// ============================================================
// stdlib/string.gg — the UTF-8 String class
// ============================================================

TEST_CASE("stdlib/string.gg - semantic analysis passes", "[stdlib][string]") {
    REQUIRE_FALSE(analyzeStdlib("string.gg").hadError);
}

TEST_CASE("stdlib/string.gg - IR defines the String type, ctor and dtor", "[stdlib][string]") {
    std::string ir = stdlibIR("string.gg");
    REQUIRE(ir.find("%String = type") != std::string::npos);
    REQUIRE(ir.find("@String_String(ptr") != std::string::npos);
    REQUIRE(ir.find("@String_dtor(ptr") != std::string::npos);
}

TEST_CASE("stdlib/string.gg - IR defines the UTF-8 accessor methods", "[stdlib][string]") {
    std::string ir = stdlibIR("string.gg");
    REQUIRE(ir.find("@String_byteLength(") != std::string::npos);
    REQUIRE(ir.find("@String_length(")     != std::string::npos);
    REQUIRE(ir.find("@String_charAt(")     != std::string::npos);
    REQUIRE(ir.find("@String_eq(")         != std::string::npos);   // the Eq impl
}

TEST_CASE("stdlib/string.gg - transitively declares the C string/memory bindings", "[stdlib][string]") {
    std::string ir = stdlibIR("string.gg");
    REQUIRE(ir.find("declare i64 @strlen(ptr)")  != std::string::npos);
    REQUIRE(ir.find("declare i32 @strcmp(ptr, ptr)") != std::string::npos);
    REQUIRE(ir.find("declare ptr @malloc(i64)")  != std::string::npos);
}

// ============================================================
// stdlib/crt/cmath.gg — <math.h> bindings
// ============================================================

TEST_CASE("stdlib/crt/cmath.gg - semantic analysis passes", "[stdlib][math]") {
    REQUIRE_FALSE(analyzeStdlib("crt/cmath.gg").hadError);
}

TEST_CASE("stdlib/crt/cmath.gg - IR declares sqrt and pow", "[stdlib][math]") {
    std::string ir = stdlibIR("crt/cmath.gg");
    REQUIRE(ir.find("declare double @sqrt(double)") != std::string::npos);
    REQUIRE(ir.find("declare double @pow(double, double)") != std::string::npos);
}

TEST_CASE("stdlib/crt/cmath.gg - IR declares trig and log functions", "[stdlib][math]") {
    std::string ir = stdlibIR("crt/cmath.gg");
    REQUIRE(ir.find("declare double @sin(double)") != std::string::npos);
    REQUIRE(ir.find("declare double @cos(double)") != std::string::npos);
    REQUIRE(ir.find("declare double @log(double)") != std::string::npos);
}

// ============================================================
// stdlib/crt/cprocess.gg — <stdlib.h> process bindings
// ============================================================

TEST_CASE("stdlib/crt/cprocess.gg - semantic analysis passes", "[stdlib][process]") {
    REQUIRE_FALSE(analyzeStdlib("crt/cprocess.gg").hadError);
}

TEST_CASE("stdlib/crt/cprocess.gg - IR declares exit and abort", "[stdlib][process]") {
    std::string ir = stdlibIR("crt/cprocess.gg");
    REQUIRE(ir.find("declare void @exit(i32)") != std::string::npos);
    REQUIRE(ir.find("declare void @abort()") != std::string::npos);
}

TEST_CASE("stdlib/crt/cprocess.gg - IR declares getenv and system", "[stdlib][process]") {
    std::string ir = stdlibIR("crt/cprocess.gg");
    REQUIRE(ir.find("declare ptr @getenv(ptr)") != std::string::npos);
    REQUIRE(ir.find("declare i32 @system(ptr)") != std::string::npos);
}

// ============================================================
// Cross-module: importing a stdlib module from a program
// ============================================================

TEST_CASE("stdlib import - crt/cio.gg is importable from a program", "[stdlib][integration]") {
    // Build an import path using the absolute location of cio.gg.
    // fs::path treats an absolute RHS as a full replacement, so the resolver
    // will find the file even though the importing temp file lives elsewhere.
    std::string absolutePath = stdlibPath("crt/cio.gg");
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

TEST_CASE("stdlib import - crt/cmem.gg is importable from a program", "[stdlib][integration]") {
    std::string absolutePath = stdlibPath("crt/cmem.gg");
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
