#include <iostream>
#include <vector>
#include <filesystem>

#include "source/GG.h"

int main(int argc, char** argv){

    std::vector<std::string> paths;
    paths.reserve(argc - 1);

    for (int i = 1; i < argc; i++){
        paths.emplace_back(std::filesystem::absolute(argv[i]).string());
    }

    GG gg{paths};
    gg.run();

    return 0;
}