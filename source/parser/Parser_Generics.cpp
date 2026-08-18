//
// Created by Vladimir Arsenijevic on 28.5.2026.
//

#include "Parser.h"
#include "Parser_internal.h"
#include <iostream>

// Number of fixed (non-pack) params in a variadic template's raw param list. Defined below; forward-
// declared so the prescan can record it for variadic methods.
static size_t fixedParamCount(const std::vector<Token>& toks);

// Splice a type-parameter's captured argument tokens into a monomorphization output stream, at the
// point where the template's raw tokens named the type parameter. Ordinarily this is a straight
// copy of `argToks` — but when the argument is ITSELF a reference/borrow (its own token span ends
// in `&` or `*`, e.g. `T` bound to `Point&`) AND the template site immediately follows with its OWN
// `&`/`*` suffix (e.g. a return type written `-> T*`), splicing the full span would leave two sigil
// tokens back to back (`Point & *`) — which the type parser can't decode at all (not even a clean
// semantic error: a raw "expected '{' before method body"-style parse failure, since `consumeType`
// only ever consumes ONE trailing sigil).
//
// The fix: the author's own sigil at the template site is authoritative and always wins — drop the
// argument's trailing sigil so only the template's survives. This is a strip, not a merge: the
// author of `-> T*` always gets a borrow, never a silently-widened owning reference, and the
// resulting concrete type is then validated by the ordinary cast rules once the substituted
// declaration is re-parsed and analyzed (e.g. `return first;` from an owning `Point&` field into a
// `Point*` return type is the existing Silent "owning -> borrow" coercion; a value-object field
// into `T&` is still the existing "cannot bind a value as an owning reference" error — nothing new
// is invented here, the doubled-sigil case just now reaches those checks instead of dying in the
// parser). Never fires when the argument carries no sigil of its own (the overwhelmingly common
// case — `T` bound to a plain value or primitive) or when the template site has no sigil following.
static void spliceTypeArg(const std::vector<Token>& argToks, const std::vector<Token>& siteToks,
                          size_t nextIdx, std::vector<Token>& out) {
    bool argIsSigiled = !argToks.empty()
        && (argToks.back().type == TokenType::AMPERSAND || argToks.back().type == TokenType::STAR);
    bool siteHasSigil = nextIdx < siteToks.size()
        && (siteToks[nextIdx].type == TokenType::AMPERSAND || siteToks[nextIdx].type == TokenType::STAR);
    size_t n = (argIsSigiled && siteHasSigil) ? argToks.size() - 1 : argToks.size();
    for (size_t i = 0; i < n; ++i) out.push_back(argToks[i]);
}


void Parser::prescanTemplateNames(const std::vector<Token>& toks) {
    // Register class names defined in this token stream. A class followed by '<'
    // is a generic template (its name goes to the generic class-name set instead).
    for (size_t i = 0; i + 1 < toks.size(); ++i) {
        if (toks[i].type == TokenType::CLASS && toks[i + 1].type == TokenType::IDENTIFIER) {
            if (i + 2 < toks.size() && toks[i + 2].type == TokenType::LESS)
                gen_->classNames.insert(toks[i + 1].lexeme);
            else
                classNames.insert(toks[i + 1].lexeme);
        }
        // Enum names are also type names (no generic enums for now).
        if (toks[i].type == TokenType::ENUM && toks[i + 1].type == TokenType::IDENTIFIER)
            classNames.insert(toks[i + 1].lexeme);
    }
    // Register generic-template NAMES (regardless of declaration order) with brace awareness, so a
    // `fn name<…>(` is classified by WHERE it appears: at top level it's a generic FREE function
    // (`funcNames`); inside a `class`/`enum` body it's a generic METHOD of that class
    // (`genericMethodKeys`/`genericMethodNames`, keyed by the — possibly generic — class name); inside
    // an `impl`/`trait`/other brace it's neither (capture handles/rejects it). `classBody` stacks the
    // enclosing class/enum name per brace level ("" for a non-class brace: fn body, impl, trait, block).
    std::vector<std::string> classBody;
    std::string pendingClass;   // class/enum name seen, attached to the next '{'
    for (size_t i = 0; i < toks.size(); ++i) {
        TokenType tt = toks[i].type;
        if ((tt == TokenType::CLASS || tt == TokenType::ENUM)
            && i + 1 < toks.size() && toks[i + 1].type == TokenType::IDENTIFIER) {
            pendingClass = toks[i + 1].lexeme;   // `class Foo` / `class Foo<T>` / `enum E`
        } else if (tt == TokenType::LEFT_BRACE) {
            classBody.push_back(pendingClass); pendingClass.clear();
        } else if (tt == TokenType::RIGHT_BRACE) {
            if (!classBody.empty()) classBody.pop_back();
        } else if (tt == TokenType::FN) {
            // Skip any `static`/`private` modifiers (in either order) between `fn` and the name, so a
            // `fn static m<…>(` / `fn private m<…>(` generic/variadic method is registered too.
            size_t nameIdx = i + 1;
            while (nameIdx < toks.size()
                   && (toks[nameIdx].type == TokenType::STATIC || toks[nameIdx].type == TokenType::PRIVATE))
                ++nameIdx;
            if (!(nameIdx + 1 < toks.size()
                  && toks[nameIdx].type == TokenType::IDENTIFIER
                  && toks[nameIdx + 1].type == TokenType::LESS))
                continue;
            size_t j = nameIdx + 2; int depth = 1;
            while (j < toks.size() && depth > 0) {
                if (toks[j].type == TokenType::LESS)             depth++;
                else if (toks[j].type == TokenType::GREATER)     depth--;
                else if (toks[j].type == TokenType::SHIFT_RIGHT) depth -= 2;
                j++;
            }
            if (j < toks.size() && toks[j].type == TokenType::LEFT_PAREN) {
                const std::string& mname = toks[nameIdx].lexeme;
                // A `...` anywhere in the `<…>` list marks this a VARIADIC template.
                bool variadic = false;
                for (size_t p = nameIdx + 2; p + 1 < j; ++p)
                    if (toks[p].type == TokenType::ELLIPSIS) { variadic = true; break; }
                if (classBody.empty()) {
                    gen_->funcNames.insert(mname);                        // top-level generic function
                } else if (!classBody.back().empty()) {
                    const std::string key = classBody.back() + "::" + mname;
                    gen_->genericMethodKeys.insert(key);                  // generic method
                    gen_->genericMethodNames.insert(mname);
                    if (variadic) {   // a variadic method — called without explicit `<…>`
                        gen_->variadicMethodKeys.insert(key);
                        gen_->variadicMethodNames.insert(mname);
                        // Count fixed params from `( … )` starting at `j` (the pack is the last param).
                        std::vector<Token> sig(toks.begin() + j, toks.end());
                        gen_->variadicMethodFixedCount[key] = fixedParamCount(sig);
                    }
                }
                // else: inside an impl/trait/other brace → neither (v1 rejects it at capture).
            }
        }
    }
}

void Parser::monomorphize(Program& program, const std::string& filenameStr) {
    filename = filenameStr;   // label any monomorphization parse error with the source file
    runMonomorphization(program);
}

// ============================================================
// Generics (monomorphization)
// ============================================================

bool Parser::tryCaptureFunctionTemplate() {
    // Unified generic function: `fn name < params > ( args ) [mut] [-> RetType [alias]] { body }`.
    // The name follows `fn` directly and precedes the '<' type-parameter list.
    if (peek().type != TokenType::FN) return false;
    size_t s = current + 1;   // first token after `fn`
    if (s + 1 >= tokens.size()
        || tokens[s].type != TokenType::IDENTIFIER
        || tokens[s + 1].type != TokenType::LESS)
        return false;
    size_t nameIdx = s;

    // Collect type-parameter names (and optional `: Trait + Trait` bounds) between
    // '<' and '>'; verify the decl continues with '('.
    std::vector<std::string>              typeParams;
    std::vector<std::vector<std::string>> bounds;
    std::vector<bool>                     isPack;
    size_t j = scanTypeParamList(s + 2, typeParams, bounds, isPack);
    if (j == 0 || tokens[j].type != TokenType::GREATER) return false;
    size_t afterGt = j + 1;
    if (afterGt >= tokens.size() || tokens[afterGt].type != TokenType::LEFT_PAREN) return false;
    if (typeParams.empty()) return false;
    // v1: at most one variadic pack, and it must be the last type parameter.
    for (size_t p = 0; p < isPack.size(); ++p)
        if (isPack[p] && p + 1 != isPack.size())
            throw error(tokens[nameIdx], "a variadic pack '...' must be the last type parameter of '"
                        + tokens[nameIdx].lexeme + "'");

    // Capture with the `<...>` list stripped so the monomorphized re-parse sees an ordinary
    // declaration: `fn`, the name, the parameter list, then everything up to the body
    // (`[mut] [-> RetType [alias]]`, whose return type may reference a type parameter).
    std::vector<Token> captured;
    captured.push_back(tokens[current]);   // `fn`
    captured.push_back(tokens[nameIdx]);   // function name

    size_t k = afterGt;
    int parenDepth = 0;
    do {
        if (tokens[k].type == TokenType::LEFT_PAREN)  parenDepth++;
        else if (tokens[k].type == TokenType::RIGHT_PAREN) parenDepth--;
        captured.push_back(tokens[k]);
        ++k;
    } while (k < tokens.size() && parenDepth > 0);

    // Capture any `mut` and/or `-> RetType alias` between the ')' and the body.
    while (k < tokens.size() && tokens[k].type != TokenType::LEFT_BRACE) {
        captured.push_back(tokens[k]);
        ++k;
    }

    if (k >= tokens.size() || tokens[k].type != TokenType::LEFT_BRACE) return false;
    int braceDepth = 0;
    do {
        if (tokens[k].type == TokenType::LEFT_BRACE)  braceDepth++;
        else if (tokens[k].type == TokenType::RIGHT_BRACE) braceDepth--;
        captured.push_back(tokens[k]);
        ++k;
    } while (k < tokens.size() && braceDepth > 0);

    const std::string& name = tokens[nameIdx].lexeme;
    gen_->templates[name] = GenericTemplate{ std::move(typeParams), std::move(bounds),
                                             std::move(isPack), std::move(captured) };
    gen_->funcNames.insert(name);
    current = k;   // advance past the captured declaration
    return true;
}

std::optional<Token> Parser::readCallSigType(size_t& k) const {
    if (k >= tokens.size()) return std::nullopt;
    const Token& base = tokens[k];
    bool isType = false;
    switch (base.type) {
        case TokenType::I8:  case TokenType::I16: case TokenType::I32: case TokenType::I64:
        case TokenType::U8:  case TokenType::U16: case TokenType::U32: case TokenType::U64:
        case TokenType::F32: case TokenType::F64: case TokenType::BOOL:
        case TokenType::CHAR_TYPE: case TokenType::VOID: case TokenType::PTR: case TokenType::SELF:
            isType = true; break;
        case TokenType::IDENTIFIER:
            isType = classNames.count(base.lexeme) > 0 || gen_->classNames.count(base.lexeme) > 0;
            break;
        default: break;
    }
    if (!isType) return std::nullopt;
    size_t j = k + 1;
    if (j < tokens.size() && tokens[j].type == TokenType::AMPERSAND) {  // `T&` reference
        k = j + 1;
        return Token{ TokenType::IDENTIFIER, base.lexeme + "&", base.line };
    }
    k = j;
    return base;   // copy-construct (Token is copyable)
}

std::optional<std::string> Parser::scanCallBound(size_t& k) {
    k += 2;   // past `Call` `(`
    std::vector<Token> paramTypes;
    while (k < tokens.size() && tokens[k].type != TokenType::RIGHT_PAREN) {
        std::optional<Token> t = readCallSigType(k);
        if (!t) return std::nullopt;
        paramTypes.push_back(*t);
        if (k < tokens.size() && tokens[k].type == TokenType::COMMA) ++k;
    }
    if (k >= tokens.size() || tokens[k].type != TokenType::RIGHT_PAREN) return std::nullopt;
    int line = tokens[k].line;
    ++k;   // past `)`
    if (k < tokens.size() && tokens[k].type == TokenType::ARROW) {
        ++k;
        std::optional<Token> r = readCallSigType(k);
        if (!r) return std::nullopt;
        return canonicalCallTrait(paramTypes, *r);
    }
    return canonicalCallTrait(paramTypes, Token{ TokenType::VOID, "void", line });
}

// Resolve a Call-signature type token to a Type for mangling: a synthesized "Class&"/"ptr<…>",
// a primitive/void keyword, or a class-name identifier. Mirrors the semantic/codegen resolvers.
static Type callSigTokenType(const Token& t) {
    Type d = decodeSynthesizedType(t);            // "Class&", "ptr<Elem>"
    if (d.kind != TypeKind::Error) return d;
    Type p = typeFromToken(t.type);               // primitives / void / opaque ptr
    if (p.kind != TypeKind::Error) return p;
    if (t.type == TokenType::IDENTIFIER || t.type == TokenType::SELF)
        return makeObjectType(t.lexeme);          // class (or already-mangled) name
    return Type{TypeKind::Error};
}

std::string Parser::canonicalCallTrait(const std::vector<Token>& paramTypeTokens, const Token& retType) {
    std::vector<Type> params;
    params.reserve(paramTypeTokens.size());
    for (const Token& t : paramTypeTokens) params.push_back(callSigTokenType(t));
    Type ret = callSigTokenType(retType);
    std::string name = mangleOverload("Call", params, ret);   // Call$P…$ret$R
    // Remember the signature's tokens so an untyped lambda argument can infer from this bound.
    gen_->callTraitSigs.emplace(name, std::make_pair(paramTypeTokens, retType));
    if (gen_->emittedCallTraits.insert(name).second) {
        // Generate `trait <name> { fn call(P a0, …) -> R; }` (bodyless required method).
        std::vector<ParamDecl> callParams;
        for (size_t i = 0; i < paramTypeTokens.size(); ++i)
            callParams.push_back(ParamDecl{ paramTypeTokens[i],
                Token{ TokenType::IDENTIFIER, "a" + std::to_string(i), retType.line }, false, false, nullptr });
        std::deque<MethodDecl> methods;
        methods.push_back(MethodDecl{
            /*isPublic=*/true, /*isConstructor=*/false, /*isDestructor=*/false, /*isStatic=*/false,
            /*isMut=*/false, /*hasBody=*/false, retType,
            Token{ TokenType::IDENTIFIER, "call", retType.line },
            std::move(callParams), BlockStmt{}, /*hasReturnSlot=*/false, "" });
        pendingCallTraits_.push_back(makeStmt(TraitDeclStmt{
            Token{ TokenType::IDENTIFIER, name, retType.line }, std::move(methods) }));
    }
    return name;
}

// Scan a `<T, U: Trait + Trait2, ...>` type-parameter list starting at token index
// `from` (the first token after '<'). Fills `typeParams` and a parallel `bounds`
// (bounds[i] = trait names on typeParams[i], empty if unbounded). Returns the index
// of the closing '>' token, or 0 if the list is malformed / unterminated.
// A `Call(P…)->R` bound is canonicalized to a mangled `Call$…` trait name.
size_t Parser::scanTypeParamList(size_t from, std::vector<std::string>& typeParams,
                                 std::vector<std::vector<std::string>>& bounds,
                                 std::vector<bool>& isPack) {
    bool   expectParam = true;
    bool   nextIsPack  = false;   // a `...` just before the next param name marks it a variadic pack
    int    depth = 1;
    size_t j = from;
    for (; j < tokens.size() && depth > 0; ++j) {
        TokenType tt = tokens[j].type;
        if (tt == TokenType::LESS)             { depth++; continue; }
        if (tt == TokenType::GREATER)          { depth--; if (depth == 0) break; continue; }
        if (tt == TokenType::SHIFT_RIGHT)      { depth -= 2; if (depth <= 0) return 0; continue; }
        if (depth != 1) continue;
        if (tt == TokenType::COMMA)            { expectParam = true; }
        else if (tt == TokenType::ELLIPSIS)    { if (expectParam) nextIsPack = true; }
        else if (tt == TokenType::IDENTIFIER) {
            if (expectParam) { typeParams.push_back(tokens[j].lexeme); bounds.push_back({});
                               isPack.push_back(nextIsPack); nextIsPack = false; expectParam = false; }
            // `Call(P…)->R` bound → canonical `Call$…` trait name.
            else if (tokens[j].lexeme == "Call" && !bounds.empty()
                     && j + 1 < tokens.size() && tokens[j + 1].type == TokenType::LEFT_PAREN) {
                size_t k = j;
                std::optional<std::string> canon = scanCallBound(k);
                if (!canon) return 0;
                bounds.back().push_back(*canon);
                j = k - 1;   // the loop's ++j resumes after the signature
            }
            else if (!bounds.empty()) bounds.back().push_back(tokens[j].lexeme);
        }
        // COLON / PLUS are separators inside a bound list — ignored.
    }
    if (j >= tokens.size() || tokens[j].type != TokenType::GREATER) return 0;
    return j;
}

bool Parser::tryCaptureClassTemplate() {
    if (!(peek().type == TokenType::CLASS
          && peekNext().type == TokenType::IDENTIFIER
          && current + 2 < tokens.size() && tokens[current + 2].type == TokenType::LESS))
        return false;

    std::vector<std::string>              typeParams;
    std::vector<std::vector<std::string>> bounds;
    std::vector<bool>                     isPack;   // (variadic packs on classes not supported in v1)
    size_t j = scanTypeParamList(current + 3, typeParams, bounds, isPack);
    if (j == 0 || tokens[j].type != TokenType::GREATER) return false;
    size_t afterGt = j + 1;
    if (afterGt >= tokens.size() || tokens[afterGt].type != TokenType::LEFT_BRACE) return false;
    if (typeParams.empty()) return false;

    std::vector<Token> captured;
    captured.push_back(tokens[current]);       // 'class'
    captured.push_back(tokens[current + 1]);   // class name

    size_t k = afterGt;
    int braceDepth = 0;
    do {
        if (tokens[k].type == TokenType::LEFT_BRACE)  braceDepth++;
        else if (tokens[k].type == TokenType::RIGHT_BRACE) braceDepth--;
        captured.push_back(tokens[k]);
        ++k;
    } while (k < tokens.size() && braceDepth > 0);

    const std::string& name = tokens[current + 1].lexeme;
    gen_->templates[name] = GenericTemplate{ std::move(typeParams), std::move(bounds),
                                             std::move(isPack), std::move(captured) };
    gen_->classNames.insert(name);
    current = k;
    return true;
}

bool Parser::tryCaptureImplTemplate() {
    // Generic impl: `impl<T…> Trait for Class<T…> { … }`.  Only fires on `impl <` — a plain
    // `impl Trait for Type { … }` still goes through parseImplDecl unchanged.
    if (!(peek().type == TokenType::IMPL
          && current + 1 < tokens.size() && tokens[current + 1].type == TokenType::LESS))
        return false;

    std::vector<std::string>              typeParams;
    std::vector<std::vector<std::string>> bounds;
    std::vector<bool>                     isPack;   // (variadic packs on impls not supported in v1)
    size_t j = scanTypeParamList(current + 2, typeParams, bounds, isPack);   // j = closing '>'
    if (j == 0 || tokens[j].type != TokenType::GREATER || typeParams.empty()) return false;

    auto isTypeParam = [&](const std::string& s) {
        for (const auto& p : typeParams) if (p == s) return true;
        return false;
    };
    auto at = [&](size_t idx) -> const Token& {
        return tokens[idx < tokens.size() ? idx : tokens.size() - 1];
    };

    // Header: `<Trait> for <Class> < args >`.
    size_t p = j + 1;
    if (at(p).type != TokenType::IDENTIFIER)
        throw error(at(p), "expected a trait name after 'impl<…>'");
    if (at(p + 1).type != TokenType::FOR)
        throw error(at(p + 1), "expected 'for' in 'impl<…> <Trait> for <Type>'");
    size_t classIdx = p + 2;
    if (at(classIdx).type != TokenType::IDENTIFIER)
        throw error(at(classIdx), "expected a class name after 'for'");
    std::string targetClass = tokens[classIdx].lexeme;
    if (at(classIdx + 1).type != TokenType::LESS)
        throw error(at(classIdx + 1), "a generic impl must target a generic type: write "
            "`impl<T> " + tokens[p].lexeme + " for " + targetClass + "<T>`");

    // Target type arguments — each must be exactly one of the impl's own type parameters.
    std::vector<std::string> targetParamAtPos;
    size_t q = classIdx + 2;
    while (q < tokens.size() && tokens[q].type != TokenType::GREATER) {
        if (tokens[q].type == TokenType::COMMA) { ++q; continue; }
        bool ok = tokens[q].type == TokenType::IDENTIFIER && isTypeParam(tokens[q].lexeme)
               && (tokens[q + 1 < tokens.size() ? q + 1 : q].type == TokenType::COMMA
                   || tokens[q + 1 < tokens.size() ? q + 1 : q].type == TokenType::GREATER);
        if (!ok)
            throw error(at(q), "a generic impl target must name each type parameter directly, e.g. "
                "`impl<T> " + tokens[p].lexeme + " for " + targetClass
                + "<T>` — nested or concrete type arguments are not supported here");
        targetParamAtPos.push_back(tokens[q].lexeme);
        ++q;
    }
    if (q >= tokens.size() || tokens[q].type != TokenType::GREATER)
        throw error(at(q), "expected '>' to close the impl target type");
    size_t braceIdx = q + 1;
    if (at(braceIdx).type != TokenType::LEFT_BRACE)
        throw error(at(braceIdx), "expected '{' after the impl header");

    // Capture `impl <Trait> for <Class<…>> { … }` WITHOUT the `impl<…>` header, so a
    // re-parse after substitution is an ordinary impl.
    std::vector<Token> captured;
    captured.push_back(tokens[current]);                     // 'impl'
    for (size_t idx = p; idx <= q; ++idx) captured.push_back(tokens[idx]);   // Trait for Class < … >

    size_t k = braceIdx;
    int braceDepth = 0;
    do {
        if (tokens[k].type == TokenType::LEFT_BRACE)       braceDepth++;
        else if (tokens[k].type == TokenType::RIGHT_BRACE) braceDepth--;
        captured.push_back(tokens[k]);
        ++k;
    } while (k < tokens.size() && braceDepth > 0);

    gen_->implTemplates.push_back(GenericImplTemplate{
        std::move(typeParams), std::move(bounds), std::move(targetClass),
        std::move(targetParamAtPos), std::move(captured), tokens[p].lexeme });
    current = k;   // past the closing '}'
    return true;
}

size_t Parser::typeSpanAt(size_t from) const {
    if (from >= tokens.size()) return 0;
    const Token& t = tokens[from];

    // Tuple type: '(' type (',' type)+ ')' — at least two element type spans. Each element is
    // validated by a recursive typeSpanAt, so a grouped expression `(a + b)` or a tuple *literal*
    // `(a, b)` (elements are values, not types) yields 0 and is not read as a type.
    if (t.type == TokenType::LEFT_PAREN) {
        size_t i = from + 1;
        size_t count = 0;
        for (;;) {
            size_t inner = typeSpanAt(i);
            if (inner == 0) return 0;
            i += inner;
            ++count;
            if (i < tokens.size() && tokens[i].type == TokenType::COMMA) { ++i; continue; }
            break;
        }
        if (count < 2) return 0;                                        // '(T)' is grouping
        if (i >= tokens.size() || tokens[i].type != TokenType::RIGHT_PAREN) return 0;
        return (i + 1) - from;                                          // include ')'
    }

    bool isType = isTypeKeyword(t.type)
               || (t.type == TokenType::IDENTIFIER
                   && (classNames.count(t.lexeme) || gen_->classNames.count(t.lexeme)));
    if (!isType) return 0;

    size_t i = from + 1;
    // generic argument list: Name<...>  or  typed pointer: ptr<...>
    if (((t.type == TokenType::IDENTIFIER && gen_->classNames.count(t.lexeme))
         || t.type == TokenType::PTR)
        && i < tokens.size() && tokens[i].type == TokenType::LESS) {
        int depth = 1; ++i;
        while (i < tokens.size() && depth > 0) {
            if (tokens[i].type == TokenType::LESS)             depth++;
            else if (tokens[i].type == TokenType::GREATER)     depth--;
            else if (tokens[i].type == TokenType::SHIFT_RIGHT) depth -= 2;
            ++i;
        }
    }
    // reference suffix: `&` (owning heap reference) or `*` (non-owning borrow)
    bool hadSigil = false;
    if (i < tokens.size()
        && (tokens[i].type == TokenType::AMPERSAND || tokens[i].type == TokenType::STAR)) {
        ++i;
        hadSigil = true;
    }
    // nullable suffix: `T?`
    bool hadQuestion = false;
    if (i < tokens.size() && tokens[i].type == TokenType::QUESTION) {
        ++i;
        hadQuestion = true;
    }
    // A sigil written AFTER '?' (`T?&`/`T?*`) is the wrong order (the sigil must come first —
    // `T&?`/`T*?`) and `consumeType()` raises a precise error for it, but that error is only
    // reached if this span still includes the sigil — otherwise the declaration-detection branch
    // in `parseExpression` never recognizes this as a type at all, and the trailing `?` gets
    // misparsed as the start of a ternary (`Node ? ...`), surfacing a confusing, unrelated
    // "expected expression" instead of the real diagnostic.
    if (!hadSigil && hadQuestion && i < tokens.size()
        && (tokens[i].type == TokenType::AMPERSAND || tokens[i].type == TokenType::STAR)) ++i;
    return i - from;
}

std::vector<std::vector<Token>> Parser::parseTypeArgList() {
    consume(TokenType::LESS, "expected '<'");
    std::vector<std::vector<Token>> args;
    if (!check(TokenType::GREATER) && !check(TokenType::SHIFT_RIGHT)) {
        do { args.push_back(parseOneTypeArg()); } while (match({ TokenType::COMMA }));
    }
    consumeCloseAngle();
    return args;
}

// Parse a single type argument. A nested generic instantiation (Name<...>) is
// collapsed into one mangled token (and its instantiation recorded); a trailing
// '&' is kept as a separate token so argMangle can render it as ".ref".
std::vector<Token> Parser::parseOneTypeArg() {
    std::vector<Token> cur;
    Token base = advance();
    if (base.type == TokenType::IDENTIFIER && gen_->classNames.count(base.lexeme)
        && check(TokenType::LESS)) {
        std::vector<std::vector<Token>> nested = parseTypeArgList();
        std::string mangled = mangleInstantiation(base.lexeme, nested);
        recordInstantiation(base.lexeme, mangled, std::move(nested));
        classNames.insert(mangled);
        cur.push_back(Token{ TokenType::IDENTIFIER, mangled, base.line });
    } else {
        cur.push_back(base);
    }
    // A trailing '&' belongs to THIS argument only when no outer close-angles are
    // pending: a '>>' that closed this arg also closes an enclosing level, so any
    // '&' following it applies to the outer type, not this argument.
    if (pendingCloseAngles_ == 0 && check(TokenType::AMPERSAND))
        cur.push_back(advance());
    return cur;
}

// Consume one closing '>'. A '>>' (SHIFT_RIGHT) closes two levels: consume it once
// and leave a virtual '>' pending for the enclosing list (the classic C++ fix).
void Parser::consumeCloseAngle() {
    if (pendingCloseAngles_ > 0) { --pendingCloseAngles_; return; }
    if (check(TokenType::GREATER))     { advance(); return; }
    if (check(TokenType::SHIFT_RIGHT)) { advance(); pendingCloseAngles_ = 1; return; }
    throw error(peek(), "expected '>' to close type arguments");
}

std::string Parser::mangleInstantiation(const std::string& base,
                                        const std::vector<std::vector<Token>>& args) const {
    std::string m = base;
    for (const auto& a : args) m += "$" + argMangle(a);
    return m;
}

void Parser::recordInstantiation(const std::string& templateName, const std::string& mangled,
                                 std::vector<std::vector<Token>> args) {
    if (gen_->instantiated.count(mangled)) return;
    gen_->instantiated.insert(mangled);
    gen_->worklist.push_back(GenericInstantiation{ templateName, mangled, std::move(args),
                                                   /*ownerClass=*/"", /*bareMethodName=*/"" });
}

// ---- Generic methods ----

// Capture a generic method `fn [static] [private] NAME<T…>(params) [mut] [-> Ret alias] { body }`
// as a template. Called from parseMemberList with `current` at NAME (the `fn` + modifiers already
// consumed, passed as bools). The captured tokens are a synthetic ORDINARY method decl with the
// `<…>` list stripped, so the per-call re-parse (via a shell class + parseMemberList) sees a normal
// method. Stored under `ownerClass::NAME`; the call site keys on genericMethodNames/Keys.
void Parser::captureMethodTemplate(const std::string& ownerClass, bool isStatic, bool isPublic) {
    size_t nameIdx = current;             // NAME; nameIdx+1 == '<'
    Token  nameTok = tokens[nameIdx];
    int    line    = nameTok.line;

    std::vector<std::string>              typeParams;
    std::vector<std::vector<std::string>> bounds;
    std::vector<bool>                     isPack;
    size_t j = scanTypeParamList(nameIdx + 2, typeParams, bounds, isPack);   // returns closing '>'
    // v1: a variadic method (a pack) may have at most one pack, and it must be the last type param.
    for (size_t p = 0; p < isPack.size(); ++p)
        if (isPack[p] && p + 1 != isPack.size())
            throw error(nameTok, "a variadic pack '...' must be the last type parameter of method '"
                        + nameTok.lexeme + "'");
    size_t afterGt = j + 1;               // the '(' (caller verified this)

    std::vector<Token> captured;
    captured.push_back(Token{ TokenType::FN, "fn", line });
    if (isStatic)  captured.push_back(Token{ TokenType::STATIC,  "static",  line });
    if (!isPublic) captured.push_back(Token{ TokenType::PRIVATE, "private", line });
    captured.push_back(nameTok);          // method name (renamed to the mangled name at instantiation)

    size_t k = afterGt;
    int parenDepth = 0;
    do {
        if (tokens[k].type == TokenType::LEFT_PAREN)  parenDepth++;
        else if (tokens[k].type == TokenType::RIGHT_PAREN) parenDepth--;
        captured.push_back(tokens[k]); ++k;
    } while (k < tokens.size() && parenDepth > 0);

    // `[mut] [-> RetType alias]` up to the body.
    while (k < tokens.size() && tokens[k].type != TokenType::LEFT_BRACE) { captured.push_back(tokens[k]); ++k; }
    if (k < tokens.size() && tokens[k].type == TokenType::LEFT_BRACE) {
        int braceDepth = 0;
        do {
            if (tokens[k].type == TokenType::LEFT_BRACE)  braceDepth++;
            else if (tokens[k].type == TokenType::RIGHT_BRACE) braceDepth--;
            captured.push_back(tokens[k]); ++k;
        } while (k < tokens.size() && braceDepth > 0);
    }

    std::string key = ownerClass + "::" + nameTok.lexeme;
    // A pack (variadic) method is called WITHOUT explicit `<…>`, so its call site keys on the variadic
    // sets, not the generic ones. Register them here too (a backstop for the prescan, which may miss a
    // `fn static/private m<…>` when the class is defined after a same-file call site).
    bool isVariadic = !isPack.empty() && isPack.back();
    gen_->methodTemplates[key] = GenericTemplate{ std::move(typeParams), std::move(bounds),
                                                  std::move(isPack), captured };
    gen_->genericMethodNames.insert(nameTok.lexeme);
    gen_->genericMethodKeys.insert(key);
    if (isVariadic) {
        gen_->variadicMethodNames.insert(nameTok.lexeme);
        gen_->variadicMethodKeys.insert(key);
        gen_->variadicMethodFixedCount[key] = fixedParamCount(captured);   // pack is the last param
    }
    current = k;                          // advance past the captured method
}

void Parser::recordMethodInstantiation(const std::string& ownerClass, const std::string& bareMethodName,
                                       const std::string& mangled, std::vector<std::vector<Token>> args) {
    std::string dedupKey = ownerClass + "::" + mangled;   // e.g. "Box::wrap$i32" (never a class/fn name)
    if (gen_->instantiated.count(dedupKey)) return;
    gen_->instantiated.insert(dedupKey);
    GenericInstantiation inst;
    inst.templateName   = ownerClass + "::" + bareMethodName;   // the methodTemplates key
    inst.mangledName    = mangled;
    inst.args           = std::move(args);
    inst.ownerClass     = ownerClass;
    inst.bareMethodName = bareMethodName;
    gen_->worklist.push_back(std::move(inst));
}

bool Parser::instantiateMethod(const GenericInstantiation& inst, Program& program) {
    auto tmplIt = gen_->methodTemplates.find(inst.templateName);   // "Owner::method"
    if (tmplIt == gen_->methodTemplates.end()) return false;       // template not registered yet → defer
    // Find the owner class declaration to inject into (a generic class's `Array$i32` exists only after
    // its body is re-parsed — defer until then).
    ClassDeclStmt* owner = nullptr;
    for (Stmt& s : program.declarations)
        if (auto* cd = std::get_if<ClassDeclStmt>(s.node.get()))
            if (cd->name.lexeme == inst.ownerClass) { owner = cd; break; }
    if (!owner) return false;

    const GenericTemplate& tmpl = tmplIt->second;
    std::unordered_map<std::string, std::vector<Token>> sub;
    for (size_t i = 0; i < tmpl.typeParams.size() && i < inst.args.size(); ++i)
        sub.emplace(tmpl.typeParams[i], inst.args[i]);

    // Bound obligations, reusing the same machinery as free-fn/class templates.
    for (size_t i = 0; i < tmpl.typeParams.size() && i < inst.args.size(); ++i) {
        if (i >= tmpl.bounds.size() || tmpl.bounds[i].empty() || inst.args[i].empty()) continue;
        const Token& argHead = inst.args[i].front();
        for (const std::string& tr : tmpl.bounds[i])
            program.genericBoundChecks.push_back(
                GenericBoundCheck{ argHead.lexeme, tr, inst.ownerClass + "_" + inst.mangledName,
                                   argHead.line });
    }

    // A variadic method's pack type-param materializes as a tuple: `Ts... args` → `Tuple$…* args`.
    const std::string packName = (!tmpl.isPack.empty() && tmpl.isPack.back())
                                ? tmpl.typeParams.back() : std::string{};

    // Substitute type-param tokens + rename the method-name token (the FIRST IDENTIFIER — the
    // preceding `fn`/`static`/`private` are keyword tokens, not IDENTIFIER) to the mangled name.
    // Recursive calls inside the body keep the bare name (re-deduced / re-instantiated per call).
    std::string packValueName;   // the pack VALUE param name (`args` in `Ts... args`)
    std::vector<Token> out;
    bool renamed = false;
    for (size_t idx = 0; idx < tmpl.tokens.size(); ++idx) {
        const Token& t = tmpl.tokens[idx];
        if (!renamed && t.type == TokenType::IDENTIFIER) {
            out.push_back(Token{ TokenType::IDENTIFIER, inst.mangledName, t.line });
            renamed = true;
            continue;
        }
        // `PackName ...` → the pack's tuple type + `*` borrow (drop the ELLIPSIS).
        if (!packName.empty() && t.type == TokenType::IDENTIFIER && t.lexeme == packName
            && idx + 1 < tmpl.tokens.size() && tmpl.tokens[idx + 1].type == TokenType::ELLIPSIS) {
            auto sit = sub.find(packName);
            if (sit != sub.end()) for (const Token& a : sit->second) out.push_back(a);
            out.push_back(Token{ TokenType::STAR, "*", t.line });
            if (idx + 2 < tmpl.tokens.size()) packValueName = tmpl.tokens[idx + 2].lexeme;
            ++idx;   // skip the ELLIPSIS (the value-param name is emitted next iteration)
            continue;
        }
        if (t.type == TokenType::IDENTIFIER) {
            auto sit = sub.find(t.lexeme);
            if (sit != sub.end()) { spliceTypeArg(sit->second, tmpl.tokens, idx + 1, out); continue; }
        }
        out.push_back(t);
    }

    // Expand any compile-time cons-`match` over the pack now that the arity is known (a no-op for a
    // spread-only body like `data[i] = T(args...)`).
    if (!packName.empty() && !packValueName.empty()) {
        auto sit = sub.find(packName);
        if (sit != sub.end() && !sit->second.empty()) {
            auto trIt = gen_->tupleRequests.find(sit->second.front().lexeme);
            std::vector<Token> elems;
            if (trIt != gen_->tupleRequests.end()) for (const Token& e : trIt->second) elems.push_back(e);
            expandPackMatch(out, packValueName, elems);
        }
    }

    // Shell re-parse: wrap the concrete method tokens in a throwaway class NAMED THE REAL OWNER (so a
    // `this.m<U>()` recursion inside the body resolves `this` to the right class), then transplant the
    // single MethodDecl into the real owner. The shell ClassDeclStmt is discarded.
    std::vector<Token> shell;
    shell.push_back(Token{ TokenType::CLASS, "class", 0 });
    shell.push_back(Token{ TokenType::IDENTIFIER, inst.ownerClass, 0 });
    shell.push_back(Token{ TokenType::LEFT_BRACE, "{", 0 });
    for (const Token& t : out) shell.push_back(t);
    shell.push_back(Token{ TokenType::RIGHT_BRACE, "}", 0 });
    shell.push_back(Token{ TokenType::END_OF_FILE, "", 0 });

    tokens  = std::move(shell);
    current = 0;
    Stmt shellDecl = parseDeclaration();
    auto* shellClass = std::get_if<ClassDeclStmt>(shellDecl.node.get());
    if (!shellClass || shellClass->methods.empty()) return false;   // (shouldn't happen)
    owner->methods.push_back(std::move(shellClass->methods.front()));
    return true;
}

std::string Parser::genericArgBaseName(const Token& t) {
    std::string s = t.lexeme;
    if (!s.empty() && s.back() == '?') s.pop_back();          // nullable suffix
    if (s.rfind("ref:", 0) == 0) s = s.substr(4);             // `T*` borrow → "ref:T"
    else if (!s.empty() && s.back() == '&') s.pop_back();     // `T&` owning reference
    return s;
}

bool Parser::inferGenericTypeArgs(const std::string& fnName,
                                  const std::vector<std::unique_ptr<Expr>>& args,
                                  std::vector<std::vector<Token>>& out) {
    auto it = gen_->templates.find(fnName);
    if (it == gen_->templates.end() || it->second.typeParams.empty()) return false;
    const GenericTemplate& tmpl = it->second;
    std::unordered_set<std::string> tparams(tmpl.typeParams.begin(), tmpl.typeParams.end());

    // 1. Scan the template's parameter list (raw tokens; params are not pre-parsed) to find, for
    //    each parameter position, whether its type is a bare type parameter (`T` / `T&` / `T*` /
    //    `T?`). Record paramPos → type-param name for those.
    const std::vector<Token>& toks = tmpl.tokens;
    size_t lp = 0;
    while (lp < toks.size() && toks[lp].type != TokenType::LEFT_PAREN) ++lp;
    if (lp == toks.size()) return false;

    std::unordered_map<size_t, std::string> paramTypeParam;   // paramPos → type-param name
    size_t depth = 1, i = lp + 1, paramPos = 0, groupStart = i;
    auto analyzeGroup = [&](size_t s, size_t e) {
        if (s >= e) return;
        size_t j = s;
        if (toks[j].lexeme == "mut") ++j;                      // optional `mut`
        if (j >= e || toks[j].type != TokenType::IDENTIFIER) return;   // primitives are keywords, skip
        if (!tparams.count(toks[j].lexeme)) return;            // not a bare type parameter
        const std::string tp = toks[j].lexeme;
        ++j;
        while (j < e && (toks[j].type == TokenType::AMPERSAND || toks[j].type == TokenType::STAR
                         || toks[j].type == TokenType::QUESTION)) ++j;   // trailing sigils
        if (j < e && toks[j].type == TokenType::IDENTIFIER)    // then the param name → a match
            paramTypeParam.emplace(paramPos, tp);
    };
    for (; i < toks.size() && depth > 0; ++i) {
        TokenType tt = toks[i].type;
        if (tt == TokenType::LEFT_PAREN || tt == TokenType::LESS) ++depth;
        else if (tt == TokenType::GREATER) { if (depth > 1) --depth; }
        else if (tt == TokenType::RIGHT_PAREN) {
            if (--depth == 0) { analyzeGroup(groupStart, i); break; }
        } else if (tt == TokenType::COMMA && depth == 1) {
            analyzeGroup(groupStart, i);
            groupStart = i + 1;
            ++paramPos;
        }
    }

    // 2. For each type parameter (in order), read the type off its matched positional argument.
    for (const std::string& tp : tmpl.typeParams) {
        size_t pos = SIZE_MAX;
        for (const auto& [p, name] : paramTypeParam) if (name == tp) { pos = std::min(pos, p); }
        if (pos == SIZE_MAX || pos >= args.size() || !args[pos]) return false;
        const Expr* n = args[pos].get();
        std::string base;
        // A binding's recorded type token whose kind is a usable base name. A `var`-typed local
        // records the `var` SENTINEL (its real type is only deduced later, in semantics, after
        // monomorphization), so it is NOT inferable here — treat it as un-inferable so the call
        // gets the clean "specify explicitly" error rather than a bogus `f$var` instantiation.
        auto baseFrom = [](const Token& t) -> std::string {
            return t.type == TokenType::VAR ? std::string{} : genericArgBaseName(t);
        };
        if (const auto* id = std::get_if<IdentifierExpr>(n->node.get())) {
            const Token* found = nullptr;
            for (size_t k = scopes_.size(); k-- > 0 && !found; ) {   // innermost binding wins (shadowing)
                auto sit = scopes_[k].find(id->name.lexeme);
                if (sit != scopes_[k].end()) found = &sit->second;
            }
            if (!found) {                                            // else an enclosing instance field
                auto fit = classFieldScope_.find(id->name.lexeme);
                if (fit != classFieldScope_.end()) found = &fit->second;
            }
            if (found) base = baseFrom(*found);                      // empty for a `var` binding → un-inferable
        } else if (const auto* c = std::get_if<CallExpr>(n->node.get())) {
            if (classNames.count(c->callee.lexeme) || gen_->classNames.count(c->callee.lexeme))
                base = c->callee.lexeme;                       // a `Class(...)` constructor call
        } else if (const auto* ne = std::get_if<NewExpr>(n->node.get())) {
            base = ne->className.lexeme;                       // `new Class(...)`
        }
        if (base.empty()) return false;                        // un-inferable argument
        out.push_back(std::vector<Token>{ Token{ TokenType::IDENTIFIER, base, 0 } });
    }
    return true;
}

// ---- Variadic packs ----

// A primitive type name → its keyword token kind (so a tuple field is by-value); non-primitive → an
// IDENTIFIER (a class element takes a reference ctor param). Mirrors the lexer's type keywords.
static TokenType primTokenFor(const std::string& name) {
    static const std::unordered_map<std::string, TokenType> m = {
        {"i8",TokenType::I8},{"i16",TokenType::I16},{"i32",TokenType::I32},{"i64",TokenType::I64},
        {"u8",TokenType::U8},{"u16",TokenType::U16},{"u32",TokenType::U32},{"u64",TokenType::U64},
        {"f32",TokenType::F32},{"f64",TokenType::F64},{"bool",TokenType::BOOL},
        {"char",TokenType::CHAR_TYPE},{"str",TokenType::STR},
    };
    auto it = m.find(name);
    return it == m.end() ? TokenType::IDENTIFIER : it->second;
}

std::optional<Token> Parser::deduceArgTypeToken(const Expr* arg) const {
    if (!arg || !arg->node) return std::nullopt;
    const auto& node = *arg->node;

    if (const auto* lit = std::get_if<LiteralExpr>(&node)) {
        switch (lit->token.type) {
            case TokenType::NUMBER: {
                bool isFloat = lit->token.lexeme.find('.') != std::string::npos
                            || lit->token.lexeme.find('e') != std::string::npos
                            || lit->token.lexeme.find('E') != std::string::npos;
                return isFloat ? Token{ TokenType::F64, "f64", lit->token.line }
                               : Token{ TokenType::I32, "i32", lit->token.line };
            }
            case TokenType::STRING: return Token{ TokenType::STR,       "str",  lit->token.line };
            case TokenType::TRUE:
            case TokenType::FALSE:  return Token{ TokenType::BOOL,      "bool", lit->token.line };
            case TokenType::CHAR:   return Token{ TokenType::CHAR_TYPE, "char", lit->token.line };
            default:                return std::nullopt;
        }
    }

    std::string base;
    int         line = 0;
    if (const auto* id = std::get_if<IdentifierExpr>(&node)) {
        line = id->name.line;
        const Token* found = nullptr;
        for (size_t k = scopes_.size(); k-- > 0 && !found; ) {   // innermost binding wins (shadowing)
            auto sit = scopes_[k].find(id->name.lexeme);
            if (sit != scopes_[k].end()) found = &sit->second;
        }
        if (!found) {
            auto fit = classFieldScope_.find(id->name.lexeme);
            if (fit != classFieldScope_.end()) found = &fit->second;
        }
        if (found && found->type != TokenType::VAR) base = genericArgBaseName(*found);
    } else if (const auto* c = std::get_if<CallExpr>(&node)) {
        if (classNames.count(c->callee.lexeme) || gen_->classNames.count(c->callee.lexeme))
            { base = c->callee.lexeme; line = c->callee.line; }        // `Class(...)` constructor
    } else if (const auto* ne = std::get_if<NewExpr>(&node)) {
        base = ne->className.lexeme; line = ne->className.line;        // `new Class(...)`
    } else if (const auto* th = std::get_if<ThisExpr>(&node)) {
        if (!currentClassName_.empty()) { base = currentClassName_; line = th->keyword.line; }  // `this`
    }
    if (base.empty()) return std::nullopt;
    return Token{ primTokenFor(base), base, line };
}

std::string Parser::requestTupleType(const std::vector<Token>& elems) {
    std::string mangled = "Tuple";
    for (const Token& e : elems) mangled += "$" + e.lexeme;
    classNames.insert(mangled);
    gen_->classNames.insert(mangled);
    if (gen_->tupleRequests.find(mangled) == gen_->tupleRequests.end()) {
        std::vector<Token> copy;
        copy.reserve(elems.size());
        for (const Token& e : elems) copy.push_back(e);
        gen_->tupleRequests.emplace(mangled, std::move(copy));
    }
    return mangled;
}

std::optional<std::vector<std::vector<Token>>> Parser::deducePackTargs(
        size_t fixedCount, const std::string& diagName,
        std::vector<std::unique_ptr<Expr>>& args, const std::vector<bool>& spreads) {
    if (args.size() < fixedCount) return std::nullopt;   // too few args — let the normal path error

    // A trailing spread anywhere other than the SOLE pack argument is unsupported in v1.
    for (size_t i = 0; i < spreads.size(); ++i)
        if (spreads[i] && i != args.size() - 1)
            throw error(previous(), "'...' spread must be the last argument to '" + diagName + "'");

    // Spread form `f(fixed…, xs...)`: the pack is one spread of an existing pack tuple `xs` — pass it
    // through directly (no re-tupling). `xs` must be a tuple-typed in-scope binding. `args` stays as-is.
    bool spreadPack = args.size() == fixedCount + 1 && !spreads.empty()
                   && spreads.size() == args.size() && spreads.back();
    if (spreadPack) {
        std::optional<Token> t = deduceArgTypeToken(args.back().get());
        if (!t || t->lexeme.rfind("Tuple", 0) != 0)
            throw error(previous(), "'...' can only spread a variadic pack (a tuple) into '" + diagName + "'");
        classNames.insert(t->lexeme);
        gen_->classNames.insert(t->lexeme);
        std::vector<std::vector<Token>> targs;
        targs.push_back(std::vector<Token>{ Token{ TokenType::IDENTIFIER, t->lexeme, 0 } });
        return targs;
    }

    // Deduce the trailing pack element types → the pack tuple.
    std::vector<Token> elems;
    for (size_t i = fixedCount; i < args.size(); ++i) {
        std::optional<Token> t = deduceArgTypeToken(args[i].get());
        if (!t)
            throw error(previous(), "cannot infer the type of a variadic argument to '" + diagName
                        + "'; pack arguments must be literals, in-scope variables, or constructor calls");
        elems.push_back(*t);
    }
    std::string tupleName = requestTupleType(elems);

    // Rewrite args: fixed prefix + one tuple literal (BraceInitExpr) holding the pack args.
    std::vector<std::unique_ptr<Expr>> packArgs;
    for (size_t i = fixedCount; i < args.size(); ++i) packArgs.push_back(std::move(args[i]));
    std::vector<std::unique_ptr<Expr>> newArgs;
    for (size_t i = 0; i < fixedCount; ++i) newArgs.push_back(std::move(args[i]));
    newArgs.push_back(box(makeExpr(BraceInitExpr{ std::move(packArgs),
                                                  Token{ TokenType::LEFT_PAREN, "(", 0 } })));
    args = std::move(newArgs);

    std::vector<std::vector<Token>> targs;               // one type arg = the pack tuple
    targs.push_back(std::vector<Token>{ Token{ TokenType::IDENTIFIER, tupleName, 0 } });
    return targs;
}

// Count the fixed (non-pack) params in a variadic template's raw param list (the last param is the
// pack). Scans from the first `(` to its match, counting top-level commas.
static size_t fixedParamCount(const std::vector<Token>& toks) {
    size_t lp = 0;
    while (lp < toks.size() && toks[lp].type != TokenType::LEFT_PAREN) ++lp;
    if (lp == toks.size()) return 0;
    size_t depth = 1, paramCount = 0; bool anyToken = false;
    for (size_t i = lp + 1; i < toks.size() && depth > 0; ++i) {
        TokenType tt = toks[i].type;
        if (tt == TokenType::LEFT_PAREN || tt == TokenType::LESS) ++depth;
        else if (tt == TokenType::GREATER)    { if (depth > 1) --depth; }
        else if (tt == TokenType::RIGHT_PAREN) { --depth; }
        else if (tt == TokenType::COMMA && depth == 1) ++paramCount;
        else if (depth == 1) anyToken = true;
    }
    if (anyToken) ++paramCount;                          // the last / only param
    return paramCount == 0 ? 0 : paramCount - 1;         // the pack is the last param
}

std::optional<std::string> Parser::deduceVariadicInstantiation(
        const std::string& fnName, std::vector<std::unique_ptr<Expr>>& args,
        const std::vector<bool>& spreads) {
    auto it = gen_->templates.find(fnName);
    if (it == gen_->templates.end()) return std::nullopt;
    const GenericTemplate& tmpl = it->second;
    if (tmpl.isPack.empty() || !tmpl.isPack.back()) return std::nullopt;   // not variadic

    auto targs = deducePackTargs(fixedParamCount(tmpl.tokens), fnName, args, spreads);
    if (!targs) return std::nullopt;
    std::string mangled = mangleInstantiation(fnName, *targs);
    recordInstantiation(fnName, mangled, std::move(*targs));
    return mangled;
}

std::optional<std::string> Parser::deduceVariadicMethodInstantiation(
        const std::string& ownerClass, const std::string& methodName, size_t fixedCount,
        std::vector<std::unique_ptr<Expr>>& args, const std::vector<bool>& spreads) {
    auto targs = deducePackTargs(fixedCount, methodName, args, spreads);
    if (!targs) return std::nullopt;
    std::string mangled = mangleInstantiation(methodName, *targs);   // e.g. emplaceBack$Tuple$i32$i32
    recordMethodInstantiation(ownerClass, methodName, mangled, std::move(*targs));
    return mangled;
}

// If `spreads` marks a spread argument (`xs...`) whose target is NOT a pack-bearing template (the
// caller has already ruled that out — see the call sites in Parser_Expr.cpp), UNWRAP the pack tuple
// into N ordinary positional arguments (`xs._0, …, xs._{N-1}`) in place, so existing overload
// resolution handles arity/type checking exactly as if the caller had hand-written them. v1: at most
// one spread per call (position need not be trailing); the spread argument must be a bare in-scope
// identifier — the only two shapes a pack variable ever has (the pack parameter itself, or a
// cons-match tail-pack local materialized by expandPackMatch) are always bare identifiers by
// construction, and there is no `Expr`-clone utility to safely duplicate anything more complex.
// Returns false (no-op, `args`/`argNames` untouched) if `spreads` has no `true` entry.
bool Parser::unwrapSpreadArgs(std::vector<std::unique_ptr<Expr>>& args,
                              std::vector<Token>& argNames,
                              const std::vector<bool>& spreads) {
    std::optional<size_t> spreadIdx;
    for (size_t i = 0; i < spreads.size(); ++i) {
        if (!spreads[i]) continue;
        if (spreadIdx) throw error(previous(), "at most one '...' spread is supported per call");
        spreadIdx = i;
    }
    if (!spreadIdx) return false;
    size_t idx = *spreadIdx;

    if (idx < argNames.size() && !argNames[idx].lexeme.empty())
        throw error(previous(), "'...' spread cannot be used as a named argument");

    Expr* spreadExpr = args[idx].get();
    const auto* id = spreadExpr && spreadExpr->node
                   ? std::get_if<IdentifierExpr>(spreadExpr->node.get()) : nullptr;
    if (!id)
        throw error(previous(), "'...' can only spread a simple pack variable here "
                    "(bind it to a local first)");

    std::optional<Token> t = deduceArgTypeToken(spreadExpr);
    auto tupIt = (t && t->lexeme.rfind("Tuple", 0) == 0) ? gen_->tupleRequests.find(t->lexeme)
                                                          : gen_->tupleRequests.end();
    if (tupIt == gen_->tupleRequests.end())
        throw error(previous(), "'...' can only spread a variadic pack (a tuple)");
    size_t n = tupIt->second.size();

    Token nameTok = id->name;
    std::vector<std::unique_ptr<Expr>> unwrapped;
    std::vector<Token> unwrappedNames;
    unwrapped.reserve(n);
    unwrappedNames.reserve(n);
    for (size_t k = 0; k < n; ++k) {
        unwrapped.push_back(box(makeExpr(MemberAccessExpr{
            box(makeExpr(IdentifierExpr{ nameTok })),
            Token{ TokenType::IDENTIFIER, "_" + std::to_string(k), nameTok.line },
            /*safe=*/false })));
        unwrappedNames.push_back(Token{ TokenType::IDENTIFIER, "", nameTok.line });
    }

    // Rebuild both vectors via fresh construction rather than vector::erase/insert — Token has
    // deleted copy/move assignment (const members), and erase/insert shift elements by assignment
    // internally, which would not compile for a vector<Token>.
    std::vector<std::unique_ptr<Expr>> newArgs;
    std::vector<Token> newNames;
    newArgs.reserve(args.size() - 1 + n);
    newNames.reserve(argNames.size() - 1 + n);
    for (size_t i = 0; i < args.size(); ++i) {
        if (i == idx) {
            for (auto& e : unwrapped)      newArgs.push_back(std::move(e));
            for (const Token& nt : unwrappedNames) newNames.push_back(nt);
        } else {
            newArgs.push_back(std::move(args[i]));
            newNames.push_back(argNames[i]);
        }
    }
    args     = std::move(newArgs);
    argNames = std::move(newNames);
    return true;
}

void Parser::expandPackMatch(std::vector<Token>& body, const std::string& pv,
                             const std::vector<Token>& elems) {
    const size_t arity = elems.size();
    auto id  = [](const std::string& s) { return Token{ TokenType::IDENTIFIER, s, 0 }; };
    auto sym = [](TokenType t, const char* s) { return Token{ t, s, 0 }; };

    for (size_t i = 0; i + 2 < body.size(); ) {
        if (!(body[i].type == TokenType::MATCH
              && body[i + 1].type == TokenType::IDENTIFIER && body[i + 1].lexeme == pv
              && body[i + 2].type == TokenType::LEFT_BRACE)) { ++i; continue; }

        // Matching close brace of the `match { … }` block.
        size_t open = i + 2, depth = 0, close = std::string::npos;
        for (size_t j = open; j < body.size(); ++j) {
            if (body[j].type == TokenType::LEFT_BRACE) ++depth;
            else if (body[j].type == TokenType::RIGHT_BRACE) { if (--depth == 0) { close = j; break; } }
        }
        if (close == std::string::npos) { ++i; continue; }

        // Parse the arms: `( )` / `( head : tail )`  `->`  ( { … } | expr ; ).
        std::vector<Token> emptyBody, consBody;
        std::string headName, tailName;
        bool haveEmpty = false, haveCons = false, malformed = false;
        size_t k = open + 1;
        while (k < close && !malformed) {
            if (body[k].type != TokenType::LEFT_PAREN) { malformed = true; break; }
            ++k;
            bool isEmpty = false; std::string h, t;
            if (body[k].type == TokenType::RIGHT_PAREN) { isEmpty = true; ++k; }
            else {
                if (body[k].type != TokenType::IDENTIFIER) { malformed = true; break; }
                h = body[k++].lexeme;
                if (body[k].type != TokenType::COLON) { malformed = true; break; }
                ++k;
                if (body[k].type != TokenType::IDENTIFIER) { malformed = true; break; }
                t = body[k++].lexeme;
                if (body[k].type != TokenType::RIGHT_PAREN) { malformed = true; break; }
                ++k;
            }
            if (body[k].type != TokenType::ARROW) { malformed = true; break; }
            ++k;
            std::vector<Token> arm;
            if (body[k].type == TokenType::LEFT_BRACE) {
                int d = 0;
                do {
                    if (body[k].type == TokenType::LEFT_BRACE) ++d;
                    else if (body[k].type == TokenType::RIGHT_BRACE) --d;
                    arm.push_back(body[k++]);
                } while (k < close && d > 0);
            } else {
                while (k < close && body[k].type != TokenType::SEMICOLON) arm.push_back(body[k++]);
                if (k < close && body[k].type == TokenType::SEMICOLON) arm.push_back(body[k++]);
            }
            if (isEmpty) { emptyBody = std::move(arm); haveEmpty = true; }
            else         { consBody  = std::move(arm); headName = h; tailName = t; haveCons = true; }
        }
        if (malformed) { ++i; continue; }   // not a well-formed pack match — leave for normal parse

        // Build the arm selected by the pack arity, wrapped in a block.
        std::vector<Token> repl;
        repl.push_back(sym(TokenType::LEFT_BRACE, "{"));
        if (arity == 0) {
            if (!haveEmpty) throw error(body[i], "match on an empty pack requires a '()' arm");
            for (const Token& tk : emptyBody) repl.push_back(tk);
        } else {
            if (!haveCons) throw error(body[i], "match on a non-empty pack requires a '(x:xs)' arm");
            // Tail tuple `<Tail> <tail> = <Tail>( pv._1, …, pv._{n-1} );` (unit for arity 1).
            std::vector<Token> tailElems(elems.begin() + 1, elems.end());
            std::string tailTuple = requestTupleType(tailElems);
            repl.push_back(id(tailTuple));
            repl.push_back(id(tailName));
            repl.push_back(sym(TokenType::EQUAL, "="));
            repl.push_back(id(tailTuple));
            repl.push_back(sym(TokenType::LEFT_PAREN, "("));
            for (size_t e = 1; e < arity; ++e) {
                if (e > 1) repl.push_back(sym(TokenType::COMMA, ","));
                repl.push_back(id(pv));
                repl.push_back(sym(TokenType::DOT, "."));
                repl.push_back(id("_" + std::to_string(e)));
            }
            repl.push_back(sym(TokenType::RIGHT_PAREN, ")"));
            repl.push_back(sym(TokenType::SEMICOLON, ";"));
            // Cons body with the head `head` rewritten to `pv._0` (skip a `.`/`::`-qualified use).
            for (size_t b = 0; b < consBody.size(); ++b) {
                bool afterAccess = b > 0 && (consBody[b - 1].type == TokenType::DOT
                                             || consBody[b - 1].type == TokenType::COLON_COLON);
                if (consBody[b].type == TokenType::IDENTIFIER && consBody[b].lexeme == headName
                    && !afterAccess) {
                    repl.push_back(id(pv));
                    repl.push_back(sym(TokenType::DOT, "."));
                    repl.push_back(id("_0"));
                } else {
                    repl.push_back(consBody[b]);
                }
            }
        }
        repl.push_back(sym(TokenType::RIGHT_BRACE, "}"));

        // Splice `repl` in place of body[i .. close].
        std::vector<Token> next;
        for (size_t a = 0; a < i; ++a)               next.push_back(body[a]);
        for (const Token& tk : repl)                 next.push_back(tk);
        for (size_t a = close + 1; a < body.size(); ++a) next.push_back(body[a]);
        body = std::move(next);
        i += repl.size();
    }
}

void Parser::runMonomorphization(Program& program) {
    // Every queued instantiation's mangled name is a concrete class name during
    // re-parse (the per-file parsers that recorded them are gone, so seed here).
    for (const auto& mangled : gen_->instantiated) classNames.insert(mangled);
    for (const auto& lname : gen_->lambdaClassNames) classNames.insert(lname);  // generated lambda classes

    // Impl specialization — a CONCRETE `impl Trait for Class<i32>` overrides the BLANKET
    // `impl<T> Trait for Class<T>` for that exact type ("most specific wins", the only tier GG can
    // express since partial specialization isn't a thing here). Collect every explicit concrete
    // impl's `Trait@MangledClass` key (the user-written impls are already parsed into the program;
    // their `Class<i32>` target was mangled to `Class$i32` by consumeType) so that below we skip
    // instantiating a blanket impl for a class a concrete impl already covers — otherwise both would
    // emit `Class$i32::method` and collide as a duplicate.
    std::unordered_set<std::string> concreteImplKeys;
    for (const Stmt& s : program.declarations)
        if (const auto* im = std::get_if<ImplDeclStmt>(s.node.get())) {
            concreteImplKeys.insert(im->traitName.lexeme + "@" + im->typeName.lexeme);
            // A concrete `impl Call for X` canonicalizes its trait to the signature-mangled
            // `Call$…$ret$…`, whereas a blanket `impl<T> Call for X<T>` stores the RAW `Call` in its
            // template. Register a raw-`Call@class` alias so the concrete still suppresses the
            // blanket for that class (any concrete `call` on a class collides with a blanket `call`).
            if (im->traitName.lexeme.rfind("Call$", 0) == 0)
                concreteImplKeys.insert("Call@" + im->typeName.lexeme);
        }

    // Outer loop: drain the main worklist, then retry any deferred generic-method instantiations (a
    // generic class's methods become available only after its body is re-parsed); repeat to a fixpoint.
    for (;;) {
    while (!gen_->worklist.empty()) {
        GenericInstantiation inst = std::move(gen_->worklist.back());
        gen_->worklist.pop_back();

        // Generic METHOD instantiation: substitute + re-parse the single method and inject it into
        // its owner class. Defer if the owner class / method template isn't available yet.
        if (!inst.ownerClass.empty()) {
            if (!instantiateMethod(inst, program))
                pendingMethodInsts_.push_back(std::move(inst));
            continue;
        }

        auto it = gen_->templates.find(inst.templateName);
        if (it == gen_->templates.end())
            throw error(tokens.empty() ? Token{TokenType::END_OF_FILE, "", 0} : tokens.back(),
                        "no generic template named '" + inst.templateName + "'");
        const GenericTemplate& tmpl = it->second;

        // Map each type parameter to its argument token slice (emplace = copy-construct;
        // Token is not copy-assignable due to its const members).
        std::unordered_map<std::string, std::vector<Token>> sub;
        for (size_t i = 0; i < tmpl.typeParams.size() && i < inst.args.size(); ++i)
            sub.emplace(tmpl.typeParams[i], inst.args[i]);

        // Record trait-bound obligations: each bounded type parameter's concrete
        // argument must implement the named trait(s). Verified in the semantic pass
        // (static dispatch — the type param itself never reaches semantics/codegen).
        for (size_t i = 0; i < tmpl.typeParams.size() && i < inst.args.size(); ++i) {
            if (i >= tmpl.bounds.size() || tmpl.bounds[i].empty() || inst.args[i].empty()) continue;
            const Token& argHead = inst.args[i].front();   // base type token ('&' is separate)
            for (const std::string& tr : tmpl.bounds[i])
                program.genericBoundChecks.push_back(
                    GenericBoundCheck{ argHead.lexeme, tr, inst.mangledName, argHead.line });
        }

        // A variadic pack type parameter (the last one, if `isPack`): the pack materializes as a
        // tuple, so a `Ts... args` parameter substitutes to `Tuple$…* args` (a tuple borrow).
        const std::string packName = (!tmpl.isPack.empty() && tmpl.isPack.back())
                                    ? tmpl.typeParams.back() : std::string{};

        // Substitute. Rename the declaration name and any constructor/destructor name
        // (a token == templateName that is NOT followed by '<'); self-references like
        // "Name<...>" are left for re-parse to mangle. Replace type-parameter tokens.
        std::string packValueName;   // the pack VALUE parameter name (`args` in `Ts... args`)
        std::vector<Token> out;
        for (size_t idx = 0; idx < tmpl.tokens.size(); ++idx) {
            const Token& t = tmpl.tokens[idx];
            // Rename occurrences of the template name to the mangled instantiation. For a VARIADIC
            // template, rename ONLY the declaration name (`fn NAME` — idx 1) and leave recursive
            // *calls* as the bare name, so `f(…, xs...)` re-deduces the SHORTER pack each level
            // (a self-recursive call over a fixed pack would otherwise loop forever). For a non-
            // variadic generic, recursive calls do share the instantiation, so all occurrences rename.
            if (t.type == TokenType::IDENTIFIER && t.lexeme == inst.templateName
                && (idx + 1 >= tmpl.tokens.size() || tmpl.tokens[idx + 1].type != TokenType::LESS)
                && (packName.empty() || idx == 1)) {
                out.push_back(Token{ TokenType::IDENTIFIER, inst.mangledName, t.line });
                continue;
            }
            // `PackName ...` → the pack's tuple type + `*` borrow (drop the ELLIPSIS).
            if (!packName.empty() && t.type == TokenType::IDENTIFIER && t.lexeme == packName
                && idx + 1 < tmpl.tokens.size() && tmpl.tokens[idx + 1].type == TokenType::ELLIPSIS) {
                auto sit = sub.find(packName);
                if (sit != sub.end()) for (const Token& a : sit->second) out.push_back(a);
                out.push_back(Token{ TokenType::STAR, "*", t.line });
                if (idx + 2 < tmpl.tokens.size()) packValueName = tmpl.tokens[idx + 2].lexeme;
                ++idx;   // skip the ELLIPSIS (the value-param name is emitted next iteration)
                continue;
            }
            if (t.type == TokenType::IDENTIFIER) {
                auto sit = sub.find(t.lexeme);
                if (sit != sub.end()) {
                    spliceTypeArg(sit->second, tmpl.tokens, idx + 1, out);
                    continue;
                }
            }
            out.push_back(t);
        }

        // Expand any compile-time cons-`match` over the pack now that the pack arity is known
        // (`match args { () -> …; (x:xs) -> … }` → the arm selected by arity, tail materialized).
        if (!packName.empty() && !packValueName.empty()) {
            auto sit = sub.find(packName);
            if (sit != sub.end() && !sit->second.empty()) {
                auto trIt = gen_->tupleRequests.find(sit->second.front().lexeme);
                std::vector<Token> elems;
                if (trIt != gen_->tupleRequests.end())
                    for (const Token& e : trIt->second) elems.push_back(e);
                expandPackMatch(out, packValueName, elems);
            }
        }
        out.push_back(Token{ TokenType::END_OF_FILE, "", 0 });

        // Re-parse the concrete declaration (may enqueue further instantiations).
        tokens  = std::move(out);
        current = 0;
        program.declarations.push_back(parseDeclaration());

        // A generic CLASS instantiation also instantiates every generic `impl … for Class<…>`
        // with the same type arguments, so e.g. `Array<i32>` gets its `Index` impl.
        if (gen_->classNames.count(inst.templateName)) {
            for (size_t ti = 0; ti < gen_->implTemplates.size(); ++ti) {
                const GenericImplTemplate& impl = gen_->implTemplates[ti];
                if (impl.targetClass != inst.templateName) continue;
                if (impl.targetParamAtPos.size() != inst.args.size()) continue;  // arity mismatch

                // Specialization: a concrete `impl Trait for Class$args` overrides this blanket one.
                if (concreteImplKeys.count(impl.traitName + "@" + inst.mangledName)) continue;

                std::string key = std::to_string(ti) + "@" + inst.mangledName;
                if (!gen_->implInstantiated.insert(key).second) continue;        // already done

                // impl type-parameter name → the class's concrete argument tokens (positional).
                std::unordered_map<std::string, std::vector<Token>> isub;
                for (size_t pos = 0; pos < impl.targetParamAtPos.size(); ++pos)
                    isub.emplace(impl.targetParamAtPos[pos], inst.args[pos]);

                std::vector<Token> iout;
                for (size_t idx = 0; idx < impl.tokens.size(); ++idx) {
                    const Token& t = impl.tokens[idx];
                    if (t.type == TokenType::IDENTIFIER) {
                        auto sit = isub.find(t.lexeme);
                        if (sit != isub.end()) { spliceTypeArg(sit->second, impl.tokens, idx + 1, iout); continue; }
                    }
                    iout.push_back(t);
                }
                iout.push_back(Token{ TokenType::END_OF_FILE, "", 0 });

                // Re-parse the concrete impl (its `Class<args>` target mangles to the same
                // `Class$args` and attaches to that class; may enqueue further instantiations).
                tokens  = std::move(iout);
                current = 0;
                program.declarations.push_back(parseDeclaration());
            }
        }
    }

    // Main worklist drained. Retry deferred generic-method instantiations — a method's owner class /
    // template becomes ready once that class's body is re-parsed; a successful re-parse may enqueue
    // more class/method work, so the outer loop drains that too. Stop when nothing is pending, or
    // when a pass makes no progress and the worklist is empty (an owner that never instantiated).
    if (pendingMethodInsts_.empty()) break;
    bool gmProgress = false;
    std::vector<GenericInstantiation> stillPending;
    for (auto& mInst : pendingMethodInsts_) {
        if (instantiateMethod(mInst, program)) gmProgress = true;
        else stillPending.push_back(std::move(mInst));
    }
    pendingMethodInsts_ = std::move(stillPending);
    if (!gmProgress && gen_->worklist.empty()) break;
    }   // for (;;)
    if (!pendingMethodInsts_.empty())
        throw error(tokens.empty() ? Token{TokenType::END_OF_FILE, "", 0} : tokens.back(),
                    "could not instantiate a generic method — its owner class was never instantiated");

    // Surface bounded generic templates for definition-time body checking (semantic pass
    // checkGenericBodies). Re-parse each template's ORIGINAL body (no type substitution) with
    // its type-parameter names registered as types, using a throwaway parser with its OWN
    // registry so the parse cannot perturb this monomorphization or enqueue real instantiations.
    for (const auto& [tmplName, tmpl] : gen_->templates) {
        bool hasBound = false;
        for (const auto& b : tmpl.bounds) if (!b.empty()) { hasBound = true; break; }
        if (!hasBound) continue;   // only bounded templates are checked

        std::unordered_set<std::string> scratchClasses = classNames;
        for (const std::string& tp : tmpl.typeParams) scratchClasses.insert(tp);

        std::vector<Token> toks(tmpl.tokens);
        toks.push_back(Token{ TokenType::END_OF_FILE, "", 0 });

        try {
            Parser scratch(scratchClasses, /*sharedRegistry=*/nullptr);
            Program bodyProg = scratch.parse(toks, filename, /*runMonomorphization=*/false);
            if (!bodyProg.declarations.empty() && bodyProg.declarations.front().node)
                program.genericTemplates.push_back(GenericTemplateDecl{
                    std::move(bodyProg.declarations.front()), tmpl.typeParams, tmpl.bounds });
        } catch (const CompileError&) {
            // Body could not be re-parsed for checking — skip silently. Monomorphized
            // instantiations are still analyzed normally; only a definition-time diagnostic is lost.
        }
    }

    // Synthesize the concrete value-object class for every tuple type recorded during parsing +
    // monomorphization (before reflection, so a tuple's fields are visible to `@fields`, and before
    // semantics/codegen, which then treat it as an ordinary class).
    synthesizeTupleClasses(program);

    // Compile-time reflection: now that every class (incl. monomorphized ones) is in the program,
    // expand every `inline for (v in @fields(T))` into ordinary statements.
    expandReflection(program);
}

// Build a concrete `ClassDeclStmt` for each recorded tuple type and append it to the Program. The
// class is an ordinary value object `class Tuple$… { E0 _0; E1 _1; … Tuple$…(P0 a0, …) { _0 = a0; … } }`.
// Object-typed elements (a class or nested tuple — an IDENTIFIER type token) take a reference ctor
// parameter (`ElemType&`) whose assignment into the value field clones (mirrors an embedded
// value-object field, e.g. `class Named { String name; Named(String& n) { name = n; } }`); primitive
// and `str` elements pass by value. Tokens are hand-built (never lexed — the lexer rejects `$` in
// identifiers) and re-parsed via parseDeclaration, exactly like a monomorphized generic class.
void Parser::synthesizeTupleClasses(Program& program) {
    auto id  = [](const std::string& s) { return Token{ TokenType::IDENTIFIER, s, 0 }; };
    auto sym = [](TokenType t, const char* s) { return Token{ t, s, 0 }; };

    // Make every tuple name a known class in THIS parser's local table, so a nested-tuple element's
    // reference constructor parameter (`Tuple$…&`) passes consumeType's class-check during re-parse
    // (the per-file parser that recorded the request may be a different instance from this one).
    for (const auto& [tn, elems] : gen_->tupleRequests) { (void)elems; classNames.insert(tn); }

    for (const auto& [name, elems] : gen_->tupleRequests) {
        std::vector<Token> toks;
        toks.push_back(sym(TokenType::CLASS, "class"));
        toks.push_back(id(name));
        toks.push_back(sym(TokenType::LEFT_BRACE, "{"));

        // Fields: Ei _i ;
        for (size_t i = 0; i < elems.size(); ++i) {
            toks.push_back(elems[i]);                                   // element type token
            toks.push_back(id("_" + std::to_string(i)));               // field name _i
            toks.push_back(sym(TokenType::SEMICOLON, ";"));
        }

        // Constructor: Tuple$…( P0 a0, P1 a1, … ) { _0 = a0; … }
        toks.push_back(id(name));
        toks.push_back(sym(TokenType::LEFT_PAREN, "("));
        for (size_t i = 0; i < elems.size(); ++i) {
            if (i > 0) toks.push_back(sym(TokenType::COMMA, ","));
            toks.push_back(elems[i]);                                   // param type
            if (elems[i].type == TokenType::IDENTIFIER)                // object element → reference param
                toks.push_back(sym(TokenType::AMPERSAND, "&"));
            toks.push_back(id("a" + std::to_string(i)));               // param name ai
        }
        toks.push_back(sym(TokenType::RIGHT_PAREN, ")"));
        toks.push_back(sym(TokenType::LEFT_BRACE, "{"));
        for (size_t i = 0; i < elems.size(); ++i) {
            toks.push_back(id("_" + std::to_string(i)));
            toks.push_back(sym(TokenType::EQUAL, "="));
            toks.push_back(id("a" + std::to_string(i)));
            toks.push_back(sym(TokenType::SEMICOLON, ";"));
        }
        toks.push_back(sym(TokenType::RIGHT_BRACE, "}"));               // end ctor body
        toks.push_back(sym(TokenType::RIGHT_BRACE, "}"));               // end class
        toks.push_back(Token{ TokenType::END_OF_FILE, "", 0 });

        tokens  = std::move(toks);
        current = 0;
        program.declarations.push_back(parseDeclaration());
    }
}