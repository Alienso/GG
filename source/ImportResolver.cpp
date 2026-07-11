//
// Created by Vladimir Arsenijevic on 05.6.2026.
//

#include "ImportResolver.h"
#include "lexer/Lexer.h"
#include "lexer/Token.h"
#include "parser/Parser.h"

#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

// ============================================================
// Public entry point
// ============================================================

Program ImportResolver::resolve(const std::string& rootFilePath) {
    processedPaths.clear();
    sharedGenerics_ = GenericRegistry{};

    // Pass 1: pre-register every generic template name across all files so that
    // use sites are recognised regardless of which file declares the template.
    Parser seedParser({}, &sharedGenerics_);
    std::unordered_set<std::string> tplVisited;
    prescanTemplates(rootFilePath, tplVisited, seedParser);

    // Pass 2: parse + flatten every file (monomorphization deferred — templates and
    // instantiation requests accumulate in the shared registry).
    Program program = processFile(rootFilePath);

    // Pass 3: expand all instantiations once, with the union of templates/requests.
    std::unordered_set<std::string> classVisited;
    std::unordered_set<std::string> allClassNames = collectClassNames(rootFilePath, classVisited);
    Parser monoParser(std::move(allClassNames), &sharedGenerics_);
    // A monomorphization error is reported at the use-site line (the line that requested the
    // instantiation), which lives in the root file for the common case — label it accordingly.
    std::error_code rootEc;
    fs::path rootCanonical = fs::weakly_canonical(fs::path(rootFilePath), rootEc);
    monoParser.monomorphize(program, rootEc ? rootFilePath : rootCanonical.string());
    return program;
}

void ImportResolver::prescanTemplates(const std::string& filePath,
                                      std::unordered_set<std::string>& visitedPaths,
                                      Parser& seedParser) {
    std::error_code ec;
    fs::path canonical = fs::weakly_canonical(fs::path(filePath), ec);
    if (ec || !fs::exists(canonical)) return;

    std::string canonicalStr = canonical.string();
    if (visitedPaths.count(canonicalStr)) return;
    visitedPaths.insert(canonicalStr);

    std::vector<std::string> paths{ canonicalStr };
    Lexer lexer(paths);
    lexer.lex();
    const auto& tokens = lexer.tokens()[0];

    seedParser.prescanTemplateNames(tokens);

    fs::path parentDir = canonical.parent_path();
    for (size_t i = 0; i + 1 < tokens.size(); ++i) {
        if (tokens[i].type == TokenType::IMPORT && tokens[i + 1].type == TokenType::STRING) {
            std::string rawPath = stripQuotes(tokens[i + 1].lexeme);
            std::string absPath = (parentDir / rawPath).string();
            prescanTemplates(absPath, visitedPaths, seedParser);
        }
    }
}

// ============================================================
// Class-name pre-scanner
// ============================================================

std::unordered_set<std::string> ImportResolver::collectClassNames(
    const std::string& filePath,
    std::unordered_set<std::string>& visitedPaths)
{
    std::error_code ec;
    fs::path canonical = fs::weakly_canonical(fs::path(filePath), ec);
    if (ec || !fs::exists(canonical)) return {};

    std::string canonicalStr = canonical.string();
    if (visitedPaths.count(canonicalStr)) return {};
    visitedPaths.insert(canonicalStr);

    std::vector<std::string> paths{ canonicalStr };
    Lexer lexer(paths);
    lexer.lex();
    const auto& tokens = lexer.tokens()[0];

    std::unordered_set<std::string> names;
    fs::path parentDir = canonical.parent_path();

    for (size_t i = 0; i < tokens.size(); ++i) {
        // Collect class names defined in this file
        if (i + 1 < tokens.size()
            && tokens[i].type    == TokenType::CLASS
            && tokens[i + 1].type == TokenType::IDENTIFIER) {
            names.insert(tokens[i + 1].lexeme);
        }
        // Follow import statements to collect from transitive dependencies
        if (tokens[i].type == TokenType::IMPORT
            && i + 1 < tokens.size()
            && tokens[i + 1].type == TokenType::STRING) {
            std::string rawPath = stripQuotes(tokens[i + 1].lexeme);
            std::string absPath = (parentDir / rawPath).string();
            auto imported = collectClassNames(absPath, visitedPaths);
            names.insert(imported.begin(), imported.end());
        }
    }
    return names;
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

    // Collect class names from this file and all its transitive imports so that
    // cross-file constructor calls ("ClassName varName(args)") are recognised as
    // variable declarations during parsing.
    std::unordered_set<std::string> classNameVisited;
    auto allClassNames = collectClassNames(canonicalString, classNameVisited);

    // Lex and parse the file, seeding the parser with the pre-collected class names.
    std::vector<std::string> paths = { canonicalString };
    Lexer lexer(paths);
    lexer.lex();
    // Bind to the shared generics registry and defer monomorphization — resolve()
    // expands all instantiations once after every file has been parsed.
    Parser parser(std::move(allClassNames), &sharedGenerics_);
    Program rawProgram = parser.parse(lexer.tokens()[0], canonicalString, /*runMonomorphization=*/false);

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
