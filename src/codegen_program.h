// Goose compiler -- the program analyses of CodeGen (codegen.h): the live specializations and
// their call interfaces, the globals, the queues and thread programs, the counts the program starts
// the runtime with, and the export header.
#pragma once

namespace goose {

// ------------------------------------------------------------------
// Specializations: the live ones, their C names and call interfaces (C.3).

inline bool CodeGen::IsPoolParam(FnSpec *sp, size_t i) {
    return sp->roots[i].reusable && CarriesPool(sp->argtypes[i]);
}

inline void CodeGen::CollectSpecs() {
    for (auto sp : ast.fnspecs)
        if (sp->live && sp->body) livespecs.push_back(sp);
    // Names: suffix whenever the goose name maps to more than one live
    // spec (overloads or multiple specializations).
    map<string_view, int> namecount;
    for (auto sp : livespecs) namecount[sp->sf->qname]++;
    for (auto sp : livespecs) {
        auto &si = sinfo[sp];
        auto base = Sanitize(sp->sf->ns, sp->sf->name);
        if (namecount[sp->sf->qname] > 1) Append(base, "_", sp->id);
        si.cname = Unique(base);
        si.hasrf = !sp->needs.empty();
        for (size_t i = 0; i < sp->rets.size(); i++)
            if (si.cret < 0 && IsFix(sp->rets[i]) && !IsLargeFixed(sp->rets[i]) &&
                !NeedsResultLocation(sp->rets[i]))
                si.cret = (int)i;
        for (auto t : sp->needs)
            if (!fromids.count(t)) fromids[t] = (int)fromids.size() + 1;
    }
    // Scan each finished body once. The call edges are local to this
    // analysis; only the summaries needed by emission survive it. Preserve
    // first-appearance order for captures, which fixes the C parameter order.
    // A body needs gs_sp wherever codegen puts anything on an indexed stack
    // (AllocStk): without it those indices count from 0, the outermost
    // callers' stacks, whose arrays may be growing meanwhile or have their
    // tops cached in locals. A node's type is the slot type the checker
    // fitted it to (FitsAt), so a value built as another type is found
    // where it is made.
    // A call whose free-variable arguments the optimizer replaced
    // (Call::fvremap) is an edge of its own: the callee's free variables
    // reach the caller as what that call passes for them.
    unordered_map<FnSpec *, vector<pair<FnSpec *, const Call *>>> callees;
    for (auto sp : livespecs) {
        auto &si = sinfo[sp];
        auto &calls = callees[sp];
        set<const VarDef *> seen;
        set<pair<FnSpec *, const Call *>> seencalls;
        for (auto pt : sp->argtypes) si.needssp |= NeedsStack(pt);
        for (auto rt : sp->rets) si.needssp |= NeedsStack(rt);
        function<void(Node *)> walk = [&](Node *n) {
            if (!n) return;
            if (auto id = Is<Ident>(n)) {
                auto v = id->vdef;
                // A global initializer's locals have no ownerspec: every spec
                // naming one captures it, up to gs_init_globals, which declares it.
                if (v && v->ownerspec != sp && !v->isglobal && seen.insert(v).second)
                    si.freevars.push_back(v);
                if (v && v->isglobal && v->type &&
                    (IsBytesT(v->type) || HoldsFatRef(v->type)))
                    si.globals.insert(v);
            }
            if (n->exprtype && n->exprtype->kind != TY_VOID && n->exprtype->kind != TY_FN &&
                n->exprtype->kind != TY_GENERIC && NeedsStack(n->exprtype))
                si.needssp = true;
            // A slice literal still constructs a fixed array to point at.
            if (auto al = Is<ArrayLit>(n); al && al->exprtype->kind == TY_SLICE &&
                IsFix(al->exprtype->sub)) {
                auto count = LitCount(al);
                si.needssp |= count > NATIVE_VALUE_LIMIT /
                    std::max<int64_t>(FixedSize(al->exprtype->sub), 1);
            }
            // A payload bound by value is copied onto a stack of its own.
            if (auto m = Is<MatchExpr>(n))
                for (auto &arm : m->arms) si.needssp |= arm.binder && NeedsStack(arm.binder->type);
            // A value adapted to a fixed-mode ADT from a variable-mode one, a
            // reference result's pointee included, is built as that first
            // (GenAdtAdapted).
            if (auto from = AdtFrom(n)) si.needssp |= NeedsStack(from);
            if (auto c = Is<Call>(n)) {
                if (c->spec && c->spec->sf->isextern && c->spec->sf->readsalltops)
                    si.readsalltops = true;
                auto add = [&](FnSpec *k) {
                    pair<FnSpec *, const Call *> e { k, c->fvremap.empty() ? nullptr : c };
                    if (k && k != sp && sinfo.count(k) && seencalls.insert(e).second)
                        calls.push_back(e);
                };
                if (c->builtin < 0) add(c->spec);
                for (auto d : c->dispatch) add(d);
                // Formatting a user type calls its format overload, and print
                // and thread_spawn assemble their text or packet on a stack.
                for (auto &fs : c->fmtspecs) add(fs.second);
                if (c->builtin == B_PRINT || c->builtin == B_THREAD_SPAWN ||
                    c->builtin == B_THREAD_SPAWN_CONTAINED)
                    si.needssp = true;
                // A result is built as the call's own type (str() passed as
                // a slice or returned as a u8[..16], say) before it is fitted.
                for (auto rt : c->rettypes) si.needssp |= NeedsStack(rt);
                // A limited receiver takes anything rendered structurally
                // from a builder of its own (EmitFormatInto).
                if (c->builtin == B_FORMAT) {
                    auto an = c->ArgNodes();
                    auto recv = an[0]->exprtype;
                    if (recv->kind == TY_REF) recv = recv->ref->sub;
                    if (recv->kind == TY_ARRAY && recv->arr->akind == A_LIMITED)
                        for (size_t i = 1; i < an.size(); i++)
                            si.needssp |= !SimpleText(FmtContext(c, an[i]), an[i]->exprtype);
                }
                // A variable-mode scrutinee is copied for the arms that take
                // their variant by value, even when it arrives by reference
                // (EmitDispatch).
                if (!c->dispatch.empty()) {
                    auto pos = c->dispatcharg;
                    auto st = CallArgNodes(c, c->dispatch[0]->params.size())[pos]->exprtype;
                    if (st->kind == TY_REF) st = st->ref->sub;
                    if (st->kind == TY_ENUM && st->enu->varmode)
                        for (auto d : c->dispatch) si.needssp |= d->argtypes[pos]->kind != TY_REF;
                }
            }
            RunChildren(n, walk);
        };
        walk(sp->body);
    }
    // These summaries follow the same call edges. Iterate together to
    // handle recursion without repeatedly traversing the AST or retaining a
    // separate global-touch table that must track the specialization table.
    for (auto changed = true; changed;) {
        changed = false;
        for (auto sp : livespecs) {
            auto &si = sinfo[sp];
            set<const VarDef *> seen(si.freevars.begin(), si.freevars.end());
            for (auto [callee, via] : callees[sp]) {
                auto &ki = sinfo[callee];
                // Self edges were omitted, so growing our list cannot
                // invalidate the callee's list as we read it.
                for (auto fv : ki.freevars) {
                    auto v = via ? via->FreeVarArg(fv) : fv;
                    if (v->ownerspec != sp && seen.insert(v).second) {
                        si.freevars.push_back(v);
                        changed = true;
                    }
                }
                for (auto v : ki.globals)
                    if (si.globals.insert(v).second) changed = true;
                if (ki.needssp && !si.needssp) {
                    si.needssp = true;
                    changed = true;
                }
                if (ki.readsalltops && !si.readsalltops) {
                    si.readsalltops = true;
                    changed = true;
                }
            }
        }
    }
}

// A struct passed by value that is not 1, 2, 4 or 8 bytes travels by
// reference to a caller's copy in the Windows x64 convention (and above 16
// bytes in AArch64's). Reading a parameter through that pointer, clang
// reloads a fat reference's header address or a slice's data and length
// after every byte store, which may alias the copy; and where a caller
// writes the copy field by field and the callee reads it as one vector
// (passing it on to a callee of its own), the load stalls until the stores
// retire. A slice, a fat reference and a pool reference are therefore
// passed as their scalar members, each a parameter of its own, which no
// convention puts in memory the body can alias, and reassembled at entry.
// The members' C types are the struct's, but for a slice's elements, which
// take `void *` whatever they are.
inline vector<pair<string, string>> CodeGen::ParamFields(TypeExpr *t, bool pool) {
    if (pool)
        return { { "gs_rhdr *", "hdr" }, { "gs_stack *", "stk" }, { "gs_rhdr *", "fl" },
                 { "gs_stack *", "flstk" } };
    if (IsFatRef(t)) return { { "gs_rhdr *", "hdr" }, { "gs_stack *", "stk" } };
    if (t->kind == TY_SLICE) return { { "void *", "data" }, { "int64_t ", "len" } };
    return {};
}

// ------------------------------------------------------------------
// Globals (§11.1).

// A C initializer for a compile-time constant of fixed, flat type. Fails
// for everything with a runtime component -- references, slices, ADTs,
// relative references -- leaving the value to gs_init_globals. A global
// that is static data itself stands for its initializer, spelled out
// again, since a C initializer cannot read another object.
inline bool CodeGen::StaticInitX(Node *n, TypeExpr *t, string &out) {
    if (!n || !t || !IsFix(t) || HasRelRef(t)) return false;
    if (auto id = Is<Ident>(n)) {
        auto it = id->vdef ? gstatic.find(id->vdef) : gstatic.end();
        // An integer constant may be read at a narrower type than its own,
        // which the checker found its value to fit.
        if (it == gstatic.end() ||
            !(TEq(id->vdef->type, t) || (t->kind == TY_INT && id->vdef->type->kind == TY_INT)))
            return false;
        return StaticInitX(it->second, t, out);
    }
    switch (t->kind) {
        case TY_INT: {
            auto i = Is<IntLit>(n);
            if (!i || t->intstorage == IS_VARINT) return false;
            out = i->val < 0 && t->intstorage == IS_U64 ? cat((uint64_t)i->val, "ULL")
                                                        : IntStr(i->val);
            return true;
        }
        case TY_FLT: {
            auto f = Is<FltLit>(n);
            if (!f) return false;
            out = FltStr(f->val, t->fltstorage == FS_F32);
            return true;
        }
        case TY_BOOL: {
            auto b = Is<BoolLit>(n);
            if (!b) return false;
            out = b->val ? "1" : "0";
            return true;
        }
        case TY_STRUCT: {
            auto sl = Is<StructLit>(n);
            auto si = SI(t);
            if (!sl || sl->sinst != si) return false;
            string s = "{ ";
            auto any = false;
            for (size_t fi = 0; fi < si->st->fields.size(); fi++) {
                if (si->st->fields[fi].ispad) continue;   // Pads zero-fill.
                string fx;
                if (!StaticInitX(sl->InitFor((int)fi), si->ftypes[fi], fx)) return false;
                Append(s, any ? ", " : "", ".", Sanitize(si->st->fields[fi].name),
                       " = ", fx);
                any = true;
            }
            out = any ? s + " }" : "{ 0 }";
            return true;
        }
        case TY_ARRAY: {
            auto &a = *t->arr;
            if (a.akind != A_FIXED) return false;
            auto k = ArrSize(t->arr);
            if (k < 1) return false;
            string s = "{ { ";
            if (auto str = Is<StrLit>(n)) {
                if (k > MAXSTATICELEMS || !IsU8(a.sub) || (int64_t)str->val.size() != k) return false;
                for (int64_t e = 0; e < k; e++)
                    Append(s, e ? ", " : "", (int)(uint8_t)str->val[(size_t)e]);
            } else if (auto al = Is<ArrayLit>(n)) {
                string ex;
                if (al->fillval) {
                    if (k > MAXSTATICELEMS || !StaticInitX(al->fillval, a.sub, ex)) return false;
                    for (int64_t e = 0; e < k; e++) Append(s, e ? ", " : "", ex);
                } else {
                    if ((int64_t)al->elems.size() != k) return false;
                    for (int64_t e = 0; e < k; e++) {
                        if (!StaticInitX(al->elems[(size_t)e], a.sub, ex)) return false;
                        Append(s, e ? ", " : "", ex);
                    }
                }
            } else {
                return false;
            }
            out = s + " } }";
            return true;
        }
        default: return false;
    }
}

// The two minima are written as a subtraction: C has no negative decimal
// constants, and MSVC's C front end types both magnitudes as unsigned
// (C90 literal rules), which the negation would carry through.
inline string CodeGen::IntStr(int64_t v) {
    if (v == INT64_MIN) return "(-9223372036854775807LL - 1)";
    if (v == INT32_MIN) return "(-2147483647 - 1)";
    if (v > INT32_MAX || v < INT32_MIN) return cat(v, "LL");
    return cat(v);
}

inline string CodeGen::FltStr(double v, bool f32) {
    // A folded NaN or infinity has no literal spelling; math.h's macros.
    if (v != v) return "NAN";
    if (std::isinf(v)) return v > 0 ? "INFINITY" : "(-INFINITY)";
    // An f32 constant from halfway between FLT_MAX and 2^128 up rounds to an
    // infinity (§6.3), which C compilers reject as a literal or warn about.
    if (f32 && std::fabs(v) >= std::ldexp(2.0 - std::ldexp(1.0, -24), 127))
        return v > 0 ? "INFINITY" : "(-INFINITY)";
    string s;
    CatOne(s, v);
    if (f32) s += "f";
    return s;
}

// Each global's name, by which the backend names its storage and accessors (GS_GL->name, or the
// static data of a constant one), its dedicated data stack and a reusable pool's freelist, and the
// constant `let` globals the backend writes as static data (gstatic).
inline void CodeGen::NameGlobals() {
    for (auto g : ast.globals) {
        for (size_t di = 0; di < g->defs.size(); di++) {
            auto d = g->defs[di];
            auto name = Unique(Sanitize(g->ns, d->name));
            gnames[d] = cat("GS_GL->", name);
            if (IsResz(d->type)) {
                auto stk = Unique(cat("gs_gstk_", name));
                gstks[d] = cat("(&GS_GL->", stk, ")");
                if (d->reusable) {
                    auto fln = Unique(cat(name, "_fl"));
                    auto flstk = Unique(cat("gs_gstk_", fln));
                    gpools[d] = { cat("GS_GL->", fln), cat("(&GS_GL->", flstk, ")") };
                }
            } else if (IsBytesT(d->type)) {
                auto stk = Unique(cat("gs_gstk_", name));
                gstks[d] = cat("(&GS_GL->", stk, ")");
            } else {
                // A `let` global of a const type with a compile-time initializer is never written
                // (§9.5): static data every instance shares.
                string init;
                if (!d->isvar && d->type->cq && !PrefVar(d) &&
                    StaticInitX(g->inits[di], d->type, init)) {
                    gstatic[d] = g->inits[di];
                    gnames[d] = cat("(*(", VarCT(d), " *)&", name, ")");
                }
            }
        }
    }
}

// What the globals' initializers leave to do at the start: nothing (noglobalinit) when every
// initialized global is static data; and the dedicated stacks the initialized ones open
// (globalregions), a resizable or bytes value's and a reusable pool's freelist.
inline void CodeGen::GlobalStacks() {
    noglobalinit = true;
    for (auto g : ast.globals) {
        if (g->inits.empty()) continue;
        for (auto d : g->defs) {
            if (gstatic.count(d)) continue;
            noglobalinit = false;
            if (IsResz(d->type) || IsBytesT(d->type)) globalregions += 1 + (d->reusable ? 1 : 0);
        }
    }
}

// ------------------------------------------------------------------
// Values and nodes.

// Reference variables with reusable-pool provenance are represented as
// gs_pref so pool operations stay available through them (§5.4). This is
// a property of the VarDef (provenance), shared by caller and callee.
inline bool CodeGen::PrefVar(const VarDef *vd) {
    return vd->type && CarriesPool(vd->type) && vd->ref.reusable;
}

// The C type a variable's storage uses (gs_pref for pool references).
inline string CodeGen::VarCT(const VarDef *vd) {
    if (PrefVar(vd)) { EmitCoreTypes(); return "gs_pref"; }
    return CT(vd->type);
}

// A path whose address takes no check and calls nothing: a variable, or a
// field of one, through references too.
static bool CheckFreePath(Node *n) {
    if (auto id = Is<Ident>(n)) return id->vdef != nullptr;
    if (auto d = Is<Dot>(n)) return d->IsField() && CheckFreePath(d->obj);
    return false;
}

inline bool CodeGen::IsCtl(Node *n) {
    return Is<IfExpr>(n) || Is<MatchExpr>(n) || Is<Block>(n) || Is<EarlyBlock>(n) ||
           Is<LoopExpr>(n) || Is<InlineBlock>(n);
}

// Operand exprtype as an operator sees it: a spliced reference reads as
// its pointee.
inline TypeExpr *CodeGen::OperandT(TypeExpr *t) {
    return t && IsPlainRef(t) ? t->ref->sub : t;
}

inline bool CodeGen::HasStmts(Node *n) {
    if (!n) return false;
    if (Is<Call>(n) || IsCtl(n) || Is<StrLit>(n)) return true;
    auto found = false;
    n->Children([&](Node *c) { found = found || HasStmts(c); });
    return found;
}

// Does a fixed type contain relative references at any depth? Literals of
// such types must construct in their final location, not via a temp.
inline bool CodeGen::HasRelRef(TypeExpr *t) {
    switch (t->kind) {
        case TY_REF: return t->ref->lenstorage >= 0;
        case TY_STRUCT: case TY_ENUM: case TY_VARIANT:
            if (t->kind == TY_ENUM && t->enu->varmode) return false;
            return AnyField(t, [&](TypeExpr *ft) { return HasRelRef(ft); });
        case TY_ARRAY:
            return (t->arr->akind == A_FIXED || t->arr->akind == A_LIMITED) &&
                   HasRelRef(t->arr->sub);
        default: return false;
    }
}

// The type a call or an inlined call body delivers its value as, where the
// checker adapted that value to an ADT it does not arrive as (FitsAt: a
// variant to its ADT, or the ADT to its other mode, §3.5); null elsewhere.
// A reference result counts as its pointee, which the receiver loads.
inline TypeExpr *CodeGen::AdtFrom(Node *n) {
    auto et = n->exprtype;
    if (!et || et->kind != TY_ENUM) return nullptr;
    TypeExpr *from = nullptr;
    if (auto c = Is<Call>(n)) {
        if (c->fvbody) from = c->fvbody->exprtype;
        else if (c->builtin != B_COPY && !c->rettypes.empty()) from = c->rettypes[0];
    } else if (auto ib = Is<InlineBlock>(n)) {
        if (ib->spec && !ib->spec->rets.empty()) from = ib->spec->rets[0];
    }
    if (from && IsPlainRef(from)) from = from->ref->sub;
    if (!from || (from->kind != TY_VARIANT && from->kind != TY_ENUM) || TEq(from, et))
        return nullptr;
    return from;
}

// The arguments the callee's parameters take (Call::ArgNodes): the
// function-value arguments that follow them are compile-time only.
inline vector<Node *> CodeGen::CallArgNodes(Call *c, size_t nparams) {
    auto an = c->ArgNodes();
    an.resize(nparams);
    return an;
}

// Structural named-result discovery on the final body (NamedResultOf). Both
// real functions and inlined bodies use it, so rewrites need not maintain an
// AST annotation.
inline const VarDef *CodeGen::NamedResult(Block *fnbody, SFunction *target,
                                         size_t nrets, size_t resultidx) {
    return NamedResultOf(fnbody, target, nrets, resultidx);
}

// ------------------------------------------------------------------
// Queues, threads and text (§11.2, §3.7).

// The queue of element type t (§11.2), by name; a program with one may run threads.
inline string CodeGen::QueueFor(TypeExpr *t) {
    usesthreads = true;
    auto m = Mangle(t);
    auto it = queues.find(m);
    if (it != queues.end()) return it->second;
    return queues[m] = Unique(cat("gs_q_", m));
}

// The globals a thread program can name (CheckThreadGlobals admits flat
// ones only), in declaration order: read-only static data is not among
// them, since every instance shares it as it is.
inline vector<VarDef *> &CodeGen::ThreadGlobals(FnSpec *entry) {
    auto it = threadglobals.find(entry);
    if (it != threadglobals.end()) return it->second;
    set<FnSpec *> seen;
    set<VarDef *> named;
    function<void(FnSpec *)> rec = [&](FnSpec *sp) {
        if (!sp || !sp->body || !seen.insert(sp).second) return;
        function<void(Node *)> walk = [&](Node *n) {
            if (!n) return;
            if (auto id = Is<Ident>(n))
                if (auto v = id->vdef; v && v->isglobal && !gstatic.count(v)) named.insert(v);
            if (auto c = Is<Call>(n)) {
                rec(c->spec);
                for (auto d : c->dispatch) rec(d);
                for (auto &fs : c->fmtspecs) rec(fs.second);
            }
            RunChildren(n, walk);
        };
        walk(sp->body);
    };
    rec(entry);
    auto &out = threadglobals[entry];
    for (auto g : ast.globals)
        for (auto d : g->defs) if (named.count(d)) out.push_back(d);
    return out;
}

// The worker's entry thunk, by name (TinyGenBackend::EmitThreads builds it).
inline string CodeGen::EnsureThreadThunk(FnSpec *sp) {
    auto it = thunks.find(sp);
    if (it != thunks.end()) return it->second;
    return thunks[sp] = Unique(cat("gs_tmain_", Sanitize(sp->sf->ns, sp->sf->name)));
}

// The dedicated stacks a worker's entry opens: one per resizable argument, and those of the
// globals it copies (ThreadGlobals), as gs_tg's thunk opens them (TinyGenBackend::EmitThreads).
inline int CodeGen::ThunkRegions(FnSpec *sp) {
    int regions = 0;
    for (auto pt : sp->argtypes) regions += IsResz(pt);
    for (auto d : ThreadGlobals(sp)) {
        if (IsResz(d->type)) regions += 1 + (!IsFrameObj(d->type) && d->reusable ? 1 : 0);
        else if (IsBytesT(d->type)) regions++;
    }
    return regions;
}

inline TypeExpr *CodeGen::GrowU8() {
    if (growu8) return growu8;
    growu8 = ast.ArrayOf(ast.inttypes[IS_U8], A_GROW, Line {});
    return growu8;
}

inline Call *CodeGen::FmtContext(Call *c, Node *arg) {
    if (!c || c->fmtcontexts.empty()) return c;
    auto args = c->ArgNodes();
    size_t start = c->builtin == B_FORMAT ? 1 : 0;
    for (size_t i = start; i < args.size(); i++)
        if (args[i] == arg) return c->fmtcontexts.at(i - start);
    return c;
}

inline FnSpec *CodeGen::FmtSpecFor(Call *c, TypeExpr *t) {
    if (!c) return nullptr;
    for (auto &fs : c->fmtspecs) if (TEq(fs.first, t)) return fs.second;
    return nullptr;
}

// The scalar, bool, u8-array cases EmitFormatInto/EmitStr handle directly.
inline bool CodeGen::SimpleText(Call *c, TypeExpr *t) {
    if (FmtSpecFor(c, t)) return false;
    if (t->kind == TY_INT || t->kind == TY_FLT || t->kind == TY_BOOL) return true;
    if (t->kind == TY_ARRAY) return IsU8(t->arr->sub);
    if (t->kind == TY_SLICE) return IsU8(t->sub);
    return false;
}

// ------------------------------------------------------------------
// What the program starts with, and the export header.

// The counts every function is compiled by now to know: the main program's stacks, and the most
// regions it and any one worker hold, which main hands gs_rt_init (rtinit).
inline void CodeGen::StartStacks() {
    BoundStacks();
    auto mainstacks = ProgramStacks(MainRoots());
    auto mainsf = ast.MainFunction();
    CheckStackLimit("the main program", mainstacks, mainsf ? mainsf->line : Line {});
    int64_t workerregions = 0;
    for (auto &[name, sp] : WorkerEntries()) {
        auto stacks = ProgramStacks({ sp });
        CheckStackLimit(cat("thread program ", name), stacks, sp->sf->line);
        thunkregions[sp] = ThunkRegions(sp);
        workerregions = std::max(workerregions, stacks + thunkregions[sp]);
    }
    rtinit[0] = mainstacks;
    rtinit[1] = mainstacks + globalregions;
    rtinit[2] = workerregions;
}

// The export header's prototypes (ExportHeader): an exported fn's C signature, a returned slice as
// its base with the length stored through an extra out-pointer.
inline void CodeGen::ExportProtos() {
    for (auto sf : ast.functions) {
        if (!sf->isexport) continue;
        if (sf->specs.size() != 1 || !sinfo.count(sf->specs[0]))
            Fail(sf->line, cat("export fn ", sf->name, " has no emitted specialization"));
        auto sp = sf->specs[0];
        auto returnslice = !sp->rets.empty() && sp->rets[0]->kind == TY_SLICE;
        string params;
        for (size_t i = 0; i < sp->argtypes.size(); i++)
            Append(params, params.empty() ? "" : ", ", CT(sp->argtypes[i]), " gs_export_arg", i);
        if (returnslice) Append(params, params.empty() ? "" : ", ", "gs_i64 *gs_export_len");
        if (params.empty()) params = "void";
        auto ret = sp->rets.empty() ? string("void")
                   : returnslice ? string("const uint8_t *") : CT(sp->rets[0]);
        Append(exportprotos, ret, " ", sf->cname, "(", params, ");\n");
    }
}

inline string CodeGen::ExportHeader() {
    // The scalar aliases the types below are written with, as runtime.h has them.
    string header = "#pragma once\n#include <stdint.h>\n"
                    "#ifndef GS_ALIGN1\n"
                    "#if (defined(__GNUC__) || defined(__clang__)) && !defined(__TINYC__)\n"
                    "#define GS_ALIGN1 __attribute__((aligned(1)))\n#else\n#define GS_ALIGN1\n#endif\n"
                    "#endif\n"
                    "typedef int16_t gs_i16 GS_ALIGN1;\ntypedef int32_t gs_i32 GS_ALIGN1;\n"
                    "typedef int64_t gs_i64 GS_ALIGN1;\ntypedef uint16_t gs_u16 GS_ALIGN1;\n"
                    "typedef uint32_t gs_u32 GS_ALIGN1;\ntypedef uint64_t gs_u64 GS_ALIGN1;\n"
                    "typedef float gs_f32 GS_ALIGN1;\ntypedef double gs_f64 GS_ALIGN1;\n"
                    "typedef struct gs_stack gs_stack;\n"
                    "#pragma pack(push, 1)\n";
    header += tdecls;
    header += "#pragma pack(pop)\n\n";
    header += "#ifdef __cplusplus\nextern \"C\" {\n#endif\n"
              "void goose_init(int argc, char **argv);\n";
    header += exportprotos;
    header += "#ifdef __cplusplus\n}\n#endif\n";
    return header;
}

}  // namespace goose
