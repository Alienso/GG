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
        } else {
            paths.emplace_back(std::filesystem::absolute(arg).string());
        }
    }

    GG gg{paths, options};
    gg.run();

    return 0;
}
