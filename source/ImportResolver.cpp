//
// Created by Vladimir Arsenijevic on 05.6.2026.
//

#include "ImportResolver.h"
#include "lexer/Lexer.h"
#include "lexer/Token.h"
#include "parser/Parser.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

// Dedup key for an already-resolved canonical path. On Windows the filesystem is case-insensitive, so
// "String.gg" and "string.gg" name the SAME file — importing both spellings (e.g. one file imports
// "std/String.gg" and another "std/string.gg") must be treated as ONE import, or the file is parsed
// twice and its classes are redefined ("class 'String' is already defined"). Lowercasing the key folds
// the spellings together. On a case-sensitive filesystem the two are genuinely distinct files, so the
// key is left unchanged there. (`weakly_canonical` normalizes `.`/`..`/separators but preserves case,
// so it alone does not dedup case-variant spellings.)
static std::string dedupKey(const fs::path& canonical) {
    std::string s = canonical.string();
#ifdef _WIN32
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
#endif
    return s;
}

// ============================================================
// Public entry point
// ============================================================

Program ImportResolver::resolve(const std::string& rootFilePath, const ModuleSearchConfig& config) {
    processedPaths.clear();
    sharedGenerics_ = GenericRegistry{};
    config_ = config;
    reportedMissing_.clear();

    // Pass 0: scan every file for its `module NAME;` + top-level decl names, populating the shared
    // (module → member names) table + the set of module names. This must precede all parsing so any
    // file can qualify references to a sibling file's same-module symbols and fold fully-qualified
    // names (`geo.Point`) written before the defining file is parsed.
    std::unordered_set<std::string> modVisited;
    scanModules(rootFilePath, modVisited);

    // Pass 1: pre-register every generic template name across all files so that
    // use sites are recognised regardless of which file declares the template. Trait names are
    // collected for the WHOLE graph up front (like collectClassNames) so seedParser's own
    // desugarTraitParams call (inside prescanTemplates) recognises a user trait no matter which file
    // in the graph declares it, or the order files are visited in — including import cycles.
    std::unordered_set<std::string> traitSeedVisited;
    std::unordered_set<std::string> traitSeedNames = collectTraitNames(rootFilePath, traitSeedVisited);
    Parser seedParser({}, &sharedGenerics_, std::move(traitSeedNames));
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

void ImportResolver::reportMissingImport(const std::string& filePath) {
    if (!reportedMissing_.insert(filePath).second) return;   // already reported this path
    std::cerr << "Error: cannot find imported file '" << filePath << "'\n";
    if (!config_.stdlibDir.empty())
        std::cerr << "  (std/ resolves to: " << config_.stdlibDir << ")\n";
    for (const std::string& root : config_.searchRoots)
        std::cerr << "  (search root: " << root << ")\n";
}

// ============================================================
// Dotted-import → module-directory resolution (self-loading imports)
// ============================================================

ImportResolver::ModuleDir ImportResolver::resolveModuleDir(const std::vector<std::string>& segments,
                                                           const fs::path& importerDir) const {
    if (segments.empty()) return {};
    std::error_code ec;
    auto isDir = [&](const fs::path& p) { return fs::exists(p, ec) && fs::is_directory(p, ec); };

    // A valid `import` is either a bare module (whole path is the module dir, e.g. `import std.crt;`)
    // or module + one trailing symbol (`import std.utility.Pair;`). So only the full length and
    // all-but-last are candidate module prefixes; anything shorter would leave >1 trailing segment.
    for (size_t prefixLen = segments.size();
         prefixLen >= 1 && prefixLen + 1 >= segments.size(); --prefixLen) {
        // Candidate roots mirror resolveImportPath: `std` → stdlibDir; else importer-relative then
        // each configured search root.
        std::vector<fs::path> bases;
        size_t relStart = 0;
        if (!config_.stdlibDir.empty() && segments[0] == "std") {
            bases.emplace_back(config_.stdlibDir);
            relStart = 1;
        } else {
            bases.push_back(importerDir);
            for (const std::string& r : config_.searchRoots) bases.emplace_back(r);
        }
        fs::path rel;
        for (size_t k = relStart; k < prefixLen; ++k) rel /= segments[k];

        for (const fs::path& base : bases) {
            fs::path cand = rel.empty() ? base : base / rel;
            if (isDir(cand)) {
                ModuleDir md;
                md.dir   = cand.string();
                md.found = true;
                for (size_t k = 0; k < prefixLen; ++k) {
                    if (k) md.moduleName += ".";
                    md.moduleName += segments[k];
                }
                return md;
            }
        }
        if (prefixLen == 1) break;   // avoid size_t wraparound past 0
    }
    return {};
}

std::vector<std::string> ImportResolver::moduleFiles(const std::string& dir) const {
    std::vector<std::string> files;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (entry.is_regular_file(ec) && entry.path().extension() == ".gg")
            files.push_back(entry.path().string());
    }
    std::sort(files.begin(), files.end());   // deterministic load order across platforms
    return files;
}

void ImportResolver::verifyModuleDir(const std::string& dir, const std::string& expectedModule) {
    if (!verifiedModuleDirs_.insert(dir).second) return;   // verify each directory only once
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (!(entry.is_regular_file(ec) && entry.path().extension() == ".gg")) continue;
        std::vector<std::string> paths{ entry.path().string() };
        Lexer lexer(paths);
        lexer.lex();
        std::string module;
        std::unordered_map<std::string, std::string> bindings;
        std::unordered_set<std::string> ambiguous;
        Parser::scanModuleDirectives(lexer.tokens()[0], module, bindings, ambiguous,
                                     sharedGenerics_.moduleTypes, sharedGenerics_.moduleFuncs);
        if (module != expectedModule) {
            std::cerr << "Error: file '" << entry.path().string()
                      << "' is loaded as part of module '" << expectedModule
                      << "' (from its directory) but declares module '"
                      << (module.empty() ? "<none>" : module) << "'\n";
        }
    }
}

std::vector<std::string> ImportResolver::dependencyPaths(const std::vector<Token>& tokens,
                                                         const fs::path& importerDir) {
    std::vector<std::string> deps;
    for (size_t i = 0; i + 1 < tokens.size(); ++i) {
        if (tokens[i].type != TokenType::IMPORT) continue;
        const Token& next = tokens[i + 1];

        // Quoted `import "path"` — a single file load (unchanged behavior).
        if (next.type == TokenType::STRING) {
            deps.push_back(resolveImportPath(importerDir, stripQuotes(next.lexeme)));
            continue;
        }
        if (next.type != TokenType::IDENTIFIER) continue;

        // Dotted `import a.b.C;`. The name may arrive already folded into one "a.b.C" token (in
        // passes that qualify their tokens) or as an unfolded IDENT '.' IDENT … run — accumulate the
        // full dotted string either way, then split into segments.
        std::string dotted = next.lexeme;
        size_t j = i + 2;
        while (j + 1 < tokens.size() && tokens[j].type == TokenType::DOT
               && tokens[j + 1].type == TokenType::IDENTIFIER) {
            dotted += "." + tokens[j + 1].lexeme;
            j += 2;
        }
        std::vector<std::string> segs;
        for (size_t k = 0, start = 0; k <= dotted.size(); ++k) {
            if (k == dotted.size() || dotted[k] == '.') {
                segs.push_back(dotted.substr(start, k - start));
                start = k + 1;
            }
        }

        ModuleDir md = resolveModuleDir(segs, importerDir);
        if (!md.found) continue;   // no matching directory → a pure name binding, loads no file
        verifyModuleDir(md.dir, md.moduleName);
        for (std::string& f : moduleFiles(md.dir)) deps.push_back(std::move(f));
    }
    return deps;
}

std::vector<Token> ImportResolver::qualifyFileTokens(const std::vector<Token>& tokens) const {
    std::string module;
    std::unordered_map<std::string, std::string> bindings;
    std::unordered_set<std::string> ambiguous;
    Parser::scanModuleDirectives(tokens, module, bindings, ambiguous,
                                 sharedGenerics_.moduleTypes, sharedGenerics_.moduleFuncs);
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
    if (ec || !fs::exists(canonical)) { reportMissingImport(filePath); return; }

    std::string canonicalStr = canonical.string();
    std::string key = dedupKey(canonical);
    if (visitedPaths.count(key)) return;
    visitedPaths.insert(key);

    std::vector<std::string> paths{ canonicalStr };
    Lexer lexer(paths);
    lexer.lex();
    const auto& tokens = lexer.tokens()[0];

    // Record this file's module + its top-level decl names into the shared tables.
    std::string module;
    std::unordered_map<std::string, std::string> bindings;
    std::unordered_set<std::string> ambiguous;
    Parser::scanModuleDirectives(tokens, module, bindings, ambiguous,
                                 sharedGenerics_.moduleTypes, sharedGenerics_.moduleFuncs);
    Parser::scanModuleMembers(tokens, module, sharedGenerics_.moduleTypes,
                              sharedGenerics_.moduleFuncs, sharedGenerics_.moduleNames);

    fs::path parentDir = canonical.parent_path();
    for (const std::string& dep : dependencyPaths(tokens, parentDir))
        scanModules(dep, visitedPaths);
}

void ImportResolver::prescanTemplates(const std::string& filePath,
                                      std::unordered_set<std::string>& visitedPaths,
                                      Parser& seedParser) {
    std::error_code ec;
    fs::path canonical = fs::weakly_canonical(fs::path(filePath), ec);
    if (ec || !fs::exists(canonical)) { reportMissingImport(filePath); return; }

    std::string canonicalStr = canonical.string();
    std::string key = dedupKey(canonical);
    if (visitedPaths.count(key)) return;
    visitedPaths.insert(key);

    std::vector<std::string> paths{ canonicalStr };
    Lexer lexer(paths);
    lexer.lex();
    const auto& tokens = lexer.tokens()[0];

    // Register template names under their qualified module names (pass 0 has populated the tables),
    // so a cross-file use site `geo.Vec<i32>` resolves to the same `geo.Vec` template. Desugar
    // bare-trait-name parameters FIRST (seedParser was pre-seeded with the full-graph trait-name set
    // in resolve(), so this sees every user trait regardless of which file in the graph declares it) —
    // otherwise a sugared function would look non-generic here and wrongly land in ordinaryFuncNames.
    std::vector<Token> qualified = qualifyFileTokens(tokens);
    qualified = seedParser.desugarTraitParams(qualified);
    seedParser.prescanTemplateNames(qualified);

    fs::path parentDir = canonical.parent_path();
    for (const std::string& dep : dependencyPaths(tokens, parentDir))
        prescanTemplates(dep, visitedPaths, seedParser);
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
    if (ec || !fs::exists(canonical)) { reportMissingImport(filePath); return {}; }

    std::string canonicalStr = canonical.string();
    std::string key = dedupKey(canonical);
    if (visitedPaths.count(key)) return {};
    visitedPaths.insert(key);

    std::vector<std::string> paths{ canonicalStr };
    Lexer lexer(paths);
    lexer.lex();
    // Qualify so the collected class/enum names carry their module prefix (`geo.Point`), matching
    // what the parser produces; file-import STRING tokens are preserved by qualification.
    const std::vector<Token> tokens = qualifyFileTokens(lexer.tokens()[0]);

    std::unordered_set<std::string> names;
    fs::path parentDir = canonical.parent_path();

    // Collect class and enum names defined in this file (both are type names — the monomorphization
    // parser needs them to recognise e.g. `@variants(Color)` after a generic type parameter is
    // substituted with a concrete enum).
    for (size_t i = 0; i + 1 < tokens.size(); ++i) {
        if ((tokens[i].type == TokenType::CLASS || tokens[i].type == TokenType::ENUM)
            && tokens[i + 1].type == TokenType::IDENTIFIER) {
            names.insert(tokens[i + 1].lexeme);
        }
    }
    // Collect transitively from every dependency (quoted file imports + dotted module directories).
    for (const std::string& dep : dependencyPaths(tokens, parentDir)) {
        auto imported = collectClassNames(dep, visitedPaths);
        names.insert(imported.begin(), imported.end());
    }
    return names;
}

// ============================================================
// Trait-name pre-scanner (mirrors collectClassNames exactly)
// ============================================================

std::unordered_set<std::string> ImportResolver::collectTraitNames(
    const std::string& filePath,
    std::unordered_set<std::string>& visitedPaths)
{
    std::error_code ec;
    fs::path canonical = fs::weakly_canonical(fs::path(filePath), ec);
    if (ec || !fs::exists(canonical)) { reportMissingImport(filePath); return {}; }

    std::string canonicalStr = canonical.string();
    std::string key = dedupKey(canonical);
    if (visitedPaths.count(key)) return {};
    visitedPaths.insert(key);

    std::vector<std::string> paths{ canonicalStr };
    Lexer lexer(paths);
    lexer.lex();
    const std::vector<Token> tokens = qualifyFileTokens(lexer.tokens()[0]);

    std::unordered_set<std::string> names;
    fs::path parentDir = canonical.parent_path();

    // Trait declarations are always top-level (no nested traits), so a flat scan suffices.
    for (size_t i = 0; i + 1 < tokens.size(); ++i) {
        if (tokens[i].type == TokenType::TRAIT && tokens[i + 1].type == TokenType::IDENTIFIER)
            names.insert(tokens[i + 1].lexeme);
    }
    for (const std::string& dep : dependencyPaths(tokens, parentDir)) {
        auto imported = collectTraitNames(dep, visitedPaths);
        names.insert(imported.begin(), imported.end());
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
        reportMissingImport(filePath);
        return Program{};
    }

    std::string canonicalString = canonical.string();
    std::string dedup = dedupKey(canonical);
    if (processedPaths.count(dedup)) return Program{};
    processedPaths.insert(dedup);  // mark before recursing — breaks cycles

    // Collect class names from this file and all its transitive imports so that
    // cross-file constructor calls ("ClassName varName(args)") are recognised as
    // variable declarations during parsing.
    std::unordered_set<std::string> classNameVisited;
    auto allClassNames = collectClassNames(canonicalString, classNameVisited);

    // Same for user-declared trait names, so a bare-trait-name parameter (desugared to a bounded
    // generic — see Parser::desugarTraitParams) recognises a trait declared in ANY imported file.
    std::unordered_set<std::string> traitNameVisited;
    auto allTraitNames = collectTraitNames(canonicalString, traitNameVisited);

    // Lex the file (parsing is deferred — see below).
    std::vector<std::string> paths = { canonicalString };
    Lexer lexer(paths);
    lexer.lex();
    const std::vector<Token>& tokens = lexer.tokens()[0];
    fs::path parentDirectory = canonical.parent_path();
    Program result;

    // Process every dependency (quoted file imports + dotted module directories) BEFORE parsing
    // this file's own tokens. This isn't just about declaration order in the flattened `result` —
    // it's load-bearing for correctness: a generic-function call written WITHOUT explicit `<...>`
    // (deduceVariadicInstantiation / inferGenericTypeArgs, Parser_Generics.cpp) looks up the
    // template directly in the shared `gen_->templates` registry AT PARSE TIME, not later at
    // monomorphization time (unlike an explicit `name<Targs>(...)` call, which only needs the
    // template to exist once `runMonomorphization` runs, well after every file is parsed). If this
    // file makes such a call to a template declared in an import — and that import hadn't been
    // parsed yet — the lookup would find nothing and (wrongly) report "cannot infer type
    // argument(s)", even though the call is perfectly valid and would have resolved correctly once
    // the dependency's own parse ran. This never surfaced before because every prior generic
    // template happened to be declared in the SAME file as its non-explicit call sites (so it was
    // already captured earlier in that file's own top-to-bottom parse); it surfaces now that a
    // consumer file can call an inferred generic-pack function declared in a separate imported file
    // (e.g. `printf(...)` from `stdlib/io/IO2.gg`, called from `samples/std_lib.gg`).
    for (const std::string& dep : dependencyPaths(tokens, parentDirectory)) {
        Program imported = processFile(dep);
        for (Stmt& importedDecl : imported.declarations)
            result.declarations.push_back(std::move(importedDecl));
    }

    // Now parse this file's own tokens — every dependency's generic templates are already captured
    // in gen_->templates. Bind to the shared generics registry and defer monomorphization —
    // resolve() expands all instantiations once after every file has been parsed.
    Parser parser(std::move(allClassNames), &sharedGenerics_, std::move(allTraitNames));
    Program rawProgram = parser.parse(tokens, canonicalString, /*runMonomorphization=*/false);
    // Append this file's own declarations, dropping ImportStmt nodes (quoted imports — already
    // followed above; dotted imports produce no AST node at all).
    for (Stmt& declaration : rawProgram.declarations) {
        if (!declaration.node) continue;
        if (std::holds_alternative<ImportStmt>(*declaration.node)) continue;
        result.declarations.push_back(std::move(declaration));
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
