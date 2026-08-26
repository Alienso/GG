//
// Created by Vladimir Arsenijevic on 01.6.2026.
//

#ifndef GG_SYMBOLTABLE_H
#define GG_SYMBOLTABLE_H

#include <string>
#include <vector>
#include <unordered_map>
#include "Type.h"
#include "../lexer/Token.h"

struct Symbol {
    enum class Kind { Variable, Function };

    Kind              kind;
    Type              type;              // variable: declared type; function: return type
    Token             declarationToken; // for "previously declared at line N" messages
    std::vector<Type> paramTypes;       // non-empty for Function symbols only
    bool              isParameter    = false; // true for function/method parameters (object params are immutable references)
    bool              isInitialized  = false; // true once the variable has been definitely assigned a value
    bool              isMutable      = false; // `mut` — reassignable; otherwise const (single-assignment)
    std::vector<bool> paramMut     = {};      // per-parameter `mut` flag (Function symbols only)
    bool              isNarrowedNonNull = false; // smart-cast: a nullable `T?` proven non-null on the
                                                 // current path (so reads narrow to `T`); dropped on
                                                 // reassignment; merged across branches like init state.
};

class SymbolTable {
public:
    void          enterScope();
    void          exitScope();

    // Returns false if name already exists in the current (innermost) scope.
    bool          declare(const std::string& name, Symbol symbol);

    // Walks all scopes from innermost to outermost.
    [[nodiscard]] const Symbol* lookup(const std::string& name) const;

    // Mutable walk — used by the semantic analyser to mark variables as initialized.
    Symbol*       lookupMutable(const std::string& name);

    // Looks only in the current (innermost) scope.
    [[nodiscard]] const Symbol* lookupCurrentScope(const std::string& name) const;

    // Snapshot / restore: used for definite-assignment analysis across branches.
    // Only Variable symbols are tracked; Function entries are ignored.
    using InitSnapshot = std::unordered_map<std::string, bool>;
    [[nodiscard]] InitSnapshot captureInitState() const;
    void                       restoreInitState(const InitSnapshot& snap);

    // Smart-cast narrowing snapshot — same shape/merge as init state, tracking isNarrowedNonNull.
    [[nodiscard]] InitSnapshot captureNarrowState() const;
    void                       restoreNarrowState(const InitSnapshot& snap);
    // Clear a binding's non-null narrowing (on reassignment). Walks innermost→outermost.
    void                       clearNarrowing(const std::string& name);

    // ---- Constructor field-initialization tracking ----
    // Whether each instance field has been definitely assigned within the constructor CURRENTLY
    // being analyzed. Rides along inside the SAME InitSnapshot as locals (under a reserved key
    // prefix no real identifier can spell, since '#' never lexes inside one) — every existing
    // branch-merge call site (analyzeIf/While/For's capture/restore) picks up field tracking for
    // free, with zero changes to those call sites: the merge loops there iterate the whole flat
    // snapshot map generically, they don't enumerate known variable names.
    void resetCtorFields(const std::vector<std::pair<std::string, bool>>& fieldsWithInitFlag);
    void setFieldInitialized(const std::string& fieldName);
    [[nodiscard]] bool isFieldInitialized(const std::string& fieldName) const;

private:
    std::vector<std::unordered_map<std::string, Symbol>> scopes;
    std::unordered_map<std::string, bool> ctorFieldInit_;   // active only during ctor analysis
    static constexpr const char* kFieldKeyPrefix = "#field#";
};

#endif //GG_SYMBOLTABLE_H
