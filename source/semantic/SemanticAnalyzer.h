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
        bool  isPublic       = false;
        bool  isMut          = false; // `mut` — writable after construction; otherwise const
        bool  hasInitializer = false; // `= expr` / `{args}` at the declaration — exempt from the
                                       // constructor-must-initialize-every-field check
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
        // Escape analysis (computed at collection): a reference parameter "escapes" if the body
        // returns or stores it, so passing a stack value object there would dangle. `thisEscapes`
        // is the same for the implicit receiver (a value-object method call would dangle).
        std::vector<bool> paramEscapes{};
        bool              thisEscapes = false;
        // For named arguments (set post-construction, like paramEscapes).
        std::vector<std::string> paramNames{};   // parameter names
        std::vector<bool>        paramHasDefault{};  // per-parameter: has a default value?
    };
    // A static (class-level) field: shared storage, not part of the struct layout.
    struct StaticField {
        bool  isPublic = false;
        bool  isMut    = false;   // const-by-default like every other binding; `mut static` opts in
        Type  type;
        Token decl;          // field name token
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
    std::vector<bool> paramEscapes{};    // per-parameter escape bit (see ClassInfo::Method)
    std::vector<std::string> paramNames{}; // parameter names (for named arguments)
    std::vector<bool> paramHasDefault{};   // per-parameter: does it have a default value?
    // `fn private` — file-local. A cross-file call warns (not errors). `sourceFile` is the
    // declaring file's canonical path; empty for extern (always public).
    bool              isPublic = true;
    std::string       sourceFile{};
};

// One overload candidate for resolution: pointers into a registry entry + its return type.
// `numDefaults` trailing params may be omitted at the call site (filled from their defaults).
struct OverloadCand {
    const std::vector<Type>* params   = nullptr;
    const std::vector<bool>* paramMut = nullptr;
    Type                     returnType;
    size_t                   numDefaults = 0;
    const std::vector<bool>* paramEscapes = nullptr;   // per-parameter escape bit (may be null)
    // For named-argument resolution: the parameter names and a per-slot "has a default value" flag.
    // Both may be null for candidates that don't support named args (defaults to positional).
    const std::vector<std::string>* paramNames = nullptr;
    const std::vector<bool>*        paramHasDefault = nullptr;
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
    // type name → set of trait names it implements (user + built-in). Surfaced for codegen's
    // `@implements(T, Trait)` reflection fold (trait info is otherwise private to semantics).
    std::unordered_map<std::string, std::unordered_set<std::string>> implementedTraits;
    // type name → set of annotation names appearing on it or any member. Surfaced for codegen's
    // `@hasAnnotation(T, Ann)` reflection fold.
    std::unordered_map<std::string, std::unordered_set<std::string>> typeAnnotations;
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
    // Untyped brace initializer (`{...}`) → the class deduced from the expected type. Codegen
    // constructs that class (like a `Class{...}` constructor rvalue). The chosen ctor overload,
    // if any, is in resolvedCallee under the same node.
    std::unordered_map<const void*, std::string> braceInitClass;
    // Named/reordered calls: call/new node → per-parameter-slot written-argument index (or -1 =
    // fill from that slot's default). Present only when the call used named arguments; a purely
    // positional call is absent (codegen keeps its identity-order + trailing-default path).
    std::unordered_map<const void*, std::vector<int>> callArgOrder;
    // Inferred `var` locals: VarDeclExpr node → the synthesized type token for the deduced type.
    // Codegen swaps this in for the `var` sentinel token so its existing branches resolve the type.
    std::unordered_map<const void*, Token> inferredVarType;
    // Built-in `obj.clone()`: MethodCallExpr node → the receiver's class name. Codegen lowers it to
    // `@Class_clone(slot, recv)` (sret-shaped), reusing the generated memberwise clone or a user
    // `impl Clone` transparently. Result is a fresh value object.
    std::unordered_map<const void*, std::string> builtinCloneCalls;
    // Object-typed local variable ASSIGNMENTS (not declarations) that are the variable's single
    // DEFINING assignment — the binding had no live value at that point (a `mut`/const Object local
    // declared with no initializer, whose class has a constructor, is no longer definitely-
    // initialized at declaration; see analyzeVarDecl). Codegen may construct the RHS directly into
    // the destination storage (skip the temp + memberwise-clone path), exactly like a var-decl
    // initializer already does — there is no live value there yet to protect.
    std::unordered_set<const void*> directConstructAssigns;
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
    std::string         currentFile_;           // declaring file of the free function being analysed
                                                 // (empty inside class/enum/impl bodies) — drives the
                                                 // cross-file private-function-call warning
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
    // Annotation types (name → AST node) and, per type, the set of annotation names appearing
    // anywhere on it (the type itself or any member) — the source for `@hasAnnotation`.
    std::unordered_map<std::string, const AnnotationDeclStmt*>       annotationRegistry;
    std::unordered_map<std::string, std::unordered_set<std::string>> typeAnnotations;
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
    // Untyped brace-init nodes → deduced class name (copied to SemanticResult).
    std::unordered_map<const void*, std::string> braceInitClass_;
    // Named/reordered call nodes → per-slot written-arg index (copied to SemanticResult).
    std::unordered_map<const void*, std::vector<int>> callArgOrder_;
    // Inferred `var` local nodes → synthesized type token (copied to SemanticResult).
    std::unordered_map<const void*, Token> inferredVarType_;
    // Built-in `obj.clone()` nodes → receiver class name (copied to SemanticResult).
    std::unordered_map<const void*, std::string> builtinCloneCalls_;
    // Object-local defining-assignment nodes eligible for direct construction (copied to SemanticResult).
    std::unordered_set<const void*> directConstructAssigns_;
    // Contextual "expected type" for return-type overload disambiguation (set/restored
    // around initializer / rhs / return / field-assign / cast-target sub-analysis).
    std::optional<Type> expectedType_;
    // Result type of each enclosing switch *expression* (a `yield` inside a block arm is checked
    // against the top). Empty ⇒ no switch expression in scope ⇒ `yield` is an error.
    std::vector<Type>   switchExprResultStack_;
    bool                allowRawPtr_      = false; // set from CompilerOptions each call
    // Root of the compiler's own stdlib (CompilerOptions::stdlibDir; "" disables the exemption).
    // A declaration whose `filename` (the declaring file — see the per-decl filename save/restore in
    // analyzeFunctionDecl/analyzeClassDecl/analyzeImplDecl/analyzeEnumDecl) lives under this directory
    // may use raw pointers even when `allowRawPtr_` is false — see `rawPtrAllowedHere`.
    std::string         stdlibDir_;
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
    // Annotation pass: register `annotation` types + which annotation names appear on each type.
    void collectAnnotations(const Program& program);
    void analyzeImplDecl(const ImplDeclStmt& impl);
    // Verify generic trait-bound obligations recorded during monomorphization:
    // each instantiation's concrete type argument must implement its declared trait(s).
    void checkGenericBounds(const Program& program);

    // ---- Concurrency: Shareable / POD (Phase 1) ----
    // Whether a class is safe to wrap in a `Shared<T>` handle (shared by reference across threads).
    // Structural + memoized (cycle-safe). A class is Shareable iff every INSTANCE field is: a POD
    // scalar (primitive/bool/char/str/enum, any `mut`); a NON-`mut` owning-reference/`Shared` field
    // whose pointee is Shareable; a NON-`mut` value-object field of a Shareable class; or a `mut`
    // value-object field of a transitively-POD class. A `mut` reference-like field (the #2 rebind
    // double-free hazard) or ANY raw `ptr`/borrow field makes it non-Shareable. See
    // docs/concurrency.md §4. `Sendable` + the thread-boundary/statics confinement are Phase-2/task-17
    // (they need a thread boundary to gate).
    bool        isShareableClass(const std::string& className);
    // Is a slot (type + mutability) safe to SHARE in place across threads — legal as a Shared<T>
    // field AND as a static a thread may touch (a `mut` reference-like slot is rejected: rebind race).
    bool        isSharedSafeField(const Type& fieldType, bool isMut);
    bool        isPODClass(const std::string& className);   // no reference/ptr fields, transitively
    std::string shareableReason(const std::string& className);  // first offending field, for the error
    std::unordered_map<std::string, bool> shareableCache_;
    std::unordered_map<std::string, bool> podCache_;
    // Sendable — may a value cross into a spawned thread? Enforced on a thread closure's captures at
    // the `__gg_heap_closure`/`__gg_trampoline` boundary. A value is Sendable if it is copied
    // (primitive/str/enum/value object of Sendable fields) or atomically shared (`Shared<T>`); a
    // non-atomic owning `Class&`, a borrow `Class*`, or a raw `ptr` is NOT (would race the refcount /
    // dangle). See docs/concurrency.md §4.
    bool isSendableType(const Type& t);
    bool isSendableClass(const std::string& className);
    std::unordered_map<std::string, bool> sendableCache_;
    // Thread closure classes (the concrete `__lambda_N`/callable passed to `__gg_heap_closure`),
    // recorded during analysis; `checkThreadClosures` (a post-pass, after typeMap is complete) walks
    // each one's call-graph and rejects any non-Sendable STATIC it transitively touches — the
    // ambient-globals counterpart to the capture check. See SemanticAnalyzer_Thread.cpp.
    std::unordered_set<std::string> threadClosureClasses_;
    void checkThreadClosures(const Program& program);
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
    // Every instance field must be definitely assigned by the time a constructor returns, unless it
    // has a default initializer at its declaration (SymbolTable::ctorFieldInit_ tracks this,
    // seeded from ClassInfo::Field::hasInitializer). Called at each bare `return;` inside a
    // constructor (catches an early exit that skips a field) and once after the body if control can
    // fall off the end (mirrors checkReturnAliasAssignedAtExit's two-point-check shape). A no-op
    // outside a constructor.
    void checkCtorFieldsInitialized(const Token& where);
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

    // Escape analysis (SemanticAnalyzer_Escape.cpp). Fills the per-parameter (and, for methods,
    // `this`) escape bits used to reject passing a stack value object to a parameter that the
    // callee returns or stores. `computeThis` requests the receiver bit (instance methods only).
    // `objectSlotName` names a non-reference return slot (object/primitive/enum value alias): an
    // assignment `slot = p` there is a *clone* into the caller's storage, not an alias, so it must
    // NOT propagate escape back to `p`. A reference/borrow return alias passes "" (its `slot = v`
    // is a real rebind that returns the borrow). See `nonRefSlotName` in the .cpp.
    void computeParamEscapes(const std::vector<ParamDecl>& params, const BlockStmt& body,
                             bool computeThis, const std::unordered_set<std::string>& fieldNames,
                             std::vector<bool>& paramEscapesOut, bool& thisEscapesOut,
                             const std::string& objectSlotName = "");

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
    void analyzeAnnotationDecl(const AnnotationDeclStmt& annDecl);
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

    // ---- match / patterns ----
    void analyzeMatchStmt(const MatchStmt& matchStmt);
    [[nodiscard]] Type analyzeMatchExpr(const MatchExpr& matchExpr);
    // Analyze one match arm: check its pattern against the scrutinee type (declaring bindings in a
    // fresh scope), then its body. `expectedResult` non-null ⇒ expression form (arm value/yield
    // checked against it, inferred from the first arm when it starts as Error).
    void analyzeMatchArm(const MatchArm& arm, const Type& scrutineeType,
                         Type* expectedResult, const Token& matchTok);
    // Type-check a pattern against the value type it destructures, declaring each binding into the
    // current scope. Records literal-sub-pattern equality classifications (for codegen) via
    // classifyEquality. `at` is a token for diagnostics.
    void checkPattern(const Pattern& pattern, const Type& scrutType, const Token& at);
    // (`patternIsIrrefutable` is a free function in Ast.h — shared with control-flow analysis.)
    // Report an arm that can never match because an earlier arm's pattern is irrefutable.
    void checkMatchReachability(const std::deque<MatchArm>& arms);
    // Report duplicate case labels that are compile-time identifiable (int/char/bool/string
    // literals, negated int literals, enum variants, and identifier labels).
    void checkDuplicateLabels(const std::deque<SwitchArm>& arms);
    [[nodiscard]] Type analyzeAssign(const AssignExpr& assign);
    [[nodiscard]] Type analyzeCompoundAssign(const CompoundAssignExpr& compoundAssign);
    [[nodiscard]] Type analyzePostfix(const PostfixExpr& postfix);
    [[nodiscard]] Type analyzeCall(const CallExpr& call);
    [[nodiscard]] Type analyzeReflect(const ReflectExpr& reflect);   // @typeName/@fieldCount/@hasField/…
    [[nodiscard]] Type analyzeVarDecl(const VarDeclExpr& varDecl);
    // Effective mutability of a parameter (`mut` flag); see the .cpp for the borrow rules.
    [[nodiscard]] bool paramIsMutable(const ParamDecl& param, const Type& resolvedType);
    // Implicit-`this` member resolution: a bare name (not shadowed by a local/param/function)
    // may refer to a member of the enclosing class. Returns nullptr when not applicable.
    [[nodiscard]] const ClassInfo::Field* currentInstanceField(const std::string& name) const;
    [[nodiscard]] const ClassInfo::StaticField* currentStaticField(const std::string& name) const;
    [[nodiscard]] const Type*             currentStaticFieldType(const std::string& name) const;
    [[nodiscard]] const std::vector<ClassInfo::Method>* currentClassMethods(const std::string& name) const;
    // Best-match overload resolution. Analyzes `args` once, ranks candidates by argument
    // conversion cost (exact > widening > narrowing), breaks ties on return type via
    // expectedType_, emits the final arg cast/mut diagnostics on the winner, and returns
    // the winning candidate index — or -1 (having reported no-match/ambiguity).
    int resolveOverload(const Token& at, const std::string& what,
                        const std::vector<OverloadCand>& cands,
                        const std::vector<std::unique_ptr<Expr>>& args,
                        const std::vector<Token>& argNames = {},
                        const void* nodeKey = nullptr);
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
    [[nodiscard]] Type analyzeUnwrap(const UnwrapExpr& unwrap);
    [[nodiscard]] Type analyzeElvis(const ElvisExpr& elvis);
    [[nodiscard]] Type analyzeIndex(const IndexExpr& indexExpr);
    [[nodiscard]] Type analyzeIndexAssign(const IndexAssignExpr& indexAssign);
    [[nodiscard]] Type analyzeDestroy(const DestroyExpr& destroy);   // destroy(place) — unsafe dtor
    // addressOf(local) — unsafe raw address of a local's/parameter's own storage slot.
    [[nodiscard]] Type analyzeAddressOf(const AddressOfExpr& addressOf);
    // True if a class (transitively) owns a raw ptr/ptr<T> field — can't be a by-value ptr<T> element.
    [[nodiscard]] bool classOwnsRawPtr(const std::string& className,
                                       std::unordered_set<std::string>& seen) const;
    // A value object that transitively owns a raw ptr and does NOT implement `Clone` cannot be
    // copied by value — memberwise clone shallow-copies the raw pointer, aliasing the buffer (leaking
    // the old one, double-freeing the new). Errors + returns true in that case; otherwise no-op false.
    bool rejectUncloneablePtrOwner(const Type& objType, const Token& at);
    // True if a class's GENERATED memberwise clone would shallow-copy (alias) a raw pointer. A class
    // implementing `Clone` deep-copies through its impl, so it's safe and STOPS the recursion; else a
    // direct raw-ptr field, or an embedded value-object field that is itself unsafe, makes it unsafe.
    [[nodiscard]] bool memberwiseCopyAliasesRawPtr(const std::string& className,
                                                   std::unordered_set<std::string>& seen) const;
    [[nodiscard]] Type analyzeThis(const ThisExpr& thisExpr);
    [[nodiscard]] Type analyzeMemberAccess(const MemberAccessExpr& memberAccess);
    [[nodiscard]] Type analyzeMemberAssign(const MemberAssignExpr& memberAssign);
    [[nodiscard]] Type analyzeRefStore(const RefStoreExpr& refStore);
    [[nodiscard]] Type analyzeBraceInit(const BraceInitExpr& braceInit);
    [[nodiscard]] Type analyzeMethodCall(const MethodCallExpr& methodCall);
    [[nodiscard]] Type analyzeCast(const CastExpr& castExpr);
    [[nodiscard]] Type analyzeNew(const NewExpr& newExpr);

    // Helpers
    // True when raw-pointer constructs (ptr/ptr<T>, destroy, addressOf, borrow fields) are allowed
    // at the currently-analyzed declaration: either --unsafe-ptr was given, or the declaration's own
    // file (the current `filename`, kept accurate per-declaration by the save/restore in
    // analyzeFunctionDecl/analyzeClassDecl/analyzeImplDecl/analyzeEnumDecl) lives under the
    // compiler's stdlib directory. A monomorphized generic class/function is attributed to its
    // TEMPLATE's own declaring file (GenericTemplate::sourceFile), not the user file that triggered
    // the instantiation, so e.g. Array<T>'s ptr<T> buffer field is exempt while user code isn't.
    [[nodiscard]] bool rawPtrAllowedHere() const;
    // Emit an error if typeToken resolves to ptr/ptr<T> and raw pointers aren't allowed here.
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
    [[nodiscard]] Type resolveTypeToken(const Token& typeToken);
    // Intersect pre-loop and post-body smart-cast narrowing (see the .cpp).
    void mergeLoopNarrowing(const SymbolTable::InitSnapshot& before);

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
