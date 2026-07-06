//
// Created by Vladimir Arsenijevic on 01.6.2026.
//

#ifndef GG_SEMANTICANALYZER_H
#define GG_SEMANTICANALYZER_H

#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "SymbolTable.h"
#include "Type.h"
#include "../parser/Ast.h"
#include "../CompileError.h"
#include "../CompilerOptions.h"

// Maps each Expr variant pointer to its resolved Type.
// Key = expr.node.get() — stable because the AST is never mutated during analysis.
using ExprTypeMap = std::unordered_map<const Expr::Variant*, Type>;

// First (leftmost) token of an expression — for error reporting on nodes that
// store no keyword token of their own. Defined in SemanticAnalyzer_Stmts.cpp.
const Token& exprFirstToken(const Expr& expr);

// ---- ClassInfo: semantic information about a class ----

struct ClassInfo {
    struct Field {
        bool  isPublic = false;
        bool  isMut    = false; // `mut` — writable after construction; otherwise const
        Type  type;
        int   index    = 0;  // field index in the struct (0-based, declaration order)
        Token decl;          // the field name token, for error reporting
    };
    struct Method {
        bool             isPublic  = false;
        bool             isStatic  = false;   // class-level method (no implicit `this`)
        bool             isMut     = false;   // `T m(...) mut` — may mutate `this`
        Type             returnType;
        std::vector<Type> paramTypes;
        std::vector<bool> paramMut;           // per-parameter `mut` flag
        size_t           numDefaults = 0;     // count of trailing params with default values
        Token            decl;     // method name token
    };
    // A static (class-level) field: shared storage, not part of the struct layout.
    struct StaticField {
        bool  isPublic = false;
        Type  type;
        Token decl;          // field name token
        bool  hasInit  = false;  // true if it carries a constant initializer
    };
    std::vector<std::string>              fieldOrder;   // preserves declaration order
    std::unordered_map<std::string, Field>  fields;
    std::unordered_map<std::string, StaticField> staticFields;
    // Overload set per method/constructor name (>1 entry ⇒ overloaded ⇒ mangled).
    std::unordered_map<std::string, std::vector<Method>> methods;
    std::optional<Method>                   destructor; // at most one per class
};

// A free-function overload (or extern). Overload sets live in SemanticAnalyzer::functionRegistry.
struct FunctionOverload {
    Type              returnType;
    std::vector<Type> paramTypes;
    std::vector<bool> paramMut;
    size_t            numDefaults = 0;   // count of trailing params with default values
    bool              isExtern = false;
    Token             decl;
};

// One overload candidate for resolution: pointers into a registry entry + its return type.
// `numDefaults` trailing params may be omitted at the call site (filled from their defaults).
struct OverloadCand {
    const std::vector<Type>* params;
    const std::vector<bool>* paramMut;
    Type                     returnType;
    size_t                   numDefaults = 0;
};

// ---- EnumInfo: semantic information about a Java-style enum ----
// An enum reuses ClassInfo (stored in classRegistry) for its fields, constructor
// and methods. EnumInfo tracks the variant list and marks the name as an enum so
// that type resolution yields TypeKind::Enum and direct construction is rejected.
struct EnumInfo {
    std::vector<std::string>        variantOrder;  // declaration / ordinal order
    std::unordered_set<std::string> variantSet;    // membership test
    Token                           decl;          // enum name token
};

struct SemanticResult {
    bool        hadError = false;
    ExprTypeMap typeMap;
    std::unordered_map<std::string, ClassInfo> classRegistry;
    std::unordered_map<std::string, EnumInfo>  enumRegistry;
    // Chosen overload's mangled symbol name per call/new expression node (keyed by the
    // node's address). Absent/empty ⇒ the callee is not overloaded ⇒ use its plain name.
    std::unordered_map<const void*, std::string> resolvedCallee;
    // `==` / `!=` BinaryExpr nodes that compare two class *references* by address (identity)
    // because the class does not implement `Eq` (codegen emits `icmp eq/ne ptr`).
    std::unordered_set<const void*> addressIdentityCmp;
    // `==` / `!=` BinaryExpr nodes where at least one operand is a value object and the class
    // does not implement `Eq` — compared by generated memberwise structural equality.
    std::unordered_set<const void*> structuralValueCmp;
    // Classes that implement the `Eq` trait. Used by codegen so a generated structeq dispatches
    // an embedded value-object field with its own `Eq` impl to that `eq` (not memberwise).
    std::unordered_set<std::string> eqImplementors;
    // `obj(args)` callable-object invocations: CallExpr node → the callee's class name. Codegen
    // emits `@Class_call(recv, args)` with the callee variable as the receiver.
    std::unordered_map<const void*, std::string> callableCalls;
};

class SemanticAnalyzer {
public:
    SemanticResult analyze(const Program& program,
                           const std::string& filename = "",
                           const CompilerOptions& options = {});  // resets all state per call

private:
    SymbolTable         symbolTable;
    ExprTypeMap         typeMap;
    bool                hadError          = false;
    std::string         filename;                // source filename for error messages
    std::optional<Type> currentReturnType; // nullopt = top-level (not inside a function)
    int                 loopDepth         = 0;  // > 0 while inside a while/for loop
    std::string         currentClassName;       // set while analysing a class body
    bool                currentClassIsEnum = false; // true while analysing an enum body
    bool                currentMethodIsStatic = false; // true while analysing a static method body
    bool                inEnumConstructor  = false; // true while analysing an enum's constructor body
    bool                inConstructor      = false; // true while analysing a class's constructor body
    bool                currentThisMutable = false; // true while analysing a `mut` method / ctor / dtor
    std::string         currentSelfType_;           // the `Self` type while in a trait/impl body
    std::string         currentReturnSlotName_;     // non-empty while inside a function/method with a return alias
    bool                currentReturnAliasIsRef_ = false; // the return alias is a reference (must be assigned before return)
    std::unordered_map<std::string, ClassInfo> classRegistry;
    std::unordered_map<std::string, EnumInfo>  enumRegistry;
    // All declared class / enum names, populated before field types are resolved so a field
    // may name a value-object type declared later in the file (forward reference).
    std::unordered_set<std::string> declaredClassNames_;
    std::unordered_set<std::string> declaredEnumNames_;
    // Trait declarations (name → AST node) and, per type, the set of traits it implements.
    std::unordered_map<std::string, const TraitDeclStmt*>          traitRegistry;
    std::unordered_map<std::string, std::unordered_set<std::string>> implementedTraits;
    // Free-function overload sets (name → overloads). >1 entry ⇒ overloaded ⇒ mangled.
    std::unordered_map<std::string, std::vector<FunctionOverload>> functionRegistry;
    // Chosen overload mangled name per call/new node address (copied to SemanticResult).
    std::unordered_map<const void*, std::string> resolvedCallee;
    // `==`/`!=` reference-identity comparison nodes (copied to SemanticResult).
    std::unordered_set<const void*> addressIdentityCmp_;
    // `==`/`!=` value-object memberwise structural comparison nodes (copied to SemanticResult).
    std::unordered_set<const void*> structuralValueCmp_;
    // `obj(args)` callable-object invocation nodes → class name (copied to SemanticResult).
    std::unordered_map<const void*, std::string> callableCalls_;
    // Contextual "expected type" for return-type overload disambiguation (set/restored
    // around initializer / rhs / return / field-assign / cast-target sub-analysis).
    std::optional<Type> expectedType_;
    // Result type of each enclosing switch *expression* (a `yield` inside a block arm is checked
    // against the top). Empty ⇒ no switch expression in scope ⇒ `yield` is an error.
    std::vector<Type>   switchExprResultStack_;
    bool                allowRawPtr_      = false; // set from CompilerOptions each call
    // Active only while checking a generic template body (checkGenericBodies): maps each type
    // parameter name to its bound trait names (empty ⇒ unbounded, permissive). A bare `T` /
    // `T&` value is then an abstract type usable only via what its bounds provide.
    std::unordered_map<std::string, std::vector<std::string>> currentTypeParamBounds_;

    // Pass 0: collect class declarations (before collectFunctions)
    void collectClasses(const Program& program);
    // Pass 0a: reject value-object field cycles (`class A{B b} class B{A a}`) — an infinite-size
    // struct. Only value-object embedding counts; reference/ptr fields break cycles.
    void checkValueFieldCycles(const Program& program);
    // Definition-time checking of bounded generic template bodies against their bounds: a value of
    // a bounded param `T: A + B` may be used only via methods/operators that A or B provide.
    void checkGenericBodies(const Program& program);
    // Type-check each parameter's default value against its declared type. Analyzed in the
    // enclosing scope with no parameters / `this` / instance fields visible, so a default cannot
    // reference them (evaluated per-call at the call site).
    void analyzeParamDefaults(const std::vector<ParamDecl>& params);
    // If `t` is (a value/reference/param of) a current generic type parameter, return its bound
    // trait names (may be empty = unbounded); nullptr if `t` is not a type parameter.
    [[nodiscard]] const std::vector<std::string>* typeParamBoundsOf(const Type& t) const;
    // Resolve method `name`/arity against a type parameter's bounds (user-trait methods + built-in
    // operator-trait conventional methods). On success sets `out` (Self → the parameter).
    [[nodiscard]] bool resolveBoundMethod(const std::vector<std::string>& bounds,
                                          const std::string& paramName, const std::string& name,
                                          size_t argc, Type& out);
    [[nodiscard]] bool builtinBoundMethod(const std::string& trait, const std::string& method,
                                          size_t argc, const std::string& paramName, Type& out) const;
    // Trait/impl passes: register trait contracts, then attach impl methods to their target
    // class and check conformance.
    void collectTraits(const Program& program);
    void collectImpls(const Program& program);
    void analyzeImplDecl(const ImplDeclStmt& impl);
    // Verify generic trait-bound obligations recorded during monomorphization:
    // each instantiation's concrete type argument must implement its declared trait(s).
    void checkGenericBounds(const Program& program);
    // Set up an arrow-form return slot for a function/method body: validate the slot type
    // is a class, inject the slot as a mutable initialized local, and set
    // currentReturnSlotName_. When there is no slot but the return type is an object value,
    // report the "requires a return slot" error. Call inside the function/method scope.
    void setupReturnSlot(bool hasReturnSlot, const std::string& slotName,
                         const Type& returnType, const Token& nameToken);
    // If the current function has a reference return alias and control can fall off the end
    // (no guaranteed return), require the alias to be definitely assigned. Call after the body,
    // before exitScope.
    void checkReturnAliasAssignedAtExit(const BlockStmt& body, const Token& where);
    // Operator → (built-in trait name, method name), or nullptr if the operator isn't
    // overloadable. Also recognises the built-in operator-trait names.
    [[nodiscard]] static const std::pair<const char*, const char*>* operatorTraitFor(TokenType op);
    [[nodiscard]] static bool isBuiltinTrait(const std::string& name);
    // Build the shared ClassInfo (fields + methods + optional destructor) for a
    // class or enum body. allowDestructor is false for enums.
    [[nodiscard]] ClassInfo buildClassInfo(const std::string& ownerName,
                                           const std::deque<FieldDecl>& fields,
                                           const std::deque<MethodDecl>& methods,
                                           bool allowDestructor);

    // Pass 1: hoist top-level function signatures into the global scope
    void collectFunctions(const Program& program);

    // Statement analysis
    void analyzeStmt(const Stmt& stmt);
    void analyzeBlock(const BlockStmt& block);
    void analyzeIf(const IfStmt& ifStmt);
    void analyzeWhile(const WhileStmt& whileStmt);
    void analyzeFor(const ForStmt& forStmt);
    void analyzeReturn(const ReturnStmt& returnStmt);
    void analyzeBreak(const BreakStmt& breakStmt);
    void analyzeContinue(const ContinueStmt& continueStmt);
    void analyzeSwitchStmt(const SwitchStmt& switchStmt);
    void analyzeYield(const YieldStmt& yieldStmt);
    void analyzeFunctionDecl(const FunctionDeclStmt& functionDecl);
    void analyzeExternFuncDecl(const ExternFuncDeclStmt& externDecl);
    void analyzeClassDecl(const ClassDeclStmt& classDecl);
    void analyzeEnumDecl(const EnumDeclStmt& enumDecl);

    // Expression analysis — returns resolved Type and records it in typeMap.
    // Not [[nodiscard]] because it is intentionally called for side effects
    // in error-recovery paths (e.g. analysing arguments after a type error).
    Type analyzeExpr(const Expr& expr);
    [[nodiscard]] Type analyzeLiteral(const LiteralExpr& literal);
    [[nodiscard]] Type analyzeIdentifier(const IdentifierExpr& identifier);
    [[nodiscard]] Type analyzeUnary(const UnaryExpr& unary);
    [[nodiscard]] Type analyzeBinary(const BinaryExpr& binary);
    [[nodiscard]] Type analyzeSwitchExpr(const SwitchExpr& switchExpr);
    // Shared equality (==/!=) classifier: records the codegen decision (Eq-impl overload /
    // reference address identity / value-object structural) keyed by `nodeKey`, and returns
    // Bool (or Error, emitting a diagnostic via `what` at `at`). Reused by switch case labels.
    [[nodiscard]] Type classifyEquality(const Type& leftType, const Type& rightType,
                                        const void* nodeKey, const Token& at,
                                        const std::string& what);
    // Analyze one switch arm's labels against the scrutinee type and its body; used by both
    // the statement and expression forms. `expectedResult` (non-null) means expression form:
    // arm value/yield must produce a value assignable to *expectedResult (updated in place if
    // it starts as Error to infer from the first arm).
    void analyzeSwitchArm(const SwitchArm& arm, const Type& scrutineeType,
                          Type* expectedResult, const Token& switchTok);
    // Report duplicate case labels that are compile-time identifiable (int/char/bool/string
    // literals, negated int literals, enum variants, and identifier labels).
    void checkDuplicateLabels(const std::deque<SwitchArm>& arms);
    [[nodiscard]] Type analyzeAssign(const AssignExpr& assign);
    [[nodiscard]] Type analyzeCompoundAssign(const CompoundAssignExpr& compoundAssign);
    [[nodiscard]] Type analyzePostfix(const PostfixExpr& postfix);
    [[nodiscard]] Type analyzeCall(const CallExpr& call);
    [[nodiscard]] Type analyzeVarDecl(const VarDeclExpr& varDecl);
    // Effective mutability of a parameter (`mut` flag); see the .cpp for the borrow rules.
    [[nodiscard]] bool paramIsMutable(const ParamDecl& param, const Type& resolvedType);
    // Implicit-`this` member resolution: a bare name (not shadowed by a local/param/function)
    // may refer to a member of the enclosing class. Returns nullptr when not applicable.
    [[nodiscard]] const ClassInfo::Field* currentInstanceField(const std::string& name) const;
    [[nodiscard]] const Type*             currentStaticFieldType(const std::string& name) const;
    [[nodiscard]] const std::vector<ClassInfo::Method>* currentClassMethods(const std::string& name) const;
    // Best-match overload resolution. Analyzes `args` once, ranks candidates by argument
    // conversion cost (exact > widening > narrowing), breaks ties on return type via
    // expectedType_, emits the final arg cast/mut diagnostics on the winner, and returns
    // the winning candidate index — or -1 (having reported no-match/ambiguity).
    int resolveOverload(const Token& at, const std::string& what,
                        const std::vector<OverloadCand>& cands,
                        const std::vector<std::unique_ptr<Expr>>& args);
    // Analyze `e` with a contextual expected type set (for return-type overload
    // disambiguation), restoring the previous expected type afterward.
    Type analyzeWithExpected(const Expr& e, const Type& expected);
    // Validate a `++`/`--` target (local, or implicit-`this` field). Emits an error and
    // returns false if the target is immutable; returns true otherwise.
    bool incDecTargetOk(const Token& op, const std::string& name);
    // True if `expr` denotes a mutable place — a `mut` binding, `this`, a freshly-owned
    // reference (`new`/call result), or a `mut`-field access chain whose root is mutable.
    // Used for transitive const (field-write receiver) and the const→mut cast warning.
    [[nodiscard]] bool exprIsMutablePlace(const Expr& expr);
    // Emit the const→mut coercion warning when a read-only reference `source` flows into a
    // `mut` reference target. No-op for non-reference targets, mutable sources, or an
    // explicit cast (`as mut T`), which is the sanctioned way to silence it.
    void warnConstToMut(const Token& at, const Expr& source, const Type& targetType);
    // True if `expr` is a compile-time constant (literal, or unary/binary/cast of
    // constants). Used to validate static-local initializers, which run pre-main.
    [[nodiscard]] static bool isConstantExpr(const Expr& expr);
    [[nodiscard]] Type analyzeIndex(const IndexExpr& indexExpr);
    [[nodiscard]] Type analyzeIndexAssign(const IndexAssignExpr& indexAssign);
    [[nodiscard]] Type analyzeThis(const ThisExpr& thisExpr);
    [[nodiscard]] Type analyzeMemberAccess(const MemberAccessExpr& memberAccess);
    [[nodiscard]] Type analyzeMemberAssign(const MemberAssignExpr& memberAssign);
    [[nodiscard]] Type analyzeMethodCall(const MethodCallExpr& methodCall);
    [[nodiscard]] Type analyzeCast(const CastExpr& castExpr);
    [[nodiscard]] Type analyzeNew(const NewExpr& newExpr);

    // Helpers
    // Emit an error if typeToken resolves to ptr/ptr<T> and --unsafe-ptr was not given.
    // Exempt from the check: extern declarations (CRT bindings always need ptr).
    void          checkRawPtrAllowed(const Token& typeToken, const Token& site);

    void          enterScope();
    void          exitScope();
    const Symbol* lookupSymbol(const Token& nameToken);  // emits error if missing
    void          error(const Token& token, const std::string& message);
    void          warn(const Token& token, const std::string& message);
    void          recordType(const Expr& expr, const Type& type);
    void          checkCast(const Type& from, const Type& to, const Token& site,
                            const std::string& context);
    // Like checkCast, but for argument position: also silently accepts a value-object →
    // reference borrow (see canPassArgument). Used by overload resolution / operator desugaring.
    void          checkArgCast(const Type& from, const Type& to, const Token& site,
                               const std::string& context);
    // Resolve a type token — handles IDENTIFIER tokens that name a known class.
    [[nodiscard]] Type resolveTypeToken(const Token& typeToken) const;

    // Resolve an Object type to its ClassInfo; emits error and returns nullptr if not an Object or class not found.
    [[nodiscard]] const ClassInfo* lookupObjectClass(Type objectType, const Token& site);
    // Type-check and analyse a call's arguments against declared param types.
    void analyzeCallArgs(const std::vector<std::unique_ptr<Expr>>& args,
                         const std::vector<Type>& paramTypes,
                         const Token& callee,
                         const std::string& context,
                         const std::vector<bool>& paramMut = {});
    // Emit a compile-time out-of-bounds error if `indexExpr` is a constant literal outside [0, arraySize).
    void checkConstantIndexBounds(const Expr& indexExpr, size_t arraySize);
};

#endif //GG_SEMANTICANALYZER_H
