//
// Created by Vladimir Arsenijevic on 01.6.2026.
//

#include "SymbolTable.h"

#include <ranges>

void SymbolTable::enterScope() {
    scopes.emplace_back();
}

void SymbolTable::exitScope() {
    if (!scopes.empty()) scopes.pop_back();
}

bool SymbolTable::declare(const std::string& name, Symbol symbol) {
    if (scopes.empty()) return false;
    auto& current = scopes.back();
    if (current.count(name)) return false;
    current.emplace(name, std::move(symbol));
    return true;
}

const Symbol* SymbolTable::lookup(const std::string& name) const {
    for (const auto& scope : std::ranges::reverse_view(scopes)) {
        auto it = scope.find(name);
        if (it != scope.end()) return &it->second;
    }
    return nullptr;
}

const Symbol* SymbolTable::lookupCurrentScope(const std::string& name) const {
    if (scopes.empty()) return nullptr;
    auto it = scopes.back().find(name);
    if (it != scopes.back().end()) return &it->second;
    return nullptr;
}

Symbol* SymbolTable::lookupMutable(const std::string& name) {
    for (auto& scope : std::ranges::reverse_view(scopes)) {
        auto it = scope.find(name);
        if (it != scope.end()) return &it->second;
    }
    return nullptr;
}

SymbolTable::InitSnapshot SymbolTable::captureInitState() const {
    InitSnapshot snap;
    for (const auto& scope : scopes)
        for (const auto& [name, sym] : scope)
            if (sym.kind == Symbol::Kind::Variable)
                snap[name] = sym.isInitialized;
    return snap;
}

void SymbolTable::restoreInitState(const InitSnapshot& snap) {
    for (auto& scope : scopes)
        for (auto& [name, sym] : scope)
            if (sym.kind == Symbol::Kind::Variable) {
                auto it = snap.find(name);
                if (it != snap.end())
                    sym.isInitialized = it->second;
            }
}
