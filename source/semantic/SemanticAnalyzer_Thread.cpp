//
// Thread static-confinement pass (concurrency Phase 1).
//
// The Sendable boundary check (in analyzeCall's `__gg_heap_closure` case) gates a thread closure's
// CAPTURES — its free local/parameter variables, reified as fields on the `__lambda_N` class. But a
// `static` field accessed by name inside the body is NOT a capture: it is ambient global state the
// thread reaches directly, so the capture check never sees it. This pass closes that hole.
//
// It walks each thread closure's call-graph (its `call` method, transitively into every function /
// method it invokes) and requires every STATIC it touches to be Sendable — a `mut static Class&`
// (non-atomic owning reference, reachable/rebindable from two threads) is a compile error; a
// `mut static i32` (POD scalar, racy-but-memory-safe) or a `static Shared<T>` (atomic) is fine.
//
// Soundness: the call graph is over-approximated (all overloads of a called free-function name are
// scanned; a method call resolves its receiver class precisely via the completed typeMap), so it
// never MISSES a reachable static — at worst it scans a body that isn't actually called, which can
// only over-report. Bare-identifier static reads are gated only when the name is not a declared
// local/parameter (which shadows the static), avoiding that false positive.
//
// v1 limits: an `obj(args)` callable-object invocation whose class isn't in `callableCalls_`, and an
// indirect call through a function value, are not followed (GG has neither function pointers nor a
// way to spell a closure type, so this is not reachable from ordinary code today). Static
// *initialization* is unaffected — statics init before `main`, single-threaded.
//

#include "SemanticAnalyzer.h"
#include "../parser/Ast.h"

#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

void SemanticAnalyzer::checkThreadClosures(const Program& program) {
    if (threadClosureClasses_.empty()) return;

    // ---- Index the program: free functions by name, methods/ctors by "Class::method". ----
    std::unordered_map<std::string, std::vector<const FunctionDeclStmt*>> freeFns;
    std::unordered_map<std::string, std::vector<const MethodDecl*>>       methods;
    auto addMethods = [&](const std::string& owner, const std::deque<MethodDecl>& ms) {
        for (const MethodDecl& m : ms) methods[owner + "::" + m.name.lexeme].push_back(&m);
    };
    for (const Stmt& s : program.declarations) {
        if (!s.node) continue;
        if (const auto* fd = std::get_if<FunctionDeclStmt>(s.node.get())) freeFns[fd->name.lexeme].push_back(fd);
        else if (const auto* cd = std::get_if<ClassDeclStmt>(s.node.get())) addMethods(cd->name.lexeme, cd->methods);
        else if (const auto* ed = std::get_if<EnumDeclStmt>(s.node.get()))  addMethods(ed->name.lexeme, ed->methods);
        else if (const auto* im = std::get_if<ImplDeclStmt>(s.node.get()))  addMethods(im->typeName.lexeme, im->methods);
    }

    // ---- Worklist over reachable bodies (deduped by body pointer). ----
    struct WorkItem { const BlockStmt* body; std::string cls; const std::vector<ParamDecl>* params; };
    std::vector<WorkItem>          work;
    std::unordered_set<const void*> visitedBodies;

    auto pushBody = [&](const BlockStmt* b, const std::string& cls, const std::vector<ParamDecl>* params) {
        if (b && visitedBodies.insert(b).second) work.push_back({ b, cls, params });
    };
    auto enqueueFn = [&](const std::string& name) {
        auto it = freeFns.find(name);
        if (it != freeFns.end()) for (const auto* fd : it->second) pushBody(&fd->body, "", &fd->params);
    };
    auto enqueueMethod = [&](const std::string& cls, const std::string& m) {
        auto it = methods.find(cls + "::" + m);
        if (it != methods.end()) for (const auto* md : it->second) pushBody(&md->body, cls, &md->params);
    };

    // The class named by an expression used as a receiver: a bare class name (static `Class::x`
    // form) or the receiver's resolved type (via the completed typeMap). "" if unresolved.
    auto classOf = [&](const Expr& e) -> std::string {
        if (!e.node) return "";
        if (const auto* id = std::get_if<IdentifierExpr>(e.node.get()))
            if (classRegistry.count(id->name.lexeme)) return id->name.lexeme;
        auto it = typeMap.find(e.node.get());
        return it != typeMap.end() ? it->second.className : std::string{};
    };
    // Gate one static reference: error iff `owner::field` is a static a thread cannot safely share.
    // A static is SHARED in place (not copied), so the rule is the Shared<T>-field rule
    // (isSharedSafeField), NOT isSendableType (which governs by-value captures) — critically, a
    // `mut` reference-like static (even an atomic `mut static Shared<T>`) is rejected because it can
    // be REBOUND from two threads (a torn store / double-free); atomicity guards the count, not the slot.
    auto gateStatic = [&](const std::string& owner, const Token& field) {
        auto cIt = classRegistry.find(owner);
        if (cIt == classRegistry.end()) return;
        auto sIt = cIt->second.staticFields.find(field.lexeme);
        if (sIt == cIt->second.staticFields.end()) return;   // instance field / not a field → not gated
        const Type& t = sIt->second.type;
        if (isSharedSafeField(t, sIt->second.isMut)) return;
        std::string why;
        if (t.kind == TypeKind::Ptr || t.kind == TypeKind::TypedPtr) why = "it is a raw pointer";
        else if (t.kind == TypeKind::Reference && t.borrow)          why = "it is a borrow";
        else if (t.kind == TypeKind::Reference && sIt->second.isMut)
            why = "a `mut` static reference/handle can be rebound from another thread (a torn store / "
                  "double-free) — make it non-`mut` (and, for an owning reference, a `static Shared<...>`)";
        else if (t.kind == TypeKind::Reference)
            why = "'" + t.className + "' is a non-atomic owning reference — use a `static Shared<...>` (atomic)";
        else
            why = "'" + typeName(t) + "' is not safe to share across threads";
        error(field, "thread closure touches static '" + owner + "::" + field.lexeme + "' of type '"
              + typeName(t) + "', which is not thread-safe: " + why);
    };

    std::function<void(const Expr&, const std::string&, std::unordered_set<std::string>&)> scanExpr;
    std::function<void(const Stmt&, const std::string&, std::unordered_set<std::string>&)> scanStmt;

    scanExpr = [&](const Expr& e, const std::string& cls, std::unordered_set<std::string>& locals) {
        if (!e.node) return;
        const auto& v = *e.node;

        if (const auto* id = std::get_if<IdentifierExpr>(&v)) {
            if (!cls.empty() && !locals.count(id->name.lexeme)) gateStatic(cls, id->name);  // implicit static read
            return;
        }
        if (const auto* a = std::get_if<AssignExpr>(&v)) {
            if (!cls.empty() && !locals.count(a->name.lexeme)) gateStatic(cls, a->name);    // implicit static write
            if (a->value) scanExpr(*a->value, cls, locals);
            return;
        }
        if (const auto* c = std::get_if<CompoundAssignExpr>(&v)) {
            if (!cls.empty() && !locals.count(c->name.lexeme)) gateStatic(cls, c->name);
            if (c->value) scanExpr(*c->value, cls, locals);
            return;
        }
        if (const auto* ma = std::get_if<MemberAccessExpr>(&v)) {
            if (ma->object) { gateStatic(classOf(*ma->object), ma->field); scanExpr(*ma->object, cls, locals); }
            return;
        }
        if (const auto* ma = std::get_if<MemberAssignExpr>(&v)) {
            if (ma->object) { gateStatic(classOf(*ma->object), ma->field); scanExpr(*ma->object, cls, locals); }
            if (ma->value) scanExpr(*ma->value, cls, locals);
            return;
        }
        if (const auto* vd = std::get_if<VarDeclExpr>(&v)) {
            locals.insert(vd->name.lexeme);   // shadows any same-named static from here on
            if (vd->initializer) scanExpr(*vd->initializer, cls, locals);
            return;
        }
        if (const auto* call = std::get_if<CallExpr>(&v)) {
            const std::string& callee = call->callee.lexeme;
            auto ccIt = callableCalls_.find(static_cast<const void*>(e.node.get()));
            if (classRegistry.count(callee))              enqueueMethod(callee, callee);   // constructor
            else if (ccIt != callableCalls_.end())        enqueueMethod(ccIt->second, "call");  // obj(args) sugar
            else if (freeFns.count(callee))               enqueueFn(callee);
            else if (!cls.empty())                        enqueueMethod(cls, callee);      // implicit this-method
            for (const auto& arg : call->args) if (arg) scanExpr(*arg, cls, locals);
            return;
        }
        if (const auto* mc = std::get_if<MethodCallExpr>(&v)) {
            if (mc->object) { enqueueMethod(classOf(*mc->object), mc->method.lexeme); scanExpr(*mc->object, cls, locals); }
            for (const auto& arg : mc->args) if (arg) scanExpr(*arg, cls, locals);
            return;
        }
        if (const auto* n = std::get_if<NewExpr>(&v)) {
            enqueueMethod(n->className.lexeme, n->className.lexeme);   // constructor
            for (const auto& arg : n->args) if (arg) scanExpr(*arg, cls, locals);
            return;
        }
        if (const auto* bi = std::get_if<BraceInitExpr>(&v)) {
            auto bIt = braceInitClass_.find(static_cast<const void*>(e.node.get()));
            if (bIt != braceInitClass_.end()) enqueueMethod(bIt->second, bIt->second);
            for (const auto& arg : bi->args) if (arg) scanExpr(*arg, cls, locals);
            return;
        }
        if (const auto* b = std::get_if<BinaryExpr>(&v)) { if (b->left) scanExpr(*b->left, cls, locals); if (b->right) scanExpr(*b->right, cls, locals); return; }
        if (const auto* u = std::get_if<UnaryExpr>(&v))  { if (u->operand) scanExpr(*u->operand, cls, locals); return; }
        if (const auto* p = std::get_if<PostfixExpr>(&v)){ if (p->operand) scanExpr(*p->operand, cls, locals); return; }
        if (const auto* ce = std::get_if<CastExpr>(&v))  { if (ce->operand) scanExpr(*ce->operand, cls, locals); return; }
        if (const auto* ix = std::get_if<IndexExpr>(&v)) { if (ix->object) scanExpr(*ix->object, cls, locals); if (ix->index) scanExpr(*ix->index, cls, locals); return; }
        if (const auto* ia = std::get_if<IndexAssignExpr>(&v)) { if (ia->object) scanExpr(*ia->object, cls, locals); if (ia->index) scanExpr(*ia->index, cls, locals); if (ia->value) scanExpr(*ia->value, cls, locals); return; }
        if (const auto* el = std::get_if<ElvisExpr>(&v)) { if (el->left) scanExpr(*el->left, cls, locals); if (el->right) scanExpr(*el->right, cls, locals); return; }
        if (const auto* uw = std::get_if<UnwrapExpr>(&v)){ if (uw->operand) scanExpr(*uw->operand, cls, locals); return; }
        if (const auto* rs = std::get_if<RefStoreExpr>(&v)) { if (rs->target) scanExpr(*rs->target, cls, locals); if (rs->value) scanExpr(*rs->value, cls, locals); return; }
        if (const auto* de = std::get_if<DestroyExpr>(&v)) { if (de->place) scanExpr(*de->place, cls, locals); return; }
        if (const auto* sw = std::get_if<SwitchExpr>(&v)) {
            if (sw->scrutinee) scanExpr(*sw->scrutinee, cls, locals);
            for (const auto& arm : sw->arms) {
                for (const auto& lab : arm.labels) if (lab) scanExpr(*lab, cls, locals);
                if (arm.valueExpr) scanExpr(*arm.valueExpr, cls, locals);
                if (arm.block) scanStmt(*arm.block, cls, locals);
            }
            return;
        }
        if (const auto* mx = std::get_if<MatchExpr>(&v)) {
            if (mx->scrutinee) scanExpr(*mx->scrutinee, cls, locals);
            for (const auto& arm : mx->arms) {
                if (arm.valueExpr) scanExpr(*arm.valueExpr, cls, locals);
                if (arm.block) scanStmt(*arm.block, cls, locals);
            }
            return;
        }
        // LiteralExpr, ThisExpr, NullLiteralExpr, SizeofExpr, AddressOfExpr(local), reflection: no
        // static reference or call reachable here.
    };

    scanStmt = [&](const Stmt& st, const std::string& cls, std::unordered_set<std::string>& locals) {
        if (!st.node) return;
        const auto& v = *st.node;
        if (const auto* es = std::get_if<ExprStmt>(&v)) { scanExpr(es->expression, cls, locals); return; }
        if (const auto* r = std::get_if<ReturnStmt>(&v)) { if (r->value) scanExpr(*r->value, cls, locals); return; }
        if (const auto* y = std::get_if<YieldStmt>(&v)) { scanExpr(y->value, cls, locals); return; }
        if (const auto* bl = std::get_if<BlockStmt>(&v)) { for (const auto& sub : bl->body) if (sub) scanStmt(*sub, cls, locals); return; }
        if (const auto* i = std::get_if<IfStmt>(&v)) {
            scanExpr(i->condition, cls, locals);
            if (i->thenBranch) scanStmt(*i->thenBranch, cls, locals);
            if (i->elseBranch) scanStmt(*i->elseBranch, cls, locals);
            return;
        }
        if (const auto* w = std::get_if<WhileStmt>(&v)) { scanExpr(w->condition, cls, locals); if (w->body) scanStmt(*w->body, cls, locals); return; }
        if (const auto* f = std::get_if<ForStmt>(&v)) {
            if (f->init) scanStmt(*f->init, cls, locals);
            if (f->condition) scanExpr(*f->condition, cls, locals);
            if (f->increment) scanExpr(*f->increment, cls, locals);
            if (f->body) scanStmt(*f->body, cls, locals);
            return;
        }
        if (const auto* sw = std::get_if<SwitchStmt>(&v)) {
            scanExpr(sw->scrutinee, cls, locals);
            for (const auto& arm : sw->arms) {
                for (const auto& lab : arm.labels) if (lab) scanExpr(*lab, cls, locals);
                if (arm.valueExpr) scanExpr(*arm.valueExpr, cls, locals);
                if (arm.block) scanStmt(*arm.block, cls, locals);
            }
            return;
        }
        if (const auto* mx = std::get_if<MatchStmt>(&v)) {
            scanExpr(mx->scrutinee, cls, locals);
            for (const auto& arm : mx->arms) {
                if (arm.valueExpr) scanExpr(*arm.valueExpr, cls, locals);
                if (arm.block) scanStmt(*arm.block, cls, locals);
            }
            return;
        }
        // Break / Continue: nothing to scan.
    };

    // ---- Seed the worklist with each thread closure's `call` method, then drain. ----
    for (const std::string& cls : threadClosureClasses_) enqueueMethod(cls, "call");
    while (!work.empty()) {
        WorkItem wi = std::move(work.back());
        work.pop_back();
        std::unordered_set<std::string> locals;
        if (wi.params) for (const ParamDecl& p : *wi.params) locals.insert(p.name.lexeme);
        for (const auto& st : wi.body->body) if (st) scanStmt(*st, wi.cls, locals);
    }
}
