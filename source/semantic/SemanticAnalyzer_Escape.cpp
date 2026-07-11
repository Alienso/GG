//
// Escape analysis (v1, intraprocedural).
//
// Goal: reject passing a *stack value object* where a `Class&` parameter is expected when the
// callee would let that reference outlive the call — i.e. the parameter (or the implicit `this`)
// is **returned** or **stored** (into a field / an indexed slot). A stack object has no owner to
// keep it alive past the call, so that would dangle. Pure-borrow callees (readers, operators,
// mutators that only touch the argument's own fields) are unaffected.
//
// This pass fills `paramEscapes` / `thisEscapes` on each collected function/method. The call-site
// check (in resolveOverload / analyzeMethodCall) consults them, gated on the argument being an
// Object coerced to a Reference — so reference→reference passing is never touched.
//
// v1 is intraprocedural and optimistic about calls: a parameter forwarded into another call is
// NOT (yet) followed into that callee. It catches the direct footguns (return/store of a borrowed
// parameter); interprocedural propagation is future work (see docs/escape-analysis.md).
//

#include "SemanticAnalyzer.h"
#include "../parser/Ast.h"

#include <unordered_set>
#include <vector>
#include <string>
#include <utility>

namespace {

// Accumulated facts over one function body.
struct EscapeScan {
    std::vector<std::pair<std::string, std::string>> aliasEdges;  // (from, to): `to` aliases `from`
    std::unordered_set<std::string>                  escaping;    // names used in an escaping position
    const std::unordered_set<std::string>*           fields = nullptr;  // enclosing class's reference-field names
    std::unordered_set<std::string>                  declaredLocals;    // local names that shadow fields
};

// The reference source named by an expression, if it is a bare name or `this`; else "".
std::string refName(const Expr& e) {
    if (!e.node) return "";
    if (const auto* id = std::get_if<IdentifierExpr>(e.node.get())) return id->name.lexeme;
    if (std::holds_alternative<ThisExpr>(*e.node)) return "this";
    return "";
}

void scanExpr(const Expr& e, EscapeScan& s);
void scanStmt(const Stmt& st, EscapeScan& s);

void markEscape(const Expr& value, EscapeScan& s) {
    std::string n = refName(value);
    if (!n.empty()) s.escaping.insert(n);
}

void scanExpr(const Expr& e, EscapeScan& s) {
    if (!e.node) return;
    const auto& v = *e.node;

    if (const auto* a = std::get_if<AssignExpr>(&v)) {
        // `field = <ident>` (implicit-`this` reference-field store) escapes; `local = <ident>` is a
        // rebind, which just makes `local` alias that name (matters only if `local` later escapes).
        // A local of the same name shadows the field, so a declared local is always a rebind.
        if (s.fields && s.fields->count(a->name.lexeme) && !s.declaredLocals.count(a->name.lexeme))
            markEscape(*a->value, s);
        else {
            std::string src = refName(*a->value);
            if (!src.empty()) s.aliasEdges.emplace_back(src, a->name.lexeme);
        }
        scanExpr(*a->value, s);
        return;
    }
    if (const auto* ma = std::get_if<MemberAssignExpr>(&v)) {
        // Storing into a REFERENCE field retains/aliases the value ⇒ escapes. Storing into a
        // value-object field is a deep copy (no escape). `s.fields` holds only reference fields.
        if (s.fields && s.fields->count(ma->field.lexeme))
            markEscape(*ma->value, s);
        if (ma->object) scanExpr(*ma->object, s);
        scanExpr(*ma->value, s);
        return;
    }
    if (const auto* ia = std::get_if<IndexAssignExpr>(&v)) {
        // Whether `a[i] = q` retains q depends on the element type (reference vs value), which
        // isn't known here — v1 does not treat it as an escape (optimistic; see the design note).
        if (ia->object) scanExpr(*ia->object, s);
        if (ia->index)  scanExpr(*ia->index, s);
        scanExpr(*ia->value, s);
        return;
    }
    if (const auto* vd = std::get_if<VarDeclExpr>(&v)) {
        s.declaredLocals.insert(vd->name.lexeme);   // shadows any same-named field
        if (vd->initializer) {
            std::string src = refName(*vd->initializer);
            if (!src.empty()) s.aliasEdges.emplace_back(src, vd->name.lexeme);
            scanExpr(*vd->initializer, s);
        }
        return;
    }
    if (const auto* b = std::get_if<BinaryExpr>(&v)) { if (b->left) scanExpr(*b->left, s); if (b->right) scanExpr(*b->right, s); return; }
    if (const auto* u = std::get_if<UnaryExpr>(&v))  { if (u->operand) scanExpr(*u->operand, s); return; }
    if (const auto* p = std::get_if<PostfixExpr>(&v)){ if (p->operand) scanExpr(*p->operand, s); return; }
    if (const auto* c = std::get_if<CompoundAssignExpr>(&v)) { if (c->value) scanExpr(*c->value, s); return; }
    if (const auto* ce = std::get_if<CastExpr>(&v))  { if (ce->operand) scanExpr(*ce->operand, s); return; }
    if (const auto* ix = std::get_if<IndexExpr>(&v)) { if (ix->object) scanExpr(*ix->object, s); if (ix->index) scanExpr(*ix->index, s); return; }
    if (const auto* m = std::get_if<MemberAccessExpr>(&v)) { if (m->object) scanExpr(*m->object, s); return; }
    if (const auto* call = std::get_if<CallExpr>(&v)) { for (const auto& a : call->args) if (a) scanExpr(*a, s); return; }
    if (const auto* mc = std::get_if<MethodCallExpr>(&v)) { if (mc->object) scanExpr(*mc->object, s); for (const auto& a : mc->args) if (a) scanExpr(*a, s); return; }
    if (const auto* n = std::get_if<NewExpr>(&v)) { for (const auto& a : n->args) if (a) scanExpr(*a, s); return; }
    if (const auto* sw = std::get_if<SwitchExpr>(&v)) {
        if (sw->scrutinee) scanExpr(*sw->scrutinee, s);
        for (const auto& arm : sw->arms) {
            for (const auto& lab : arm.labels) if (lab) scanExpr(*lab, s);
            if (arm.valueExpr) { markEscape(*arm.valueExpr, s); scanExpr(*arm.valueExpr, s); }  // arm value flows out
            if (arm.block) scanStmt(*arm.block, s);
        }
        return;
    }
    // LiteralExpr, IdentifierExpr, ThisExpr, SizeofExpr: no children, no escape by themselves.
}

void scanStmt(const Stmt& st, EscapeScan& s) {
    if (!st.node) return;
    const auto& v = *st.node;

    if (const auto* es = std::get_if<ExprStmt>(&v)) { scanExpr(es->expression, s); return; }
    if (const auto* r = std::get_if<ReturnStmt>(&v)) {
        if (r->value) { markEscape(*r->value, s); scanExpr(*r->value, s); }   // returned ⇒ escapes
        return;
    }
    if (const auto* y = std::get_if<YieldStmt>(&v)) { markEscape(y->value, s); scanExpr(y->value, s); return; }
    if (const auto* bl = std::get_if<BlockStmt>(&v)) { for (const auto& sub : bl->body) if (sub) scanStmt(*sub, s); return; }
    if (const auto* i = std::get_if<IfStmt>(&v)) {
        scanExpr(i->condition, s);
        if (i->thenBranch) scanStmt(*i->thenBranch, s);
        if (i->elseBranch) scanStmt(*i->elseBranch, s);
        return;
    }
    if (const auto* w = std::get_if<WhileStmt>(&v)) { scanExpr(w->condition, s); if (w->body) scanStmt(*w->body, s); return; }
    if (const auto* f = std::get_if<ForStmt>(&v)) {
        if (f->init) scanStmt(*f->init, s);
        if (f->condition)  scanExpr(*f->condition, s);
        if (f->increment)  scanExpr(*f->increment, s);
        if (f->body) scanStmt(*f->body, s);
        return;
    }
    if (const auto* sw = std::get_if<SwitchStmt>(&v)) {
        scanExpr(sw->scrutinee, s);
        for (const auto& arm : sw->arms) {
            for (const auto& lab : arm.labels) if (lab) scanExpr(*lab, s);
            if (arm.valueExpr) scanExpr(*arm.valueExpr, s);
            if (arm.block) scanStmt(*arm.block, s);
        }
        return;
    }
    // Break/Continue and nested declarations: nothing to scan.
}

// Does `start` reach an escaping name through alias edges?
bool reaches(const std::string& start, const EscapeScan& s) {
    std::unordered_set<std::string> seen{start};
    std::vector<std::string> work{start};
    while (!work.empty()) {
        std::string n = std::move(work.back());
        work.pop_back();
        if (s.escaping.count(n)) return true;
        for (const auto& [from, to] : s.aliasEdges)
            if (from == n && seen.insert(to).second) work.push_back(to);
    }
    return false;
}

}  // namespace

void SemanticAnalyzer::computeParamEscapes(const std::vector<ParamDecl>& params,
                                           const BlockStmt& body, bool computeThis,
                                           const std::unordered_set<std::string>& fieldNames,
                                           std::vector<bool>& paramEscapesOut, bool& thisEscapesOut) {
    EscapeScan s;
    s.fields = &fieldNames;
    for (const auto& sub : body.body) if (sub) scanStmt(*sub, s);

    paramEscapesOut.assign(params.size(), false);
    for (size_t i = 0; i < params.size(); ++i)
        paramEscapesOut[i] = reaches(params[i].name.lexeme, s);

    thisEscapesOut = computeThis && reaches("this", s);
}
