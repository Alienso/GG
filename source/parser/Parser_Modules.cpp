//
// Created by Vladimir Arsenijevic on 28.5.2026.
//

#include "Parser.h"


// ============================================================
// Module namespacing (see the "Module namespacing" invariant)
// ============================================================

void Parser::scanModuleDirectives(const std::vector<Token>& toks, std::string& outModule,
                                  std::unordered_map<std::string, std::string>& outBindings,
                                  std::unordered_set<std::string>& outAmbiguous,
                                  const std::unordered_map<std::string, std::unordered_set<std::string>>& types,
                                  const std::unordered_map<std::string, std::unordered_set<std::string>>& funcs) {
    for (size_t i = 0; i < toks.size(); ++i) {
        // `module NAME ('.' NAME)* ;` — the file's module (first one wins). Dotted names (`std.io`)
        // are read as a greedy chain, joined with '.' — must mirror the real parse()'s module-line
        // skip exactly (same greediness), or a dotted `module` line would parse differently between
        // this prescan and the real parse and `currentModule_` would desync between the two passes.
        if (toks[i].type == TokenType::MODULE && outModule.empty()
            && i + 1 < toks.size() && toks[i + 1].type == TokenType::IDENTIFIER) {
            size_t j = i + 1;
            std::string name = toks[j].lexeme;
            ++j;
            while (j + 1 < toks.size() && toks[j].type == TokenType::DOT
                   && toks[j + 1].type == TokenType::IDENTIFIER) {
                name += "." + toks[j + 1].lexeme;
                j += 2;
            }
            outModule = name;
            continue;
        }
        // `import a.B ;` / `import a.b.C ;` — a symbol import (dotted, no quotes). `import "path";`
        // (STRING) is a file load, handled elsewhere. The LAST segment is always the imported
        // symbol; everything before it (dot-joined) is always the module path — unambiguous at any
        // depth, since GG top-level decls are always flat, never nested.
        // `import a.b.* ;` — wildcard: every segment is part of the module path, and every member
        // that module declares (types + free functions) is bound, exactly as if each had its own
        // `import a.b.Name;` line. Non-recursive: a submodule "a.b.c" is a distinct registry key,
        // never pulled in by a wildcard on "a.b".
        if (toks[i].type == TokenType::IMPORT && i + 1 < toks.size()
            && toks[i + 1].type == TokenType::IDENTIFIER) {
            size_t j = i + 1;
            std::vector<std::string> segs{ toks[j].lexeme };
            ++j;
            while (j + 1 < toks.size() && toks[j].type == TokenType::DOT
                   && toks[j + 1].type == TokenType::IDENTIFIER) {
                segs.push_back(toks[j + 1].lexeme);
                j += 2;
            }
            auto bind = [&](const std::string& simple, const std::string& qualified) {
                auto it = outBindings.find(simple);
                if (it != outBindings.end() && it->second != qualified)
                    outAmbiguous.insert(simple);        // same simple name from two modules → must qualify
                else
                    outBindings.emplace(simple, qualified);
            };
            bool wildcard = j + 1 < toks.size() && toks[j].type == TokenType::DOT
                            && toks[j + 1].type == TokenType::STAR;
            if (wildcard) {
                std::string module = segs[0];
                for (size_t k = 1; k < segs.size(); ++k) module += "." + segs[k];
                auto tIt = types.find(module);
                if (tIt != types.end())
                    for (const std::string& name : tIt->second) bind(name, module + "." + name);
                auto fIt = funcs.find(module);
                if (fIt != funcs.end())
                    for (const std::string& name : fIt->second) bind(name, module + "." + name);
            } else if (segs.size() >= 2) {
                const std::string simple = segs.back();
                std::string qualified = segs[0];
                for (size_t k = 1; k + 1 < segs.size(); ++k) qualified += "." + segs[k];
                qualified += "." + simple;
                bind(simple, qualified);
            }
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
    // Pass 1: fold a fully-qualified `mod.Name` (mod a known, possibly-dotted module) into a single
    // "mod.Name" token. Greedy LONGEST-PREFIX match: collect the maximal dotted identifier chain,
    // then try module-name candidates from longest to shortest (always leaving >=1 trailing segment
    // as "Name"), taking the first (i.e. longest) hit — so a longer registered module (`std.io`)
    // always wins over a shorter overlapping one (`std`) when both are registered.
    std::vector<Token> folded;
    folded.reserve(toks.size());
    for (size_t i = 0; i < toks.size(); ) {
        if (toks[i].type != TokenType::IDENTIFIER) { folded.push_back(toks[i]); ++i; continue; }

        std::vector<size_t> segStart{ i };
        size_t k = i + 1;
        while (k + 1 < toks.size() && toks[k].type == TokenType::DOT
               && toks[k + 1].type == TokenType::IDENTIFIER) {
            segStart.push_back(k + 1);
            k += 2;
        }

        size_t bestPrefixLen = 0;   // in segments; 0 = no fold
        // Try prefixes from longest (segStart.size()-1, leaving exactly 1 trailing "Name" segment)
        // down to 1 (a single-identifier module), taking the first (longest) match.
        for (size_t plen = segStart.size() - 1; plen != 0; --plen) {
            std::string cand = toks[segStart[0]].lexeme;
            for (size_t s = 1; s < plen; ++s) cand += "." + toks[segStart[s]].lexeme;
            if (moduleNames.count(cand)) { bestPrefixLen = plen; break; }
        }

        if (bestPrefixLen > 0) {
            std::string modPart = toks[segStart[0]].lexeme;
            for (size_t s = 1; s < bestPrefixLen; ++s) modPart += "." + toks[segStart[s]].lexeme;
            folded.push_back(Token{ TokenType::IDENTIFIER,
                                    modPart + "." + toks[segStart[bestPrefixLen]].lexeme, toks[i].line });
            i = segStart[bestPrefixLen] + 1;
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
            // A top-level free function's OWN declared name (depth == 0 here, since depth > 0 is
            // already excluded above) is NEVER looked up via an import binding — a declaration
            // doesn't "refer" to anything, it introduces a new symbol that belongs to this file's
            // own module by construction. Without this, an unrelated `import mod.f;` for some OTHER
            // module's same-named function would silently hijack THIS file's own `fn f(...)`
            // declaration to that other qualified name instead of its own module (or, in a
            // module-less file, leave it bare exactly as before — there's nothing to prefix).
            if (afterFn) {
                if (!module.empty()) {
                    out.push_back(Token{ TokenType::IDENTIFIER, module + "." + t.lexeme, t.line });
                    continue;
                }
                out.push_back(t);
                continue;
            }
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