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
};

class SymbolTable {
public:
    void          enterScope();
    void          exitScope();

    // Returns false if name already exists in the current (innermost) scope.
    bool          declare(const std::string& name, Symbol symbol);

    // Walks all scopes from innermost to outermost.
    const Symbol* lookup(const std::string& name) const;

    // Looks only in the current (innermost) scope.
    const Symbol* lookupCurrentScope(const std::string& name) const;

private:
    std::vector<std::unordered_map<std::string, Symbol>> scopes_;
};

#endif //GG_SYMBOLTABLE_H
