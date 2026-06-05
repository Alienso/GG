//
// Created by Vladimir Arsenijevic on 05.6.2026.
//

#ifndef GG_IMPORTRESOLVER_H
#define GG_IMPORTRESOLVER_H

#include <string>
#include <unordered_set>
#include "parser/Ast.h"

// Resolves all import statements in a root file and returns a single flat
// Program whose declarations are the union of the root file and every imported
// file (transitively), in dependency-first order.
//
// A canonical-path set prevents duplicate inclusion and breaks import cycles:
// if file A imports B and B imports A, B receives A's declarations (A was
// already added to the set before recursing) but does not re-process A.

class ImportResolver {
public:
    // Entry point. Clears internal state, then processes rootFilePath.
    Program resolve(const std::string& rootFilePath);

private:
    std::unordered_set<std::string> processedPaths;

    Program processFile(const std::string& filePath);

    // Strips the surrounding double-quote characters from a STRING token lexeme.
    static std::string stripQuotes(const std::string& lexeme);
};

#endif //GG_IMPORTRESOLVER_H
