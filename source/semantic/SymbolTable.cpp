//
// Created by Vladimir Arsenijevic on 01.6.2026.
//

#include "SymbolTable.h"

#include <ranges>

void SymbolTable::enterScope() {
    scopes_.emplace_back();
}

void SymbolTable::exitScope() {
    if (!scopes_.empty()) scopes_.pop_back();
}

bool SymbolTable::declare(const std::string& name, Symbol symbol) {
    if (scopes_.empty()) return false;
    auto& current = scopes_.back();
    if (current.count(name)) return false;
    current.emplace(name, std::move(symbol));
    return true;
}

const Symbol* SymbolTable::lookup(const std::string& name) const {
    for (const auto & scope : std::ranges::reverse_view(scopes_)) {
        auto found = scope.find(name);
        if (found != scope.end()) return &found->second;
    }
    return nullptr;
}

const Symbol* SymbolTable::lookupCurrentScope(const std::string& name) const {
    if (scopes_.empty()) return nullptr;
    auto found = scopes_.back().find(name);
    if (found != scopes_.back().end()) return &found->second;
    return nullptr;
}
