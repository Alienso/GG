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

Program ImportResolver::resolve(const std::string& rootFilePath, const ModuleSearchConfig& config) {
    processedPaths.clear();
    sharedGenerics_ = GenericRegistry{};
    config_ = config;

    // Pass 0: scan every file for its `module NAME;` + top-level decl names, populating the shared
    // (module → member names) table + the set of module names. This must precede all parsing so any
    // file can qualify references to a sibling file's same-module symbols and fold fully-qualified
    // names (`geo.Point`) written before the defining file is parsed.
    std::unordered_set<std::string> modVisited;
    scanModules(rootFilePath, modVisited);

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

// ============================================================
// Import-path resolution (std/ prefix, file-relative, search roots)
// ============================================================

std::string ImportResolver::resolveImportPath(const fs::path& importerDir,
                                               const std::string& rawPath) const {
    std::error_code ec;
    auto isFile = [&](const fs::path& p) { return fs::exists(p, ec) && fs::is_regular_file(p, ec); };

    // 1. Reserved logical prefix `std/` → the compiler's standard library.
    //    Resolved here even when the target is missing, so a bad stdlib import
    //    reports against the stdlib directory rather than the importer.
    if (!config_.stdlibDir.empty()) {
        static const std::string prefix = "std/";
        if (rawPath.rfind(prefix, 0) == 0)
            return (fs::path(config_.stdlibDir) / rawPath.substr(prefix.size())).string();
    }

    // 2. File-relative — the original, backward-compatible behavior.
    fs::path relative = importerDir / rawPath;
    if (isFile(relative)) return relative.string();

    // 3. Configured search roots, in order.
    for (const std::string& root : config_.searchRoots) {
        fs::path candidate = fs::path(root) / rawPath;
        if (isFile(candidate)) return candidate.string();
    }

    // 4. Nothing matched — hand back the file-relative candidate so the caller's
    //    existing "cannot find imported file" error names the most likely path.
    return relative.string();
}

std::vector<Token> ImportResolver::qualifyFileTokens(const std::vector<Token>& tokens) const {
    std::string module;
    std::unordered_map<std::string, std::string> bindings;
    std::unordered_set<std::string> ambiguous;
    Parser::scanModuleDirectives(tokens, module, bindings, ambiguous);
    if (module.empty() && sharedGenerics_.moduleNames.empty())
        return tokens;   // no modules in play — leave tokens (and thus scanned names) unchanged
    return Parser::qualifyTokens(tokens, module, bindings, ambiguous,
                                 sharedGenerics_.moduleTypes, sharedGenerics_.moduleFuncs,
                                 sharedGenerics_.moduleNames);
}

void ImportResolver::scanModules(const std::string& filePath,
                                 std::unordered_set<std::string>& visitedPaths) {
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

    // Record this file's module + its top-level decl names into the shared tables.
    std::string module;
    std::unordered_map<std::string, std::string> bindings;
    std::unordered_set<std::string> ambiguous;
    Parser::scanModuleDirectives(tokens, module, bindings, ambiguous);
    Parser::scanModuleMembers(tokens, module, sharedGenerics_.moduleTypes,
                              sharedGenerics_.moduleFuncs, sharedGenerics_.moduleNames);

    fs::path parentDir = canonical.parent_path();
    for (size_t i = 0; i + 1 < tokens.size(); ++i) {
        if (tokens[i].type == TokenType::IMPORT && tokens[i + 1].type == TokenType::STRING)
            scanModules(resolveImportPath(parentDir, stripQuotes(tokens[i + 1].lexeme)), visitedPaths);
    }
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

    // Register template names under their qualified module names (pass 0 has populated the tables),
    // so a cross-file use site `geo.Vec<i32>` resolves to the same `geo.Vec` template.
    seedParser.prescanTemplateNames(qualifyFileTokens(tokens));

    fs::path parentDir = canonical.parent_path();
    for (size_t i = 0; i + 1 < tokens.size(); ++i) {
        if (tokens[i].type == TokenType::IMPORT && tokens[i + 1].type == TokenType::STRING) {
            std::string rawPath = stripQuotes(tokens[i + 1].lexeme);
            std::string absPath = resolveImportPath(parentDir, rawPath);
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
    // Qualify so the collected class/enum names carry their module prefix (`geo.Point`), matching
    // what the parser produces; file-import STRING tokens are preserved by qualification.
    const std::vector<Token> tokens = qualifyFileTokens(lexer.tokens()[0]);

    std::unordered_set<std::string> names;
    fs::path parentDir = canonical.parent_path();

    for (size_t i = 0; i < tokens.size(); ++i) {
        // Collect class and enum names defined in this file (both are type names — the
        // monomorphization parser needs them to recognise e.g. `@variants(Color)` after a
        // generic type parameter is substituted with a concrete enum).
        if (i + 1 < tokens.size()
            && (tokens[i].type == TokenType::CLASS || tokens[i].type == TokenType::ENUM)
            && tokens[i + 1].type == TokenType::IDENTIFIER) {
            names.insert(tokens[i + 1].lexeme);
        }
        // Follow import statements to collect from transitive dependencies
        if (tokens[i].type == TokenType::IMPORT
            && i + 1 < tokens.size()
            && tokens[i + 1].type == TokenType::STRING) {
            std::string rawPath = stripQuotes(tokens[i + 1].lexeme);
            std::string absPath = resolveImportPath(parentDir, rawPath);
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
        if (!config_.stdlibDir.empty())
            std::cerr << "  (std/ resolves to: " << config_.stdlibDir << ")\n";
        for (const std::string& root : config_.searchRoots)
            std::cerr << "  (search root: " << root << ")\n";
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
            std::string absolutePath = resolveImportPath(parentDirectory, relativePath);

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
