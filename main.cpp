#include <iostream>
#include <vector>
#include <filesystem>
#include <cstdlib>

#include "source/GG.h"

namespace fs = std::filesystem;

// Locate the standard-library directory. Honors the GG_STDLIB environment
// override first; otherwise walks up from the compiler executable's directory
// looking for a `stdlib/` folder (confirmed by its `crt/` subdirectory). Returns
// an empty string when none is found — the `std/` import prefix is then disabled.
static std::string findStdlibDir(const char* argv0) {
    if (const char* env = std::getenv("GG_STDLIB"); env && *env) return env;

    std::error_code ec;
    fs::path dir = fs::weakly_canonical(fs::path(argv0), ec);
    if (ec) dir = fs::path(argv0);
    dir = dir.parent_path();
    for (fs::path d = dir; !d.empty(); d = d.parent_path()) {
        fs::path candidate = d / "stdlib";
        if (fs::exists(candidate / "crt", ec)) return candidate.string();
        if (d == d.root_path()) break;
    }
    return {};
}

// Extra import search roots from the GG_PATH environment variable (';'-separated).
static std::vector<std::string> findSearchRoots() {
    std::vector<std::string> roots;
    const char* env = std::getenv("GG_PATH");
    if (!env || !*env) return roots;
    std::string value = env;
    size_t start = 0;
    while (start <= value.size()) {
        size_t pos = value.find(';', start);
        std::string part = value.substr(start, pos == std::string::npos ? std::string::npos : pos - start);
        if (!part.empty()) roots.push_back(part);
        if (pos == std::string::npos) break;
        start = pos + 1;
    }
    return roots;
}

int main(int argc, char** argv) {
    CompilerOptions options;
    std::vector<std::string> paths;
    paths.reserve(argc - 1);

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--no-bounds-check") {
            options.boundsCheck = false;
        } else if (arg == "--unsafe-ptr") {
            options.allowRawPtr = true;
        } else if (arg == "--debug" || arg == "-g") {
            options.debugInfo = true;
        } else if (arg == "--overflow-checks") {
            options.overflowChecks = true;
        } else if (arg.rfind("--target=", 0) == 0) {
            options.targetTriple = arg.substr(std::string("--target=").size());
        } else {
            paths.emplace_back(std::filesystem::absolute(arg).string());
        }
    }

    // The first source path is the DWARF compile-unit file (single-file debug mapping).
    if (options.debugInfo && !paths.empty()) options.sourceFile = paths.front();

    ModuleSearchConfig moduleConfig;
    moduleConfig.stdlibDir   = findStdlibDir(argv[0]);
    moduleConfig.searchRoots = findSearchRoots();

    GG gg{paths, options, moduleConfig};
    return gg.run();
}
