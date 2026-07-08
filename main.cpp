#include <iostream>
#include <vector>
#include <filesystem>

#include "source/GG.h"

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
        } else {
            paths.emplace_back(std::filesystem::absolute(arg).string());
        }
    }

    // The first source path is the DWARF compile-unit file (single-file debug mapping).
    if (options.debugInfo && !paths.empty()) options.sourceFile = paths.front();

    GG gg{paths, options};
    gg.run();

    return 0;
}
