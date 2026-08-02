#include "SemanticAnalyzer.h"
#include <algorithm>

// Best-match overload pick for operator desugaring (defined below).
static int pickOverloadByArgs(const std::vector<ClassInfo::Method>& set,
                              const std::vector<Type>& argTypes);

// An addressable expression (has a storage location that can be borrowed): a name, an element
// `a[i]`, a member `x.f`, or `this`. Temporaries (literals, arithmetic, calls returning a value)
// are not addressable. Used to validate that a `ref <primitive>` borrows a real lvalue.
static bool isLvalueExpr(const Expr& e) {
    if (!e.node) return false;
    return std::holds_alternative<IdentifierExpr>(*e.node)
        || std::holds_alternative<IndexExpr>(*e.node)
        || std::holds_alternative<MemberAccessExpr>(*e.node)
        || std::holds_alternative<ThisExpr>(*e.node);
}

// ============================================================
// Expression analysis
// ============================================================

Type SemanticAnalyzer::analyzeExpr(const Expr& expr) {
    Type resolvedType = std::visit(overloaded{
        [&](const LiteralExpr& literal)               { return analyzeLiteral(literal); },
        [&](const IdentifierExpr& identifier)         { return analyzeIdentifier(identifier); },
        [&](const UnaryExpr& unary)                   { return analyzeUnary(unary); },
        [&](const BinaryExpr& binary)                 { return analyzeBinary(binary); },
        [&](const AssignExpr& assign)                 { return analyzeAssign(assign); },
        [&](const CompoundAssignExpr& compoundAssign) { return analyzeCompoundAssign(compoundAssign); },
        [&](const PostfixExpr& postfix)               { return analyzePostfix(postfix); },
        [&](const CallExpr& call)                     { return analyzeCall(call); },
        [&](const VarDeclExpr& varDecl)               { return analyzeVarDecl(varDecl); },
        [&](const IndexExpr& indexExpr)               { return analyzeIndex(indexExpr); },
        [&](const IndexAssignExpr& indexAssign)        { return analyzeIndexAssign(indexAssign); },
        [&](const ThisExpr& thisExpr)                 { return analyzeThis(thisExpr); },
        [&](const MemberAccessExpr& ma)               { return analyzeMemberAccess(ma); },
        [&](const MemberAssignExpr& ma)               { return analyzeMemberAssign(ma); },
        [&](const MethodCallExpr& mc)                 { return analyzeMethodCall(mc); },
        [&](const RefStoreExpr& refStore)             { return analyzeRefStore(refStore); },
        [&](const BraceInitExpr& braceInit)           { return analyzeBraceInit(braceInit); },
        [&](const CastExpr& castExpr)                 { return analyzeCast(castExpr); },
        [&](const NewExpr& newExpr)                   { return analyzeNew(newExpr); },
        [&](const SizeofExpr&)                        { return Type{TypeKind::U64}; },
        [&](const DestroyExpr& destroyExpr)           { return analyzeDestroy(destroyExpr); },
        [&](const ReflectExpr& reflect)               { return analyzeReflect(reflect); },
        [&](const SwitchExpr& switchExpr)             { return analyzeSwitchExpr(switchExpr); },
        [&](const NullLiteralExpr&)                   { return makeNullType(); },
        [&](const UnwrapExpr& unwrap)                 { return analyzeUnwrap(unwrap); },
        [&](const ElvisExpr& elvis)                   { return analyzeElvis(elvis); },
    }, *expr.node);
    recordType(expr, resolvedType);
    return resolvedType;
}

// Reduce a type token lexeme to its base class name (drop '?', "ref:", trailing '&').
static std::string reflBaseClassName(std::string s) {
    if (!s.empty() && s.back() == '?') s.pop_back();
    if (s.rfind("ref:", 0) == 0)       s = s.substr(4);
    if (!s.empty() && s.back() == '&') s.pop_back();
    return s;
}

// Compile-time reflection builtins that survive to semantics (the scalar queries). `inline for`,
// `@field`, and `f.name` are expanded away by the parser and never reach here.
Type SemanticAnalyzer::analyzeReflect(const ReflectExpr& r) {
    switch (r.kind) {
        case ReflectKind::TypeName:
            // A compile-time string — a `str` view (like a string literal), so `.len` / `.data` work.
            return Type{TypeKind::Str};

        case ReflectKind::FieldCount:
        case ReflectKind::HasField:
        case ReflectKind::OffsetOf: {
            std::string cls = reflBaseClassName(r.typeArgs.empty() ? "" : r.typeArgs[0].lexeme);
            if (!classRegistry.count(cls) || enumRegistry.count(cls))
                error(r.at, "reflection query: '" + cls + "' is not a class type");
            if (!r.valueArgs.empty()) analyzeExpr(*r.valueArgs[0]);   // the "name" string, if any
            // @offsetOf's field name must be a string literal naming an actual instance field.
            if (r.kind == ReflectKind::OffsetOf && classRegistry.count(cls)) {
                std::string field;
                if (!r.valueArgs.empty())
                    if (const auto* lit = std::get_if<LiteralExpr>(r.valueArgs[0]->node.get()))
                        if (lit->token.type == TokenType::STRING) field = lit->token.lexeme;
                const auto& fields = classRegistry.at(cls).fields;   // instance fields only
                if (field.empty() || !fields.count(field))
                    error(r.at, "@offsetOf: '" + cls + "' has no instance field '" + field + "'");
            }
            return r.kind == ReflectKind::HasField ? Type{TypeKind::Bool} : Type{TypeKind::U64};
        }

        case ReflectKind::VariantCount: {
            std::string en = reflBaseClassName(r.typeArgs.empty() ? "" : r.typeArgs[0].lexeme);
            if (!enumRegistry.count(en))
                error(r.at, "@variantCount: '" + en + "' is not an enum type");
            return Type{TypeKind::U64};
        }

        case ReflectKind::AlignOf:
            // Works on any type (primitive / class / enum); the fold happens in codegen.
            return Type{TypeKind::U64};

        case ReflectKind::HasAnnotation: {
            // `@hasAnnotation(T, Ann)` — Ann (typeArgs[1]) must be a declared annotation type.
            if (r.typeArgs.size() > 1 && !annotationRegistry.count(r.typeArgs[1].lexeme))
                error(r.at, "unknown annotation '" + r.typeArgs[1].lexeme + "' in '@hasAnnotation'");
            return Type{TypeKind::Bool};
        }

        case ReflectKind::Implements:
        case ReflectKind::IsInteger:
        case ReflectKind::IsFloat:
        case ReflectKind::IsClass:
        case ReflectKind::IsEnum:
        case ReflectKind::IsPrimitive:
            // Compile-time predicates — always a bool; the value is folded in codegen.
            return Type{TypeKind::Bool};

        case ReflectKind::CompileError: {
            std::string msg = "compile-time error";
            if (!r.valueArgs.empty())
                if (const auto* lit = std::get_if<LiteralExpr>(r.valueArgs[0]->node.get()))
                    if (lit->token.type == TokenType::STRING) msg = lit->token.lexeme;
            error(r.at, msg);
            return Type{TypeKind::Error};
        }

        case ReflectKind::Field:
            error(r.at, "'@field' is only valid inside an 'inline for' body");
            return Type{TypeKind::Error};
    }
    return Type{TypeKind::Error};
}

Type SemanticAnalyzer::analyzeLiteral(const LiteralExpr& literal) {
    switch (literal.token.type) {
        case TokenType::TRUE:
        case TokenType::FALSE:
            return Type{TypeKind::Bool};
        case TokenType::NUMBER: {
            // A bare numeric literal is *untyped*: it adopts the contextual expected type when one
            // is a numeric type, and otherwise falls back to its default (i32 for integers, f64 for
            // decimals). So `i64 y = 5;` types `5` as i64 (no widening warning) and `f32 f = 1.0;`
            // types `1.0` as f32 (no narrowing warning). Only *direct* literals adopt — operands of
            // a binary expression are analysed with the expected type cleared (see analyzeBinary).
            const std::string& lex = literal.token.lexeme;
            bool isDecimal = lex.find('.') != std::string::npos
                          || lex.find('e') != std::string::npos
                          || lex.find('E') != std::string::npos;

            // The contextual numeric target, if any (strip nullable; ignore non-numeric contexts).
            const Type* want = nullptr;
            Type stripped;
            if (expectedType_) {
                stripped = expectedType_->isNullable ? stripNullable(*expectedType_) : *expectedType_;
                if (isInteger(stripped.kind) || isFloat(stripped.kind)) want = &stripped;
            }

            if (isDecimal) {
                // Decimal literal → adopt an expected float type, else default f64.
                if (want && isFloat(want->kind)) return Type{want->kind};
                return Type{TypeKind::F64};
            }

            // Integer literal — parse its magnitude (a leading '-' is a separate UnaryExpr).
            unsigned long long mag = 0;
            bool parsed = true;
            try { mag = std::stoull(lex); } catch (...) { parsed = false; }

            if (want) {
                if (isFloat(want->kind)) return Type{want->kind};   // integer literal in a float slot
                // Integer context: adopt the type; warn (and still adopt) if the value won't fit.
                if (!parsed || !integerLiteralFits(mag, want->kind))
                    warn(literal.token, "integer literal '" + lex + "' does not fit in '"
                         + typeName(*want) + "'");
                return Type{want->kind};
            }

            // No numeric context → default i32; warn if the literal overflows i32.
            if (!parsed || !integerLiteralFits(mag, TypeKind::I32))
                warn(literal.token, "integer literal '" + lex
                     + "' overflows the default type 'i32'; annotate a wider type (e.g. 'i64')");
            return Type{TypeKind::I32};
        }
        case TokenType::STRING:
            return Type{TypeKind::Str};   // a string literal is a `str` view (decays to `ptr` for FFI)
        case TokenType::CHAR:
            return Type{TypeKind::Char};
        default:
            return Type{TypeKind::Error};
    }
}

const ClassInfo::Field* SemanticAnalyzer::currentInstanceField(const std::string& name) const {
    if (currentMethodIsStatic || currentClassName.empty()) return nullptr;
    auto cit = classRegistry.find(currentClassName);
    if (cit == classRegistry.end()) return nullptr;
    auto fit = cit->second.fields.find(name);
    return fit == cit->second.fields.end() ? nullptr : &fit->second;
}

const ClassInfo::StaticField* SemanticAnalyzer::currentStaticField(const std::string& name) const {
    if (currentClassName.empty()) return nullptr;
    auto cit = classRegistry.find(currentClassName);
    if (cit == classRegistry.end()) return nullptr;
    auto sfit = cit->second.staticFields.find(name);
    return sfit == cit->second.staticFields.end() ? nullptr : &sfit->second;
}

const Type* SemanticAnalyzer::currentStaticFieldType(const std::string& name) const {
    const ClassInfo::StaticField* sf = currentStaticField(name);
    return sf ? &sf->type : nullptr;
}

// `destroy(place)` — run a destructor in place on a value object (or `ptr<T>` element). An unsafe
// low-level container primitive: requires --unsafe-ptr, and yields void. A primitive/enum place is
// permitted and lowers to a no-op (so container code is uniform across primitive and object T).
Type SemanticAnalyzer::analyzeDestroy(const DestroyExpr& destroy) {
    Type placeType = analyzeExpr(*destroy.place);
    if (!allowRawPtr_)
        error(destroy.keyword, "'destroy' is an unsafe operation and requires --unsafe-ptr "
              "(it is a low-level container primitive)");
    else if (placeType.kind == TypeKind::Reference || placeType.kind == TypeKind::Ptr
             || placeType.kind == TypeKind::TypedPtr)
        error(destroy.keyword, "'destroy' expects a value object or ptr<T> element to destroy in "
              "place, not a reference or raw pointer");
    // Guard the double-free footgun: a bare local object variable is already destroyed automatically
    // at scope exit, so `destroy(local)` would run its dtor twice. `destroy` is for buffer elements
    // (`data[i]`) and fields the compiler does NOT auto-destroy.
    else if (placeType.kind == TypeKind::Object) {
        if (const auto* id = std::get_if<IdentifierExpr>(destroy.place->node.get())) {
            const Symbol* sym = symbolTable.lookup(id->name.lexeme);
            if (sym && sym->kind == Symbol::Kind::Variable)
                error(destroy.keyword, "cannot 'destroy' the scope-managed local object '"
                      + id->name.lexeme + "' — it is destroyed automatically at scope exit, so this "
                      "would double-free; destroy a ptr<T> element (e.g. data[i]) or a field instead");
        }
    }
    return Type{TypeKind::Void};
}

// True if `className` (transitively, through embedded value-object fields) owns a raw `ptr`/`ptr<T>`
// field. Such a value object can't be stored/copied by value in a `ptr<T>` container: memberwise
// clone is shallow over a raw pointer, so the copy would alias the buffer and double-free. Reference
// (`Class&`) and enum fields are fine (clone retains / shares them). (Phase 2 relaxes this for types
// that provide a user `Clone` impl.)
bool SemanticAnalyzer::classOwnsRawPtr(const std::string& className,
                                       std::unordered_set<std::string>& seen) const {
    if (!seen.insert(className).second) return false;   // cycle-safe
    auto it = classRegistry.find(className);
    if (it == classRegistry.end()) return false;
    for (const std::string& fname : it->second.fieldOrder) {
        const Type& ft = it->second.fields.at(fname).type;
        if (ft.kind == TypeKind::Ptr || ft.kind == TypeKind::TypedPtr) return true;
        if (ft.kind == TypeKind::Object && classOwnsRawPtr(ft.className, seen)) return true;
    }
    return false;
}

const std::vector<ClassInfo::Method>* SemanticAnalyzer::currentClassMethods(const std::string& name) const {
    if (currentClassName.empty()) return nullptr;
    auto cit = classRegistry.find(currentClassName);
    if (cit == classRegistry.end()) return nullptr;
    auto mit = cit->second.methods.find(name);
    return mit == cit->second.methods.end() ? nullptr : &mit->second;
}

Type SemanticAnalyzer::analyzeWithExpected(const Expr& e, const Type& expected) {
    std::optional<Type> saved = expectedType_;
    expectedType_ = expected;
    Type t = analyzeExpr(e);
    expectedType_ = saved;
    return t;
}

// Best-match overload resolution — see the header for the algorithm.
int SemanticAnalyzer::resolveOverload(const Token& at, const std::string& what,
                                      const std::vector<OverloadCand>& cands,
                                      const std::vector<std::unique_ptr<Expr>>& args,
                                      const std::vector<Token>& argNames,
                                      const void* nodeKey) {
    // The expected type applies to THIS call's return, not to its arguments — snapshot and
    // clear it so it doesn't leak into argument sub-expression resolution.
    std::optional<Type> expected = expectedType_;
    expectedType_ = std::nullopt;

    // Number of leading POSITIONAL arguments (all named args follow them — the parser enforces
    // this). Empty argNames ⇒ every argument is positional (the common, fast path).
    auto positionalCount = [&]() -> size_t {
        for (size_t k = 0; k < argNames.size(); ++k)
            if (!argNames[k].lexeme.empty()) return k;
        return args.size();
    };
    const size_t positional = positionalCount();
    const bool   hasNames    = positional < args.size();

    auto slotHasDefault = [](const OverloadCand& c, size_t i, size_t total) -> bool {
        if (c.paramHasDefault && i < c.paramHasDefault->size()) return (*c.paramHasDefault)[i];
        return i >= total - std::min(c.numDefaults, total);   // fallback: trailing run
    };
    auto nameSlot = [](const OverloadCand& c, const std::string& n) -> int {
        if (!c.paramNames) return -1;
        for (size_t i = 0; i < c.paramNames->size(); ++i) if ((*c.paramNames)[i] == n) return int(i);
        return -1;
    };
    // Map a call's args onto a candidate's parameter slots → per-slot written-arg index (or -1 =
    // use that slot's default). Returns false with a reason on failure.
    auto mapSlots = [&](const OverloadCand& c, std::vector<int>& order, std::string& reason) -> bool {
        const size_t total = c.params->size();
        order.assign(total, -1);
        if (positional > total) { reason = "too many arguments"; return false; }
        for (size_t k = 0; k < positional; ++k) order[k] = int(k);
        for (size_t k = positional; k < args.size(); ++k) {
            int slot = nameSlot(c, argNames[k].lexeme);
            if (slot < 0) { reason = "unknown parameter name '" + argNames[k].lexeme + "'"; return false; }
            if (order[slot] >= 0) {
                reason = "parameter '" + argNames[k].lexeme + "' " +
                         (size_t(slot) < positional ? "already given positionally" : "given more than once");
                return false;
            }
            order[slot] = int(k);
        }
        for (size_t i = 0; i < total; ++i)
            if (order[i] < 0 && !slotHasDefault(c, i, total)) {
                reason = "no argument for required parameter" +
                         (c.paramNames && i < c.paramNames->size() ? " '" + (*c.paramNames)[i] + "'" : "");
                return false;
            }
        return true;
    };

    // Analyse the written arguments once, in source order. For a single candidate an untyped
    // brace-init argument (`{...}`) deduces its class from the parameter type it maps to.
    std::vector<int> soleOrder;
    std::string soleReason;
    bool haveSole = (cands.size() == 1) && mapSlots(cands[0], soleOrder, soleReason);
    auto slotOfWritten = [&](size_t k) -> int {         // where written arg k lands in the sole cand
        if (k < positional) return int(k);
        return haveSole ? nameSlot(cands[0], argNames[k].lexeme) : -1;
    };
    std::vector<Type> argTypes(args.size());
    bool anyArgError = false;
    for (size_t k = 0; k < args.size(); ++k) {
        int slot = slotOfWritten(k);
        if (args[k]->node && std::holds_alternative<BraceInitExpr>(*args[k]->node)
            && cands.size() == 1 && slot >= 0 && size_t(slot) < cands[0].params->size())
            expectedType_ = (*cands[0].params)[slot];
        else
            expectedType_ = std::nullopt;
        Type t = analyzeExpr(*args[k]);
        if (isError(t)) anyArgError = true;
        argTypes[k] = t;
    }
    expectedType_ = std::nullopt;
    if (anyArgError) return -1;   // a bad argument already reported an error; avoid cascades

    struct Viable { int idx; int cost; std::vector<int> order; };
    std::vector<Viable> viable;
    std::string lastReason;
    for (int i = 0; i < static_cast<int>(cands.size()); ++i) {
        const OverloadCand& c = cands[i];
        std::vector<int> order;
        if (!mapSlots(c, order, lastReason)) continue;
        bool ok = true;
        int  cost = 0;
        for (size_t s = 0; s < order.size() && ok; ++s) {
            if (order[s] < 0) continue;                            // filled from a default
            const Type& pt = (*c.params)[s];
            const Type& at2 = argTypes[order[s]];
            if (at2 == pt) continue;                               // exact: cost 0
            CastResult cr = canPassArgument(at2, pt);              // incl. value-object borrow
            if (cr == CastResult::None) { ok = false; break; }
            cost += (cr == CastResult::Warn) ? 2 : 1;              // narrowing worse than widening
        }
        if (ok) viable.push_back({i, cost, std::move(order)});
    }

    if (viable.empty()) {
        // Single non-overloaded candidate with a positional-only call → keep the precise arity
        // diagnostic; otherwise surface the mapping reason (unknown name, missing required, …).
        size_t total    = cands.empty() ? 0 : cands[0].params->size();
        size_t required = cands.empty() ? 0 : total - std::min(cands[0].numDefaults, total);
        if (cands.size() == 1 && !hasNames && (args.size() < required || args.size() > total)) {
            std::string want = (required == total)
                ? std::to_string(total)
                : std::to_string(required) + " to " + std::to_string(total);
            error(at, what + " expects " + want + " argument(s), got " + std::to_string(args.size()));
        } else if (cands.size() == 1 && !lastReason.empty()) {
            error(at, "no matching call to " + what + ": " + lastReason);
        } else {
            error(at, "no matching overload for " + what + " with the given argument types");
        }
        return -1;
    }

    int minCost = viable.front().cost;
    for (const Viable& v : viable) if (v.cost < minCost) minCost = v.cost;
    std::vector<int> best;
    for (size_t vi = 0; vi < viable.size(); ++vi) if (viable[vi].cost == minCost) best.push_back(int(vi));

    int chosenVi = -1;
    if (best.size() == 1) {
        chosenVi = best[0];
    } else if (expected) {
        // Tie on argument cost → disambiguate on return type via the contextual expected type.
        int rtBest = -1, rtCost = -1, rtTies = 0;
        for (int vi : best) {
            const Type& rt = cands[viable[vi].idx].returnType;
            int c;
            if (rt == *expected) c = 0;
            else {
                CastResult cr = canImplicitlyCast(rt, *expected);
                if (cr == CastResult::None) continue;
                c = (cr == CastResult::Warn) ? 2 : 1;
            }
            if (rtBest < 0 || c < rtCost) { rtBest = vi; rtCost = c; rtTies = 1; }
            else if (c == rtCost) rtTies++;
        }
        if (rtBest >= 0 && rtTies == 1) chosenVi = rtBest;
    }
    if (chosenVi < 0) {
        error(at, "ambiguous call to overloaded " + what
              + "; add an explicit cast to select an overload");
        return -1;
    }

    const int chosen = viable[chosenVi].idx;
    const std::vector<int>& order = viable[chosenVi].order;
    const OverloadCand& w = cands[chosen];
    // Emit the per-argument cast / mut diagnostics on the chosen overload, per filled slot.
    for (size_t s = 0; s < order.size(); ++s) {
        if (order[s] < 0) continue;                       // default-filled: nothing to diagnose here
        size_t k = size_t(order[s]);
        std::string argLabel = "argument " + std::to_string(k + 1) + " of " + what;
        checkArgCast(argTypes[k], (*w.params)[s], at, argLabel);
        if (w.paramMut && s < w.paramMut->size() && (*w.paramMut)[s])
            warnConstToMut(at, *args[k], (*w.params)[s]);
        // A temporary bound to a primitive borrow is materialized into a hidden slot (like C++
        // binding a temporary to a `const int&`) — safe because a primitive borrow does not escape
        // and the temp lives for the call. Only a `mut` primitive borrow rejects a temporary, since
        // a write-through would be lost when the temp dies.
        bool slotMut = w.paramMut && s < w.paramMut->size() && (*w.paramMut)[s];
        if (isPrimitiveBorrow((*w.params)[s]) && !isBorrow(argTypes[k]) && !isLvalueExpr(*args[k]) && slotMut)
            error(at, argLabel + " expects a mutable borrow '" + typeName((*w.params)[s])
                  + "' but got a temporary; a write through it would be lost — pass a variable");
        if (argTypes[k].kind == TypeKind::Object && (*w.params)[s].kind == TypeKind::Reference
            && w.paramEscapes && s < w.paramEscapes->size() && (*w.paramEscapes)[s])
            error(at, "cannot pass the value object '" + argTypes[k].className + "' as " + argLabel
                  + ": that parameter escapes (the callee stores or returns it), but a stack value "
                  "object has no owner to keep it alive past the call — allocate it with `new "
                  + argTypes[k].className + "(...)` (a heap reference) instead");
    }
    // Record the argument permutation for codegen when the call used named arguments (a purely
    // positional call keeps codegen's identity-order + trailing-default path).
    if (hasNames && nodeKey) callArgOrder_[nodeKey] = order;
    return chosen;
}

Type SemanticAnalyzer::analyzeIdentifier(const IdentifierExpr& identifier) {
    const Symbol* sym = symbolTable.lookup(identifier.name.lexeme);
    if (!sym) {
        // Implicit `this`: a bare name may refer to a member of the enclosing class
        // (lowest priority — only when no local/param/function shadows it).
        if (const ClassInfo::Field* f = currentInstanceField(identifier.name.lexeme))
            return f->type;
        if (const Type* sft = currentStaticFieldType(identifier.name.lexeme))
            return *sft;
        error(identifier.name, "use of undeclared identifier '" + identifier.name.lexeme + "'");
        return Type{TypeKind::Error};
    }
    if (sym->kind == Symbol::Kind::Function) {
        error(identifier.name, "cannot use function '" + identifier.name.lexeme + "' as a value");
        return Type{TypeKind::Error};
    }
    if (!sym->isInitialized) {
        error(identifier.name, "variable '" + identifier.name.lexeme
              + "' is used before it has been assigned a value");
        // Return the declared type anyway so downstream analysis uses the right type
        // and does not cascade into spurious "undeclared identifier" errors.
    }
    // Smart-cast: a nullable binding proven non-null on this path reads as its non-null form.
    // Recording this narrowed type on the use-node (via the dispatcher's recordType) is what lets
    // member access / passing / etc. treat it as `T` — and, for references, needs no codegen change.
    if (sym->isNarrowedNonNull && sym->type.isNullable)
        return stripNullable(sym->type);
    // Reading a `ref <primitive>` yields the primitive value (lvalue-to-rvalue deref). The symbol
    // keeps its borrow type (so assignment writes through and the codegen loads correctly).
    return decayPrimitiveBorrow(sym->type);
}

bool SemanticAnalyzer::incDecTargetOk(const Token& op, const std::string& name) {
    if (const Symbol* sym = symbolTable.lookup(name)) {
        // '++'/'--' through a `ref <primitive>` is not supported yet (the write would need to go
        // through the referent). Point at the explicit form.
        if (isPrimitiveBorrow(sym->type)) {
            error(op, "'" + op.lexeme + "' through a borrow ('" + typeName(sym->type)
                  + "') is not supported; write it out, e.g. `" + name + " = " + name + " + 1`");
            return false;
        }
        if (sym->kind == Symbol::Kind::Variable && !sym->isMutable) {
            error(op, "cannot mutate immutable variable '" + name + "'; declare it 'mut' to allow mutation");
            return false;
        }
        return true;
    }
    // Implicit `this` field: `++`/`--` always mutates → needs a mut field in a mut method.
    if (const ClassInfo::Field* f = currentInstanceField(name)) {
        if (!f->isMut) {
            error(op, "cannot mutate immutable field '" + name + "'; declare it 'mut'");
            return false;
        }
        if (!currentThisMutable) {
            error(op, "cannot write to field '" + name + "' in a read-only method; declare the method 'mut'");
            return false;
        }
    }
    // Implicit `this` static field: `++`/`--` mutates → needs a `mut static` field.
    if (const ClassInfo::StaticField* sf = currentStaticField(name)) {
        if (!sf->isMut) {
            error(op, "cannot mutate immutable static field '" + name
                  + "'; declare it 'mut static' to allow mutation");
            return false;
        }
    }
    // not-a-field → analyzeExpr(operand) already reported it.
    return true;
}

Type SemanticAnalyzer::analyzeUnary(const UnaryExpr& unary) {
    Type operandType = decayPrimitiveBorrow(analyzeExpr(*unary.operand));

    switch (unary.operatorToken.type) {
        case TokenType::BANG:
            if (!isError(operandType) && !isBoolCompatible(operandType)) {
                error(unary.operatorToken, "operand of '!' must be bool-compatible, got " + typeName(operandType));
                return Type{TypeKind::Error};
            }
            return Type{TypeKind::Bool};

        case TokenType::MINUS:
            // Generic body-check: unary '-' on a type parameter requires a `Neg` bound.
            if (const std::vector<std::string>* bounds = typeParamBoundsOf(operandType)) {
                if (bounds->empty()) return Type{TypeKind::Error};
                if (std::find(bounds->begin(), bounds->end(), "Neg") != bounds->end())
                    return makeTypeParam(operandType.className);
                error(unary.operatorToken, "unary '-' on type parameter '" + operandType.className
                      + "' requires bound 'Neg'");
                return Type{TypeKind::Error};
            }
            // Operator overloading: unary '-' on a class → the Neg trait's `neg` method.
            if (operandType.kind == TypeKind::Object || operandType.kind == TypeKind::Reference) {
                auto implIt = implementedTraits.find(operandType.className);
                if (implIt == implementedTraits.end() || !implIt->second.count("Neg")) {
                    error(unary.operatorToken, "type '" + operandType.className
                          + "' does not implement 'Neg' for unary '-'");
                    return Type{TypeKind::Error};
                }
                ClassInfo& info = classRegistry.at(operandType.className);
                auto mit = info.methods.find("neg");
                int idx = (mit == info.methods.end()) ? -1 : pickOverloadByArgs(mit->second, {});
                if (idx < 0) {
                    error(unary.operatorToken, "no matching 'neg' method on '" + operandType.className + "'");
                    return Type{TypeKind::Error};
                }
                const ClassInfo::Method& m = mit->second[idx];
                if (mit->second.size() > 1)
                    resolvedCallee[&unary] = mangleOverload(operandType.className + "_neg",
                                                            m.paramTypes, m.returnType);
                return m.returnType;
            }
            if (!isError(operandType) && !isNumeric(operandType.kind)) {
                error(unary.operatorToken, "operand of unary '-' must be numeric, got " + typeName(operandType));
                return Type{TypeKind::Error};
            }
            return operandType;

        case TokenType::TILDE:
            if (!isError(operandType) && !isInteger(operandType.kind)) {
                error(unary.operatorToken, "operand of '~' must be integer, got " + typeName(operandType));
                return Type{TypeKind::Error};
            }
            return operandType;

        case TokenType::INCREMENT:
        case TokenType::DECREMENT: {
            if (!std::holds_alternative<IdentifierExpr>(*unary.operand->node)) {
                error(unary.operatorToken, "operand of '" + unary.operatorToken.lexeme + "' must be an identifier");
                return Type{TypeKind::Error};
            }
            if (!isError(operandType) && !isNumeric(operandType.kind)) {
                error(unary.operatorToken, "operand of '" + unary.operatorToken.lexeme + "' must be numeric, got "
                      + typeName(operandType));
                return Type{TypeKind::Error};
            }
            // '++'/'--' mutate an existing value, so the target must be `mut`.
            const auto& ident = std::get<IdentifierExpr>(*unary.operand->node);
            if (!incDecTargetOk(unary.operatorToken, ident.name.lexeme))
                return Type{TypeKind::Error};
            return operandType;
        }

        default:
            return Type{TypeKind::Error};
    }
}

// Pick the best-matching overload in `set` for the given argument types (exact > widening >
// narrowing), or -1 if none is viable. A lightweight resolver for operator desugaring.
static int pickOverloadByArgs(const std::vector<ClassInfo::Method>& set,
                              const std::vector<Type>& argTypes) {
    int best = -1, bestCost = 0;
    for (int i = 0; i < static_cast<int>(set.size()); ++i) {
        const ClassInfo::Method& m = set[i];
        if (m.paramTypes.size() != argTypes.size()) continue;
        bool ok = true;
        int  cost = 0;
        for (size_t k = 0; k < argTypes.size(); ++k) {
            if (argTypes[k] == m.paramTypes[k]) continue;
            CastResult cr = canPassArgument(argTypes[k], m.paramTypes[k]);  // incl. value-object borrow
            if (cr == CastResult::None) { ok = false; break; }
            cost += (cr == CastResult::Warn) ? 2 : 1;
        }
        if (ok && (best < 0 || cost < bestCost)) { best = i; bestCost = cost; }
    }
    return best;
}

Type SemanticAnalyzer::classifyEquality(const Type& leftType, const Type& rightType,
                                        const void* nodeKey, const Token& at,
                                        const std::string& what) {
    // Comparison with `null`: a machine-pointer identity check (like the enum/reference-identity
    // path). Valid against a reference-like value (Reference / Enum — both lower to `ptr`).
    if (leftType.kind == TypeKind::Null || rightType.kind == TypeKind::Null) {
        const Type& other = (leftType.kind == TypeKind::Null) ? rightType : leftType;
        // Reference-like (`ptr`) → pointer identity against null (recorded for codegen). A nullable
        // primitive is NOT recorded — genBinary detects the Null operand and tests its tag directly.
        bool refLike = other.kind == TypeKind::Reference || other.kind == TypeKind::Enum;
        bool optPrim = other.isNullable && (isNumeric(other.kind) || other.kind == TypeKind::Bool
                                            || other.kind == TypeKind::Char);
        if (other.kind == TypeKind::Null || refLike) {
            addressIdentityCmp_.insert(nodeKey);   // codegen: `icmp eq/ne ptr …, null`
            return Type{TypeKind::Bool};
        }
        if (optPrim) return Type{TypeKind::Bool};   // codegen tests the tag
        error(at, "cannot compare a value of type '" + typeName(other) + "' with 'null'");
        return Type{TypeKind::Error};
    }
    // Class operands: an `Eq` impl overrides; otherwise default equality —
    //   • two REFERENCES of the same class → address identity (`icmp eq/ne ptr`);
    //   • at least one VALUE object of the same class → memberwise structural equality.
    if (leftType.kind == TypeKind::Object || leftType.kind == TypeKind::Reference) {
        auto implIt = implementedTraits.find(leftType.className);
        bool hasEq  = implIt != implementedTraits.end() && implIt->second.count("Eq");
        if (!hasEq) {
            if ((rightType.kind == TypeKind::Object || rightType.kind == TypeKind::Reference)
                && rightType.className == leftType.className) {
                if (leftType.kind == TypeKind::Reference && rightType.kind == TypeKind::Reference)
                    addressIdentityCmp_.insert(nodeKey);
                else
                    structuralValueCmp_.insert(nodeKey);
                return Type{TypeKind::Bool};
            }
            error(at, "type '" + leftType.className + "' does not implement 'Eq' for " + what);
            return Type{TypeKind::Error};
        }
        ClassInfo& info = classRegistry.at(leftType.className);
        auto mit = info.methods.find("eq");
        int idx = (mit == info.methods.end()) ? -1 : pickOverloadByArgs(mit->second, { rightType });
        if (idx < 0) {
            error(at, "no matching 'eq' method on '" + leftType.className + "' for " + what);
            return Type{TypeKind::Error};
        }
        const ClassInfo::Method& m = mit->second[idx];
        if (mit->second.size() > 1)
            resolvedCallee[nodeKey] = mangleOverload(leftType.className + "_eq",
                                                     m.paramTypes, m.returnType);
        return Type{TypeKind::Bool};
    }
    // Enum identity comparison: both operands must be the same enum type.
    if (leftType.kind == TypeKind::Enum || rightType.kind == TypeKind::Enum) {
        bool sameEnum = leftType.kind == TypeKind::Enum && rightType.kind == TypeKind::Enum
                     && leftType.className == rightType.className;
        if (!sameEnum) {
            error(at, "incompatible types in " + what + ": "
                  + typeName(leftType) + " and " + typeName(rightType));
            return Type{TypeKind::Error};
        }
        return Type{TypeKind::Bool};
    }
    // `str` has no equality yet (Phase 1). Reject cleanly rather than fall through to the numeric
    // compare below, which would emit a bogus `icmp` on the { ptr, i64 } view.
    if (leftType.kind == TypeKind::Str || rightType.kind == TypeKind::Str) {
        error(at, "'==' / '!=' is not supported on 'str' yet — build a 'String' to compare (" + what + ")");
        return Type{TypeKind::Error};
    }
    // Primitives (numeric widen to a common type; bool/char match exactly).
    bool compatible = (leftType.kind == rightType.kind)
                   || (isNumeric(leftType.kind) && isNumeric(rightType.kind));
    if (!compatible) {
        error(at, "incompatible types in " + what + ": "
              + typeName(leftType) + " and " + typeName(rightType));
        return Type{TypeKind::Error};
    }
    return Type{TypeKind::Bool};
}

// A bare numeric literal `5` / `2.5`, optionally negated (`-5`) — the operands that participate in
// literal-type adoption. (A negated literal is a UnaryExpr(-) over a NUMBER literal; analyzeUnary
// does not clear the expected type, so the inner literal still adopts.)
static bool isNumericLiteralOperand(const Expr& e) {
    if (const auto* lit = std::get_if<LiteralExpr>(e.node.get()))
        return lit->token.type == TokenType::NUMBER;
    if (const auto* un = std::get_if<UnaryExpr>(e.node.get()))
        if (un->operatorToken.type == TokenType::MINUS && un->operand)
            if (const auto* lit = std::get_if<LiteralExpr>(un->operand->node.get()))
                return lit->token.type == TokenType::NUMBER;
    return false;
}

Type SemanticAnalyzer::analyzeBinary(const BinaryExpr& binary) {
    // The contextual expected type applies to the binary result, NOT to its operands, so it is
    // cleared here — a bare literal keeps its default type (this confines the outer binding's type
    // to *direct* bindings, and keeps `1 / 2` integer division even in an `f64` slot).
    //
    // EXCEPTION: a bare numeric literal operand adopts the type of a *non-literal* numeric sibling,
    // so `bigI64 == 6000000000` / `bigI64 + 6000000000` type the literal as i64 rather than the
    // wrapping i32 default. The both-literals case (`1 / 2`) has no non-literal sibling, so it is
    // untouched; a *decimal* literal never adopts an integer sibling (analyzeLiteral keeps it f64),
    // so `1.0 / count` stays float division.
    std::optional<Type> savedExpected = expectedType_;
    expectedType_ = std::nullopt;

    // A `ref <primitive>` operand decays to its value (deref) before any numeric / operator rule —
    // otherwise a borrow (kind == Reference) would be misrouted into class operator overloading.
    bool leftLit  = isNumericLiteralOperand(*binary.left);
    bool rightLit = isNumericLiteralOperand(*binary.right);
    Type leftType, rightType;
    auto isNum = [](const Type& t) { return isInteger(t.kind) || isFloat(t.kind); };
    if (leftLit && !rightLit) {
        rightType = decayPrimitiveBorrow(analyzeExpr(*binary.right));
        leftType  = decayPrimitiveBorrow(isNum(rightType)
                        ? analyzeWithExpected(*binary.left, rightType)
                        : analyzeExpr(*binary.left));
    } else if (rightLit && !leftLit) {
        leftType  = decayPrimitiveBorrow(analyzeExpr(*binary.left));
        rightType = decayPrimitiveBorrow(isNum(leftType)
                        ? analyzeWithExpected(*binary.right, leftType)
                        : analyzeExpr(*binary.right));
    } else {
        leftType  = decayPrimitiveBorrow(analyzeExpr(*binary.left));
        rightType = decayPrimitiveBorrow(analyzeExpr(*binary.right));
    }
    expectedType_ = savedExpected;

    if (isError(leftType) || isError(rightType)) return Type{TypeKind::Error};

    // Generic body-check: an operator on a value of a type parameter requires the matching
    // operator trait among the parameter's bounds. Unbounded ⇒ permissive (suppressed).
    if (const std::vector<std::string>* bounds = typeParamBoundsOf(leftType)) {
        if (bounds->empty()) return Type{TypeKind::Error};
        const auto* ot = operatorTraitFor(binary.operatorToken.type);
        if (ot) {
            const std::string& trait = ot->first;
            if (std::find(bounds->begin(), bounds->end(), trait) != bounds->end())
                return (trait == "Eq" || trait == "Ord") ? Type{TypeKind::Bool}
                                                         : makeTypeParam(leftType.className);
            error(binary.operatorToken, "operator '" + binary.operatorToken.lexeme
                  + "' on type parameter '" + leftType.className + "' requires bound '" + trait + "'");
            return Type{TypeKind::Error};
        }
        error(binary.operatorToken, "operator '" + binary.operatorToken.lexeme
              + "' is not available on type parameter '" + leftType.className + "'");
        return Type{TypeKind::Error};
    }

    // Equality (==/!=) goes through the shared classifier (Eq-impl / reference identity / value
    // structural / enum / primitive) — the same decision switch case labels reuse.
    if (binary.operatorToken.type == TokenType::EQUAL_EQUAL
        || binary.operatorToken.type == TokenType::BANG_EQUAL) {
        return classifyEquality(leftType, rightType, &binary, binary.operatorToken,
                                "operator '" + binary.operatorToken.lexeme + "'");
    }

    // Operator overloading (non-equality): if the left operand is a class, desugar to its trait
    // method (Add/Sub/Mul/Div/Rem/Ord).
    if (leftType.kind == TypeKind::Object || leftType.kind == TypeKind::Reference) {
        if (const auto* ot = operatorTraitFor(binary.operatorToken.type)) {
            const std::string trait  = ot->first;
            const std::string method = ot->second;
            auto implIt = implementedTraits.find(leftType.className);
            if (implIt == implementedTraits.end() || !implIt->second.count(trait)) {
                error(binary.operatorToken, "type '" + leftType.className + "' does not implement '"
                      + trait + "' for operator '" + binary.operatorToken.lexeme + "'");
                return Type{TypeKind::Error};
            }
            ClassInfo& info = classRegistry.at(leftType.className);
            auto mit = info.methods.find(method);
            int idx = (mit == info.methods.end()) ? -1 : pickOverloadByArgs(mit->second, { rightType });
            if (idx < 0) {
                error(binary.operatorToken, "no matching '" + method + "' method on '"
                      + leftType.className + "' for operator '" + binary.operatorToken.lexeme + "'");
                return Type{TypeKind::Error};
            }
            const ClassInfo::Method& m = mit->second[idx];
            if (mit->second.size() > 1)
                resolvedCallee[&binary] = mangleOverload(leftType.className + "_" + method,
                                                         m.paramTypes, m.returnType);
            return (trait == "Ord") ? Type{TypeKind::Bool} : m.returnType;
        }
    }

    switch (binary.operatorToken.type) {
        // Arithmetic
        case TokenType::PLUS:
        case TokenType::MINUS:
        case TokenType::STAR:
        case TokenType::SLASH:
        case TokenType::PERCENT: {
            if (!isNumeric(leftType.kind)) {
                error(binary.operatorToken, "left operand of '" + binary.operatorToken.lexeme + "' must be numeric, got "
                      + typeName(leftType));
                return Type{TypeKind::Error};
            }
            if (!isNumeric(rightType.kind)) {
                error(binary.operatorToken, "right operand of '" + binary.operatorToken.lexeme + "' must be numeric, got "
                      + typeName(rightType));
                return Type{TypeKind::Error};
            }
            return commonArithmeticType(leftType, rightType);
        }

        // Bitwise (& | ^) and shifts (<< >>).
        case TokenType::PIPE:
        case TokenType::CARET:
        case TokenType::AMPERSAND:
        case TokenType::SHIFT_LEFT:
        case TokenType::SHIFT_RIGHT: {
            if (!isInteger(leftType.kind)) {
                error(binary.operatorToken, "left operand of '" + binary.operatorToken.lexeme + "' must be integer, got "
                      + typeName(leftType));
                return Type{TypeKind::Error};
            }
            if (!isInteger(rightType.kind)) {
                error(binary.operatorToken, "right operand of '" + binary.operatorToken.lexeme + "' must be integer, got "
                      + typeName(rightType));
                return Type{TypeKind::Error};
            }
            // A shift is asymmetric: its result type AND its arithmetic-vs-logical behaviour
            // (`ashr` vs `lshr`) follow the LEFT operand only — so it takes the left operand's type,
            // NOT the symmetric common type (the shift count is coerced to it in codegen, as LLVM
            // requires equal operand types). Symmetric bitwise ops keep the common type.
            bool isShift = binary.operatorToken.type == TokenType::SHIFT_LEFT
                        || binary.operatorToken.type == TokenType::SHIFT_RIGHT;
            return isShift ? leftType : commonArithmeticType(leftType, rightType);
        }

        // Ordering comparisons
        case TokenType::LESS:
        case TokenType::LESS_EQUAL:
        case TokenType::GREATER:
        case TokenType::GREATER_EQUAL: {
            if (!isNumeric(leftType.kind))
                error(binary.operatorToken, "left operand of '" + binary.operatorToken.lexeme + "' must be numeric, got "
                      + typeName(leftType));
            if (!isNumeric(rightType.kind))
                error(binary.operatorToken, "right operand of '" + binary.operatorToken.lexeme + "' must be numeric, got "
                      + typeName(rightType));
            return Type{TypeKind::Bool};
        }

        // Logical
        case TokenType::AND:
        case TokenType::OR: {
            if (!isBoolCompatible(leftType))
                error(binary.operatorToken, "left operand of '" + binary.operatorToken.lexeme
                      + "' must be bool-compatible, got " + typeName(leftType));
            if (!isBoolCompatible(rightType))
                error(binary.operatorToken, "right operand of '" + binary.operatorToken.lexeme
                      + "' must be bool-compatible, got " + typeName(rightType));
            return Type{TypeKind::Bool};
        }

        default:
            return Type{TypeKind::Error};
    }
}

// ---- switch analysis ----

// If `e` is an enum-variant label (`Enum.VARIANT`), return the variant name; else "".
static std::string enumVariantOfLabel(const Expr& e) {
    if (const auto* ma = std::get_if<MemberAccessExpr>(e.node.get()))
        return ma->field.lexeme;
    return "";
}

// True if every normal-completion path through the arm block ends in `yield` or `return`.
static bool blockAlwaysYields(const Stmt& stmt);
static bool blockAlwaysYields(const BlockStmt& block) {
    for (const auto& s : block.body)
        if (blockAlwaysYields(*s)) return true;
    return false;
}
static bool blockAlwaysYields(const Stmt& stmt) {
    return std::visit(overloaded{
        [](const YieldStmt&)       { return true; },
        [](const ReturnStmt&)      { return true; },   // exits the function — no fall-through value
        [](const BlockStmt& b)     { return blockAlwaysYields(b); },
        [](const IfStmt& i)        {
            return i.elseBranch != nullptr
                   && blockAlwaysYields(*i.thenBranch)
                   && blockAlwaysYields(*i.elseBranch);
        },
        [](const SwitchStmt& sw)   {
            bool hasDefault = false;
            for (const SwitchArm& arm : sw.arms) {
                if (arm.isDefault) hasDefault = true;
                if (!(arm.block && blockAlwaysYields(*arm.block))) return false;
            }
            return hasDefault;
        },
        [](const auto&)            { return false; },
    }, *stmt.node);
}

// A canonical key for a compile-time-identifiable case label (for duplicate detection), or "" if
// the label can't be identified at compile time (arbitrary expression / value-object value).
static std::string labelKey(const Expr& e) {
    const auto& node = *e.node;
    if (const auto* lit = std::get_if<LiteralExpr>(&node)) {
        switch (lit->token.type) {
            case TokenType::NUMBER:
                try { return "int:" + std::to_string(std::stoll(lit->token.lexeme, nullptr, 0)); }
                catch (...) { return "num:" + lit->token.lexeme; }
            case TokenType::TRUE:   return "bool:1";
            case TokenType::FALSE:  return "bool:0";
            case TokenType::CHAR:   return "char:" + lit->token.lexeme;
            case TokenType::STRING: return "str:" + lit->token.lexeme;
            default:                return "";
        }
    }
    // Negated integer literal (`case -1`) — a UnaryExpr over a NUMBER.
    if (const auto* un = std::get_if<UnaryExpr>(&node)) {
        if (un->operatorToken.type == TokenType::MINUS && un->operand && un->operand->node) {
            if (const auto* lit = std::get_if<LiteralExpr>(un->operand->node.get()))
                if (lit->token.type == TokenType::NUMBER)
                    try { return "int:" + std::to_string(-std::stoll(lit->token.lexeme, nullptr, 0)); }
                    catch (...) { return "num:-" + lit->token.lexeme; }
        }
        return "";
    }
    // Enum variant (`Enum.VARIANT`) — within a switch the scrutinee is one enum type, so the
    // variant name alone is a unique key.
    if (const auto* ma = std::get_if<MemberAccessExpr>(&node))
        return "mem:" + ma->field.lexeme;
    // A bare identifier label (`case lo`) — catches obvious copy-paste duplicates.
    if (const auto* id = std::get_if<IdentifierExpr>(&node))
        return "id:" + id->name.lexeme;
    return "";
}

void SemanticAnalyzer::checkDuplicateLabels(const std::deque<SwitchArm>& arms) {
    std::unordered_set<std::string> seen;
    for (const SwitchArm& arm : arms) {
        for (const auto& label : arm.labels) {
            std::string key = labelKey(*label);
            if (key.empty()) continue;   // not compile-time identifiable — skip
            if (!seen.insert(key).second)
                error(exprFirstToken(*label), "duplicate case label in switch");
        }
    }
}

void SemanticAnalyzer::analyzeSwitchArm(const SwitchArm& arm, const Type& scrutineeType,
                                        Type* expectedResult, const Token& switchTok) {
    // Labels: each must be comparable to the scrutinee under the same rules as `==`.
    for (const auto& label : arm.labels) {
        Type labelType = analyzeExpr(*label);
        if (!isError(labelType) && !isError(scrutineeType))
            // Called for its side effects (records the comparison machinery codegen reads back for
            // this label node); the returned type isn't needed here — a case compare is always bool.
            (void)classifyEquality(scrutineeType, labelType, label->node.get(), switchTok, "switch case");
    }

    enterScope();
    if (expectedResult) {
        // Expression form: the arm must produce a value.
        if (arm.valueExpr) {
            Type v = (expectedResult->kind == TypeKind::Error)
                       ? analyzeExpr(*arm.valueExpr)
                       : analyzeWithExpected(*arm.valueExpr, *expectedResult);
            if (expectedResult->kind == TypeKind::Error && !isError(v))
                *expectedResult = v;   // infer the result type from the first concrete arm
            else if (!isError(*expectedResult) && !isError(v))
                checkCast(v, *expectedResult, exprFirstToken(*arm.valueExpr), "switch arm value");
        } else if (arm.block) {
            switchExprResultStack_.push_back(*expectedResult);
            analyzeStmt(*arm.block);
            switchExprResultStack_.pop_back();
            if (!blockAlwaysYields(*arm.block))
                error(arm.arrow, "every path through a switch-expression block arm must 'yield' a value");
        }
    } else {
        // Statement form: value discarded.
        if (arm.valueExpr)  analyzeExpr(*arm.valueExpr);
        else if (arm.block) analyzeStmt(*arm.block);
    }
    exitScope();
}

Type SemanticAnalyzer::analyzeSwitchExpr(const SwitchExpr& switchExpr) {
    Type scrutineeType = analyzeExpr(*switchExpr.scrutinee);
    checkDuplicateLabels(switchExpr.arms);

    // Result type: the contextual expected type if available; otherwise inferred from the arms.
    Type result = expectedType_.has_value() ? *expectedType_ : Type{TypeKind::Error};

    // A value object can't be produced by value (would need sret/clone) — require a reference.
    if (result.kind == TypeKind::Object) {
        error(switchExpr.keyword,
              "a switch expression cannot produce a value object '" + result.className
              + "'; use a reference");
        result = Type{TypeKind::Error};
    }

    bool hasDefault = false;
    std::unordered_set<std::string> covered;
    for (const SwitchArm& arm : switchExpr.arms) {
        if (arm.isDefault) { hasDefault = true; continue; }
        for (const auto& label : arm.labels) {
            std::string v = enumVariantOfLabel(*label);
            if (!v.empty()) covered.insert(v);
        }
    }

    for (const SwitchArm& arm : switchExpr.arms)
        analyzeSwitchArm(arm, scrutineeType, &result, switchExpr.keyword);

    // Exhaustiveness: an explicit `default`, or (enum scrutinee) full variant coverage.
    bool enumExhaustive = false;
    if (scrutineeType.kind == TypeKind::Enum) {
        auto eit = enumRegistry.find(scrutineeType.className);
        if (eit != enumRegistry.end())
            enumExhaustive = (covered == eit->second.variantSet);
    }
    if (!hasDefault && !enumExhaustive)
        error(switchExpr.keyword,
              std::string("a switch expression must be exhaustive: add a 'default' arm")
              + (scrutineeType.kind == TypeKind::Enum ? " or cover every enum variant" : ""));

    return isError(result) ? Type{TypeKind::Error} : result;
}

Type SemanticAnalyzer::analyzeAssign(const AssignExpr& assign) {
    const Symbol* sym = symbolTable.lookup(assign.name.lexeme);
    if (!sym) {
        // Implicit `this`: a bare name may be a field of the enclosing class.
        if (const ClassInfo::Field* f = currentInstanceField(assign.name.lexeme)) {
            if (!f->isMut && !inConstructor) {
                error(assign.name, "cannot assign to immutable field '" + assign.name.lexeme
                      + "'; declare it 'mut'");
                analyzeExpr(*assign.value);
                return f->type;
            }
            if (!currentThisMutable) {
                error(assign.name, "cannot write to field '" + assign.name.lexeme
                      + "' in a read-only method; declare the method 'mut'");
                analyzeExpr(*assign.value);
                return f->type;
            }
            Type rhs = analyzeWithExpected(*assign.value, f->type);
            checkCast(rhs, f->type, assign.name, "field assignment");
            return f->type;
        }
        if (const ClassInfo::StaticField* sf = currentStaticField(assign.name.lexeme)) {
            if (!sf->isMut)
                error(assign.name, "cannot assign to immutable static field '" + assign.name.lexeme
                      + "'; declare it 'mut static' to allow reassignment");
            Type rhs = analyzeWithExpected(*assign.value, sf->type);
            checkCast(rhs, sf->type, assign.name, "static field assignment");
            return sf->type;
        }
        error(assign.name, "use of undeclared identifier '" + assign.name.lexeme + "'");
        analyzeExpr(*assign.value);
        return Type{TypeKind::Error};
    }
    if (sym->kind == Symbol::Kind::Function) {
        error(assign.name, "cannot assign to function '" + assign.name.lexeme + "'");
        analyzeExpr(*assign.value);
        return Type{TypeKind::Error};
    }

    // Assigning to a primitive borrow (`i32*`) writes THROUGH the borrow (like C++ `int& r; r = 5;`);
    // it does not rebind. Requires a mutable borrow (`mut i32*`); the value must match the element.
    if (isPrimitiveBorrow(sym->type)) {
        if (!sym->isMutable) {
            error(assign.name, "cannot write through a shared borrow '" + assign.name.lexeme
                  + "'; declare it 'mut " + typeName(sym->type) + "' to allow writing to the borrowed value");
            analyzeExpr(*assign.value);
            return borrowElementType(sym->type);
        }
        Type elem = borrowElementType(sym->type);
        Type rhs  = analyzeWithExpected(*assign.value, elem);
        checkCast(rhs, elem, assign.name, "write through borrow");
        return elem;
    }

    // Object/reference parameters may not be *rebound* (obj = ...), even when declared
    // `mut` — a reference parameter is a borrow, so rebinding it would corrupt refcounts.
    // (`mut` on such a parameter only unlocks writes to the object's mut fields.)
    if (sym->isParameter
        && (sym->type.kind == TypeKind::Object || sym->type.kind == TypeKind::Reference)) {
        std::string kindWord = sym->type.kind == TypeKind::Object ? "object" : "reference";
        error(assign.name, "cannot rebind " + kindWord + " parameter '" + assign.name.lexeme
              + "': it is a borrow, not an owning binding");
        analyzeExpr(*assign.value);
        return sym->type;
    }

    // Const bindings permit exactly one defining assignment: allowed only while the
    // variable is not yet initialized. `mut` bindings may be reassigned freely.
    if (!sym->isMutable && sym->isInitialized) {
        error(assign.name, "cannot reassign immutable variable '" + assign.name.lexeme
              + "'; declare it 'mut' to allow reassignment");
        analyzeExpr(*assign.value);
        return sym->type;
    }

    Type lhsType = sym->type;
    Type rhsType = analyzeWithExpected(*assign.value, lhsType);
    checkCast(rhsType, lhsType, assign.name, "assignment");
    // Rebinding a `mut` reference from a read-only reference is a const→mut coercion.
    if (sym->isMutable)
        warnConstToMut(assign.name, *assign.value, lhsType);
    // Any successful assignment makes the variable definitely initialized.
    if (Symbol* mut = symbolTable.lookupMutable(assign.name.lexeme)) {
        mut->isInitialized = true;
        // Smart-cast invalidation/refresh: after a reassignment, the binding is known non-null
        // only if the assigned value is itself non-null; assigning `null` or a `T?` drops it back.
        if (mut->type.isNullable)
            mut->isNarrowedNonNull = (!rhsType.isNullable && rhsType.kind != TypeKind::Null);
    }
    return lhsType;
}

Type SemanticAnalyzer::analyzeCompoundAssign(const CompoundAssignExpr& compoundAssign) {
    const Symbol* sym = symbolTable.lookup(compoundAssign.name.lexeme);
    Type lhsType;
    if (sym) {
        if (sym->kind == Symbol::Kind::Function) {
            error(compoundAssign.name, "cannot assign to function '" + compoundAssign.name.lexeme + "'");
            analyzeExpr(*compoundAssign.value);
            return Type{TypeKind::Error};
        }
        // Compound assignment always mutates an existing value, so the target must be `mut`.
        if (!sym->isMutable) {
            error(compoundAssign.name, "cannot mutate immutable variable '" + compoundAssign.name.lexeme
                  + "'; declare it 'mut' to allow mutation");
            analyzeExpr(*compoundAssign.value);
            return sym->type;
        }
        // Compound assignment reads the variable before writing — check initialization.
        if (!sym->isInitialized) {
            error(compoundAssign.name, "variable '" + compoundAssign.name.lexeme
                  + "' is used before it has been assigned a value");
        }
        lhsType = sym->type;
    } else if (const ClassInfo::Field* f = currentInstanceField(compoundAssign.name.lexeme)) {
        // Implicit `this.field op= v` — always mutates, so needs a mut field in a mut method.
        if (!f->isMut) {
            error(compoundAssign.name, "cannot mutate immutable field '" + compoundAssign.name.lexeme
                  + "'; declare it 'mut'");
            analyzeExpr(*compoundAssign.value);
            return f->type;
        }
        if (!currentThisMutable) {
            error(compoundAssign.name, "cannot write to field '" + compoundAssign.name.lexeme
                  + "' in a read-only method; declare the method 'mut'");
            analyzeExpr(*compoundAssign.value);
            return f->type;
        }
        lhsType = f->type;
    } else if (const ClassInfo::StaticField* sf = currentStaticField(compoundAssign.name.lexeme)) {
        if (!sf->isMut)
            error(compoundAssign.name, "cannot modify immutable static field '"
                  + compoundAssign.name.lexeme + "'; declare it 'mut static' to allow mutation");
        lhsType = sf->type;
    } else {
        error(compoundAssign.name, "use of undeclared identifier '" + compoundAssign.name.lexeme + "'");
        analyzeExpr(*compoundAssign.value);
        return Type{TypeKind::Error};
    }
    Type rhsType = analyzeExpr(*compoundAssign.value);

    if (isError(lhsType) || isError(rhsType)) return Type{TypeKind::Error};

    // Compound assignment through a `ref <primitive>` is not supported yet (the write would need
    // to go through the referent, not rebind). Point at the explicit form.
    if (isPrimitiveBorrow(lhsType)) {
        error(compoundAssign.operatorToken, "compound assignment through a borrow ('"
              + typeName(lhsType) + "') is not supported; write it out, e.g. `"
              + compoundAssign.name.lexeme + " = " + compoundAssign.name.lexeme + " + ...`");
        return borrowElementType(lhsType);
    }

    bool isArith =
        compoundAssign.operatorToken.type == TokenType::PLUS_EQUAL   ||
        compoundAssign.operatorToken.type == TokenType::MINUS_EQUAL  ||
        compoundAssign.operatorToken.type == TokenType::STAR_EQUAL   ||
        compoundAssign.operatorToken.type == TokenType::SLASH_EQUAL  ||
        compoundAssign.operatorToken.type == TokenType::PERCENT_EQUAL;

    bool isBitw =
        compoundAssign.operatorToken.type == TokenType::CARET_EQUAL     ||
        compoundAssign.operatorToken.type == TokenType::AMPERSAND_EQUAL ||
        compoundAssign.operatorToken.type == TokenType::PIPE_EQUAL;

    if (isArith) {
        if (!isNumeric(lhsType.kind))
            error(compoundAssign.operatorToken, "left operand of '" + compoundAssign.operatorToken.lexeme
                  + "' must be numeric, got " + typeName(lhsType));
        if (!isNumeric(rhsType.kind))
            error(compoundAssign.operatorToken, "right operand of '" + compoundAssign.operatorToken.lexeme
                  + "' must be numeric, got " + typeName(rhsType));
    } else if (isBitw) {
        if (!isInteger(lhsType.kind))
            error(compoundAssign.operatorToken, "left operand of '" + compoundAssign.operatorToken.lexeme
                  + "' must be integer, got " + typeName(lhsType));
        if (!isInteger(rhsType.kind))
            error(compoundAssign.operatorToken, "right operand of '" + compoundAssign.operatorToken.lexeme
                  + "' must be integer, got " + typeName(rhsType));
    }

    checkCast(rhsType, lhsType, compoundAssign.operatorToken, "compound assignment");
    // Compound assignment writes to the variable — mark it as definitely initialized.
    if (Symbol* mut = symbolTable.lookupMutable(compoundAssign.name.lexeme))
        mut->isInitialized = true;
    return lhsType;
}

Type SemanticAnalyzer::analyzePostfix(const PostfixExpr& postfix) {
    Type operandType = analyzeExpr(*postfix.operand);

    if (!std::holds_alternative<IdentifierExpr>(*postfix.operand->node)) {
        error(postfix.operatorToken, "operand of '" + postfix.operatorToken.lexeme + "' must be an identifier");
        return Type{TypeKind::Error};
    }
    if (!isError(operandType) && !isNumeric(operandType.kind)) {
        error(postfix.operatorToken, "operand of '" + postfix.operatorToken.lexeme + "' must be numeric, got "
              + typeName(operandType));
        return Type{TypeKind::Error};
    }
    // Postfix '++'/'--' mutate an existing value, so the target must be `mut`.
    const auto& ident = std::get<IdentifierExpr>(*postfix.operand->node);
    if (!incDecTargetOk(postfix.operatorToken, ident.name.lexeme))
        return Type{TypeKind::Error};
    // Postfix writes back to the variable — mark as initialized to suppress
    // cascading "uninitialized" errors on subsequent reads.
    if (Symbol* mut = symbolTable.lookupMutable(ident.name.lexeme))
        mut->isInitialized = true;
    return operandType;
}

Type SemanticAnalyzer::analyzeCall(const CallExpr& call) {
    // Enums cannot be constructed directly — variants are the only instances.
    if (enumRegistry.count(call.callee.lexeme)) {
        error(call.callee, "cannot construct enum '" + call.callee.lexeme
              + "' directly; use one of its variants");
        for (const auto& arg : call.args) analyzeExpr(*arg);
        return makeEnumType(call.callee.lexeme);
    }
    const std::string& name = call.callee.lexeme;

    // Constructor call: callee is a class name → resolve among the constructor overloads.
    if (classRegistry.count(name)) {
        const ClassInfo& cls = classRegistry.at(name);
        auto ctorIt = cls.methods.find(name);
        if (ctorIt == cls.methods.end() || ctorIt->second.empty()) {
            if (!call.args.empty())
                error(call.callee, "class '" + name + "' has no constructor but was called with arguments");
            for (const auto& arg : call.args) analyzeExpr(*arg);
            return makeObjectType(name);
        }
        const std::vector<ClassInfo::Method>& set = ctorIt->second;
        std::vector<OverloadCand> cands;
        for (const auto& m : set) cands.push_back({&m.paramTypes, &m.paramMut, m.returnType, m.numDefaults, &m.paramEscapes, &m.paramNames, &m.paramHasDefault});
        int idx = resolveOverload(call.callee, "constructor '" + name + "'", cands, call.args,
                                  call.argNames, &call);
        if (idx >= 0 && set.size() > 1)
            resolvedCallee[&call] = mangleOverload(name + "_" + name, set[idx].paramTypes, set[idx].returnType);
        return makeObjectType(name);
    }

    // A local/parameter variable shadows any same-named function. It may still be *callable*:
    //   (a) a bounded type parameter `F: Call(…)` — resolve `call` against the bound (body check);
    //   (b) a value/reference of a class implementing a `Call` trait → `name.call(args)`.
    const Symbol* sym = symbolTable.lookup(name);
    if (sym && sym->kind == Symbol::Kind::Variable) {
        // (a) bounded type parameter
        if (const std::vector<std::string>* bounds = typeParamBoundsOf(sym->type)) {
            for (const std::string& b : *bounds) {
                auto tit = traitRegistry.find(b);
                if (tit == traitRegistry.end()) continue;
                for (const MethodDecl& md : tit->second->methods) {
                    if (md.name.lexeme != "call" || md.params.size() != call.args.size()) continue;
                    for (size_t i = 0; i < call.args.size(); ++i) {
                        Type at = analyzeExpr(*call.args[i]);
                        Type pt = resolveTypeToken(md.params[i].typeName);
                        checkCast(at, pt, call.callee, "call argument");
                    }
                    return resolveTypeToken(md.returnType);
                }
            }
            error(call.callee, "type parameter '" + sym->type.className
                  + "' is not callable with these argument types");
            for (const auto& arg : call.args) analyzeExpr(*arg);
            return Type{TypeKind::Error};
        }
        // (b) a class value/reference implementing Call
        const std::string& cn = sym->type.className;
        auto implIt = implementedTraits.find(cn);
        bool callable = false;
        if (implIt != implementedTraits.end())
            for (const std::string& tr : implIt->second)
                if (tr.rfind("Call", 0) == 0) { callable = true; break; }
        if (callable) {
            // Named arguments are not supported on the callable-object sugar `obj(...)` (it lowers
            // through the trait-method call path, which doesn't reorder). Use `obj.call(...)`.
            for (const Token& n : call.argNames)
                if (!n.lexeme.empty()) {
                    error(call.callee, "named arguments are not supported on a callable-object call '"
                          + call.callee.lexeme + "(...)'; call '" + call.callee.lexeme
                          + ".call(...)' explicitly to pass arguments by name");
                    return Type{TypeKind::Error};
                }
            ClassInfo& info = classRegistry.at(cn);
            auto mit = info.methods.find("call");
            if (mit != info.methods.end() && !mit->second.empty()) {
                std::vector<OverloadCand> cands;
                for (const auto& m : mit->second)
                    cands.push_back({&m.paramTypes, &m.paramMut, m.returnType, m.numDefaults, &m.paramEscapes, &m.paramNames, &m.paramHasDefault});
                int idx = resolveOverload(call.callee, "call on '" + cn + "'", cands, call.args,
                                          call.argNames, &call);
                if (idx < 0) return Type{TypeKind::Error};
                const ClassInfo::Method& m = mit->second[idx];
                callableCalls_[&call] = cn;
                if (mit->second.size() > 1)
                    resolvedCallee[&call] = mangleOverload(cn + "_call", m.paramTypes, m.returnType);
                return m.returnType;
            }
        }
        error(call.callee, "'" + name + "' is not a function");
        for (const auto& arg : call.args) analyzeExpr(*arg);
        return Type{TypeKind::Error};
    }

    // Free-function overload set (higher priority than an implicit-`this` method).
    auto fit = functionRegistry.find(name);
    if (fit != functionRegistry.end()) {
        const std::vector<FunctionOverload>& set = fit->second;
        std::vector<OverloadCand> cands;
        for (const auto& f : set) cands.push_back({&f.paramTypes, &f.paramMut, f.returnType, f.numDefaults, &f.paramEscapes, &f.paramNames, &f.paramHasDefault});
        int idx = resolveOverload(call.callee, "function '" + name + "'", cands, call.args,
                                  call.argNames, &call);
        if (idx < 0) return Type{TypeKind::Error};
        // Access control: a `private` free function is file-local. Calling it from a
        // different source file is a warning (not an error), like a private field accessed
        // outside its class. Only fires when both files are known and differ, so same-file
        // calls and unknown-origin contexts (class/enum/impl bodies) never warn.
        const FunctionOverload& chosen = set[idx];
        if (!chosen.isPublic && !chosen.sourceFile.empty()
            && !currentFile_.empty() && chosen.sourceFile != currentFile_) {
            warn(call.callee, "function '" + name + "' is private to its source file");
        }
        if (set.size() > 1 && !set[idx].isExtern)
            resolvedCallee[&call] = mangleOverload(name, set[idx].paramTypes, set[idx].returnType);
        return set[idx].returnType;
    }

    // Implicit `this`: a bare call may target a method of the enclosing class.
    if (const std::vector<ClassInfo::Method>* ms = currentClassMethods(name)) {
        std::vector<OverloadCand> cands;
        for (const auto& m : *ms) cands.push_back({&m.paramTypes, &m.paramMut, m.returnType, m.numDefaults, &m.paramEscapes, &m.paramNames, &m.paramHasDefault});
        int idx = resolveOverload(call.callee, "method '" + name + "'", cands, call.args,
                                  call.argNames, &call);
        if (idx < 0) return Type{TypeKind::Error};
        const ClassInfo::Method& m = (*ms)[idx];
        if (!m.isStatic && currentMethodIsStatic) {
            error(call.callee, "cannot call instance method '" + name + "' from a static method");
            return m.returnType;
        }
        if (m.isMut && !currentThisMutable) {
            error(call.callee, "cannot call mutating method '" + name
                  + "' on 'this' in a read-only method; declare the calling method 'mut'");
            return m.returnType;
        }
        if (ms->size() > 1)
            resolvedCallee[&call] = mangleOverload(currentClassName + "_" + name, m.paramTypes, m.returnType);
        return m.returnType;
    }

    error(call.callee, "undeclared function '" + name + "'");
    for (const auto& arg : call.args) analyzeExpr(*arg);
    return Type{TypeKind::Error};
}

bool SemanticAnalyzer::isConstantExpr(const Expr& expr) {
    return std::visit(overloaded{
        [](const LiteralExpr&)            { return true; },
        [](const UnaryExpr& u)            { return isConstantExpr(*u.operand); },
        [](const BinaryExpr& b)           { return isConstantExpr(*b.left) && isConstantExpr(*b.right); },
        [](const CastExpr& c)             { return isConstantExpr(*c.operand); },
        [](const auto&)                   { return false; },
    }, *expr.node);
}

Type SemanticAnalyzer::analyzeVarDecl(const VarDeclExpr& varDecl) {
    // ---- Inferred `var` local: deduce the type from the initializer ----
    if (varDecl.typeName.type == TokenType::VAR) {
        // Redeclaration in the same scope?
        if (const Symbol* existing = symbolTable.lookupCurrentScope(varDecl.name.lexeme)) {
            error(varDecl.name, "variable '" + varDecl.name.lexeme + "' already declared in this scope"
                  + " (previously declared at line "
                  + std::to_string(existing->declarationToken.line) + ")");
            if (varDecl.initializer) analyzeExpr(*varDecl.initializer);
            return Type{TypeKind::Error};
        }
        if (!varDecl.initializer) {   // parser enforces this; defensive
            error(varDecl.name, "'var " + varDecl.name.lexeme + "' requires an initializer to infer its type");
            return Type{TypeKind::Error};
        }

        // Analyse the initializer with NO expected type — the inferred type IS its type.
        Type inferred = analyzeExpr(*varDecl.initializer);

        // Reject types that carry no usable representation for a local binding.
        if (isError(inferred)) return Type{TypeKind::Error};
        if (inferred.kind == TypeKind::Void) {
            error(varDecl.name, "cannot infer type of '" + varDecl.name.lexeme
                  + "' from a 'void' initializer");
            return Type{TypeKind::Error};
        }
        if (inferred.kind == TypeKind::Null || (inferred.isNullable && stripNullable(inferred).kind == TypeKind::Null)) {
            error(varDecl.name, "cannot infer type of '" + varDecl.name.lexeme
                  + "' from 'null' — annotate the type explicitly (e.g. `Point&? " + varDecl.name.lexeme + " = null;`)");
            return Type{TypeKind::Error};
        }
        if (inferred.kind == TypeKind::Object && inferred.className.rfind("__lambda", 0) == 0) {
            error(varDecl.name, "cannot infer the type of a lambda into a 'var' — pass it directly "
                  "to a 'Call'-bounded generic instead");
            return Type{TypeKind::Error};
        }
        // A raw-pointer inference (e.g. `var s = "literal";` → ptr) obeys the same --unsafe-ptr gate.
        if (!allowRawPtr_ && (inferred.kind == TypeKind::Ptr || inferred.kind == TypeKind::TypedPtr)) {
            error(varDecl.name, "'" + typeName(inferred) + "' is a raw pointer type and requires "
                  "--unsafe-ptr (raw pointers are for stdlib/internal use only)");
            return Type{TypeKind::Error};
        }

        // A `static var` still obeys the static-local rules against the *inferred* type.
        if (varDecl.isStatic) {
            bool primitive = isNumeric(inferred.kind)
                          || inferred.kind == TypeKind::Bool
                          || inferred.kind == TypeKind::Char;
            if (!primitive)
                error(varDecl.name, "static local variable '" + varDecl.name.lexeme
                      + "' must have a primitive type (numeric, bool or char)");
            if (!isConstantExpr(*varDecl.initializer))
                error(varDecl.name, "static local variable '" + varDecl.name.lexeme
                      + "' requires a constant initializer");
        }

        // Initialising a `mut` reference binding from a read-only reference is a const→mut coercion.
        if (varDecl.isMut)
            warnConstToMut(varDecl.name, *varDecl.initializer, inferred);

        // Record the synthesized type token so codegen can resolve the type like an explicit one.
        // (Token has const members → not assignable; emplace constructs in place.)
        inferredVarType_.emplace(&varDecl, synthTypeToken(inferred, varDecl.name.line));

        symbolTable.declare(varDecl.name.lexeme, Symbol{
            Symbol::Kind::Variable,
            inferred,
            varDecl.name,
            {},
            /*isParameter=*/false,
            /*isInitialized=*/true,
            /*isMutable=*/varDecl.isMut
        });
        return inferred;
    }

    // Resolve the declared type — handles class names (Object) and Class& (Reference).
    Type elementType = resolveTypeToken(varDecl.typeName);
    Type declaredType = varDecl.arraySize > 0
        ? makeArrayType(elementType.kind, varDecl.arraySize)
        : elementType;

    if (elementType.kind == TypeKind::Void) {
        error(varDecl.typeName, "variable '" + varDecl.name.lexeme + "' cannot have type 'void'");
        if (varDecl.initializer) analyzeExpr(*varDecl.initializer);
        return Type{TypeKind::Error};
    }

    // C-style static local: persistent single-storage variable. Phase 3 supports
    // scalar primitive (numeric / bool / char) static locals with a constant
    // initializer that runs once before main.
    if (varDecl.isStatic) {
        bool primitive = varDecl.arraySize == 0
                      && (isNumeric(elementType.kind)
                          || elementType.kind == TypeKind::Bool
                          || elementType.kind == TypeKind::Char);
        if (!primitive)
            error(varDecl.typeName, "static local variable '" + varDecl.name.lexeme
                  + "' must have a primitive type (numeric, bool or char)");
        if (varDecl.initializer && !isConstantExpr(*varDecl.initializer))
            error(varDecl.name, "static local variable '" + varDecl.name.lexeme
                  + "' requires a constant initializer");
    }

    // Raw pointer types are gated behind --unsafe-ptr.
    checkRawPtrAllowed(varDecl.typeName, varDecl.name);

    // Array initializers are not yet supported
    if (varDecl.arraySize > 0 && varDecl.initializer) {
        error(varDecl.typeName, "array initializers are not yet supported");
        analyzeExpr(*varDecl.initializer);
    }

    // Redeclaration in the same scope?
    const Symbol* existing = symbolTable.lookupCurrentScope(varDecl.name.lexeme);
    if (existing) {
        error(varDecl.name, "variable '" + varDecl.name.lexeme + "' already declared in this scope"
              + " (previously declared at line "
              + std::to_string(existing->declarationToken.line) + ")");
        if (varDecl.initializer && varDecl.arraySize == 0) analyzeExpr(*varDecl.initializer);
        return declaredType;
    }

    // Scalar: analyse initializer (with the declared type as the overload-resolution context)
    if (varDecl.arraySize == 0 && varDecl.initializer) {
        Type initializerType = analyzeWithExpected(*varDecl.initializer, declaredType);
        checkCast(initializerType, declaredType, varDecl.name, "variable initializer");
        // A primitive borrow (`i32*`) must borrow an addressable value. Binding from a fresh
        // primitive (not itself a borrow being passed along) requires an lvalue — a temporary
        // has no address.
        if (isPrimitiveBorrow(declaredType) && !isBorrow(initializerType)
            && !isLvalueExpr(*varDecl.initializer))
            error(varDecl.name, "a '" + typeName(declaredType)
                  + "' must borrow an addressable value (a variable or an element like `a[i]`), "
                  "not a temporary");
        // Initialising a `mut` reference from a read-only reference is a const→mut coercion.
        if (varDecl.isMut)
            warnConstToMut(varDecl.name, *varDecl.initializer, declaredType);
    }

    // Decide whether the variable starts as definitely initialized:
    //   - explicit initializer present                → yes
    //   - Object (class value): zero-initialized struct → yes
    //   - Array: zero-initialized by the runtime       → yes
    //   - Everything else (primitives, references)     → no (must be assigned before use)
    bool isInit = varDecl.initializer != nullptr
               || elementType.kind == TypeKind::Object
               || varDecl.arraySize > 0
               || varDecl.isStatic;   // static locals are zero-initialised storage

    symbolTable.declare(varDecl.name.lexeme, Symbol{
        Symbol::Kind::Variable,
        declaredType,
        varDecl.name,
        {},
        /*isParameter=*/false,
        /*isInitialized=*/isInit,
        /*isMutable=*/varDecl.isMut
    });

    return declaredType;
}

Type SemanticAnalyzer::analyzeIndex(const IndexExpr& indexExpr) {
    const Token& site = exprFirstToken(*indexExpr.object);
    Type objectType   = analyzeExpr(*indexExpr.object);
    Type indexType    = analyzeExpr(*indexExpr.index);

    if (isError(objectType)) return Type{TypeKind::Error};

    // Generic body-check: `t[i]` on a type parameter requires an `Index` bound. The element type
    // is not knowable abstractly, so the result is left suppressed (no associated types in v1).
    if (const std::vector<std::string>* bounds = typeParamBoundsOf(objectType)) {
        if (bounds->empty()) return Type{TypeKind::Error};
        if (std::find(bounds->begin(), bounds->end(), "Index") != bounds->end())
            return Type{TypeKind::Error};   // element type unknown — permissive
        error(site, "'[]' on type parameter '" + objectType.className + "' requires bound 'Index'");
        return Type{TypeKind::Error};
    }

    // Operator overloading: a[i] on a class → the Index trait's `get` method.
    if (objectType.kind == TypeKind::Object || objectType.kind == TypeKind::Reference) {
        auto implIt = implementedTraits.find(objectType.className);
        if (implIt == implementedTraits.end() || !implIt->second.count("Index")) {
            error(site, "type '" + objectType.className + "' does not implement 'Index' for '[]'");
            return Type{TypeKind::Error};
        }
        ClassInfo& info = classRegistry.at(objectType.className);
        auto mit = info.methods.find("get");
        int idx = (mit == info.methods.end()) ? -1 : pickOverloadByArgs(mit->second, { indexType });
        if (idx < 0) {
            error(site, "no matching 'get' method on '" + objectType.className + "' for '[]'");
            return Type{TypeKind::Error};
        }
        const ClassInfo::Method& m = mit->second[idx];
        if (mit->second.size() > 1)
            resolvedCallee[&indexExpr] = mangleOverload(objectType.className + "_get",
                                                        m.paramTypes, m.returnType);
        return m.returnType;
    }

    // Index must be an integer for built-in array / pointer indexing.
    if (!isError(indexType) && !isInteger(indexType.kind))
        error(site, "index must be an integer type, got " + typeName(indexType));

    if (objectType.kind == TypeKind::Array) {
        if (!isError(indexType))
            checkConstantIndexBounds(*indexExpr.index, objectType.arraySize);
        return Type{objectType.elementKind};
    }

    if (objectType.kind == TypeKind::TypedPtr)
        return typedPtrElement(objectType);

    error(site, "cannot index a value of type " + typeName(objectType)
        + " with '[]'; indexing works on a fixed-size array 'T[N]', a raw pointer 'ptr<T>', "
          "or a class that implements the 'Index' trait");
    return Type{TypeKind::Error};
}

Type SemanticAnalyzer::analyzeIndexAssign(const IndexAssignExpr& indexAssign) {
    const Token& site = exprFirstToken(*indexAssign.object);
    Type objectType   = analyzeExpr(*indexAssign.object);

    Type indexType = analyzeExpr(*indexAssign.index);

    if (isError(objectType)) {
        analyzeExpr(*indexAssign.value);
        return Type{TypeKind::Error};
    }

    // Operator overloading: a[i] = v on a class → the Index trait's `set(i, v)` method.
    if (objectType.kind == TypeKind::Object || objectType.kind == TypeKind::Reference) {
        Type valueType = analyzeExpr(*indexAssign.value);
        auto implIt = implementedTraits.find(objectType.className);
        if (implIt == implementedTraits.end() || !implIt->second.count("Index")) {
            error(site, "type '" + objectType.className + "' does not implement 'Index' for '[]'");
            return Type{TypeKind::Error};
        }
        ClassInfo& info = classRegistry.at(objectType.className);
        auto mit = info.methods.find("set");
        int idx = (mit == info.methods.end()) ? -1 : pickOverloadByArgs(mit->second, { indexType, valueType });
        if (idx < 0) {
            error(site, "no matching 'set' method on '" + objectType.className + "' for indexed assignment");
            return Type{TypeKind::Error};
        }
        const ClassInfo::Method& m = mit->second[idx];
        if (mit->second.size() > 1)
            resolvedCallee[&indexAssign] = mangleOverload(objectType.className + "_set",
                                                          m.paramTypes, m.returnType);
        return valueType;
    }

    if (!isError(indexType) && !isInteger(indexType.kind))
        error(site, "index must be an integer type, got " + typeName(indexType));

    Type elementType;
    if (objectType.kind == TypeKind::Array) {
        if (!isError(indexType))
            checkConstantIndexBounds(*indexAssign.index, objectType.arraySize);
        elementType = Type{objectType.elementKind};
    } else if (objectType.kind == TypeKind::TypedPtr) {
        elementType = typedPtrElement(objectType);
        // Storing a value object by value deep-copies it (clone). A value object that owns a raw
        // buffer (String, a nested container) would be shallow-cloned → aliased buffer → double-free.
        // Reject it cleanly. (Phase 2: allow it when the class provides a user `Clone` impl.)
        if (elementType.kind == TypeKind::Object) {
            std::unordered_set<std::string> seen;
            auto tIt = implementedTraits.find(elementType.className);
            bool implsClone = tIt != implementedTraits.end() && tIt->second.count("Clone") > 0;
            if (classOwnsRawPtr(elementType.className, seen) && !implsClone)
                error(site, "cannot store value objects of type '" + elementType.className
                      + "' in a 'ptr<T>' buffer: it owns a raw buffer (e.g. String or a nested "
                        "container), and memberwise copy would alias that buffer. Define an "
                        "'impl Clone for " + elementType.className + "' to deep-copy it.");
        }
    } else {
        error(site, "cannot index a value of type " + typeName(objectType)
            + " with '[]'; indexing works on a fixed-size array 'T[N]', a raw pointer 'ptr<T>', "
              "or a class that implements the 'Index' trait");
        analyzeExpr(*indexAssign.value);
        return Type{TypeKind::Error};
    }

    // Value type must be assignable to element type
    Type valueType = analyzeExpr(*indexAssign.value);
    checkCast(valueType, elementType, site, "element assignment");

    return elementType;
}

// ============================================================
// Class expression analysis
// ============================================================

// `x!!` — non-null assertion: yields the non-null form. On null at runtime it aborts (codegen).
Type SemanticAnalyzer::analyzeUnwrap(const UnwrapExpr& unwrap) {
    Type t = analyzeExpr(*unwrap.operand);
    if (isError(t)) return Type{TypeKind::Error};
    if (!t.isNullable) {
        warn(unwrap.op, "'!!' applied to a non-nullable value — the assertion is unnecessary");
        return t;
    }
    return stripNullable(t);
}

// `a ?: b` — Elvis. Result is the non-null form of `a` (or `b`'s type widened in). `a` must be
// nullable to be meaningful; `b` must be assignable to the non-null result.
Type SemanticAnalyzer::analyzeElvis(const ElvisExpr& elvis) {
    Type lt = analyzeExpr(*elvis.left);
    Type rt = analyzeExpr(*elvis.right);
    if (isError(lt) || isError(rt)) return Type{TypeKind::Error};
    if (lt.kind == TypeKind::Null) return rt;   // `null ?: b` is just `b`
    if (!lt.isNullable)
        warn(elvis.op, "left side of '?:' is not nullable — the default is never used");
    // The result is the non-null form of the left, unless the default itself may be null (then the
    // whole expression may still be null). Check the RHS against that result type — crucially the
    // *nullable* one when the default is nullable, so `a ?: b` with a nullable `b` is allowed.
    Type result = rt.isNullable ? makeNullable(stripNullable(lt)) : stripNullable(lt);
    checkCast(rt, result, elvis.op, "right side of '?:'");
    return result;
}

Type SemanticAnalyzer::analyzeThis(const ThisExpr& thisExpr) {
    if (currentClassName.empty()) {
        error(thisExpr.keyword, "'this' used outside of a class method");
        return Type{TypeKind::Error};
    }
    if (currentMethodIsStatic) {
        error(thisExpr.keyword, "'this' cannot be used in a static method");
        return Type{TypeKind::Error};
    }
    if (currentClassIsEnum)
        return makeEnumType(currentClassName);
    return makeObjectType(currentClassName);
}

Type SemanticAnalyzer::analyzeMemberAccess(const MemberAccessExpr& memberAccess) {
    // Static access through a type name: EnumName.VARIANT or ClassName::field.
    if (std::holds_alternative<IdentifierExpr>(*memberAccess.object->node)) {
        const auto& ident = std::get<IdentifierExpr>(*memberAccess.object->node);
        auto enumIt = enumRegistry.find(ident.name.lexeme);
        if (enumIt != enumRegistry.end()) {
            // It's an enum name — the member must be a declared variant.
            if (!enumIt->second.variantSet.count(memberAccess.field.lexeme)) {
                error(memberAccess.field, "enum '" + ident.name.lexeme
                      + "' has no variant '" + memberAccess.field.lexeme + "'");
                return Type{TypeKind::Error};
            }
            return makeEnumType(ident.name.lexeme);
        }
        // ClassName::field — static member field access through the type name.
        auto clsIt = classRegistry.find(ident.name.lexeme);
        if (clsIt != classRegistry.end()) {
            auto sfIt = clsIt->second.staticFields.find(memberAccess.field.lexeme);
            if (sfIt == clsIt->second.staticFields.end()) {
                error(memberAccess.field, "class '" + ident.name.lexeme
                      + "' has no static member '" + memberAccess.field.lexeme + "'");
                return Type{TypeKind::Error};
            }
            const ClassInfo::StaticField& sf = sfIt->second;
            if (!sf.isPublic && currentClassName != ident.name.lexeme)
                warn(memberAccess.field, "static field '" + memberAccess.field.lexeme
                     + "' is private in class '" + ident.name.lexeme + "'");
            return sf.type;
        }
    }

    Type objectType = analyzeExpr(*memberAccess.object);
    if (isError(objectType)) return Type{TypeKind::Error};

    // `str` view: `.data` → the (NUL-terminated) byte pointer (`ptr`), `.len` → byte length (`u64`).
    if (objectType.kind == TypeKind::Str) {
        if (memberAccess.field.lexeme == "data") return Type{TypeKind::Ptr};
        if (memberAccess.field.lexeme == "len")  return Type{TypeKind::U64};
        error(memberAccess.field, "'str' has no member '" + memberAccess.field.lexeme
              + "' (only '.data' and '.len')");
        return Type{TypeKind::Error};
    }

    // Nullable receiver: `x.f` on a `T?` is an error unless it's a `?.` safe access (which narrows
    // the receiver and makes the whole access nullable).
    if (objectType.isNullable) {
        if (!memberAccess.safe) {
            error(memberAccess.field, "cannot access member '" + memberAccess.field.lexeme
                  + "' on a possibly-null value of type '" + typeName(objectType)
                  + "'; narrow it with a null check, '!!', or '?.'");
            return Type{TypeKind::Error};
        }
        objectType = stripNullable(objectType);
    }
    auto wrapSafe = [&](Type t) -> Type {
        if (!memberAccess.safe) return t;
        if (t.kind == TypeKind::Object) {
            error(memberAccess.field, "'?.' cannot yield a value object '" + t.className
                  + "'; return a reference or a primitive");
            return Type{TypeKind::Error};
        }
        return makeNullable(t);
    };

    // Generic body-check: a type parameter is opaque — traits declare no fields, so field access
    // on a bounded `T` is an error (unbounded ⇒ permissive/suppressed).
    if (const std::vector<std::string>* bounds = typeParamBoundsOf(objectType)) {
        if (bounds->empty()) return Type{TypeKind::Error};
        error(memberAccess.field, "cannot access field '" + memberAccess.field.lexeme
              + "' of type parameter '" + objectType.className + "' (a bound provides methods, not fields)");
        return Type{TypeKind::Error};
    }

    const ClassInfo* cls = lookupObjectClass(objectType, memberAccess.field);
    if (!cls) return Type{TypeKind::Error};

    // A static field may also be read through an instance: obj.staticField.
    auto staticIt = cls->staticFields.find(memberAccess.field.lexeme);
    if (staticIt != cls->staticFields.end()) {
        const ClassInfo::StaticField& sf = staticIt->second;
        if (!sf.isPublic && currentClassName != objectType.className)
            warn(memberAccess.field, "static field '" + memberAccess.field.lexeme
                 + "' is private in class '" + objectType.className + "'");
        return wrapSafe(sf.type);
    }

    auto fieldIt = cls->fields.find(memberAccess.field.lexeme);
    if (fieldIt == cls->fields.end()) {
        error(memberAccess.field, "class '" + objectType.className
              + "' has no field '" + memberAccess.field.lexeme + "'");
        return Type{TypeKind::Error};
    }

    const ClassInfo::Field& field = fieldIt->second;
    // Access control: private fields emit a warning (not an error) when accessed from outside the class.
    if (!field.isPublic && currentClassName != objectType.className) {
        warn(memberAccess.field, "field '" + memberAccess.field.lexeme
             + "' is private in class '" + objectType.className + "'");
    }

    return wrapSafe(field.type);
}

bool SemanticAnalyzer::exprIsMutablePlace(const Expr& expr) {
    const auto& node = *expr.node;
    // `this` is a mutable receiver only inside a `mut` method / ctor / dtor (Rust &mut self).
    if (std::holds_alternative<ThisExpr>(node))       return currentThisMutable;
    // Freshly-owned references: `new T(...)` and call/method results.
    if (std::holds_alternative<NewExpr>(node))        return true;
    if (std::holds_alternative<CallExpr>(node))       return true;
    if (std::holds_alternative<MethodCallExpr>(node)) return true;
    if (std::holds_alternative<IndexExpr>(node))
        return exprIsMutablePlace(*std::get<IndexExpr>(node).object);
    if (std::holds_alternative<CastExpr>(node))
        return std::get<CastExpr>(node).isMut;   // `x as mut T` yields a mutable view
    if (std::holds_alternative<IdentifierExpr>(node)) {
        const Symbol* s = symbolTable.lookup(std::get<IdentifierExpr>(node).name.lexeme);
        // Non-variable identifiers (e.g. class names for statics) are not gated here.
        return !s || s->kind != Symbol::Kind::Variable || s->isMutable;
    }
    if (std::holds_alternative<MemberAccessExpr>(node)) {
        const auto& ma = std::get<MemberAccessExpr>(node);
        if (!exprIsMutablePlace(*ma.object)) return false;
        // A field is a mutable place only if the field itself is `mut`.
        Type ownerT = analyzeExpr(*ma.object);
        if (ownerT.kind == TypeKind::Object || ownerT.kind == TypeKind::Reference) {
            auto cit = classRegistry.find(ownerT.className);
            if (cit != classRegistry.end()) {
                auto fit = cit->second.fields.find(ma.field.lexeme);
                if (fit != cit->second.fields.end()) return fit->second.isMut;
            }
        }
        return true;   // unknown shape → don't over-report
    }
    return true;
}

void SemanticAnalyzer::warnConstToMut(const Token& at, const Expr& source, const Type& targetType) {
    if (targetType.kind != TypeKind::Reference) return;   // refs only (value copies are independent)
    if (std::holds_alternative<CastExpr>(*source.node)) return;   // explicit cast silences
    if (exprIsMutablePlace(source)) return;               // source already grants write access
    warn(at, "coercing a read-only (const) reference into a 'mut' binding; add an explicit "
             "'as mut " + typeName(targetType) + "' cast to silence this warning");
}

Type SemanticAnalyzer::analyzeMemberAssign(const MemberAssignExpr& memberAssign) {
    // Static field write through the type name: ClassName::field = value.
    if (std::holds_alternative<IdentifierExpr>(*memberAssign.object->node)) {
        const auto& ident = std::get<IdentifierExpr>(*memberAssign.object->node);
        auto clsIt = classRegistry.find(ident.name.lexeme);
        if (clsIt != classRegistry.end() && enumRegistry.find(ident.name.lexeme) == enumRegistry.end()) {
            auto sfIt = clsIt->second.staticFields.find(memberAssign.field.lexeme);
            if (sfIt == clsIt->second.staticFields.end()) {
                error(memberAssign.field, "class '" + ident.name.lexeme
                      + "' has no static member '" + memberAssign.field.lexeme + "'");
                analyzeExpr(*memberAssign.value);
                return Type{TypeKind::Error};
            }
            const ClassInfo::StaticField& sf = sfIt->second;
            if (!sf.isPublic && currentClassName != ident.name.lexeme)
                warn(memberAssign.field, "static field '" + memberAssign.field.lexeme
                     + "' is private in class '" + ident.name.lexeme + "'");
            if (!sf.isMut)
                error(memberAssign.field, "cannot assign to immutable static field '"
                      + memberAssign.field.lexeme + "'; declare it 'mut static' to allow reassignment");
            Type valueType = analyzeExpr(*memberAssign.value);
            checkCast(valueType, sf.type, memberAssign.field, "static field assignment");
            return sf.type;
        }
    }

    Type objectType = analyzeExpr(*memberAssign.object);
    if (isError(objectType)) {
        analyzeExpr(*memberAssign.value);
        return Type{TypeKind::Error};
    }

    const ClassInfo* cls = lookupObjectClass(objectType, memberAssign.field);
    if (!cls) {
        analyzeExpr(*memberAssign.value);
        return Type{TypeKind::Error};
    }

    // Static field write through an instance: obj.staticField = value.
    auto staticIt = cls->staticFields.find(memberAssign.field.lexeme);
    if (staticIt != cls->staticFields.end()) {
        const ClassInfo::StaticField& sf = staticIt->second;
        if (!sf.isPublic && currentClassName != objectType.className)
            warn(memberAssign.field, "static field '" + memberAssign.field.lexeme
                 + "' is private in class '" + objectType.className + "'");
        if (!sf.isMut)
            error(memberAssign.field, "cannot assign to immutable static field '"
                  + memberAssign.field.lexeme + "'; declare it 'mut static' to allow reassignment");
        Type valueType = analyzeExpr(*memberAssign.value);
        checkCast(valueType, sf.type, memberAssign.field, "static field assignment");
        return sf.type;
    }

    auto fieldIt = cls->fields.find(memberAssign.field.lexeme);
    if (fieldIt == cls->fields.end()) {
        error(memberAssign.field, "class '" + objectType.className
              + "' has no field '" + memberAssign.field.lexeme + "'");
        analyzeExpr(*memberAssign.value);
        return Type{TypeKind::Error};
    }

    const ClassInfo::Field& field = fieldIt->second;
    // Enum fields are immutable: only assignable via 'this.field' inside the
    // enum's own constructor.
    if (objectType.kind == TypeKind::Enum) {
        bool isThis = std::holds_alternative<ThisExpr>(*memberAssign.object->node);
        if (!inEnumConstructor || !isThis) {
            error(memberAssign.field, "cannot assign to field '" + memberAssign.field.lexeme
                  + "' of enum '" + objectType.className
                  + "'; enum fields are immutable");
            analyzeExpr(*memberAssign.value);
            return field.type;
        }
    }
    // Instance fields are const by default: a non-`mut` field may be written only via
    // 'this.field' inside the class's own constructor (mirrors the enum-field rule).
    // Applies whether the instance is a value (`Object`) or a heap reference
    // (`Reference`); a reference target is never `this`, so it is always gated.
    if ((objectType.kind == TypeKind::Object || objectType.kind == TypeKind::Reference)
        && !field.isMut) {
        bool isThis = std::holds_alternative<ThisExpr>(*memberAssign.object->node);
        if (!inConstructor || !isThis) {
            error(memberAssign.field, "cannot assign to immutable field '" + memberAssign.field.lexeme
                  + "' of class '" + objectType.className
                  + "'; declare it 'mut' to allow mutation");
            analyzeExpr(*memberAssign.value);
            return field.type;
        }
    }
    // Transitive const: writing any field also requires the *receiver* to be a mutable
    // place — a `mut` local/borrow, or `this` inside a `mut` method / ctor / dtor.
    if ((objectType.kind == TypeKind::Object || objectType.kind == TypeKind::Reference)
        && !exprIsMutablePlace(*memberAssign.object)) {
        // Pinpoint the immutable link: a `mut`-reachable receiver whose *intermediate field*
        // is const (e.g. `b.p.x = …` where `p` is a const value/reference field) blames that
        // field, rather than the generic "immutable binding" (which fits a const root binding).
        const void* blamed = nullptr;   // sentinel: set once we emit a specific diagnostic
        if (std::holds_alternative<ThisExpr>(*memberAssign.object->node)) {
            error(memberAssign.field, "cannot write to field '" + memberAssign.field.lexeme
                  + "' in a read-only method; declare the method 'mut'");
            blamed = &memberAssign;
        } else if (std::holds_alternative<MemberAccessExpr>(*memberAssign.object->node)) {
            const auto& inner = std::get<MemberAccessExpr>(*memberAssign.object->node);
            if (exprIsMutablePlace(*inner.object)) {   // receiver is fine → the field is the const link
                Type ownerT = analyzeExpr(*inner.object);
                auto cit = classRegistry.find(ownerT.className);
                if (cit != classRegistry.end()) {
                    auto fit = cit->second.fields.find(inner.field.lexeme);
                    if (fit != cit->second.fields.end() && !fit->second.isMut) {
                        error(memberAssign.field, "cannot assign to field '" + memberAssign.field.lexeme
                              + "': the enclosing field '" + inner.field.lexeme
                              + "' is not mutable; declare it 'mut'");
                        blamed = &inner;
                    }
                }
            }
        }
        if (!blamed)
            error(memberAssign.field, "cannot assign to field '" + memberAssign.field.lexeme
                  + "' through an immutable binding; declare the "
                  + (objectType.kind == TypeKind::Reference ? std::string("reference") : std::string("variable"))
                  + " 'mut'");
        analyzeExpr(*memberAssign.value);
        return field.type;
    }
    if (!field.isPublic && currentClassName != objectType.className) {
        warn(memberAssign.field, "field '" + memberAssign.field.lexeme
             + "' is private in class '" + objectType.className + "'");
    }

    Type valueType = analyzeWithExpected(*memberAssign.value, field.type);
    checkCast(valueType, field.type, memberAssign.field, "field assignment");
    return field.type;
}

Type SemanticAnalyzer::analyzeMethodCall(const MethodCallExpr& methodCall) {
    // Static method call through the type name: ClassName::method(args).
    // The leading identifier names a class (and is not shadowed by a variable).
    if (std::holds_alternative<IdentifierExpr>(*methodCall.object->node)) {
        const auto& ident = std::get<IdentifierExpr>(*methodCall.object->node);
        if (!symbolTable.lookup(ident.name.lexeme)) {
            auto clsIt = classRegistry.find(ident.name.lexeme);
            if (clsIt != classRegistry.end()) {
                auto mIt = clsIt->second.methods.find(methodCall.method.lexeme);
                if (mIt == clsIt->second.methods.end() || mIt->second.empty()) {
                    error(methodCall.method, "class '" + ident.name.lexeme
                          + "' has no static method '" + methodCall.method.lexeme + "'");
                    for (const auto& arg : methodCall.args) analyzeExpr(*arg);
                    return Type{TypeKind::Error};
                }
                const std::vector<ClassInfo::Method>& set = mIt->second;
                std::vector<OverloadCand> cands;
                for (const auto& m : set) cands.push_back({&m.paramTypes, &m.paramMut, m.returnType, m.numDefaults, &m.paramEscapes, &m.paramNames, &m.paramHasDefault});
                int idx = resolveOverload(methodCall.method,
                            "static method '" + methodCall.method.lexeme + "'", cands, methodCall.args,
                            methodCall.argNames, &methodCall);
                if (idx < 0) return Type{TypeKind::Error};
                const ClassInfo::Method& sm = set[idx];
                if (!sm.isStatic) {
                    error(methodCall.method, "method '" + methodCall.method.lexeme
                          + "' is not static; call it on an instance");
                    return sm.returnType;
                }
                if (!sm.isPublic && currentClassName != ident.name.lexeme)
                    warn(methodCall.method, "static method '" + methodCall.method.lexeme
                         + "' is private in class '" + ident.name.lexeme + "'");
                if (set.size() > 1)
                    resolvedCallee[&methodCall] = mangleOverload(
                        ident.name.lexeme + "_" + methodCall.method.lexeme, sm.paramTypes, sm.returnType);
                return sm.returnType;
            }
        }
    }

    Type objectType = analyzeExpr(*methodCall.object);
    if (isError(objectType)) {
        for (const auto& arg : methodCall.args) analyzeExpr(*arg);
        return Type{TypeKind::Error};
    }

    // Nullable receiver: `x.m()` on a `T?` errors unless it's a `?.` safe call.
    if (objectType.isNullable) {
        if (!methodCall.safe) {
            error(methodCall.method, "cannot call method '" + methodCall.method.lexeme
                  + "' on a possibly-null value of type '" + typeName(objectType)
                  + "'; narrow it with a null check, '!!', or '?.'");
            for (const auto& arg : methodCall.args) analyzeExpr(*arg);
            return Type{TypeKind::Error};
        }
        objectType = stripNullable(objectType);
    }
    auto wrapSafe = [&](Type t) -> Type {
        if (!methodCall.safe) return t;
        if (t.kind == TypeKind::Void)   return t;   // `x?.doThing()` — a null-guarded void call
        if (t.kind == TypeKind::Object) {
            error(methodCall.method, "'?.' cannot yield a value object '" + t.className
                  + "'; return a reference or a primitive");
            return Type{TypeKind::Error};
        }
        return makeNullable(t);
    };

    // Generic body-check: a method call on a value of a type parameter resolves against the
    // parameter's bounds (not a concrete class). Unbounded ⇒ permissive (suppressed).
    if (const std::vector<std::string>* bounds = typeParamBoundsOf(objectType)) {
        for (const auto& arg : methodCall.args) analyzeExpr(*arg);
        if (bounds->empty()) return Type{TypeKind::Error};   // unbounded — duck-typed at instantiation
        Type ret;
        if (resolveBoundMethod(*bounds, objectType.className, methodCall.method.lexeme,
                               methodCall.args.size(), ret))
            return ret;
        std::string list;
        for (size_t i = 0; i < bounds->size(); ++i) list += (i ? ", " : "") + (*bounds)[i];
        error(methodCall.method, "no method '" + methodCall.method.lexeme
              + "' provided by the bounds (" + list + ") of type parameter '"
              + objectType.className + "'");
        return Type{TypeKind::Error};
    }

    const ClassInfo* cls = lookupObjectClass(objectType, methodCall.method);
    if (!cls) {
        for (const auto& arg : methodCall.args) analyzeExpr(*arg);
        return Type{TypeKind::Error};
    }

    auto methodIt = cls->methods.find(methodCall.method.lexeme);
    if (methodIt == cls->methods.end() || methodIt->second.empty()) {
        error(methodCall.method, "class '" + objectType.className
              + "' has no method '" + methodCall.method.lexeme + "'");
        for (const auto& arg : methodCall.args) analyzeExpr(*arg);
        return Type{TypeKind::Error};
    }

    const std::vector<ClassInfo::Method>& set = methodIt->second;
    std::vector<OverloadCand> cands;
    for (const auto& m : set) cands.push_back({&m.paramTypes, &m.paramMut, m.returnType, m.numDefaults, &m.paramEscapes, &m.paramNames, &m.paramHasDefault});
    int idx = resolveOverload(methodCall.method,
                "method '" + methodCall.method.lexeme + "'", cands, methodCall.args,
                methodCall.argNames, &methodCall);
    if (idx < 0) return Type{TypeKind::Error};
    const ClassInfo::Method& method = set[idx];

    // Escape analysis: calling a method that stores or returns `this` on a *stack value object*
    // would let a reference to it outlive the object. Only value receivers are at risk — a heap
    // reference owns its target.
    if (objectType.kind == TypeKind::Object && method.thisEscapes)
        error(methodCall.method, "cannot call '" + methodCall.method.lexeme
              + "' on the stack value object '" + objectType.className + "': it stores or returns "
              "'this', which would outlive the object — use a heap reference (`new "
              + objectType.className + "(...)`)");

    // A `mut` method mutates its receiver, so the receiver must be a mutable place —
    // a `mut` binding, or `this` inside a `mut` method / ctor / dtor.
    if (method.isMut
        && (objectType.kind == TypeKind::Object || objectType.kind == TypeKind::Reference)
        && !exprIsMutablePlace(*methodCall.object)) {
        if (std::holds_alternative<ThisExpr>(*methodCall.object->node))
            error(methodCall.method, "cannot call mutating method '" + methodCall.method.lexeme
                  + "' on 'this' in a read-only method; declare the calling method 'mut'");
        else
            error(methodCall.method, "cannot call mutating method '" + methodCall.method.lexeme
                  + "' through an immutable binding; declare it 'mut'");
        return method.returnType;
    }
    if (!method.isPublic && currentClassName != objectType.className) {
        warn(methodCall.method, "method '" + methodCall.method.lexeme
             + "' is private in class '" + objectType.className + "'");
    }
    if (set.size() > 1)
        resolvedCallee[&methodCall] = mangleOverload(
            objectType.className + "_" + methodCall.method.lexeme, method.paramTypes, method.returnType);
    return wrapSafe(method.returnType);
}

// ============================================================
// Store through a reference-valued expression: `<target> = value`
// ============================================================

Type SemanticAnalyzer::analyzeRefStore(const RefStoreExpr& refStore) {
    Type targetType = analyzeExpr(*refStore.target);
    if (isError(targetType)) { analyzeExpr(*refStore.value); return Type{TypeKind::Error}; }

    // The target must evaluate to a reference/borrow — a storage location we can write through.
    // (A plain value, e.g. `f() = x` where `f` returns `i32`, is not assignable.)
    if (targetType.kind != TypeKind::Reference) {
        error(refStore.op, "the left side of '=' is not assignable: this expression is not a "
              "reference. Assign to a variable, an element `a[i]`, a field `x.f`, or a call that "
              "returns a borrow (`T*`)");
        analyzeExpr(*refStore.value);
        return Type{TypeKind::Error};
    }

    // Primitive borrow (`ref i32`): store the value into the referent (like C++ `v.at(i) = x`).
    if (isPrimitiveBorrow(targetType)) {
        Type elem = borrowElementType(targetType);
        Type rhs  = analyzeWithExpected(*refStore.value, elem);
        checkCast(rhs, elem, refStore.op, "store through reference");
        return elem;
    }

    // Storing a whole object through a class reference is not yet supported.
    error(refStore.op, "storing through a class reference ('" + typeName(targetType)
          + "') is not yet supported; assign the object's fields individually instead");
    analyzeExpr(*refStore.value);
    return Type{TypeKind::Error};
}

// ============================================================
// Untyped brace initializer: `{ args }` — class deduced from the expected type
// ============================================================

Type SemanticAnalyzer::analyzeBraceInit(const BraceInitExpr& braceInit) {
    auto fail = [&](const std::string& msg) {
        error(braceInit.brace, msg);
        for (const auto& a : braceInit.args) analyzeExpr(*a);
        return Type{TypeKind::Error};
    };

    // The class comes from the expected type at the use site (a constructor argument, a var
    // initializer, or a return). A borrow/reference/value of a class all name the same class.
    std::optional<Type> expected = expectedType_;
    if (expected && expected->kind == TypeKind::Enum)
        return fail("cannot construct enum '" + expected->className + "' with `{...}`; enums are "
                    "created only through their variants");
    if (!expected || expected->className.empty()
        || (expected->kind != TypeKind::Object && expected->kind != TypeKind::Reference))
        return fail("cannot infer the type of `{...}` here; name the class explicitly, e.g. `Point{...}`");

    const std::string cls = expected->className;
    if (enumRegistry.count(cls))
        return fail("cannot construct enum '" + cls + "' with `{...}`");
    auto clsIt = classRegistry.find(cls);
    if (clsIt == classRegistry.end())
        return fail("unknown class '" + cls + "' for brace initializer");

    braceInitClass_[&braceInit] = cls;

    // Resolve the constructor overload exactly like a `Class(args)` call.
    auto ctorIt = clsIt->second.methods.find(cls);
    if (ctorIt == clsIt->second.methods.end() || ctorIt->second.empty()) {
        if (!braceInit.args.empty())
            return fail("class '" + cls + "' has no constructor but `{...}` has arguments");
        return makeObjectType(cls);
    }
    const std::vector<ClassInfo::Method>& set = ctorIt->second;
    std::vector<OverloadCand> cands;
    for (const auto& m : set)
        cands.push_back({&m.paramTypes, &m.paramMut, m.returnType, m.numDefaults, &m.paramEscapes, &m.paramNames, &m.paramHasDefault});
    expectedType_ = std::nullopt;   // the class was consumed; don't leak it into ctor args
    int idx = resolveOverload(braceInit.brace, "constructor '" + cls + "'", cands, braceInit.args);
    if (idx >= 0 && set.size() > 1)
        resolvedCallee[&braceInit] = mangleOverload(cls + "_" + cls, set[idx].paramTypes, set[idx].returnType);
    return makeObjectType(cls);
}

// ============================================================
// Cast expression analysis
// ============================================================

Type SemanticAnalyzer::analyzeCast(const CastExpr& castExpr) {
    Type toType   = resolveTypeToken(castExpr.targetType);
    // The cast target is the expected type for the operand (an explicit `as T` selects a
    // return-type overload).
    Type fromType = analyzeWithExpected(*castExpr.operand, toType);

    if (isError(fromType) || isError(toType)) return Type{TypeKind::Error};

    // Cannot cast from or to void
    if (fromType.kind == TypeKind::Void)
        error(castExpr.targetType, "cannot cast from 'void'");
    if (toType.kind == TypeKind::Void)
        error(castExpr.targetType, "cannot cast to 'void'");

    // Identity — always fine (no-op)
    if (fromType == toType) return toType;

    // Numeric ↔ numeric (any combination of I8/I16/I32/I64/U8/U16/U32/U64/F32/F64)
    if (isNumeric(fromType.kind) && isNumeric(toType.kind)) return toType;

    // Char ↔ integer / numeric
    if (fromType.kind == TypeKind::Char && (isInteger(toType.kind) || isFloat(toType.kind))) return toType;
    if ((isInteger(fromType.kind) || isFloat(fromType.kind)) && toType.kind == TypeKind::Char) return toType;

    // Bool ↔ integer / float
    if (fromType.kind == TypeKind::Bool && isNumeric(toType.kind)) return toType;
    if (isNumeric(fromType.kind) && toType.kind == TypeKind::Bool) return toType;

    // Char ↔ Bool
    if (fromType.kind == TypeKind::Char && toType.kind == TypeKind::Bool) return toType;
    if (fromType.kind == TypeKind::Bool && toType.kind == TypeKind::Char) return toType;

    // Integer ↔ ptr
    if (isInteger(fromType.kind) && toType.kind == TypeKind::Ptr) return toType;
    if (fromType.kind == TypeKind::Ptr && isInteger(toType.kind)) return toType;

    // Object → ptr (take address of stack-allocated struct)
    if (fromType.kind == TypeKind::Object && toType.kind == TypeKind::Ptr) return toType;

    // Array → ptr (pointer to first element)
    if (fromType.kind == TypeKind::Array && toType.kind == TypeKind::Ptr) return toType;

    error(castExpr.targetType,
          "cannot cast '" + typeName(fromType) + "' to '" + typeName(toType) + "'");
    return Type{TypeKind::Error};
}

// ============================================================
// new expression analysis — allocates a heap instance (Class&)
// ============================================================

Type SemanticAnalyzer::analyzeNew(const NewExpr& newExpr) {
    const std::string& className = newExpr.className.lexeme;

    if (enumRegistry.count(className)) {
        error(newExpr.className, "cannot 'new' an enum '" + className
              + "'; use one of its variants");
        for (const auto& arg : newExpr.args) analyzeExpr(*arg);
        return Type{TypeKind::Error};
    }

    auto it = classRegistry.find(className);
    if (it == classRegistry.end()) {
        error(newExpr.className, "unknown class '" + className + "' in 'new' expression");
        for (const auto& arg : newExpr.args) analyzeExpr(*arg);
        return Type{TypeKind::Error};
    }

    const ClassInfo& cls = it->second;

    // Copy construction: new Class(x) where x is a value/reference of the same
    // class. Deep-copies x; bypasses regular constructor matching.
    if (newExpr.args.size() == 1) {
        Type argType = analyzeExpr(*newExpr.args[0]);
        if (!isError(argType)
            && (argType.kind == TypeKind::Object || argType.kind == TypeKind::Reference)
            && argType.className == className) {
            return makeReferenceType(className);
        }
        // Not a copy — fall through to regular constructor matching.
    }

    auto ctorIt = cls.methods.find(className);
    if (ctorIt == cls.methods.end() || ctorIt->second.empty()) {
        // No explicit constructor — only a zero-argument `new` is allowed.
        if (!newExpr.args.empty())
            error(newExpr.className, "class '" + className
                  + "' has no constructor but 'new' was given arguments");
        for (const auto& arg : newExpr.args) analyzeExpr(*arg);
        return makeReferenceType(className);
    }

    const std::vector<ClassInfo::Method>& set = ctorIt->second;
    std::vector<OverloadCand> cands;
    for (const auto& m : set) cands.push_back({&m.paramTypes, &m.paramMut, m.returnType, m.numDefaults, &m.paramEscapes, &m.paramNames, &m.paramHasDefault});
    int idx = resolveOverload(newExpr.className, "constructor '" + className + "'", cands, newExpr.args,
                              newExpr.argNames, &newExpr);
    if (idx >= 0 && set.size() > 1)
        resolvedCallee[&newExpr] = mangleOverload(className + "_" + className,
                                                  set[idx].paramTypes, set[idx].returnType);
    return makeReferenceType(className);
}
