//
// Created by Vladimir Arsenijevic on 28.5.2026.
//

#include "Parser.h"


// ============================================================
// Module namespacing (see the "Module namespacing" invariant)
// ============================================================

void Parser::scanModuleDirectives(const std::vector<Token>& toks, std::string& outModule,
                                  std::unordered_map<std::string, std::string>& outBindings,
                                  std::unordered_set<std::string>& outAmbiguous) {
    for (size_t i = 0; i < toks.size(); ++i) {
        // `module NAME ;` — the file's module (first one wins).
        if (toks[i].type == TokenType::MODULE && outModule.empty()
            && i + 1 < toks.size() && toks[i + 1].type == TokenType::IDENTIFIER) {
            outModule = toks[i + 1].lexeme;
            continue;
        }
        // `import a.B ;` — a symbol import (dotted, no quotes). `import "path";` (STRING) is a file
        // load, handled elsewhere. Single-segment module in v1: IMPORT IDENT '.' IDENT ';'.
        if (toks[i].type == TokenType::IMPORT && i + 3 < toks.size()
            && toks[i + 1].type == TokenType::IDENTIFIER && toks[i + 2].type == TokenType::DOT
            && toks[i + 3].type == TokenType::IDENTIFIER) {
            const std::string simple    = toks[i + 3].lexeme;
            const std::string qualified = toks[i + 1].lexeme + "." + toks[i + 3].lexeme;
            auto it = outBindings.find(simple);
            if (it != outBindings.end() && it->second != qualified)
                outAmbiguous.insert(simple);            // same simple name from two modules → must qualify
            else
                outBindings.emplace(simple, qualified);
        }
    }
}

void Parser::scanModuleMembers(const std::vector<Token>& toks, const std::string& module,
                               std::unordered_map<std::string, std::unordered_set<std::string>>& types,
                               std::unordered_map<std::string, std::unordered_set<std::string>>& funcs,
                               std::unordered_set<std::string>& names) {
    if (module.empty()) return;   // global module — names are never qualified
    names.insert(module);
    auto& typeSet = types[module];
    auto& funcSet = funcs[module];
    int depth = 0;   // only TOP-LEVEL decls (depth 0) are module members — methods live at depth > 0
    for (size_t i = 0; i < toks.size(); ++i) {
        TokenType t = toks[i].type;
        if (t == TokenType::LEFT_BRACE)  { depth++; continue; }
        if (t == TokenType::RIGHT_BRACE) { if (depth > 0) depth--; continue; }
        if (depth != 0) continue;
        if (t == TokenType::CLASS || t == TokenType::ENUM || t == TokenType::TRAIT
            || t == TokenType::ANNOTATION) {
            if (i + 1 < toks.size() && toks[i + 1].type == TokenType::IDENTIFIER)
                typeSet.insert(toks[i + 1].lexeme);
        } else if (t == TokenType::FN) {
            size_t j = i + 1;
            if (j < toks.size() && toks[j].type == TokenType::PRIVATE) j++;
            if (j < toks.size() && toks[j].type == TokenType::IDENTIFIER && toks[j].lexeme != "main")
                funcSet.insert(toks[j].lexeme);
        }
        // `extern` names are C-ABI symbols — never module members (intentionally skipped).
    }
}

std::vector<Token> Parser::qualifyTokens(
    const std::vector<Token>& toks, const std::string& module,
    const std::unordered_map<std::string, std::string>& bindings,
    const std::unordered_set<std::string>& ambiguous,
    const std::unordered_map<std::string, std::unordered_set<std::string>>& types,
    const std::unordered_map<std::string, std::unordered_set<std::string>>& funcs,
    const std::unordered_set<std::string>& moduleNames) {
    // Pass 1: fold a fully-qualified `mod.Name` (mod a known module) into a single "mod.Name" token.
    std::vector<Token> folded;
    folded.reserve(toks.size());
    for (size_t i = 0; i < toks.size(); ) {
        if (toks[i].type == TokenType::IDENTIFIER && i + 2 < toks.size()
            && toks[i + 1].type == TokenType::DOT && toks[i + 2].type == TokenType::IDENTIFIER
            && moduleNames.count(toks[i].lexeme)) {
            folded.push_back(Token{ TokenType::IDENTIFIER,
                                    toks[i].lexeme + "." + toks[i + 2].lexeme, toks[i].line });
            i += 3;
        } else {
            folded.push_back(toks[i]);
            ++i;
        }
    }

    const auto* modTypes = module.empty() || !types.count(module) ? nullptr : &types.at(module);
    const auto* modFuncs = module.empty() || !funcs.count(module) ? nullptr : &funcs.at(module);
    // Classify a binding/current-module name as a TYPE (a known type) — else it is treated as a
    // function. A binding's kind comes from its target module's type/func sets.
    auto isTypeSymbol = [&](const std::string& simple, const std::string& qualified) -> bool {
        auto dot = qualified.rfind('.');
        if (dot != std::string::npos) {
            std::string mod = qualified.substr(0, dot), nm = qualified.substr(dot + 1);
            auto it = types.find(mod);
            if (it != types.end() && it->second.count(nm)) return true;
            auto fit = funcs.find(mod);
            if (fit != funcs.end() && fit->second.count(nm)) return false;
        }
        return modTypes && modTypes->count(simple);   // fallback: current module's own type?
    };

    // Pass 2: qualify each bare reference/decl name. A TYPE name is qualified in any position; a
    // FUNCTION name only when the next token is `(` or `<` (a call or generic instantiation) — so a
    // field/local named like a free function is not rewritten. A method / enum-member decl name
    // (a bare name right after `fn` inside a body, i.e. at brace depth > 0) is left alone, as is any
    // name after `.`/`::`, an already-qualified name, and `main`.
    std::vector<Token> out;
    out.reserve(folded.size());
    int depth = 0;
    for (size_t i = 0; i < folded.size(); ++i) {
        const Token& t = folded[i];
        if (t.type == TokenType::LEFT_BRACE)  { out.push_back(t); depth++; continue; }
        if (t.type == TokenType::RIGHT_BRACE) { out.push_back(t); if (depth > 0) depth--; continue; }

        bool afterDot = !out.empty() && (out.back().type == TokenType::DOT
                                         || out.back().type == TokenType::COLON_COLON);
        bool afterFn  = !out.empty() && out.back().type == TokenType::FN;
        if (t.type == TokenType::IDENTIFIER && !afterDot && t.lexeme != "main"
            && t.lexeme.find('.') == std::string::npos && !ambiguous.count(t.lexeme)
            && !(afterFn && depth > 0)) {                  // not a method / enum-member decl name
            // Resolve the name to a (qualified, isType) decision: import binding first, else the
            // current module's own type/func sets.
            std::string qualified;
            bool isType = false, found = false;
            auto b = bindings.find(t.lexeme);
            if (b != bindings.end()) {
                qualified = b->second; found = true; isType = isTypeSymbol(t.lexeme, qualified);
            } else if (modTypes && modTypes->count(t.lexeme)) {
                qualified = module + "." + t.lexeme; found = true; isType = true;
            } else if (modFuncs && modFuncs->count(t.lexeme)) {
                qualified = module + "." + t.lexeme; found = true; isType = false;
            }
            if (found) {
                TokenType next = (i + 1 < folded.size()) ? folded[i + 1].type : TokenType::END_OF_FILE;
                bool callPos = next == TokenType::LEFT_PAREN || next == TokenType::LESS;
                if (isType || callPos) {
                    out.push_back(Token{ TokenType::IDENTIFIER, qualified, t.line });
                    continue;
                }
            }
        }
        out.push_back(t);
    }
    return out;
}