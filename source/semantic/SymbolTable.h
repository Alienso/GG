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

private:
    std::vector<std::unordered_map<std::string, Symbol>> scopes;
};

#endif //GG_SYMBOLTABLE_H
