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
    for (const auto& [name, init] : ctorFieldInit_)
        snap[kFieldKeyPrefix + name] = init;
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
    for (auto& [name, init] : ctorFieldInit_) {
        auto it = snap.find(kFieldKeyPrefix + name);
        if (it != snap.end()) init = it->second;
    }
}

void SymbolTable::resetCtorFields(const std::vector<std::pair<std::string, bool>>& fieldsWithInitFlag) {
    ctorFieldInit_.clear();
    for (const auto& [name, hasInit] : fieldsWithInitFlag)
        ctorFieldInit_[name] = hasInit;
}

void SymbolTable::setFieldInitialized(const std::string& fieldName) {
    auto it = ctorFieldInit_.find(fieldName);
    if (it != ctorFieldInit_.end()) it->second = true;
}

bool SymbolTable::isFieldInitialized(const std::string& fieldName) const {
    auto it = ctorFieldInit_.find(fieldName);
    return it == ctorFieldInit_.end() || it->second;   // unknown name ⇒ not our concern, treat as ok
}

SymbolTable::InitSnapshot SymbolTable::captureNarrowState() const {
    InitSnapshot snap;
    for (const auto& scope : scopes)
        for (const auto& [name, sym] : scope)
            if (sym.kind == Symbol::Kind::Variable)
                snap[name] = sym.isNarrowedNonNull;
    return snap;
}

void SymbolTable::restoreNarrowState(const InitSnapshot& snap) {
    for (auto& scope : scopes)
        for (auto& [name, sym] : scope)
            if (sym.kind == Symbol::Kind::Variable) {
                auto it = snap.find(name);
                if (it != snap.end())
                    sym.isNarrowedNonNull = it->second;
            }
}

void SymbolTable::clearNarrowing(const std::string& name) {
    for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
        auto found = it->find(name);
        if (found != it->end()) { found->second.isNarrowedNonNull = false; return; }
    }
}
