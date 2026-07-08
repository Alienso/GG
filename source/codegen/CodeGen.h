//
// Created by Vladimir Arsenijevic on 4.6.2026.
//

#ifndef GG_CODE_GEN_H
#define GG_CODE_GEN_H

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <functional>
#include "IR.h"
#include "../parser/Ast.h"
#include "../semantic/SemanticAnalyzer.h"
#include "../CompilerOptions.h"

class CodeGen {
public:
    IRModule generate(const Program& program, const SemanticResult& semanticResult, const CompilerOptions& options = {});

private:
    // ---- Per-class info (populated in generate()) ----
    struct CGClassInfo {
        std::string                             irTypeName;      // "%Point"
        std::vector<std::pair<std::string,Type>> fields;        // instance fields: (name, type)
        std::vector<std::pair<std::string,Type>> staticFields;  // class-level fields: (name, type)
        std::unordered_set<std::string>          staticMethods; // class-level method names (no `this`)
        bool                                    hasDestructor = false;  // user-written ~Class()
        bool                                    needsDtor     = false;  // user dtor OR has reference fields
    };
    std::unordered_map<std::string, CGClassInfo>      cgClasses_;

    // Names of all enum types (used to detect static variant access EnumName.VARIANT
    // and enum variable declarations). Enum field/method info is stored in cgClasses_.
    std::unordered_set<std::string>                   cgEnumNames_;

    // ---- Scope-exit cleanup tracking ----
    // One entry per object/reference that needs cleanup when its scope ends.
    //   isReference == false : a value object living in `allocaPtr`
    //                          → call <className>_dtor(allocaPtr)
    //   isReference == true  : a reference variable whose slot `allocaPtr` holds a
    //                          heap pointer → load it and gg_release(ptr, dtor)
    struct DtorEntry {
        std::string allocaPtr;
        std::string className;
        bool        isReference = false;
    };
    std::vector<std::vector<DtorEntry>> dtorScopes_;

    // Set true the first time `new` is lowered; triggers emission of the refcount runtime.
    bool usesRefcount_ = false;

    // Classes that need a generated @Class_clone helper (deep copy: memberwise
    // copy with retain at reference-field boundaries). Populated on demand.
    std::unordered_set<std::string> clonesNeeded_;
    // Classes that need a generated @Class_structeq helper (memberwise structural `==`:
    // primitives by value, embedded value objects recursively, references/enums/ptrs by
    // address). Populated on demand for value-object comparisons without an `Eq` impl.
    std::unordered_set<std::string> structEqNeeded_;

    // Reference values produced with a +1 count (from `new` or a reference-returning
    // call) that are not yet owned by anyone. Released at the end of the enclosing
    // full expression unless a consumer claims ownership first.
    struct TempRelease { std::string ptr; std::string className; };
    std::vector<TempRelease> pendingTemps_;

    // ---- Module-level state ----
    IRModule           module;
    const ExprTypeMap* typeMap        = nullptr;
    int                stringCounter  = 0;
    std::string        currentClassName_;  // set while generating a method

    // ---- Per-function state (reset in genFunction) ----
    IRFunction*  currentFunction    = nullptr;
    BasicBlock*  currentBasicBlock  = nullptr;
    int          tempCounter        = 0;
    int          labelCounter       = 0;
    Type         currentReturnType  = Type{TypeKind::Void};

    // funcName (possibly overload-mangled) → ordered parameter types (populated in generate())
    std::unordered_map<std::string, std::vector<Type>> funcParamTypes;
    // funcName (same key) → return type. Used to emit operator-desugared method calls.
    std::unordered_map<std::string, Type> funcReturnTypes;
    // funcName (same key) → per-parameter default-value expression (nullptr = no default). A call
    // that omits trailing arguments fills them by emitting these defaults at the call site.
    std::unordered_map<std::string, std::vector<const Expr*>> funcDefaults_;
    // Base symbol names (free-fn name, or `Class_method`) declared more than once ⇒ overloaded
    // ⇒ their definitions/calls use the overload-mangled name.
    std::unordered_set<std::string> overloadedBases_;
    // Base names of all free functions + externs — used to give free functions priority over
    // an implicit-`this` method of the same name at a bare call site.
    std::unordered_set<std::string> freeFnBases_;
    // Emitted symbol names of functions/methods that return an object via a caller-provided
    // return slot (sret): they lower to `void` with a hidden first `ptr` parameter, and call
    // sites pass the destination slot instead of copying the result.
    std::unordered_set<std::string> slotReturningFns_;
    // True while emitting an object return-alias (sret) body — `return` is `ret void`.
    bool                            currentFnHasReturnSlot_ = false;
    // Non-empty while emitting a function/method whose return alias is an ordinary returned
    // local (primitive/reference alias — not sret). `return;`/fall-through return this local.
    std::string                     currentReturnAliasLocal_;
    // Chosen overload's mangled name per call/new node (from SemanticResult; may be null).
    const std::unordered_map<const void*, std::string>* resolvedCallee_ = nullptr;
    // `==`/`!=` nodes that compare two references by address (no `Eq`); emit `icmp eq/ne ptr`.
    const std::unordered_set<const void*>* addressIdentityCmp_ = nullptr;
    // `==`/`!=` value-object nodes (no `Eq`) → call the generated @Class_structeq helper.
    const std::unordered_set<const void*>* structuralValueCmp_ = nullptr;
    // Classes implementing `Eq` — a generated structeq dispatches such an embedded value field
    // to its own `@Class_eq` instead of comparing it memberwise.
    const std::unordered_set<std::string>* eqImplementors_ = nullptr;
    // `obj(args)` callable-object invocations: CallExpr node → class name; emit `@Class_call(recv,…)`.
    const std::unordered_map<const void*, std::string>* callableCalls_ = nullptr;
    // The emitted symbol name for a call/new node: the resolved mangled name if the callee is
    // overloaded, otherwise `plainBase`.
    std::string calleeName(const void* node, const std::string& plainBase) const;
    // The emitted definition name for a base symbol: overload-mangled when `base` is overloaded.
    std::string overloadEmittedName(const std::string& base,
                                    const std::vector<Type>& params, const Type& ret) const;

    bool boundsCheck = true;   // from CompilerOptions; false disables runtime checks

    // ---- Debug info (DWARF via LLVM metadata), gated by CompilerOptions::debugInfo ----
    bool        debug_ = false;
    std::string dbgSourceFile_;            // main source path → DWARF DIFile
    int         dbgNextId_ = 0;            // next "!N" metadata id
    int         dbgFileId_ = -1;           // !DIFile
    int         dbgCUId_   = -1;           // !DICompileUnit
    int         currentSubprogram_ = -1;   // !DISubprogram of the function being emitted (-1 = none)
    std::string currentDbgLoc_;            // ", !dbg !N" suffix appended by emit() (empty ⇒ none)
    std::unordered_map<int, int>         dbgLineCache_;  // line → !DILocation id (per function; cleared each fn)
    std::unordered_map<std::string, int> dbgTypeCache_;  // type key → !DIType id
    // Append a "!<id> = <body>" metadata node; returns its id.
    int  dbgAdd(const std::string& body);
    // !DIType id for a GG type (a distinct sentinel of -1 means the DWARF `null` type, i.e. void).
    int  dbgTypeOf(const Type& t);
    // Byte (size, alignment) of a GG type, matching the LLVM struct layout used for `%Class`.
    std::pair<long long, long long> dbgSizeAlign(const Type& t);
    void dbgInit();   // create the compile unit + file (called once from generate())
    // Begin/end a user function's debug scope: create its !DISubprogram, attach it, set the line.
    void dbgBeginFunction(const std::string& prettyName, const std::string& linkageName, int line,
                          const std::vector<Type>& paramTypes, const Type& returnType, bool hasThis,
                          const std::string& thisClass);
    void dbgEndFunction();
    void dbgSetLine(int line);   // update currentDbgLoc_ to a !DILocation for `line`
    void dbgStmtLine(const Stmt& stmt);   // set the current line from a statement's leading token
    // Emit a #dbg_declare associating an alloca with a source variable (argIndex>0 ⇒ parameter).
    void dbgDeclare(const std::string& allocaPtr, const std::string& name, const Type& t,
                    int line, int argIndex);

    // varName → alloca pointer register name, e.g. "x" → "%x.addr"
    std::unordered_map<std::string, std::string> allocaMap;

    // varName → declared Type (needed to emit correct load/store sizes)
    std::unordered_map<std::string, Type>        varTypeMap;

    // All alloca pointer names ever created in the current function.
    // Persists across scope save/restore so re-used variable names (e.g.
    // two for-loops both declaring 'i') always get distinct LLVM names.
    std::unordered_set<std::string>              usedAllocaNames;

    // Innermost loop exit label (for break) and re-entry label (for continue).
    // Pushed on loop entry, popped on loop exit — supports arbitrary nesting.
    std::vector<std::string> breakLabelStack;
    std::vector<std::string> continueLabelStack;

    // Innermost switch-*expression* result target: where a `yield` stores its value and the
    // block-arm merge label it branches to. Pushed per switch expression, popped after.
    struct SwitchExprTarget { std::string slotPtr; Type resultType; std::string mergeLabel; };
    std::vector<SwitchExprTarget> switchExprStack_;

    // ---- Function / extern / class codegen ----
    void genFunction(const FunctionDeclStmt& function);
    void genExternDecl(const ExternFuncDeclStmt& ext);
    void genClassDecl(const ClassDeclStmt& classDecl);
    // Emit an enum: %Enum type, @Enum$VARIANT global singletons, ctor + methods.
    void genEnumDecl(const EnumDeclStmt& enumDecl);
    // Emit @gg_enum_init (constructs every variant singleton). Records the function
    // name in globalCtors_ for a single combined @llvm.global_ctors registration.
    void genEnumInit(const Program& program);
    // Emit @gg_static_init (runs every static-field initializer). Records the
    // function name in globalCtors_.
    void genStaticInit(const Program& program);
    // Emit a single @llvm.global_ctors array from globalCtors_ (if non-empty).
    void emitGlobalCtors();
    // Names of pre-main initializer functions to register in @llvm.global_ctors.
    std::vector<std::string> globalCtors_;

    // ---- C-style static local variables ----
    // Prefix used to mangle a function's static locals into globals
    // (e.g. "counter" → @counter$calls, "Counter_make" → @Counter_make$n).
    std::string currentStaticPrefix_;
    // Global names already handed out (ensures uniqueness across same-named locals).
    std::unordered_set<std::string> usedStaticGlobals_;
    // One pending constant-initializer store per static local, emitted in @gg_static_init.
    struct StaticLocalInit { std::string global; Type type; const Expr* init; };
    std::vector<StaticLocalInit> staticLocalInits_;
    // Lower a `static <prim> name = const;` local: emit a persistent global, map the
    // name to it, and queue its initializer for pre-main execution.
    std::string genStaticLocal(const VarDeclExpr& varDecl);
    void genMethod(const std::string& className, const MethodDecl& method);
    // Generate @Class_dtor: runs the user destructor body (if any), then releases
    // each reference field in reverse declaration order. Emitted for any class that
    // has a user destructor or any reference field.
    void genDestructor(const ClassDeclStmt& classDecl);
    // Generate @Class_clone(ptr %dest, ptr %src): memberwise deep copy — copy
    // primitive fields, copy+retain reference fields, releasing dest's old targets.
    void genCloneFunction(const std::string& className);
    // Generate @Class_structeq(ptr %a, ptr %b) -> i1: memberwise structural equality.
    void genStructEqFunction(const std::string& className);

    // ---- Statement codegen ----
    void genStmt(const Stmt& stmt);
    void genBlock(const BlockStmt& blockStmt);
    void genIf(const IfStmt& ifStmt);
    void genWhile(const WhileStmt& whileStmt);
    void genFor(const ForStmt& forStmt);
    void genReturn(const ReturnStmt& returnStmt);
    void genBreak(const BreakStmt& breakStmt);
    void genContinue(const ContinueStmt& continueStmt);
    void genSwitchStmt(const SwitchStmt& switchStmt);
    void genYield(const YieldStmt& yieldStmt);
    // Store a switch-expression arm/yield value into the result slot, taking one +1 of ownership
    // for a reference result (claim a producer / retain a borrow).
    void storeSwitchArmValue(const Expr& value, const std::string& slot, const Type& resultType);
    // Emit the comparison-chain skeleton shared by the statement and expression forms.
    // `emitArmBody(armIndex)` is invoked in the matched-arm block (it emits the body and, for
    // the expression form, stores into the result slot); it must leave the block ready for the
    // trailing branch to `mergeLabel`. Returns nothing.
    void genSwitchArms(const std::deque<SwitchArm>& arms, const std::string& scrutVal,
                       const Type& scrutType, const std::string& mergeLabel,
                       const std::function<void(const SwitchArm&)>& emitArmBody);

    // ---- Expression codegen — return SSA value string ("%t3", "42", …) ----
    std::string genExpr(const Expr& expr);
    std::string genLiteral(const LiteralExpr& literal, const Type& resolvedType);
    std::string genIdentifier(const IdentifierExpr& identifier);
    std::string genUnary(const UnaryExpr& unary, const Type& resolvedType);
    std::string genBinary(const BinaryExpr& binary, const Type& resolvedType);
    std::string genSwitchExpr(const SwitchExpr& switchExpr, const Type& resolvedType);
    // Emit `lhs == rhs` (or `!=` when `op` is BANG_EQUAL) as an i1, choosing the path recorded by
    // semantics (value-object structural / reference or enum identity / Eq-impl method / primitive).
    // Operands are already-evaluated SSA values (value objects → their address). Switch labels pass
    // EQUAL_EQUAL.
    std::string emitEquality(const void* nodeKey, const std::string& lval, const Type& lt,
                             const std::string& rval, const Type& rt, TokenType op);
    std::string genAssign(const AssignExpr& assign);
    std::string genCompoundAssign(const CompoundAssignExpr& compoundAssign);
    std::string genPostfix(const PostfixExpr& postfix);
    std::string genCall(const CallExpr& call, const Type& resolvedType);
    std::string genVarDecl(const VarDeclExpr& varDecl);
    std::string genIndex(const IndexExpr& indexExpr);
    std::string genIndexAssign(const IndexAssignExpr& indexAssign);
    std::string genElementAddress(const Expr& object, const Expr& index,
                                  std::string& elementIrTypeOut);
    std::string genThis(const ThisExpr& thisExpr);
    // Implicit-`this` member access: `name` is a field (or static field) of the current
    // class, referenced without an explicit `this.`. Returns the loaded value, or ""
    // when `name` is not a member of the current class (caller falls back to "0").
    std::string genImplicitFieldLoad(const std::string& name);
    // GEP pointer for an implicit-`this` instance field, or "" if not such a field.
    std::string genImplicitFieldPtr(const std::string& name, Type& fieldTypeOut);
    // Resolve a bare name to its storage pointer + type for an assignment target: a local
    // (allocaMap), a static field global, or an implicit-`this` instance field GEP.
    // Returns false when the name is none of these.
    bool resolveAssignTarget(const std::string& name, std::string& ptrOut, Type& typeOut);
    std::string genMemberAccess(const MemberAccessExpr& memberAccess);
    std::string genMemberAssign(const MemberAssignExpr& memberAssign);
    std::string genMethodCall(const MethodCallExpr& methodCall, const Type& resolvedType);
    // If `init` is a call to a return-slot (sret) function/method, emit it writing the result
    // directly into `slotPtr` (no copy) and return true; otherwise return false (no emission).
    bool emitSlotCall(const Expr& init, const std::string& slotPtr);
    // Emit `call void @fn(ptr slot[, ptr recv], args...)`. `recvPtr` empty ⇒ no receiver.
    void emitSretCall(const std::string& fn, const std::vector<std::unique_ptr<Expr>>& args,
                      const std::string& slotPtr, const std::string& recvPtr);
    // Allocate + zero-init a temp object slot for a slot-call result used as a value (not a
    // variable initializer); registers it for scope-exit destruction. Returns the temp ptr.
    std::string materializeSlotTemp(const std::string& className);
    // True if the return-type token names a class *value* (the sret alias case), as opposed to
    // a primitive / reference / ptr / enum (which use an ordinary returned-local alias).
    bool isObjectReturnType(const Token& typeToken) const;
    // Allocate + zero/null-initialize the returned-local alias for a primitive/reference alias
    // function and record it in currentReturnAliasLocal_ (references also join the dtor list).
    void setupReturnAliasLocal(const std::string& aliasName, const Type& aliasType);
    // Emit the return of the current returned-local alias (bare `return;` / fall-through):
    // load it, apply the +1 convention for references, run dtors, `ret`.
    void emitReturnAlias();
    // Emit a desugared trait-method call `recvPtr.method(args)` for an overloaded operator.
    // Returns the result register ("" for void); writes the callee's return type to retOut.
    std::string genTraitMethodCall(const void* node, const std::string& className,
                                   const std::string& method, const std::string& recvPtr,
                                   const std::vector<Type>& argTypes,
                                   const std::vector<std::string>& argVals, Type& retOut);
    // Emit a static method call (no implicit `this`): @ClassName_method(args).
    std::string genStaticCall(const std::string& className,
                              const MethodCallExpr& methodCall,
                              const Type& resolvedType,
                              const std::string& returnIrType);
    std::string genCast(const CastExpr& castExpr, const Type& toType);
    std::string genNew(const NewExpr& newExpr, const Type& resolvedType);
    std::string genSizeof(const SizeofExpr& sizeofExpr);

    // ---- Destructor helpers ----
    // Emit destructor / release calls for all entries in one scope (reverse order).
    void emitDtorsForScope(const std::vector<DtorEntry>& scope);

    // Emit the refcount runtime (gg_alloc/gg_retain/gg_release) + malloc/free decls.
    void emitRefcountRuntime();

    // ---- Shared codegen helpers ----
    // Returns a unique alloca pointer name for `varName` (e.g. "%x.addr" or "%x.addr.1")
    // and registers it in usedAllocaNames.
    std::string freshAllocaName(const std::string& varName);

    // Spill a parameter list into alloca slots; populates allocaMap / varTypeMap.
    void spillParamsToAllocas(const std::vector<ParamDecl>& params);

    // Emit the body of a function (open dtor scope, gen stmts, ensure termination, close scope).
    void emitFunctionBody(const BlockStmt& body, const std::string& returnIrType);

    // Build a comma-separated LLVM argument string for a call, casting each arg to
    // the declared param type when provided. Any omitted trailing parameters are filled by
    // emitting their default-value expressions (`defaults`, keyed positionally; may be null).
    std::string buildArgString(const std::vector<std::unique_ptr<Expr>>& args,
                               const std::vector<Type>* declaredParamTypes,
                               const std::vector<const Expr*>* defaults = nullptr);
    // Per-parameter default expressions for an emitted callee name, or nullptr if it has none.
    const std::vector<const Expr*>* defaultsFor(const std::string& emittedName) const;

    // Emit a GEP for field `fieldName` on `objPtr` of class `className`.
    // Returns {gepRegister, fieldType}; returns {"0", Error} on lookup failure.
    std::pair<std::string, Type> resolveFieldGEP(const std::string& objPtr,
                                                  const std::string& className,
                                                  const std::string& fieldName);

    // Return the type of a static field `className::fieldName`, or nullptr if no
    // such static field exists (e.g. it is an instance field).
    const Type* findStaticField(const std::string& className,
                                const std::string& fieldName) const;

    // ---- Bounds check helpers ----
    void ensureAbortDeclared();
    void emitBoundsCheck(const std::string& indexValue, size_t arraySize);

    // ---- Low-level emit helpers ----
    void        emit(const std::string& instruction);
    void        emitAlloca(const std::string& ptrName, const std::string& irType);
    void        emitStore(const std::string& irType,
                          const std::string& value,
                          const std::string& ptr);
    std::string emitLoad(const std::string& irType, const std::string& ptr);
    void        emitBr(const std::string& label);
    void        emitCondBr(const std::string& cond,
                           const std::string& trueLabel,
                           const std::string& falseLabel);
    void        switchBlock(const std::string& label);

    // ---- Value / type helpers ----
    std::string freshTemp();
    std::string freshLabel(const std::string& hint);

    // Resolve a ParamDecl type token — handles IDENTIFIER tokens naming a class.
    Type        resolveParamType(const ParamDecl& param) const;

    // Resolve a return-type token. Decodes "Class&" → Reference; otherwise primitives/void.
    Type        resolveReturnType(const Token& typeToken) const;

    // ---- Reference-return / temporary lifetime helpers ----
    // True if `e` yields a +1 reference (a `new` or a reference-returning call).
    [[nodiscard]] bool producesPlusOne(const Expr& e) const;
    // A consumer takes ownership of the +1 temporary `ptr` (drop its pending release).
    void claimTemp(const std::string& ptr);
    // Release every still-pending +1 reference temporary (end of a full expression).
    void flushTempReleases();

    // IR type used to pass `type` as a function argument / parameter.
    // Objects are passed by reference (ptr); all other types by value.
    static std::string paramIrType(const Type& type);

    // Look up the resolved type for an expression via the side-table.
    // Returns TypeKind::Error if not found (should not happen after semantic pass).
    Type        exprType(const Expr& expression) const;

    // Insert a cast instruction if from != to; return (possibly unchanged) value reg.
    std::string emitCast(const std::string& value, const Type& from, const Type& to);

    // Ensure result is i1 (suitable for conditional branches).
    std::string emitToBool(const std::string& value, const Type& valueType);

    // Arithmetic instruction name (add/fadd/sub/…) for a binary op on type t.
    static std::string arithInstr(TokenType operatorType, const Type& type);

    // Comparison instruction fragment (icmp slt / fcmp olt / …).
    static std::string cmpInstr(TokenType operatorType, const Type& type);

    // Compound-assign base op (PLUS_EQUAL → PLUS, etc.).
    static TokenType compoundBaseOp(TokenType operatorType);
};


#endif //GG_CODE_GEN_H
