#include <iostream>
#include <vector>

#include "source/GG.h"

int main(int argc, char** argv){

    std::vector<std::string> paths;
    paths.reserve(argc - 1);

    for (int i = 1; i <= argc; i++){
        char* path = argv[i];
        paths[i] = std::string(path);
    }

    GG gg{paths};
    gg.run();

    return 0;
}