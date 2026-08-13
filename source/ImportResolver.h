//
// Created by Vladimir Arsenijevic on 05.6.2026.
//

#ifndef GG_IMPORTRESOLVER_H
#define GG_IMPORTRESOLVER_H

#include <string>
#include <vector>
#include <unordered_set>
#include <filesystem>
#include "parser/Ast.h"
#include "parser/Parser.h"

// Configures how an `import "..."` string is resolved to a file on disk.
//
// Resolution order for an import path P requested from a file in directory D:
//   1. Reserved logical prefix `std/` → <stdlibDir>/<rest of P>. `std/` always
//      names the compiler's standard library, independent of the importer's
//      location on disk (so `import "std/String.gg"` works from anywhere).
//   2. File-relative — D/P (the original, backward-compatible behavior).
//   3. Each entry of searchRoots, in order — <root>/P.
// The first candidate that exists wins; a `std/` path resolves under stdlibDir
// even when missing, so a bad stdlib import produces a clear error there.
struct ModuleSearchConfig {
    std::string stdlibDir;                   // target of the reserved `std/` prefix (empty = disabled)
    std::vector<std::string> searchRoots;    // extra root directories, tried after file-relative
};

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
    // `config` controls stdlib / search-root resolution (default = file-relative only).
    Program resolve(const std::string& rootFilePath, const ModuleSearchConfig& config = {});

private:
    std::unordered_set<std::string> processedPaths;
    // Shared across every file so generic templates and their instantiations span files.
    GenericRegistry sharedGenerics_;
    // Active module-resolution configuration for the current resolve() call.
    ModuleSearchConfig config_;

    Program processFile(const std::string& filePath);

    // Pass 0 (module namespacing): transitively scan every file for its `module NAME;` + top-level
    // decl names, populating sharedGenerics_.moduleMembers / moduleNames before any parsing.
    void scanModules(const std::string& filePath, std::unordered_set<std::string>& visitedPaths);

    // Apply module qualification (fold FQNs + prefix bare names) to a lexed file's tokens using the
    // shared module tables — so names scanned/registered here match what the parser produces.
    std::vector<Token> qualifyFileTokens(const std::vector<Token>& tokens) const;

    // Maps an import path (as written) requested from `importerDir` to a concrete
    // filesystem path, honouring the `std/` prefix and searchRoots (see the
    // ModuleSearchConfig comment). Returns the file-relative candidate as a fallback
    // when nothing matches, so callers' existing "cannot find" errors still fire.
    std::string resolveImportPath(const std::filesystem::path& importerDir,
                                  const std::string& rawPath) const;

    // Transitively pre-register generic template names (so cross-file use sites are
    // recognised regardless of which file declares the template) into sharedGenerics_.
    void prescanTemplates(const std::string& filePath,
                          std::unordered_set<std::string>& visitedPaths,
                          Parser& seedParser);

    // Collects all class names (transitively) reachable from filePath by lexing
    // each file and scanning for "class IDENTIFIER" tokens.  visitedPaths prevents
    // cycles and duplicate work.
    std::unordered_set<std::string> collectClassNames(
        const std::string& filePath,
        std::unordered_set<std::string>& visitedPaths);

    // Strips the surrounding double-quote characters from a STRING token lexeme.
    static std::string stripQuotes(const std::string& lexeme);

    // Prints the standard "cannot find imported file" diagnostic (path + std:/searchRoots hints),
    // once per unique missing path per resolve() call (dedup via reportedMissing_) — several passes
    // independently walk the same import graph, and without dedup a single bad import would print
    // the same message once per pass. Shared by every pass that resolves an import path against the
    // filesystem — a pass whose own missing-file check stayed silent (as collectClassNames's did)
    // leaves a type name unregistered with NO explanation, surfacing later as a baffling, unrelated
    // parse error at the use site instead of pointing at the actual broken import.
    void reportMissingImport(const std::string& filePath);
    std::unordered_set<std::string> reportedMissing_;
};

#endif //GG_IMPORTRESOLVER_H
