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
        [&](const SwitchExpr& switchExpr)             { return analyzeSwitchExpr(switchExpr); },
    }, *expr.node);
    recordType(expr, resolvedType);
    return resolvedType;
}

Type SemanticAnalyzer::analyzeLiteral(const LiteralExpr& literal) {
    switch (literal.token.type) {
        case TokenType::TRUE:
        case TokenType::FALSE:
            return Type{TypeKind::Bool};
        case TokenType::NUMBER:
            // Decimal point present → floating-point literal → f64
            if (literal.token.lexeme.find('.') != std::string::npos)
                return Type{TypeKind::F64};
            return Type{TypeKind::I32};
        case TokenType::STRING:
            return Type{TypeKind::Ptr};
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

const Type* SemanticAnalyzer::currentStaticFieldType(const std::string& name) const {
    if (currentClassName.empty()) return nullptr;
    auto cit = classRegistry.find(currentClassName);
    if (cit == classRegistry.end()) return nullptr;
    auto sfit = cit->second.staticFields.find(name);
    return sfit == cit->second.staticFields.end() ? nullptr : &sfit->second.type;
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
                                      const std::vector<std::unique_ptr<Expr>>& args) {
    // The expected type applies to THIS call's return, not to its arguments — snapshot and
    // clear it so it doesn't leak into argument sub-expression resolution.
    std::optional<Type> expected = expectedType_;
    expectedType_ = std::nullopt;

    std::vector<Type> argTypes;
    argTypes.reserve(args.size());
    bool anyArgError = false;
    for (size_t k = 0; k < args.size(); ++k) {
        // An untyped brace-init argument (`{...}`) deduces its class from the parameter type.
        // Supported when the callee is unambiguous at this position (a single candidate); with
        // overloads the type must be spelled out (analyzeBraceInit reports that).
        if (args[k]->node && std::holds_alternative<BraceInitExpr>(*args[k]->node)
            && cands.size() == 1 && k < cands[0].params->size())
            expectedType_ = (*cands[0].params)[k];
        else
            expectedType_ = std::nullopt;
        Type t = analyzeExpr(*args[k]);
        if (isError(t)) anyArgError = true;
        argTypes.push_back(t);
    }
    expectedType_ = std::nullopt;
    if (anyArgError) return -1;   // a bad argument already reported an error; avoid cascades

    struct Viable { int idx; int cost; };
    std::vector<Viable> viable;
    for (int i = 0; i < static_cast<int>(cands.size()); ++i) {
        const OverloadCand& c = cands[i];
        // Arity is a range: omitted trailing args are filled from defaults, so the call is viable
        // when it supplies between (total - numDefaults) and total arguments.
        size_t total    = c.params->size();
        size_t required = total - std::min(c.numDefaults, total);
        if (args.size() < required || args.size() > total) continue;
        bool ok = true;
        int  cost = 0;
        for (size_t k = 0; k < args.size(); ++k) {   // only the supplied (leftmost) args
            const Type& pt = (*c.params)[k];
            if (argTypes[k] == pt) continue;                       // exact: cost 0
            CastResult cr = canPassArgument(argTypes[k], pt);      // incl. value-object borrow
            if (cr == CastResult::None) { ok = false; break; }
            cost += (cr == CastResult::Warn) ? 2 : 1;              // narrowing worse than widening
        }
        if (ok) viable.push_back({i, cost});
    }

    if (viable.empty()) {
        // Non-overloaded case: keep the precise arity diagnostic.
        size_t total    = cands.empty() ? 0 : cands[0].params->size();
        size_t required = cands.empty() ? 0 : total - std::min(cands[0].numDefaults, total);
        if (cands.size() == 1 && (args.size() < required || args.size() > total)) {
            std::string want = (required == total)
                ? std::to_string(total)
                : std::to_string(required) + " to " + std::to_string(total);
            error(at, what + " expects " + want + " argument(s), got " + std::to_string(args.size()));
        } else {
            error(at, "no matching overload for " + what + " with the given argument types");
        }
        return -1;
    }

    int minCost = viable.front().cost;
    for (const Viable& v : viable) if (v.cost < minCost) minCost = v.cost;
    std::vector<int> best;
    for (const Viable& v : viable) if (v.cost == minCost) best.push_back(v.idx);

    int chosen = -1;
    if (best.size() == 1) {
        chosen = best[0];
    } else if (expected) {
        // Tie on argument cost → disambiguate on return type via the contextual expected type.
        int rtBest = -1, rtCost = -1, rtTies = 0;
        for (int i : best) {
            const Type& rt = cands[i].returnType;
            int c;
            if (rt == *expected) c = 0;
            else {
                CastResult cr = canImplicitlyCast(rt, *expected);
                if (cr == CastResult::None) continue;
                c = (cr == CastResult::Warn) ? 2 : 1;
            }
            if (rtBest < 0 || c < rtCost) { rtBest = i; rtCost = c; rtTies = 1; }
            else if (c == rtCost) rtTies++;
        }
        if (rtBest >= 0 && rtTies == 1) chosen = rtBest;
    }
    if (chosen < 0) {
        error(at, "ambiguous call to overloaded " + what
              + "; add an explicit cast to select an overload");
        return -1;
    }

    // Emit the normal per-argument cast / mut diagnostics on the chosen overload only.
    const OverloadCand& w = cands[chosen];
    for (size_t k = 0; k < args.size(); ++k) {
        checkArgCast(argTypes[k], (*w.params)[k], at, "argument " + std::to_string(k + 1) + " of " + what);
        if (w.paramMut && k < w.paramMut->size() && (*w.paramMut)[k])
            warnConstToMut(at, *args[k], (*w.params)[k]);
        // Borrowing a primitive lvalue into a `ref <primitive>` parameter requires an addressable
        // argument — a temporary (e.g. `a + b`) has no address to borrow.
        if (isPrimitiveBorrow((*w.params)[k]) && !isBorrow(argTypes[k]) && !isLvalueExpr(*args[k]))
            error(at, "argument " + std::to_string(k + 1) + " of " + what + " expects a 'ref "
                  + typeName(borrowElementType((*w.params)[k]))
                  + "' but got a temporary; a borrow needs an addressable value (a variable or an "
                  "element like `a[i]`)");
        // Escape analysis: borrowing a *stack value object* as a reference is safe only if the
        // callee just borrows it. If the parameter escapes (the callee returns or stores it), the
        // reference would outlive the stack object → reject.
        if (argTypes[k].kind == TypeKind::Object && (*w.params)[k].kind == TypeKind::Reference
            && w.paramEscapes && k < w.paramEscapes->size() && (*w.paramEscapes)[k])
            error(at, "cannot pass the value object '" + argTypes[k].className + "' as argument "
                  + std::to_string(k + 1) + " of " + what + ": that parameter escapes (the callee "
                  "stores or returns it), but a stack value object has no owner to keep it alive past "
                  "the call — allocate it with `new " + argTypes[k].className
                  + "(...)` (a heap reference) instead");
    }
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
    // Static field → mutable; not-a-field → analyzeExpr(operand) already reported it.
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

Type SemanticAnalyzer::analyzeBinary(const BinaryExpr& binary) {
    // A `ref <primitive>` operand decays to its value (deref) before any numeric / operator rule —
    // otherwise a borrow (kind == Reference) would be misrouted into class operator overloading.
    Type leftType  = decayPrimitiveBorrow(analyzeExpr(*binary.left));
    Type rightType = decayPrimitiveBorrow(analyzeExpr(*binary.right));

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

        // Bitwise
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
            return commonArithmeticType(leftType, rightType);
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
            classifyEquality(scrutineeType, labelType, label->node.get(), switchTok, "switch case");
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
        if (const Type* sft = currentStaticFieldType(assign.name.lexeme)) {
            Type rhs = analyzeWithExpected(*assign.value, *sft);
            checkCast(rhs, *sft, assign.name, "static field assignment");
            return *sft;
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

    // Assigning to a `ref <primitive>` writes THROUGH the borrow (like C++ `int& r; r = 5;`); it
    // does not rebind. Requires a mutable borrow (`mut ref`); the value must match the element.
    if (isPrimitiveBorrow(sym->type)) {
        if (!sym->isMutable) {
            error(assign.name, "cannot write through a shared borrow '" + assign.name.lexeme
                  + "'; declare it 'mut ref' to allow writing to the borrowed value");
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
    if (Symbol* mut = symbolTable.lookupMutable(assign.name.lexeme))
        mut->isInitialized = true;
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
    } else if (const Type* sft = currentStaticFieldType(compoundAssign.name.lexeme)) {
        lhsType = *sft;
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
        for (const auto& m : set) cands.push_back({&m.paramTypes, &m.paramMut, m.returnType, m.numDefaults, &m.paramEscapes});
        int idx = resolveOverload(call.callee, "constructor '" + name + "'", cands, call.args);
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
            ClassInfo& info = classRegistry.at(cn);
            auto mit = info.methods.find("call");
            if (mit != info.methods.end() && !mit->second.empty()) {
                std::vector<OverloadCand> cands;
                for (const auto& m : mit->second)
                    cands.push_back({&m.paramTypes, &m.paramMut, m.returnType, m.numDefaults, &m.paramEscapes});
                int idx = resolveOverload(call.callee, "call on '" + cn + "'", cands, call.args);
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
        for (const auto& f : set) cands.push_back({&f.paramTypes, &f.paramMut, f.returnType, f.numDefaults, &f.paramEscapes});
        int idx = resolveOverload(call.callee, "function '" + name + "'", cands, call.args);
        if (idx < 0) return Type{TypeKind::Error};
        if (set.size() > 1 && !set[idx].isExtern)
            resolvedCallee[&call] = mangleOverload(name, set[idx].paramTypes, set[idx].returnType);
        return set[idx].returnType;
    }

    // Implicit `this`: a bare call may target a method of the enclosing class.
    if (const std::vector<ClassInfo::Method>* ms = currentClassMethods(name)) {
        std::vector<OverloadCand> cands;
        for (const auto& m : *ms) cands.push_back({&m.paramTypes, &m.paramMut, m.returnType, m.numDefaults, &m.paramEscapes});
        int idx = resolveOverload(call.callee, "method '" + name + "'", cands, call.args);
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
        // A `ref <primitive>` must borrow an addressable value. Binding from a fresh primitive
        // (not itself a borrow being passed along) requires an lvalue — a temporary has no address.
        if (isPrimitiveBorrow(declaredType) && !isBorrow(initializerType)
            && !isLvalueExpr(*varDecl.initializer))
            error(varDecl.name, "a 'ref " + typeName(borrowElementType(declaredType))
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
        return sf.type;
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

    return field.type;
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
                for (const auto& m : set) cands.push_back({&m.paramTypes, &m.paramMut, m.returnType, m.numDefaults, &m.paramEscapes});
                int idx = resolveOverload(methodCall.method,
                            "static method '" + methodCall.method.lexeme + "'", cands, methodCall.args);
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
    for (const auto& m : set) cands.push_back({&m.paramTypes, &m.paramMut, m.returnType, m.numDefaults, &m.paramEscapes});
    int idx = resolveOverload(methodCall.method,
                "method '" + methodCall.method.lexeme + "'", cands, methodCall.args);
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
    return method.returnType;
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
              "returns a reference (`ref T`)");
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
        cands.push_back({&m.paramTypes, &m.paramMut, m.returnType, m.numDefaults, &m.paramEscapes});
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
    for (const auto& m : set) cands.push_back({&m.paramTypes, &m.paramMut, m.returnType, m.numDefaults, &m.paramEscapes});
    int idx = resolveOverload(newExpr.className, "constructor '" + className + "'", cands, newExpr.args);
    if (idx >= 0 && set.size() > 1)
        resolvedCallee[&newExpr] = mangleOverload(className + "_" + className,
                                                  set[idx].paramTypes, set[idx].returnType);
    return makeReferenceType(className);
}
