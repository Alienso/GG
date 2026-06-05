//
// Created by Vladimir Arsenijevic on 05.6.2026.
//

#include "ImportResolver.h"
#include "lexer/Lexer.h"
#include "parser/Parser.h"

#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

// ============================================================
// Public entry point
// ============================================================

Program ImportResolver::resolve(const std::string& rootFilePath) {
    processedPaths.clear();
    return processFile(rootFilePath);
}

// ============================================================
// Recursive file processor
// ============================================================

Program ImportResolver::processFile(const std::string& filePath) {
    // Resolve to a canonical absolute path so that "./a.gg" and "a.gg" refer
    // to the same file and cycles are detected regardless of how the path is spelled.
    std::error_code errorCode;
    fs::path canonical = fs::weakly_canonical(fs::path(filePath), errorCode);
    if (errorCode || !fs::exists(canonical)) {
        std::cerr << "Error: cannot find imported file '" << filePath << "'\n";
        return Program{};
    }

    std::string canonicalString = canonical.string();
    if (processedPaths.count(canonicalString)) return Program{};
    processedPaths.insert(canonicalString);  // mark before recursing — breaks cycles

    // Lex and parse the file.
    std::vector<std::string> paths = { canonicalString };
    Lexer lexer(paths);
    lexer.lex();
    Parser parser;
    Program rawProgram = parser.parse(lexer.tokens()[0]);

    fs::path parentDirectory = canonical.parent_path();
    Program result;

    for (Stmt& declaration : rawProgram.declarations) {
        if (!declaration.node) continue;

        if (std::holds_alternative<ImportStmt>(*declaration.node)) {
            // Resolve the import path relative to the current file's directory.
            const auto& importStmt   = std::get<ImportStmt>(*declaration.node);
            std::string relativePath = stripQuotes(importStmt.path.lexeme);
            std::string absolutePath = (parentDirectory / relativePath).string();

            Program imported = processFile(absolutePath);
            for (Stmt& importedDecl : imported.declarations)
                result.declarations.push_back(std::move(importedDecl));
        } else {
            result.declarations.push_back(std::move(declaration));
        }
    }

    return result;
}

// ============================================================
// Helpers
// ============================================================

std::string ImportResolver::stripQuotes(const std::string& lexeme) {
    if (lexeme.size() >= 2 && lexeme.front() == '"' && lexeme.back() == '"')
        return lexeme.substr(1, lexeme.size() - 2);
    return lexeme;
}
