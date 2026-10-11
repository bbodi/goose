// Goose compiler — the tinygen backend (docs/design/tinygen_backend.md).
// tinygen (../tinygen, a C99 library) lowers a small IR to machine code and
// writes an object file with debug info. This backend compiles the
// specializations it supports from the same optimized, annotated bodies the
// C backend reads, into one object linked beside the C the C backend still
// writes: that C keeps everything else -- main, the globals, the runtime's
// inline half, and every specialization this backend does not take, which it
// declares with external linkage (CodeGen::SpecBackend). The two halves call
// each other through the C calling convention of C.3, so a specialization
// falls back to C on its own.
//
// The runtime's helpers that runtime.h defines as static functions (checked
// arithmetic, `as` checks, the extern support) are reached through small
// exported shims appended to that C (Shims), compiled with its defines, so
// GS_DEBUG means the same on both sides.
#pragma once

#if GOOSE_HAVE_TINYGEN
#include "tinygen.h"

namespace goose {

// Thrown wherever the backend meets what it does not compile yet; the
// specialization then goes to the C backend.
struct TgUnsupported {
    Line line;
    string why;
};

// One tinygen module and what its procedures share: types, debug types,
// symbols, file names, and the shims and extern trampolines their calls
// need from the C half.
struct TgModule {
    Ast &ast;
    CodeGen &cg;
    bool checks;           // GS_DEBUG: the overflow and `as` checks of §6.2/§6.3.
    tyg_module *m = nullptr;
    map<int, tyg_file> files;
    map<int, tyg_sym> filenames;
    map<string, tyg_sym> strings;
    map<string, tyg_sym> syms;
    map<string, const tyg_dtype *> dtypes;
    map<string, const tyg_type *> atypes;
    map<FnSpec *, tyg_sig *> sigs;
    // The C the procedures call into, by name: shim definitions in order.
    vector<pair<string, string>> shims;
    vector<FnSpec *> externs;   // Extern fns called, for trampolines.

    TgModule(Ast &_ast, CodeGen &_cg, bool _checks, const string &source)
        : ast(_ast), cg(_cg), checks(_checks) {
        auto t = tyg_host_target();
        t.debug_info = 1;
        m = tyg_module_new(t, source.c_str());
        // C11, the C half's: lldb formats the same under every code
        // tinygen offers (docs/design/tinygen_backend.md), and the two
        // halves' units then read alike.
        if (m) tyg_module_language(m, TYG_LANG_C11);
    }
    ~TgModule() { if (m) tyg_module_free(m); }

    tyg_file File(int fileidx) {
        auto it = files.find(fileidx);
        if (it != files.end()) return it->second;
        string path = fileidx >= 0 && fileidx < (int)ast.sources.size()
                          ? ast.sources[fileidx].first : string("?");
        std::error_code ec;
        auto abs = filesystem::absolute(filesystem::path(path), ec);
        if (!ec) path = abs.lexically_normal().string();
        return files[fileidx] = tyg_debug_file(m, path.c_str());
    }

    tyg_sym Sym(const string &name, unsigned flags) {
        auto it = syms.find(name);
        if (it != syms.end()) return it->second;
        return syms[name] = tyg_symbol(m, name.c_str(), flags);
    }

    // Read-only bytes: a string literal's, or a file name's (with its NUL)
    // for the runtime's abort messages.
    tyg_sym Bytes(const string &bytes) {
        auto it = strings.find(bytes);
        if (it != strings.end()) return it->second;
        auto s = tyg_symbol(m, cat("gs_tg_str", strings.size()).c_str(), 0);
        tyg_data(m, s, TYG_SECTION_RODATA, bytes.data(), (int64_t)max<size_t>(bytes.size(), 1), 1);
        return strings[bytes] = s;
    }
    tyg_sym FileName(int fileidx) {
        auto it = filenames.find(fileidx);
        if (it != filenames.end()) return it->second;
        auto f = fileidx >= 0 && fileidx < (int)ast.sources.size()
                     ? ast.sources[fileidx].first : string("?");
        for (auto &c : f) if (c == '\\') c = '/';
        f.push_back('\0');
        return filenames[fileidx] = Bytes(f);
    }

    void NeedShim(const string &name, const string &text) {
        for (auto &s : shims) if (s.first == name) return;
        shims.emplace_back(name, text);
    }

    // ---- Types -------------------------------------------------------------

    static bool IsSigned(TypeExpr *t) { return t->kind == TY_INT && !IsUnsigned(t->intstorage); }

    // A type this backend holds: a machine integer, a float, a bool, a plain
    // reference to a fixed value it holds, or a fixed struct of those.
    bool TypeOk(TypeExpr *t, int depth = 0) {
        if (!t || depth > 32) return false;
        switch (t->kind) {
            case TY_INT: return t->intstorage != IS_VARINT;
            case TY_FLT: case TY_BOOL: return true;
            case TY_REF:
                return IsPlainRef(t) && cg.IsFix(t->ref->sub) && TypeOk(t->ref->sub, depth + 1);
            case TY_ARRAY:
                // T[k]: k elements, packed. A static-capacity T[..k] (a
                // length beside them) is not held yet.
                return t->arr->akind == A_FIXED && cg.IsFix(t) && !cg.IsLargeFixed(t) &&
                       cg.ArrSize(t->arr) > 0 && TypeOk(t->arr->sub, depth + 1);
            case TY_SLICE:
                // { data, len }, over fixed elements (C.3).
                return cg.IsFix(t->sub) && TypeOk(t->sub, depth + 1);
            case TY_STRUCT: {
                if (!cg.IsFix(t) || cg.IsLargeFixed(t)) return false;
                auto si = cg.SI(t);
                if (!si || EmptyLayout(si->st->fields)) return false;
                for (size_t i = 0; i < si->st->fields.size(); i++) {
                    if (si->st->fields[i].ispad) continue;
                    if (!TypeOk(si->ftypes[i], depth + 1)) return false;
                }
                return true;
            }
            default: return false;
        }
    }
    bool IsScalar(TypeExpr *t) {
        return t && (t->kind == TY_INT || t->kind == TY_FLT || t->kind == TY_BOOL ||
                     t->kind == TY_REF);
    }
    tyg_scalar Scal(TypeExpr *t) {
        switch (t->kind) {
            case TY_INT:
                switch (IntBits(t->intstorage)) {
                    case 8: return TYG_I8;
                    case 16: return TYG_I16;
                    case 32: return TYG_I32;
                    default: return TYG_I64;
                }
            case TY_FLT: return t->fltstorage == FS_F32 ? TYG_F32 : TYG_F64;
            case TY_BOOL: return TYG_I8;
            default: return TYG_I64;   // A reference: an address.
        }
    }
    static int ScalSize(tyg_scalar s) {
        switch (s) {
            case TYG_I8: return 1;
            case TYG_I16: return 2;
            case TYG_I32: case TYG_F32: return 4;
            default: return 8;
        }
    }
    int64_t Size(TypeExpr *t) { return IsScalar(t) ? ScalSize(Scal(t)) : cg.FixedSize(t); }

    struct FieldAt {
        TypeExpr *t;
        int64_t off;
        string_view name;
    };
    vector<FieldAt> Fields(TypeExpr *t) {
        auto si = cg.SI(t);
        auto &lay = cg.StructLayout(si);
        vector<FieldAt> fs;
        for (size_t i = 0; i < si->st->fields.size(); i++)
            if (!si->st->fields[i].ispad)
                fs.push_back({ si->ftypes[i], lay.offs[i], si->st->fields[i].name });
        return fs;
    }

    // The type the C calling convention sees: packed structs are records of
    // their fields at their packed offsets, aligned to 1, as #pragma pack(1)
    // makes them in the C half.
    const tyg_type *AT(TypeExpr *t) {
        if (t->kind == TY_REF) return tyg_type_ptr(m);
        if (t->kind == TY_INT) return tyg_type_int(m, Scal(t), IsSigned(t));
        if (t->kind == TY_BOOL) return tyg_type_int(m, TYG_I8, 0);
        if (t->kind == TY_FLT) return tyg_type_scalar(m, Scal(t));
        if (t->kind == TY_SLICE) return SliceAT();
        auto key = cg.Mangle(t);
        if (auto it = atypes.find(key); it != atypes.end()) return it->second;
        if (t->kind == TY_ARRAY) {
            // The C half's struct { T e[k]; }, packed.
            tyg_field f { tyg_type_array(m, AT(t->arr->sub), cg.ArrSize(t->arr)), 0 };
            return atypes[key] = tyg_type_record(m, cg.FixedSize(t), 1, &f, 1);
        }
        vector<tyg_field> fs;
        for (auto &f : Fields(t)) fs.push_back({ AT(f.t), f.off });
        return atypes[key] = tyg_type_record(m, cg.FixedSize(t), 1, fs.data(), (int)fs.size());
    }

    // The u8 slice of the extern boundary, sl_u8: data and length.
    const tyg_type *sliceat = nullptr;
    const tyg_type *SliceAT() {
        if (sliceat) return sliceat;
        tyg_field fs[2] = { { tyg_type_ptr(m), 0 }, { tyg_type_int(m, TYG_I64, 1), 8 } };
        return sliceat = tyg_type_record(m, 16, 1, fs, 2);
    }

    // The type the debugger sees, under its Goose name.
    const tyg_dtype *DT(TypeExpr *t) {
        string name;
        t->Dump(name);
        if (auto it = dtypes.find(name); it != dtypes.end()) return it->second;
        const tyg_dtype *d = nullptr;
        // A scalar is a typedef of the C type of its size, by its Goose name:
        // lldb reads DWARF through clang's type system, which gives a base
        // type a C name of its own choosing (an i64 shows as `long`) but
        // keeps a typedef's.
        auto scalar = [&](const char *cname, tyg_dt_encoding enc) {
            auto base = tyg_dt_base(m, cname, ScalSize(Scal(t)), enc);
            return tyg_dt_typedef(m, name.c_str(), base, File(0), 0);
        };
        switch (t->kind) {
            case TY_INT: {
                static const char *cn[] = { "signed char", "short", "int", "long long",
                                            "unsigned char", "unsigned short", "unsigned int",
                                            "unsigned long long" };
                auto u = IsUnsigned(t->intstorage);
                d = scalar(cn[(u ? 4 : 0) + (ScalSize(Scal(t)) == 1   ? 0
                                             : ScalSize(Scal(t)) == 2 ? 1
                                             : ScalSize(Scal(t)) == 4 ? 2
                                                                      : 3)],
                           u ? TYG_DT_UNSIGNED : TYG_DT_SIGNED);
                break;
            }
            case TY_FLT:
                d = scalar(Scal(t) == TYG_F32 ? "float" : "double", TYG_DT_FLOAT);
                break;
            case TY_BOOL: d = scalar("_Bool", TYG_DT_BOOL); break;
            case TY_REF: d = tyg_dt_pointer(m, DT(t->ref->sub)); break;
            case TY_ARRAY: d = tyg_dt_array(m, DT(t->arr->sub), cg.ArrSize(t->arr)); break;
            case TY_SLICE: {
                // A record of its two members: tinygen has no array type
                // whose count is another member (the design doc's gaps).
                auto rec = tyg_dt_record(m, TYG_DT_STRUCT, name.c_str(), File(0), 0);
                dtypes[name] = rec;
                tyg_dt_member(m, rec, "data", tyg_dt_pointer(m, DT(t->sub)), 0);
                tyg_dt_member(m, rec, "len", DT(ast.inttypes[IS_I64]), 8);
                tyg_dt_record_complete(m, rec, 16, 1);
                return rec;
            }
            case TY_STRUCT: {
                auto si = cg.SI(t);
                auto rec = tyg_dt_record(m, TYG_DT_STRUCT, name.c_str(), File(si->st->line.fileidx),
                                         si->st->line.line);
                dtypes[name] = rec;   // A field may refer back to it through a reference.
                for (auto &f : Fields(t))
                    tyg_dt_member(m, rec, string(f.name).c_str(), DT(f.t), f.off);
                tyg_dt_record_complete(m, rec, cg.FixedSize(t), 1);
                return rec;
            }
            default: d = tyg_dt_void(m); break;
        }
        return dtypes[name] = d;
    }

    // ---- Signatures --------------------------------------------------------

    // Whether a call to sp, or sp's own definition (`define`), has a C
    // interface this backend can match: declared parameters and results it
    // holds (the first result the C value, every other one written through
    // an out-pointer, `T *gs_r<i>`, after the parameters: CodeGen::SigParams),
    // no free variables, no `return from` channel. A definition may take gs_sp (which it never
    // uses, building nothing on a data stack); a callee must not, since
    // the stacks it is handed would need accounting (codegen_stacks.h).
    // The specializations this module's procedures may call although the C
    // gives them gs_sp: the ones this backend compiles too, which never use
    // it (Choose), and are passed 0.
    const set<FnSpec *> *ours = nullptr;

    bool SigOk(FnSpec *sp, bool define, string &why) {
        auto &si = cg.sinfo[sp];
        if (sp->sf->isextern) { why = "extern"; return false; }
        if (!si.freevars.empty()) { why = "free variables"; return false; }
        if (si.hasrf || cg.fromids.count(sp)) { why = "return from"; return false; }
        if (si.needssp && !define && !(ours && ours->count(sp))) {
            why = "calls a function using data stacks";
            return false;
        }
        // A result this backend holds is fixed and not large, so the C
        // returns the first one (CodeGen::CollectSpecs).
        if (!sp->rets.empty() && si.cret != 0) {
            why = "results";
            return false;
        }
        for (auto r : sp->rets)
            if (!TypeOk(r)) { why = "result type"; return false; }
        for (size_t i = 0; i < sp->argtypes.size(); i++)
            if (cg.IsPoolParam(sp, i) || !TypeOk(sp->argtypes[i])) {
                why = "parameter type";
                return false;
            }
        return true;
    }

    tyg_sig *Sig(FnSpec *sp) {
        if (auto it = sigs.find(sp); it != sigs.end()) return it->second;
        vector<const tyg_type *> ps;
        for (auto t : sp->argtypes) ps.push_back(AT(t));
        for (size_t i = 1; i < sp->rets.size(); i++) ps.push_back(tyg_type_ptr(m));
        if (!sp->sf->isextern && cg.sinfo[sp].needssp) ps.push_back(tyg_type_int(m, TYG_I64, 1));
        const tyg_type *ret = sp->rets.empty() ? nullptr : AT(sp->rets[0]);
        return sigs[sp] = tyg_sig_new(m, ret, ps.data(), (int)ps.size(), 0);
    }
    tyg_sig *ExternSig(FnSpec *sp) {
        if (auto it = sigs.find(sp); it != sigs.end()) return it->second;
        vector<const tyg_type *> ps;
        for (auto t : sp->argtypes) ps.push_back(t->kind == TY_SLICE ? SliceAT() : AT(t));
        const tyg_type *ret = sp->rets.empty() ? nullptr : AT(sp->rets[0]);
        return sigs[sp] = tyg_sig_new(m, ret, ps.data(), (int)ps.size(), 0);
    }
};

// One procedure: a specialization's body, lowered statement by statement.
struct TgFn {
    TgModule &M;
    CodeGen &cg;
    FnSpec *sp;
    tyg_module *m;
    tyg_proc *p = nullptr;
    unordered_map<const VarDef *, tyg_local> locals;
    struct LoopTargets {
        tyg_block brk, cnt;
    };
    vector<LoopTargets> loops;
    // The specializations taking gs_sp this body calls, which it may only
    // while they are tinygen's too (TinyGenBackend::Choose).
    set<FnSpec *> relied;
    // The out-pointers of results after the first (rets[i] at outs[i - 1]),
    // each kept in a local.
    vector<tyg_local> outs;

    TgFn(TgModule &_M, FnSpec *_sp) : M(_M), cg(_M.cg), sp(_sp), m(_M.m) {}

    [[noreturn]] static void No(Line l, const string &why) { throw TgUnsupported { l, why }; }

    // A value: a scalar in a virtual register, or an aggregate in memory.
    struct V {
        tyg_val v = 0;
        tyg_addr a {};
        bool mem = false;
        TypeExpr *t = nullptr;
    };
    // A place: memory holding a value of type t.
    struct P {
        tyg_addr a;
        TypeExpr *t;
    };

    // ---- Blocks and positions ------------------------------------------------

    // Code after a jump or return is unreachable, but tinygen wants every
    // instruction in an open block: such code goes into one of its own.
    void Open() {
        if (tyg_block_terminated(p)) tyg_block_set(p, tyg_block_new(p));
    }
    bool Ended() { return tyg_block_terminated(p) != 0; }
    void Jump(tyg_block b) {
        if (!Ended()) tyg_jump(p, b);
    }
    void Loc(Line l) {
        if (l.line > 0) tyg_loc(p, M.File(l.fileidx), l.line, 0);
    }

    // A local for a value of type t, aligned to the largest power of two
    // up to 8 that its size allows (a packed value needs none).
    tyg_local Temp(TypeExpr *t) {
        auto sz = M.Size(t);
        int64_t align = 1;
        while (align < 8 && align * 2 <= sz) align *= 2;
        return tyg_local_new(p, sz, align);
    }

    // ---- Conversions -------------------------------------------------------

    // C's conversion of a scalar between the scalar types: what an
    // assignment of one to the other does.
    tyg_val Conv(tyg_val v, TypeExpr *from, TypeExpr *to, Line l) {
        auto fs = M.Scal(from), ts = M.Scal(to);
        auto fflt = from->kind == TY_FLT, tflt = to->kind == TY_FLT;
        if (fflt && tflt) {
            if (fs == ts) return v;
            return tyg_convert(p, fs == TYG_F32 ? TYG_FEXT : TYG_FTRUNC, ts, v);
        }
        if (!fflt && tflt) {
            auto uns = from->kind == TY_BOOL || (from->kind == TY_INT && IsUnsigned(from->intstorage));
            if (fs != TYG_I64 && fs != TYG_I32) {
                v = tyg_convert(p, uns ? TYG_ZEXT : TYG_SEXT, TYG_I32, v);
                fs = TYG_I32;
            }
            return tyg_convert(p, uns ? TYG_UITOF : TYG_SITOF, ts, v);
        }
        if (fflt) No(l, "implicit float to integer conversion");
        if (fs == ts) return v;
        auto fz = TgModule::ScalSize(fs), tz = TgModule::ScalSize(ts);
        if (tz < fz) return tyg_convert(p, TYG_TRUNC, ts, v);
        return tyg_convert(p, TgModule::IsSigned(from) ? TYG_SEXT : TYG_ZEXT, ts, v);
    }

    // A value as type `want`: a reference read through where a value is
    // wanted (reference transparency, §3.8), a scalar converted.
    V Fit(V x, TypeExpr *want, Line l) {
        if (!want || want->kind == TY_VOID || !x.t) return x;
        while (IsPlainRef(x.t) && want->kind != TY_REF) {
            auto sub = x.t->ref->sub;
            P pl { tyg_at_val(x.v, 0), sub };
            x = Load(pl, l);
        }
        if (M.IsScalar(x.t) && M.IsScalar(want)) {
            if (x.t->kind == TY_REF || want->kind == TY_REF) {
                if (x.t->kind != want->kind) No(l, "reference conversion");
                x.t = want;
                return x;
            }
            x.v = Conv(x.v, x.t, want, l);
            x.t = want;
            return x;
        }
        if (M.IsScalar(x.t) != M.IsScalar(want) || !cg.TEq(x.t, want)) {
            string a, b;
            x.t->Dump(a);
            want->Dump(b);
            No(l, cat("value conversion from ", a, " to ", b));
        }
        return x;
    }

    V Load(const P &pl, Line l) {
        V x;
        x.t = pl.t;
        if (M.IsScalar(pl.t)) {
            x.v = tyg_load(p, M.Scal(pl.t), pl.a, 0);
            return x;
        }
        if (!M.TypeOk(pl.t)) No(l, "type");
        // An aggregate read is a copy: what is read must not change under
        // a later argument or operand.
        auto t = Temp(pl.t);
        tyg_copy(p, tyg_at_local(t, 0), pl.a, M.Size(pl.t));
        x.mem = true;
        x.a = tyg_at_local(t, 0);
        return x;
    }

    void Store(const P &pl, const V &x) {
        if (Ended()) return;
        if (M.IsScalar(pl.t)) tyg_store(p, M.Scal(pl.t), pl.a, x.v, 0);
        else tyg_move(p, pl.a, x.a, M.Size(pl.t));
    }

    // ---- Places --------------------------------------------------------------

    P Deref(P pl) {
        while (IsPlainRef(pl.t)) {
            auto ptr = tyg_load(p, TYG_I64, pl.a, 0);
            pl = P { tyg_at_val(ptr, 0), pl.t->ref->sub };
        }
        return pl;
    }

    P Place(Node *n) {
        if (auto id = Is<Ident>(n)) {
            auto vd = id->vdef;
            if (!vd) No(n->line, "function as a value");
            auto it = locals.find(vd);
            if (it == locals.end())
                No(n->line, vd->isglobal ? "global variable" : "variable from outside");
            return P { tyg_at_local(it->second, 0), vd->type };
        }
        if (auto d = Is<Dot>(n); d && d->IsField()) {
            auto base = Deref(Place(d->obj));
            if (base.t->kind != TY_STRUCT || !M.TypeOk(base.t)) No(n->line, "field of this type");
            auto si = cg.SI(base.t);
            auto &lay = cg.StructLayout(si);
            auto a = base.a;
            a.offset += lay.offs[d->fieldidx];
            return P { a, si->ftypes[d->fieldidx] };
        }
        if (auto ix = Is<Index>(n)) return IndexPlace(ix);
        // Any other value: in memory of its own.
        auto x = Gen(n);
        if (x.mem) return P { x.a, x.t };
        if (!x.t || x.t->kind == TY_VOID) No(n->line, "place of no value");
        auto t = Temp(x.t);
        P pl { tyg_at_local(t, 0), x.t };
        Store(pl, x);
        return pl;
    }

    // Builds n's value at the place.
    void Into(const P &pl, Node *n) {
        auto x = GenAs(n, pl.t);
        if (M.IsScalar(pl.t) || x.mem) Store(pl, x);
        else if (!Ended()) No(n->line, "aggregate value");
    }

    // ---- Expressions -------------------------------------------------------

    V Gen(Node *n) {
        auto x = Gen1(n);
        if (Ended() || !x.t || !n->exprtype || n->exprtype->kind == TY_VOID) return x;
        return Fit(x, n->exprtype, n->line);
    }
    V GenAs(Node *n, TypeExpr *t) {
        if (t && t->kind == TY_SLICE) return GenSliceAs(n, t);
        return Fit(Gen(n), t, n->line);
    }
    tyg_val Truth(Node *n) {
        auto x = GenAs(n, M.ast.booltype);
        return x.v;
    }
    V Dummy(TypeExpr *t) {
        V x;
        x.t = t;
        if (t && M.IsScalar(t)) x.v = M.Scal(t) >= TYG_F32 ? tyg_fconst(p, M.Scal(t), 0)
                                                           : tyg_iconst(p, M.Scal(t), 0);
        return x;
    }

    V Gen1(Node *n) {
        switch (n->nkind) {
            case NK_IntLit: {
                auto il = static_cast<IntLit *>(n);
                auto t = n->exprtype;
                V x;
                x.t = t;
                if (t->kind == TY_FLT) {
                    x.v = tyg_fconst(p, M.Scal(t), il->uns ? (double)(uint64_t)il->val
                                                           : (double)il->val);
                } else if (t->kind == TY_INT) {
                    x.v = tyg_iconst(p, M.Scal(t), il->val);
                } else {
                    No(n->line, "integer literal of this type");
                }
                return x;
            }
            case NK_FltLit: {
                V x;
                x.t = n->exprtype;
                if (x.t->kind != TY_FLT) No(n->line, "float literal of this type");
                x.v = tyg_fconst(p, M.Scal(x.t), static_cast<FltLit *>(n)->val);
                return x;
            }
            case NK_BoolLit: {
                V x;
                x.t = M.ast.booltype;
                x.v = tyg_iconst(p, TYG_I8, static_cast<BoolLit *>(n)->val ? 1 : 0);
                return x;
            }
            case NK_Ident: {
                auto pl = Place(n);
                return Load(pl, n->line);
            }
            case NK_Dot: {
                auto d = static_cast<Dot *>(n);
                if (d->member == B_LEN) return GenLen(d->obj, n);
                if (!d->IsField()) No(n->line, "property or variant constant");
                return Load(Place(n), n->line);
            }
            case NK_Index: return Load(Place(n), n->line);
            case NK_ArrayLit: return GenArrayLit(static_cast<ArrayLit *>(n));
            case NK_SliceExpr: return GenSlice(static_cast<SliceExpr *>(n));
            case NK_StrLit: {
                // A string literal as a u8[:] (its bytes, read-only).
                auto t = n->exprtype;
                if (!t || t->kind != TY_SLICE || !IsU8(t->sub)) No(n->line, "string of this type");
                auto &val = static_cast<StrLit *>(n)->val;
                auto tmp = tyg_local_new(p, 16, 8);
                tyg_store(p, TYG_I64, tyg_at_local(tmp, 0),
                          tyg_addr_of(p, tyg_at_sym(M.Bytes(val), 0)), 0);
                tyg_store(p, TYG_I64, tyg_at_local(tmp, 8), tyg_iconst(p, TYG_I64, (int64_t)val.size()), 0);
                V x;
                x.t = t;
                x.mem = true;
                x.a = tyg_at_local(tmp, 0);
                return x;
            }
            case NK_Unary: return GenUnary(static_cast<Unary *>(n));
            case NK_Binary: return GenBinary(static_cast<Binary *>(n));
            case NK_AsCast: return GenCast(static_cast<AsCast *>(n));
            case NK_Call: return GenCall(static_cast<Call *>(n));
            case NK_StructLit: return GenStructLit(static_cast<StructLit *>(n));
            case NK_Block: return GenBlock(static_cast<Block *>(n), false);
            case NK_IfExpr: return GenIf(static_cast<IfExpr *>(n), false);
            case NK_Return: case NK_Break: case NK_Continue: case NK_VarDecl: case NK_Assign:
            case NK_IncDec: case NK_While: case NK_ForLoop: case NK_LoopExpr:
                Stmt(n);
                return Dummy(n->exprtype && n->exprtype->kind != TY_VOID ? n->exprtype : nullptr);
            default: No(n->line, cat("expression kind ", (int)n->nkind));
        }
    }

    V GenUnary(Unary *u) {
        auto et = u->exprtype;
        if (u->op == T_BITAND) {
            if (et && et->kind == TY_REF) {
                if (!IsPlainRef(et)) No(u->line, "reference kind");
                auto pl = Deref(Place(u->child));
                V x;
                x.t = et;
                x.v = tyg_addr_of(p, pl.a);
                return x;
            }
            return Load(Deref(Place(u->child)), u->line);
        }
        auto ot = cg.OperandT(u->child->exprtype);
        if (!M.IsScalar(ot) || ot->kind == TY_REF) No(u->line, "unary operand type");
        auto x = GenAs(u->child, ot);
        switch (u->op) {
            case T_MINUS:
                if (ot->kind == TY_FLT) x.v = tyg_un(p, TYG_FNEG, x.v);
                else if (ot->kind == TY_INT && !IsUnsigned(ot->intstorage) && M.checks)
                    x.v = CheckedShim("neg", ot, { x.v }, u->line);
                else if (ot->kind == TY_INT) x.v = tyg_un(p, TYG_NEG, x.v);
                else No(u->line, "negation type");
                return x;
            case T_NOT:
                x.v = tyg_cmp(p, TYG_EQ, x.v, tyg_iconst(p, M.Scal(ot), 0));
                x.t = M.ast.booltype;
                return x;
            case T_BITNOT:
                if (ot->kind != TY_INT) No(u->line, "~ type");
                x.v = tyg_un(p, TYG_NOT, x.v);
                return x;
            default: No(u->line, "unary operator");
        }
    }

    // A file name and line for the runtime's abort messages.
    tyg_val FileArg(Line l) { return tyg_addr_of(p, tyg_at_sym(M.FileName(l.fileidx), 0)); }
    tyg_val LineArg(Line l) { return tyg_iconst(p, TYG_I32, l.line); }

    // Calls a C function by name with scalar arguments (types for the
    // signature), a C shim or a runtime function.
    tyg_val CallC(const string &name, const vector<const tyg_type *> &pts,
                  const vector<tyg_val> &args, const tyg_type *ret) {
        auto sig = tyg_sig_new(m, ret, pts.data(), (int)pts.size(), 0);
        vector<tyg_arg> as;
        for (auto a : args) {
            tyg_arg x {};
            x.val = a;
            as.push_back(x);
        }
        return tyg_call(p, sig, tyg_direct(M.Sym(name, TYG_SYM_FUNC | TYG_SYM_GLOBAL)), as.data(),
                        (int)as.size(), nullptr, nullptr);
    }
    const tyg_type *IT(tyg_scalar s, bool sgn = true) { return tyg_type_int(m, s, sgn); }

    // The runtime.h helper gs_<op>_<sfx> for t (checked arithmetic,
    // division), through a shim of the same signature.
    tyg_val CheckedShim(const string &op, TypeExpr *t, vector<tyg_val> args, Line l) {
        auto sfx = string(CodeGen::IntSfx(t->intstorage));
        auto ct = string(CodeGen::IntCT(t->intstorage));
        auto name = cat("gs_tg_", op, "_", sfx);
        auto unary = op == "neg";
        M.NeedShim(name, unary ? cat(ct, " ", name, "(", ct, " a, const char *f, int l) { return gs_",
                                     op, "_", sfx, "(a, f, l); }\n")
                               : cat(ct, " ", name, "(", ct, " a, ", ct,
                                     " b, const char *f, int l) { return gs_", op, "_", sfx,
                                     "(a, b, f, l); }\n"));
        auto at = M.AT(t);
        vector<const tyg_type *> pts(args.size(), at);
        pts.push_back(tyg_type_ptr(m));
        pts.push_back(IT(TYG_I32));
        args.push_back(FileArg(l));
        args.push_back(LineArg(l));
        return CallC(name, pts, args, at);
    }

    V GenBinary(Binary *b) {
        auto op = b->op;
        V x;
        if (op == T_ANDAND || op == T_OROR) {
            auto t = tyg_local_new(p, 1, 1);
            auto l = Truth(b->left);
            auto lb = tyg_cmp(p, TYG_NE, l, tyg_iconst(p, TYG_I8, 0));
            tyg_store(p, TYG_I8, tyg_at_local(t, 0), lb, 0);
            auto rhs = tyg_block_new(p), done = tyg_block_new(p);
            if (op == T_ANDAND) tyg_branch(p, lb, rhs, done);
            else tyg_branch(p, lb, done, rhs);
            tyg_block_set(p, rhs);
            auto r = Truth(b->right);
            if (!Ended()) {
                tyg_store(p, TYG_I8, tyg_at_local(t, 0), tyg_cmp(p, TYG_NE, r, tyg_iconst(p, TYG_I8, 0)), 0);
                tyg_jump(p, done);
            }
            tyg_block_set(p, done);
            x.t = M.ast.booltype;
            x.v = tyg_load(p, TYG_I8, tyg_at_local(t, 0), 0);
            return x;
        }
        if (op == T_DOTEQ || op == T_DOTNEQ) No(b->line, "reference identity");
        if (Is<NullLit>(b->left) || Is<NullLit>(b->right)) No(b->line, "null test");
        auto lt = cg.OperandT(b->left->exprtype);
        if (!M.IsScalar(lt) || lt->kind == TY_REF) No(b->line, "operand type");
        auto isflt = lt->kind == TY_FLT;
        auto sgn = TgModule::IsSigned(lt);
        if (op == T_SHL || op == T_SHR) {
            if (lt->kind != TY_INT) No(b->line, "shift type");
            auto l = GenAs(b->left, lt);
            auto r = GenAs(b->right, M.ast.inttypes[IS_I64]);
            auto s = M.Scal(lt);
            auto bits = IntBits(lt->intstorage);
            auto n = tyg_bin(p, TYG_AND, r.v, tyg_iconst(p, TYG_I64, bits - 1));
            auto w = s == TYG_I64 ? l.v : tyg_convert(p, sgn ? TYG_SEXT : TYG_ZEXT, TYG_I64, l.v);
            auto res = tyg_bin(p, op == T_SHL ? TYG_SHL : sgn ? TYG_ASHR : TYG_LSHR, w, n);
            x.t = lt;
            x.v = s == TYG_I64 ? res : tyg_convert(p, TYG_TRUNC, s, res);
            return x;
        }
        auto l = GenAs(b->left, lt);
        auto r = GenAs(b->right, lt);
        if (Ended()) return Dummy(lt);
        x.t = lt;
        auto cmp = [&](tyg_cond si, tyg_cond un, tyg_cond fl) {
            x.t = M.ast.booltype;
            x.v = tyg_cmp(p, isflt ? fl : sgn ? si : un, l.v, r.v);
            return x;
        };
        switch (op) {
            case T_LT: return cmp(TYG_SLT, TYG_ULT, TYG_FLT);
            case T_GT: return cmp(TYG_SGT, TYG_UGT, TYG_FGT);
            case T_LTEQ: return cmp(TYG_SLE, TYG_ULE, TYG_FLE);
            case T_GTEQ: return cmp(TYG_SGE, TYG_UGE, TYG_FGE);
            case T_EQ: return cmp(TYG_EQ, TYG_EQ, TYG_FEQ);
            case T_NEQ: return cmp(TYG_NE, TYG_NE, TYG_FNE);
            default: break;
        }
        if (isflt) {
            switch (op) {
                case T_PLUS: x.v = tyg_bin(p, TYG_FADD, l.v, r.v); return x;
                case T_MINUS: x.v = tyg_bin(p, TYG_FSUB, l.v, r.v); return x;
                case T_MUL: x.v = tyg_bin(p, TYG_FMUL, l.v, r.v); return x;
                case T_DIV: x.v = tyg_bin(p, TYG_FDIV, l.v, r.v); return x;
                case T_MOD: {
                    auto f32 = M.Scal(lt) == TYG_F32;
                    auto ft = tyg_type_scalar(m, M.Scal(lt));
                    vector<const tyg_type *> pts { ft, ft };
                    auto sig = tyg_sig_new(m, ft, pts.data(), 2, 0);
                    tyg_arg as[2] {};
                    as[0].val = l.v;
                    as[1].val = r.v;
                    x.v = tyg_call(p, sig, tyg_direct(M.Sym(f32 ? "fmodf" : "fmod",
                                                            TYG_SYM_FUNC | TYG_SYM_GLOBAL |
                                                            TYG_SYM_FOREIGN)),
                                   as, 2, nullptr, nullptr);
                    return x;
                }
                default: No(b->line, "float operator");
            }
        }
        if (lt->kind != TY_INT) {
            if (lt->kind == TY_BOOL && (op == T_BITAND || op == T_BITOR || op == T_XOR)) {
                x.v = tyg_bin(p, op == T_BITAND ? TYG_AND : op == T_BITOR ? TYG_OR : TYG_XOR, l.v, r.v);
                return x;
            }
            No(b->line, "operator on this type");
        }
        x.v = IntOp(op, lt, l.v, r.v, b->line);
        return x;
    }

    // An integer operation at t's width: unsigned and release-build signed
    // arithmetic wraps, a debug build's signed arithmetic checks (runtime.h),
    // and division and modulo check in every build (§6.2).
    tyg_val IntOp(TType op, TypeExpr *t, tyg_val l, tyg_val r, Line ln) {
        auto sgn = TgModule::IsSigned(t);
        switch (op) {
            case T_PLUS: case T_MINUS: case T_MUL: {
                if (sgn && M.checks)
                    return CheckedShim(op == T_PLUS ? "add" : op == T_MINUS ? "sub" : "mul", t, { l, r },
                                       ln);
                return tyg_bin(p, op == T_PLUS ? TYG_ADD : op == T_MINUS ? TYG_SUB : TYG_MUL, l, r);
            }
            case T_DIV: return CheckedShim("div", t, { l, r }, ln);
            case T_MOD: return CheckedShim("mod", t, { l, r }, ln);
            case T_BITAND: return tyg_bin(p, TYG_AND, l, r);
            case T_BITOR: return tyg_bin(p, TYG_OR, l, r);
            case T_XOR: return tyg_bin(p, TYG_XOR, l, r);
            default: No(ln, "integer operator");
        }
    }

    V GenCast(AsCast *c) {
        auto st = c->child->exprtype;
        auto tt = c->totype;
        auto x = GenAs(c->child, cg.OperandT(st));
        st = x.t;
        if (!M.IsScalar(st) || !M.IsScalar(tt) || st->kind == TY_REF || tt->kind == TY_REF)
            No(c->line, "cast types");
        V y;
        y.t = tt;
        if (tt->kind == TY_FLT) {
            if (st->kind == TY_BOOL) No(c->line, "bool cast");
            y.v = Conv(x.v, st, tt, c->line);
            return y;
        }
        if (tt->kind != TY_INT || tt->intstorage == IS_VARINT) No(c->line, "cast target");
        auto is = tt->intstorage;
        if (st->kind == TY_FLT) {
            auto d = M.Scal(st) == TYG_F32 ? tyg_convert(p, TYG_FEXT, TYG_F64, x.v) : x.v;
            auto f64 = tyg_type_scalar(m, TYG_F64);
            tyg_val r;
            if (c->unchecked) {
                M.NeedShim("gs_tg_f2iwrap",
                           "int64_t gs_tg_f2iwrap(double d) { return gs_f2iwrap(d); }\n");
                r = CallC("gs_tg_f2iwrap", { f64 }, { d }, IT(TYG_I64));
            } else if (is == IS_U64) {
                M.NeedShim("gs_tg_f2u", "uint64_t gs_tg_f2u(double d, int s, const char *f, int l) "
                                        "{ (void)s; (void)f; (void)l; return GS_F2U(d, s, f, l); }\n");
                r = CallC("gs_tg_f2u", { f64, IT(TYG_I32), tyg_type_ptr(m), IT(TYG_I32) },
                          { d, tyg_iconst(p, TYG_I32, M.Scal(st) == TYG_F32), FileArg(c->line),
                            LineArg(c->line) },
                          IT(TYG_I64, false));
            } else {
                M.NeedShim("gs_tg_f2i",
                           "int64_t gs_tg_f2i(double d, int s, int64_t lo, int64_t hi, const char *t, "
                           "const char *f, int l) { (void)s; (void)lo; (void)hi; (void)t; (void)f; "
                           "(void)l; return GS_F2I(d, s, lo, hi, t, f, l); }\n");
                auto [lo, hi] = IntRange(is);
                r = CallC("gs_tg_f2i",
                          { f64, IT(TYG_I32), IT(TYG_I64), IT(TYG_I64), tyg_type_ptr(m), tyg_type_ptr(m),
                            IT(TYG_I32) },
                          { d, tyg_iconst(p, TYG_I32, M.Scal(st) == TYG_F32), tyg_iconst(p, TYG_I64, lo),
                            tyg_iconst(p, TYG_I64, hi), TypeNameArg(is), FileArg(c->line),
                            LineArg(c->line) },
                          IT(TYG_I64));
            }
            y.v = Conv(r, M.ast.inttypes[IS_I64], tt, c->line);
            return y;
        }
        if (st->kind == TY_BOOL) No(c->line, "bool cast");
        auto fits = [&] {
            auto [slo, shi] = IntRange(st->intstorage);
            auto [lo, hi] = IntRange(is);
            return st->intstorage != IS_U64 && slo >= lo && shi <= hi;
        };
        if (c->unchecked || cg.TEq(st, tt) || fits() || (st->intstorage == IS_U64 && is == IS_U64)) {
            y.v = Conv(x.v, st, tt, c->line);
            return y;
        }
        // The checked narrowings of GS_RANGE and GS_RANGE_U: identities in a
        // release build.
        tyg_val r;
        if (st->intstorage == IS_U64) {
            M.NeedShim("gs_tg_range_u",
                       "uint64_t gs_tg_range_u(uint64_t v, uint64_t hi, const char *t, const char *f, "
                       "int l) { (void)hi; (void)t; (void)f; (void)l; "
                       "return GS_RANGE_U(v, hi, t, f, l); }\n");
            r = CallC("gs_tg_range_u",
                      { IT(TYG_I64, false), IT(TYG_I64, false), tyg_type_ptr(m), tyg_type_ptr(m),
                        IT(TYG_I32) },
                      { x.v, tyg_iconst(p, TYG_I64, IntRange(is).second), TypeNameArg(is),
                        FileArg(c->line), LineArg(c->line) },
                      IT(TYG_I64, false));
            y.v = Conv(r, M.ast.inttypes[IS_U64], tt, c->line);
            return y;
        }
        M.NeedShim("gs_tg_range",
                   "int64_t gs_tg_range(int64_t v, int64_t lo, int64_t hi, const char *t, "
                   "const char *f, int l) { (void)lo; (void)hi; (void)t; (void)f; (void)l; "
                   "return GS_RANGE(v, lo, hi, t, f, l); }\n");
        auto [lo, hi] = IntRange(is);
        auto w = Conv(x.v, st, M.ast.inttypes[IS_I64], c->line);
        r = CallC("gs_tg_range",
                  { IT(TYG_I64), IT(TYG_I64), IT(TYG_I64), tyg_type_ptr(m), tyg_type_ptr(m), IT(TYG_I32) },
                  { w, tyg_iconst(p, TYG_I64, lo), tyg_iconst(p, TYG_I64, hi), TypeNameArg(is),
                    FileArg(c->line), LineArg(c->line) },
                  IT(TYG_I64));
        y.v = Conv(r, M.ast.inttypes[IS_I64], tt, c->line);
        return y;
    }
    tyg_val TypeNameArg(IntStorage is) {
        return tyg_addr_of(p, tyg_at_sym(M.Bytes(cat(IntStorageName(is), string(1, '\0'))), 0));
    }

    V GenStructLit(StructLit *sl) {
        auto t = sl->exprtype;
        if (!sl->sinst || sl->variant || t->kind != TY_STRUCT || !M.TypeOk(t))
            No(sl->line, "literal of this type");
        auto tmp = Temp(t);
        tyg_zero(p, tyg_at_local(tmp, 0), M.Size(t));
        auto si = cg.SI(t);
        auto &lay = cg.StructLayout(si);
        for (size_t i = 0; i < si->st->fields.size(); i++) {
            if (si->st->fields[i].ispad) continue;
            auto init = sl->InitFor((int)i);
            if (!init) continue;
            Into(P { tyg_at_local(tmp, lay.offs[i]), si->ftypes[i] }, init);
        }
        V x;
        x.t = t;
        x.mem = true;
        x.a = tyg_at_local(tmp, 0);
        return x;
    }

    // ---- Arrays and slices -------------------------------------------------

    // The elements of an array or slice in memory at pl (references read
    // through) and their count: a fixed array's lie in place, a slice's
    // where its data member points.
    struct View {
        tyg_val elems, len;
        TypeExpr *elem;
        int64_t k;   // A fixed array's length, else -1.
    };
    View ViewOf(P pl, Line l) {
        pl = Deref(pl);
        if (pl.t->kind == TY_ARRAY && pl.t->arr->akind == A_FIXED && M.TypeOk(pl.t)) {
            auto k = cg.ArrSize(pl.t->arr);
            return { tyg_addr_of(p, pl.a), tyg_iconst(p, TYG_I64, k), pl.t->arr->sub, k };
        }
        if (pl.t->kind == TY_SLICE && M.TypeOk(pl.t)) {
            auto a8 = pl.a;
            a8.offset += 8;
            return { tyg_load(p, TYG_I64, pl.a, 0), tyg_load(p, TYG_I64, a8, 0), pl.t->sub, -1 };
        }
        No(l, "array of this kind");
    }
    tyg_val ElemAddr(const View &v, tyg_val i) {
        auto esz = cg.FixedSize(v.elem);
        auto off = tyg_bin(p, TYG_MUL, i, tyg_iconst(p, TYG_I64, esz));
        return tyg_bin(p, TYG_ADD, v.elems, off);
    }

    // An index as the i64 the checks and the address arithmetic take.
    tyg_val Index64(Node *n) {
        auto it = cg.OperandT(n->exprtype);
        if (!it || it->kind != TY_INT || it->intstorage == IS_VARINT) No(n->line, "index type");
        return Conv(GenAs(n, it).v, it, M.ast.inttypes[IS_I64], n->line);
    }

    // GS_IDX: i in [0, n), or gs_idxfail with the Goose file and line,
    // which says what the C backend's says.
    void BoundsCheck(tyg_val i, tyg_val n, Line l) {
        auto ok = tyg_block_new(p), bad = tyg_block_new(p);
        tyg_branch(p, tyg_cmp(p, TYG_ULT, i, n), ok, bad);
        tyg_block_set(p, bad);
        CallC("gs_idxfail", { IT(TYG_I64), IT(TYG_I64), tyg_type_ptr(m), IT(TYG_I32) },
              { i, n, FileArg(l), LineArg(l) }, nullptr);
        tyg_unreachable(p);
        tyg_block_set(p, ok);
    }

    // a[i] (CodeGen::IndexLoc): the array's place, then the index, then the
    // check, which BCE (Index::nobc) or a constant within a fixed array's
    // length elides.
    P IndexPlace(Index *ix) {
        auto base = Deref(Place(ix->obj));
        auto il = Is<IntLit>(ix->idx);
        auto i = Index64(ix->idx);
        auto v = ViewOf(base, ix->line);
        auto known = il && v.k >= 0 && il->val >= 0 && il->val < v.k;
        if (!ix->nobc && !known) BoundsCheck(i, v.len, ix->line);
        return P { tyg_at_val(ElemAddr(v, i), 0), v.elem };
    }

    // .len and len(): a fixed array's is its type's; a slice's is read.
    V GenLen(Node *obj, Node *n) {
        auto ot = obj->exprtype;
        while (ot && IsPlainRef(ot)) ot = ot->ref->sub;
        if (!ot || !M.TypeOk(ot) || (ot->kind != TY_ARRAY && ot->kind != TY_SLICE))
            No(n->line, "len of this type");
        tyg_val len;
        if (ot->kind == TY_ARRAY) {
            if (!Is<Ident>(obj) && !Is<Dot>(obj)) Gen(obj);   // What it does, it does.
            len = tyg_iconst(p, TYG_I64, cg.ArrSize(ot->arr));
        } else {
            len = ViewOf(Place(obj), n->line).len;
        }
        V x;
        x.t = M.ast.inttypes[IS_I64];
        x.v = len;
        return x;
    }

    // A slice { data, len } in a temporary.
    V SliceValue(TypeExpr *t, tyg_val data, tyg_val len) {
        auto tmp = tyg_local_new(p, 16, 8);
        tyg_store(p, TYG_I64, tyg_at_local(tmp, 0), data, 0);
        tyg_store(p, TYG_I64, tyg_at_local(tmp, 8), len, 0);
        V x;
        x.t = t;
        x.mem = true;
        x.a = tyg_at_local(tmp, 0);
        return x;
    }

    // A value at a slice destination: a whole array there is a view of the
    // array where it lies (§3.10), or in a temporary of its own if it is a
    // value. The checker may have typed the node as the slice already.
    V GenSliceAs(Node *n, TypeExpr *t) {
        if (!M.TypeOk(t)) No(n->line, "slice type");
        auto d = Is<Dot>(n);
        P pl;
        if (Is<Ident>(n) || Is<Index>(n) || (d && d->IsField())) {
            pl = Deref(Place(n));
        } else {
            auto x = Gen1(n);
            if (Ended()) return Dummy(t);
            if (x.t && IsPlainRef(x.t)) pl = Deref(P { tyg_at_val(x.v, 0), x.t->ref->sub });
            else if (x.t && x.t->kind == TY_ARRAY && x.mem) pl = P { x.a, x.t };
            else return Fit(x, t, n->line);
        }
        if (pl.t->kind != TY_ARRAY) return Fit(Load(pl, n->line), t, n->line);
        auto v = ViewOf(pl, n->line);
        if (cg.Mangle(v.elem) != cg.Mangle(t->sub)) No(n->line, "slice of another element type");
        return SliceValue(t, v.elems, v.len);
    }

    // a[lo:hi] (CodeGen::GenSlice): the view, then the bounds (^k counts
    // from the end), checked as the C checks them unless BCE proved them.
    V GenSlice(SliceExpr *se) {
        auto v = ViewOf(Place(se->obj), se->line);
        auto bound = [&](Node *b, bool fromend, tyg_val dflt) {
            if (!b) return dflt;
            auto x = Index64(b);
            return fromend ? tyg_bin(p, TYG_SUB, v.len, x) : x;
        };
        auto lo = bound(se->lo, se->lo_from_end, tyg_iconst(p, TYG_I64, 0));
        auto hi = bound(se->hi, se->hi_from_end, v.len);
        if (!se->nobc) {
            auto ok = tyg_block_new(p), bad = tyg_block_new(p), c2 = tyg_block_new(p),
                 c3 = tyg_block_new(p);
            tyg_branch(p, tyg_cmp(p, TYG_SLT, lo, tyg_iconst(p, TYG_I64, 0)), bad, c2);
            tyg_block_set(p, c2);
            tyg_branch(p, tyg_cmp(p, TYG_SLT, hi, lo), bad, c3);
            tyg_block_set(p, c3);
            tyg_branch(p, tyg_cmp(p, TYG_SGT, hi, v.len), bad, ok);
            tyg_block_set(p, bad);
            M.NeedShim("gs_tg_slicefail",
                       "void gs_tg_slicefail(const char *f, int l) { gs_abort(GS_E_SLICE, f, l); }\n");
            CallC("gs_tg_slicefail", { tyg_type_ptr(m), IT(TYG_I32) },
                  { FileArg(se->line), LineArg(se->line) }, nullptr);
            tyg_unreachable(p);
            tyg_block_set(p, ok);
        }
        auto t = se->exprtype;
        if (!t || t->kind != TY_SLICE || cg.Mangle(t->sub) != cg.Mangle(v.elem)) No(se->line, "slice to this type");
        return SliceValue(t, ElemAddr(v, lo), tyg_bin(p, TYG_SUB, hi, lo));
    }

    // [a, b, c] and [v; k] as a fixed array.
    V GenArrayLit(ArrayLit *al) {
        auto t = al->exprtype;
        if (!t || t->kind != TY_ARRAY || t->arr->akind != A_FIXED || !M.TypeOk(t) || al->capexpr)
            No(al->line, "array literal of this type");
        auto k = cg.ArrSize(t->arr);
        auto et = t->arr->sub;
        auto esz = cg.FixedSize(et);
        auto tmp = Temp(t);
        tyg_zero(p, tyg_at_local(tmp, 0), M.Size(t));
        if (al->fillval) {
            auto il = Is<IntLit>(al->fillcount);
            if (!il || il->val != k) No(al->line, "fill count");
            // The value once, then copied into every slot.
            auto one = Temp(et);
            Into(P { tyg_at_local(one, 0), et }, al->fillval);
            if (k <= 16) {
                for (int64_t i = 0; i < k; i++)
                    tyg_copy(p, tyg_at_local(tmp, i * esz), tyg_at_local(one, 0), esz);
            } else {
                auto ctr = tyg_local_new(p, 8, 8);
                tyg_store(p, TYG_I64, tyg_at_local(ctr, 0), tyg_iconst(p, TYG_I64, 0), 0);
                auto head = tyg_block_new(p), body = tyg_block_new(p), done = tyg_block_new(p);
                tyg_jump(p, head);
                tyg_block_set(p, head);
                auto c = tyg_load(p, TYG_I64, tyg_at_local(ctr, 0), 0);
                tyg_branch(p, tyg_cmp(p, TYG_SLT, c, tyg_iconst(p, TYG_I64, k)), body, done);
                tyg_block_set(p, body);
                View v { tyg_addr_of(p, tyg_at_local(tmp, 0)), 0, et, k };
                tyg_copy(p, tyg_at_val(ElemAddr(v, c), 0), tyg_at_local(one, 0), esz);
                tyg_store(p, TYG_I64, tyg_at_local(ctr, 0), tyg_bin(p, TYG_ADD, c, tyg_iconst(p, TYG_I64, 1)), 0);
                tyg_jump(p, head);
                tyg_block_set(p, done);
            }
        } else {
            if ((int64_t)al->elems.size() > k) No(al->line, "array literal length");
            for (size_t i = 0; i < al->elems.size(); i++)
                Into(P { tyg_at_local(tmp, (int64_t)i * esz), et }, al->elems[i]);
        }
        V x;
        x.t = t;
        x.mem = true;
        x.a = tyg_at_local(tmp, 0);
        return x;
    }

    // ---- Calls -------------------------------------------------------------

    tyg_arg Arg(Node *n, TypeExpr *pt) {
        tyg_arg a {};
        auto x = GenAs(n, pt);
        if (M.IsScalar(pt)) a.val = x.v;
        else a.mem = x.a;
        return a;
    }

    // Every result of the latest call (GenCall): the first as the call's
    // value, the others where the callee wrote them through out-pointers.
    vector<V> lastrets;

    V Result(FnSpec *callee, tyg_sig *sig, tyg_callee ce, vector<tyg_arg> &args) {
        V x;
        if (callee->rets.empty()) {
            tyg_call(p, sig, ce, args.data(), (int)args.size(), nullptr, nullptr);
            return x;
        }
        auto rt = callee->rets[0];
        x.t = rt;
        if (M.IsScalar(rt)) {
            x.v = tyg_call(p, sig, ce, args.data(), (int)args.size(), nullptr, nullptr);
            return x;
        }
        auto t = Temp(rt);
        auto ra = tyg_at_local(t, 0);
        tyg_call(p, sig, ce, args.data(), (int)args.size(), nullptr, &ra);
        x.mem = true;
        x.a = ra;
        return x;
    }

    V GenCall(Call *c) {
        if (c->builtin == B_PRINT) return GenPrint(c);
        if (c->builtin == B_LEN && c->ArgNodes().size() == 1) return GenLen(c->ArgNodes()[0], c);
        if (c->builtin >= 0) No(c->line, cat("builtin ", BuiltinByKind(c->builtin).name));
        if (c->fvbody || !c->dispatch.empty() || !c->fvremap.empty() || !c->spec)
            No(c->line, "call kind");
        auto callee = c->spec;
        auto an = c->ArgNodes();
        if (an.size() < callee->argtypes.size()) No(c->line, "argument count");
        an.resize(callee->argtypes.size());
        if (callee->sf->isextern) return GenExternCall(c, callee, an);
        string why;
        if (!M.SigOk(callee, false, why)) No(c->line, cat("call to ", callee->sf->name, ": ", why));
        vector<tyg_arg> args;
        for (size_t i = 0; i < an.size(); i++) args.push_back(Arg(an[i], callee->argtypes[i]));
        lastrets.clear();
        if (Ended()) {
            for (auto r : callee->rets) lastrets.push_back(Dummy(r));
            return Dummy(callee->rets.empty() ? nullptr : callee->rets[0]);
        }
        // Results after the first land in temporaries, through out-pointers
        // after the declared arguments (C.3).
        vector<V> extra;
        for (size_t i = 1; i < callee->rets.size(); i++) {
            V r;
            r.t = callee->rets[i];
            auto t = Temp(r.t);
            r.a = tyg_at_local(t, 0);
            tyg_arg a {};
            a.val = tyg_addr_of(p, r.a);
            args.push_back(a);
            if (M.IsScalar(r.t)) r.v = 0;
            else r.mem = true;
            extra.push_back(r);
        }
        if (cg.sinfo[callee].needssp) {
            relied.insert(callee);
            tyg_arg sp0 {};
            sp0.val = tyg_iconst(p, TYG_I64, 0);
            args.push_back(sp0);
        }
        auto sym = M.Sym(cg.sinfo[callee].cname, TYG_SYM_FUNC | TYG_SYM_GLOBAL);
        auto x = Result(callee, M.Sig(callee), tyg_direct(sym), args);
        lastrets.push_back(x);
        for (auto &r : extra) {
            if (M.IsScalar(r.t)) {
                lastrets.push_back(Load(P { r.a, r.t }, c->line));
            } else {
                lastrets.push_back(r);
            }
        }
        return x;
    }

    // A call whose every result is wanted: `let a, b = f()`, `return f()`.
    vector<V> GenCallAll(Node *n, size_t count) {
        auto c = Is<Call>(n);
        if (!c || c->builtin >= 0 || !c->spec || c->spec->sf->isextern) No(n->line, "multiple results of this kind");
        GenCall(c);
        if (lastrets.size() != count) No(n->line, "result count");
        return lastrets;
    }

    // An extern fn, through a trampoline the C half defines: the C function
    // may be a static one of an --include header or of the runtime's extern
    // support, or a macro.
    V GenExternCall(Call *c, FnSpec *callee, vector<Node *> &an) {
        if (callee->rets.size() > 1) No(c->line, "extern results");
        lastrets.clear();
        for (auto r : callee->rets) if (!M.TypeOk(r)) No(c->line, "extern result type");
        vector<tyg_arg> args;
        for (size_t i = 0; i < an.size(); i++) {
            auto pt = callee->argtypes[i];
            if (pt->kind == TY_SLICE) {
                auto s = Is<StrLit>(an[i]);
                if (!s && M.TypeOk(pt)) {
                    args.push_back(Arg(an[i], pt));
                    continue;
                }
                if (!s || !IsU8(pt->sub)) No(c->line, "slice argument");
                auto t = tyg_local_new(p, 16, 8);
                tyg_store(p, TYG_I64, tyg_at_local(t, 0),
                          tyg_addr_of(p, tyg_at_sym(M.Bytes(s->val), 0)), 0);
                tyg_store(p, TYG_I64, tyg_at_local(t, 8), tyg_iconst(p, TYG_I64, (int64_t)s->val.size()), 0);
                tyg_arg a {};
                a.mem = tyg_at_local(t, 0);
                args.push_back(a);
                continue;
            }
            if (!M.TypeOk(pt)) No(c->line, "extern parameter type");
            args.push_back(Arg(an[i], pt));
        }
        // The C names of its types, now, while the C half can still take
        // their declarations.
        for (auto t : callee->argtypes) cg.CT(t);
        for (auto t : callee->rets) cg.CT(t);
        // One trampoline per C name: a call may have a specialization of
        // its own of the same extern.
        auto known = false;
        for (auto e : M.externs) known = known || e->sf->cname == callee->sf->cname;
        if (!known) M.externs.push_back(callee);
        if (Ended()) return Dummy(callee->rets.empty() ? nullptr : callee->rets[0]);
        auto sym = M.Sym(cat("gs_tg_x_", callee->sf->cname), TYG_SYM_FUNC | TYG_SYM_GLOBAL);
        return Result(callee, M.ExternSig(callee), tyg_direct(sym), args);
    }

    // print(...) (§3.7): every argument is evaluated before any text is
    // written, as the C backend renders the whole line first.
    V GenPrint(Call *c) {
        if (!c->fmtspecs.empty()) No(c->line, "print with format overloads");
        struct Out {
            string fn;
            vector<tyg_val> args;
            vector<const tyg_type *> types;
        };
        vector<Out> outs;
        for (auto a : c->ArgNodes()) {
            auto t = a->exprtype;
            if (auto s = Is<StrLit>(a)) {
                outs.push_back({ "gs_out_bytes",
                                 { tyg_addr_of(p, tyg_at_sym(M.Bytes(s->val), 0)),
                                   tyg_iconst(p, TYG_I64, (int64_t)s->val.size()) },
                                 { tyg_type_ptr(m), IT(TYG_I64) } });
                continue;
            }
            auto ot = cg.OperandT(t);
            if (!ot || !M.IsScalar(ot) || ot->kind == TY_REF) No(a->line, "print of this type");
            if (ot->kind == TY_INT && ot->intstorage == IS_U64) {
                outs.push_back({ "gs_out_uint", { GenAs(a, ot).v }, { IT(TYG_I64, false) } });
            } else if (ot->kind == TY_INT) {
                outs.push_back({ "gs_out_int", { GenAs(a, M.ast.inttypes[IS_I64]).v }, { IT(TYG_I64) } });
            } else if (ot->kind == TY_FLT && M.Scal(ot) == TYG_F32) {
                outs.push_back({ "gs_out_f32", { GenAs(a, ot).v }, { tyg_type_scalar(m, TYG_F32) } });
            } else if (ot->kind == TY_FLT) {
                outs.push_back({ "gs_out_flt", { GenAs(a, ot).v }, { tyg_type_scalar(m, TYG_F64) } });
            } else {
                auto v = GenAs(a, ot).v;
                outs.push_back({ "gs_out_bool", { tyg_convert(p, TYG_ZEXT, TYG_I64, v) }, { IT(TYG_I64) } });
            }
        }
        if (Ended()) return V {};
        for (auto &o : outs) CallC(o.fn, o.types, o.args, nullptr);
        CallC("gs_out_nl", {}, {}, nullptr);
        return V {};
    }

    // ---- Control -----------------------------------------------------------

    // A block's value is its tail's (none in statement position).
    V GenBlock(Block *b, bool discard) {
        tyg_scope_begin(p);
        for (auto s : b->stmts) Stmt(s);
        V x;
        if (b->tail) {
            if (discard) Stmt(b->tail);
            else {
                Open();
                Loc(b->tail->line);
                x = Gen(b->tail);
            }
        }
        tyg_scope_end(p);
        return x;
    }

    V GenArm(Node *n, bool discard) {
        if (auto b = Is<Block>(n)) return GenBlock(b, discard);
        if (discard) {
            Stmt(n);
            return V {};
        }
        return Gen(n);
    }

    V GenIf(IfExpr *i, bool discard) {
        auto et = i->exprtype;
        auto valued = !discard && et && et->kind != TY_VOID;
        if (valued && !M.TypeOk(et)) No(i->line, "if value type");
        tyg_local res = 0;
        if (valued) res = Temp(et);
        auto c = Truth(i->cond);
        auto tb = tyg_block_new(p), eb = tyg_block_new(p), done = tyg_block_new(p);
        tyg_branch(p, c, tb, i->elseb ? eb : done);
        tyg_block_set(p, tb);
        auto arm = [&](Node *n) {
            auto x = GenArm(n, !valued);
            if (valued && !Ended()) Store(P { tyg_at_local(res, 0), et }, Fit(x, et, n->line));
            Jump(done);
        };
        arm(i->thenb);
        if (i->elseb) {
            tyg_block_set(p, eb);
            arm(i->elseb);
        }
        tyg_block_set(p, done);
        if (!valued) return V {};
        return Load(P { tyg_at_local(res, 0), et }, i->line);
    }

    // ---- Statements ----------------------------------------------------------

    void Stmt(Node *n) {
        Open();
        Loc(n->line);
        switch (n->nkind) {
            case NK_VarDecl: return StmtVarDecl(static_cast<VarDecl *>(n));
            case NK_Assign: return StmtAssign(static_cast<Assign *>(n));
            case NK_IncDec: {
                auto id = static_cast<IncDec *>(n);
                auto pl = Deref(Place(id->lval));
                if (pl.t->kind != TY_INT || pl.t->intstorage == IS_VARINT) No(n->line, "++/-- type");
                auto v = tyg_load(p, M.Scal(pl.t), pl.a, 0);
                auto r = IntOp(id->op == T_INC ? T_PLUS : T_MINUS, pl.t, v,
                               tyg_iconst(p, M.Scal(pl.t), 1), n->line);
                tyg_store(p, M.Scal(pl.t), pl.a, r, 0);
                return;
            }
            case NK_While: {
                auto w = static_cast<While *>(n);
                auto head = tyg_block_new(p), body = tyg_block_new(p), done = tyg_block_new(p);
                tyg_jump(p, head);
                tyg_block_set(p, head);
                Loc(w->cond->line);
                tyg_branch(p, Truth(w->cond), body, done);
                tyg_block_set(p, body);
                loops.push_back({ done, head });
                GenBlock(w->body, true);
                loops.pop_back();
                Jump(head);
                tyg_block_set(p, done);
                return;
            }
            case NK_LoopExpr: {
                auto lp = static_cast<LoopExpr *>(n);
                for (auto b : lp->breaks) if (b->val) No(n->line, "loop with a value");
                auto body = tyg_block_new(p), done = tyg_block_new(p);
                tyg_jump(p, body);
                tyg_block_set(p, body);
                loops.push_back({ done, body });
                GenBlock(lp->body, true);
                loops.pop_back();
                Jump(body);
                tyg_block_set(p, done);
                return;
            }
            case NK_ForLoop: return StmtFor(static_cast<ForLoop *>(n));
            case NK_Return: {
                auto r = static_cast<Return *>(n);
                if (r->target != sp->sf) No(n->line, "return from");
                if (sp->rets.size() > 1) return ReturnMany(r);
                if (r->vals.empty() || sp->rets.empty()) {
                    for (auto v : r->vals) Gen(v);
                    if (!Ended()) tyg_ret(p, 0);
                    return;
                }
                auto x = GenAs(r->vals[0], sp->rets[0]);
                if (Ended()) return;
                if (x.mem) tyg_ret_mem(p, x.a);
                else tyg_ret(p, x.v);
                return;
            }
            case NK_Break: {
                auto b = static_cast<Break *>(n);
                if (b->val) No(n->line, "break with a value");
                if (loops.empty()) No(n->line, "break target");
                tyg_jump(p, loops.back().brk);
                return;
            }
            case NK_Continue:
                if (loops.empty()) No(n->line, "continue target");
                tyg_jump(p, loops.back().cnt);
                return;
            case NK_FnDecl: case NK_StructDecl: case NK_EnumDecl: case NK_AliasDecl: return;
            case NK_Block: GenBlock(static_cast<Block *>(n), true); return;
            case NK_IfExpr: GenIf(static_cast<IfExpr *>(n), true); return;
            default: Gen(n); return;
        }
    }

    // return a, b (each result in turn, the ones after the first written
    // through their out-pointers as they are made, as the C does), or
    // return f() forwarding all of a call's results (§7.1).
    void ReturnMany(Return *r) {
        auto &rets = sp->rets;
        vector<V> xs;
        if (r->vals.size() == 1) xs = GenCallAll(r->vals[0], rets.size());
        else if (r->vals.size() != rets.size()) No(r->line, "result count");
        P first { tyg_at_local(Temp(rets[0]), 0), rets[0] };
        for (size_t i = 0; i < rets.size(); i++) {
            auto x = xs.empty() ? GenAs(r->vals[i], rets[i]) : Fit(xs[i], rets[i], r->line);
            if (Ended()) return;
            if (!M.IsScalar(rets[i]) && !x.mem) No(r->line, "aggregate value");
            if (i == 0) {
                Store(first, x);
                continue;
            }
            auto ptr = tyg_load(p, TYG_I64, tyg_at_local(outs[i - 1], 0), 0);
            Store(P { tyg_at_val(ptr, 0), rets[i] }, x);
        }
        if (M.IsScalar(rets[0])) tyg_ret(p, tyg_load(p, M.Scal(rets[0]), first.a, 0));
        else tyg_ret_mem(p, first.a);
    }

    void Declare(VarDef *d, Line l) {
        if (!M.TypeOk(d->type)) No(l, cat("variable type of ", d->name));
        auto loc = Temp(d->type);
        locals[d] = loc;
        tyg_debug_var(p, string(d->name).c_str(), M.DT(d->type), tyg_at_local(loc, 0),
                      M.File(d->line.fileidx), d->line.line);
    }

    void StmtVarDecl(VarDecl *vd) {
        if (vd->isglobal || vd->reusable) No(vd->line, "declaration kind");
        if (vd->defs.size() > 1 && vd->inits.size() == 1) {
            // let a, b = f(): the locals, then the call into them.
            for (auto d : vd->defs) Declare(d, vd->line);
            auto xs = GenCallAll(vd->inits[0], vd->defs.size());
            for (size_t i = 0; i < xs.size(); i++) {
                auto d = vd->defs[i];
                auto x = Fit(xs[i], d->type, vd->line);
                if (!M.IsScalar(d->type) && !x.mem && !Ended()) No(vd->line, "aggregate value");
                Store(P { tyg_at_local(locals[d], 0), d->type }, x);
            }
            return;
        }
        if (!vd->inits.empty() && vd->inits.size() != vd->defs.size()) No(vd->line, "declaration kind");
        for (size_t i = 0; i < vd->defs.size(); i++) {
            auto d = vd->defs[i];
            Declare(d, vd->line);
            P pl { tyg_at_local(locals[d], 0), d->type };
            if (vd->inits.empty()) tyg_zero(p, pl.a, M.Size(d->type));
            else Into(pl, vd->inits[i]);
        }
    }

    void StmtAssign(Assign *a) {
        auto pl = Place(a->lval);
        if (a->pointee) pl = Deref(pl);
        if (a->op == T_DOTASSIGN) No(a->line, "rebind");
        if (a->op == T_ASSIGN) {
            // The destination's address is taken before the value runs (it
            // is a local slot or an address in a register), as the C does.
            auto x = GenAs(a->rhs, pl.t);
            if (!M.IsScalar(pl.t) && !x.mem && !Ended()) No(a->line, "aggregate value");
            Store(pl, x);
            return;
        }
        pl = Deref(pl);
        auto t = pl.t;
        if (!M.IsScalar(t) || t->kind == TY_REF) No(a->line, "compound assignment type");
        auto old = tyg_load(p, M.Scal(t), pl.a, 0);
        TType op;
        switch (a->op) {
            case T_PLUSEQ: op = T_PLUS; break;
            case T_MINUSEQ: op = T_MINUS; break;
            case T_MULEQ: op = T_MUL; break;
            case T_DIVEQ: op = T_DIV; break;
            case T_MODEQ: op = T_MOD; break;
            case T_ANDEQ: op = T_BITAND; break;
            case T_OREQ: op = T_BITOR; break;
            case T_XOREQ: op = T_XOR; break;
            case T_SHLEQ: op = T_SHL; break;
            case T_SHREQ: op = T_SHR; break;
            default: No(a->line, "assignment operator");
        }
        tyg_val r;
        if (op == T_SHL || op == T_SHR) {
            if (t->kind != TY_INT) No(a->line, "shift type");
            auto n = GenAs(a->rhs, M.ast.inttypes[IS_I64]).v;
            auto sgn = TgModule::IsSigned(t);
            auto s = M.Scal(t);
            n = tyg_bin(p, TYG_AND, n, tyg_iconst(p, TYG_I64, IntBits(t->intstorage) - 1));
            auto w = s == TYG_I64 ? old : tyg_convert(p, sgn ? TYG_SEXT : TYG_ZEXT, TYG_I64, old);
            auto res = tyg_bin(p, op == T_SHL ? TYG_SHL : sgn ? TYG_ASHR : TYG_LSHR, w, n);
            r = s == TYG_I64 ? res : tyg_convert(p, TYG_TRUNC, s, res);
        } else {
            auto rv = GenAs(a->rhs, t).v;
            if (t->kind == TY_FLT) {
                switch (op) {
                    case T_PLUS: r = tyg_bin(p, TYG_FADD, old, rv); break;
                    case T_MINUS: r = tyg_bin(p, TYG_FSUB, old, rv); break;
                    case T_MUL: r = tyg_bin(p, TYG_FMUL, old, rv); break;
                    case T_DIV: r = tyg_bin(p, TYG_FDIV, old, rv); break;
                    default: No(a->line, "float compound assignment");
                }
            } else if (t->kind == TY_INT) {
                r = IntOp(op, t, old, rv, a->line);
            } else if (op == T_BITAND || op == T_BITOR || op == T_XOR) {
                r = tyg_bin(p, op == T_BITAND ? TYG_AND : op == T_BITOR ? TYG_OR : TYG_XOR, old, rv);
            } else {
                No(a->line, "compound assignment type");
            }
        }
        if (!Ended()) tyg_store(p, M.Scal(t), pl.a, r, 0);
    }

    // A counted loop over a range or a count (§6.5): the counter runs at the
    // range's own type, the binding is a copy of it at its own.
    void StmtFor(ForLoop *f) {
        if (f->iterkind == IK_ARRAY || f->iterkind == IK_SLICE) return StmtForEach(f);
        if (f->iterkind != IK_RANGE && f->iterkind != IK_COUNT) No(f->line, "for kind");
        auto r = Is<RangeExpr>(f->iter);
        auto ct = r ? r->exprtype : f->iter->exprtype;
        if (!ct || ct->kind != TY_INT || ct->intstorage == IS_VARINT) No(f->line, "range type");
        auto s = M.Scal(ct);
        auto lo = r ? GenAs(r->lo, ct).v : tyg_iconst(p, s, 0);
        auto hi = GenAs(r ? r->hi : f->iter, ct).v;
        auto ctr = tyg_local_new(p, TgModule::ScalSize(s), TgModule::ScalSize(s));
        auto hiv = tyg_local_new(p, TgModule::ScalSize(s), TgModule::ScalSize(s));
        tyg_store(p, s, tyg_at_local(ctr, 0), lo, 0);
        tyg_store(p, s, tyg_at_local(hiv, 0), hi, 0);
        tyg_local ix = 0;
        tyg_scope_begin(p);
        if (f->idxdef) {
            ix = tyg_local_new(p, 8, 8);
            tyg_store(p, TYG_I64, tyg_at_local(ix, 0), tyg_iconst(p, TYG_I64, 0), 0);
        }
        if (f->vdef) Declare(f->vdef, f->line);
        if (f->idxdef) Declare(f->idxdef, f->line);
        auto head = tyg_block_new(p), body = tyg_block_new(p), step = tyg_block_new(p),
             done = tyg_block_new(p);
        tyg_jump(p, head);
        tyg_block_set(p, head);
        Loc(f->line);
        auto cv = tyg_load(p, s, tyg_at_local(ctr, 0), 0);
        auto hv = tyg_load(p, s, tyg_at_local(hiv, 0), 0);
        tyg_branch(p, tyg_cmp(p, TgModule::IsSigned(ct) ? TYG_SLT : TYG_ULT, cv, hv), body, done);
        tyg_block_set(p, body);
        if (f->vdef)
            tyg_store(p, M.Scal(f->vdef->type), tyg_at_local(locals[f->vdef], 0),
                      Conv(cv, ct, f->vdef->type, f->line), 0);
        if (f->idxdef)
            tyg_store(p, M.Scal(f->idxdef->type), tyg_at_local(locals[f->idxdef], 0),
                      Conv(tyg_load(p, TYG_I64, tyg_at_local(ix, 0), 0), M.ast.inttypes[IS_I64],
                           f->idxdef->type, f->line), 0);
        loops.push_back({ done, step });
        GenBlock(f->body, true);
        loops.pop_back();
        Jump(step);
        tyg_block_set(p, step);
        Loc(f->line);
        auto c2 = tyg_load(p, s, tyg_at_local(ctr, 0), 0);
        tyg_store(p, s, tyg_at_local(ctr, 0), tyg_bin(p, TYG_ADD, c2, tyg_iconst(p, s, 1)), 0);
        if (f->idxdef) {
            auto i2 = tyg_load(p, TYG_I64, tyg_at_local(ix, 0), 0);
            tyg_store(p, TYG_I64, tyg_at_local(ix, 0), tyg_bin(p, TYG_ADD, i2, tyg_iconst(p, TYG_I64, 1)), 0);
        }
        tyg_jump(p, head);
        tyg_block_set(p, done);
        tyg_scope_end(p);
    }

    // for x in a / for &x in a / for x, i in a over a fixed array or a
    // slice (ForLoop::CgStmt): the length is read again at the top of every
    // iteration, and the elements where the path to them leads then (a
    // body may re-point a slice on it, §6.5); an iterated expression that
    // is not such a path runs once, into a temporary. The binding is the
    // element's address where the checker typed it as a reference to the
    // element, else a copy of the element.
    void StmtForEach(ForLoop *f) {
        P once {};
        auto path = CheckFreePath(f->iter);
        if (!path) once = Place(f->iter);
        auto at = [&]() { return path ? Place(f->iter) : once; };
        auto gi = tyg_local_new(p, 8, 8);
        tyg_store(p, TYG_I64, tyg_at_local(gi, 0), tyg_iconst(p, TYG_I64, 0), 0);
        tyg_scope_begin(p);
        if (f->vdef) Declare(f->vdef, f->line);
        if (f->idxdef) Declare(f->idxdef, f->line);
        auto head = tyg_block_new(p), body = tyg_block_new(p), step = tyg_block_new(p),
             done = tyg_block_new(p);
        tyg_jump(p, head);
        tyg_block_set(p, head);
        Loc(f->line);
        auto v = ViewOf(at(), f->line);
        auto i = tyg_load(p, TYG_I64, tyg_at_local(gi, 0), 0);
        tyg_branch(p, tyg_cmp(p, TYG_SLT, i, v.len), body, done);
        tyg_block_set(p, body);
        auto ea = ElemAddr(v, i);
        if (f->vdef) {
            auto vt = f->vdef->type;
            P dst { tyg_at_local(locals[f->vdef], 0), vt };
            if (vt->kind == TY_REF && v.elem->kind != TY_REF) {
                if (!IsPlainRef(vt)) No(f->line, "binding kind");
                tyg_store(p, TYG_I64, dst.a, ea, 0);
            } else {
                Store(dst, Fit(Load(P { tyg_at_val(ea, 0), v.elem }, f->line), vt, f->line));
            }
        }
        if (f->idxdef)
            tyg_store(p, M.Scal(f->idxdef->type), tyg_at_local(locals[f->idxdef], 0),
                      Conv(i, M.ast.inttypes[IS_I64], f->idxdef->type, f->line), 0);
        loops.push_back({ done, step });
        GenBlock(f->body, true);
        loops.pop_back();
        Jump(step);
        tyg_block_set(p, step);
        Loc(f->line);
        auto i2 = tyg_load(p, TYG_I64, tyg_at_local(gi, 0), 0);
        tyg_store(p, TYG_I64, tyg_at_local(gi, 0), tyg_bin(p, TYG_ADD, i2, tyg_iconst(p, TYG_I64, 1)), 0);
        tyg_jump(p, head);
        tyg_block_set(p, done);
        tyg_scope_end(p);
    }

    // ---- The procedure -------------------------------------------------------

    void Emit() {
        auto &si = cg.sinfo[sp];
        auto sym = M.Sym(si.cname, TYG_SYM_FUNC | TYG_SYM_GLOBAL);
        auto sig = M.Sig(sp);
        if (!sig) No(sp->sf->line, cat("signature: ", tyg_error(m) ? tyg_error(m) : "?"));
        p = tyg_proc_begin(m, sym, sig);
        if (!p) No(sp->sf->line, cat("tyg_proc_begin: ", tyg_error(m) ? tyg_error(m) : "?"));
        auto file = M.File(sp->sf->line.fileidx);
        vector<const tyg_dtype *> pdt;
        for (auto t : sp->argtypes) pdt.push_back(M.DT(t));
        auto fdt = tyg_dt_func(m, sp->rets.empty() ? tyg_dt_void(m) : M.DT(sp->rets[0]), pdt.data(),
                               (int)pdt.size(), 0);
        tyg_debug_proc(p, string(sp->sf->qname).c_str(), file, sp->sf->line.line, fdt);
        Loc(sp->sf->line);
        for (size_t i = 0; i < sp->params.size(); i++) {
            auto vd = sp->params[i];
            auto t = sp->argtypes[i];
            auto loc = Temp(t);
            locals[vd] = loc;
            auto pv = tyg_param(p, (int)i);
            if (M.IsScalar(t)) tyg_store(p, M.Scal(t), tyg_at_local(loc, 0), pv, 0);
            else tyg_copy(p, tyg_at_local(loc, 0), tyg_at_val(pv, 0), M.Size(t));
            tyg_debug_param(p, string(vd->name).c_str(), M.DT(t), tyg_at_local(loc, 0),
                            M.File(vd->line.fileidx), vd->line.line);
        }
        for (size_t i = 1; i < sp->rets.size(); i++) {
            auto loc = tyg_local_new(p, 8, 8);
            tyg_store(p, TYG_I64, tyg_at_local(loc, 0),
                      tyg_param(p, (int)(sp->argtypes.size() + i - 1)), 0);
            outs.push_back(loc);
        }
        auto body = sp->body;
        auto x = GenBlock(body, sp->rets.empty());
        if (!Ended()) {
            if (sp->rets.empty()) {
                tyg_ret(p, 0);
            } else if (body->tail && x.t && sp->rets.size() == 1) {
                x = Fit(x, sp->rets[0], body->line);
                if (x.mem) tyg_ret_mem(p, x.a);
                else tyg_ret(p, x.v);
            } else {
                // Control cannot reach the end of a function with a result
                // (the checker's rule); the C emits GS_UNREACHABLE.
                tyg_trap(p);
            }
        }
        if (tyg_proc_end(p) != 0) {
            p = nullptr;
            No(sp->sf->line, cat("tinygen: ", tyg_error(m) ? tyg_error(m) : "?"));
        }
        p = nullptr;
    }
};

// The backend: chooses the specializations it takes as CodeGen asks, then
// compiles those into one object.
struct TinyGenBackend : CodeGen::SpecBackend {
    Ast &ast;
    bool checks;
    string source;
    vector<FnSpec *> taken;
    vector<pair<FnSpec *, string>> left;   // Specializations left to C, and why.
    unique_ptr<TgModule> mod;
    string_view runtime_ext_text;
    // Functions left to C whatever they hold (--tinygen-skip): what tests
    // calls across the two halves with.
    vector<string> skip;

    TinyGenBackend(Ast &_ast, bool _checks, const string &_source, string_view _ext)
        : ast(_ast), checks(_checks), source(_source), runtime_ext_text(_ext) {}

    // Builds sp into the module, with the specializations in `ours`
    // callable. A construct this backend does not support anywhere in the
    // body abandons the procedure (tyg_proc_abort), and what it asked of the
    // module for the C half (shims, trampolines) is taken back, so that the
    // whole body goes to C. "" when it compiled, else why not.
    string Build(FnSpec *sp, set<FnSpec *> &relied) {
        string why;
        if (sp->sf->issimd) return "simd function";
        if (!mod->SigOk(sp, true, why)) return cat("interface: ", why);
        auto nshims = mod->shims.size(), nexterns = mod->externs.size();
        TgFn fn(*mod, sp);
        try {
            fn.Emit();
        } catch (TgUnsupported &u) {
            if (fn.p) tyg_proc_abort(fn.p);
            mod->shims.resize(nshims);
            mod->externs.resize(nexterns);
            return u.line.line > 0 ? cat(u.why, " (line ", u.line.line, ")") : u.why;
        }
        relied.insert(fn.relied.begin(), fn.relied.end());
        return "";
    }

    // The specializations this backend takes: the largest set each of
    // whose bodies compiles while calling into the set. Every candidate
    // starts in it and is built straight into the module; one that does
    // not compile leaves it. A body may call a specialization that takes
    // gs_sp only while that one is in the set: where one that left was
    // called so, the module is built again from scratch without it, until
    // nothing that was called leaves.
    set<FnSpec *> Choose(CodeGen &cg, const vector<FnSpec *> &specs) override {
        set<FnSpec *> ours;
        map<FnSpec *, string> why;
        for (auto sp : specs) {
            if (std::find(skip.begin(), skip.end(), sp->sf->qname) != skip.end())
                why[sp] = "--tinygen-skip";
            else
                ours.insert(sp);
        }
        for (;;) {
            mod = make_unique<TgModule>(ast, cg, checks, source);
            if (!mod->m) {
                for (auto sp : specs) left.emplace_back(sp, "tinygen has no module for this target");
                mod.reset();
                return {};
            }
            mod->ours = &ours;
            set<FnSpec *> relied, gone;
            for (auto sp : specs) {
                if (!ours.count(sp)) continue;
                auto w = Build(sp, relied);
                if (w.empty()) continue;
                why[sp] = w;
                ours.erase(sp);
                gone.insert(sp);
            }
            auto again = false;
            for (auto sp : gone) again = again || relied.count(sp);
            if (!again) break;
        }
        for (auto sp : specs) {
            if (ours.count(sp)) taken.push_back(sp);
            else left.emplace_back(sp, why[sp]);
        }
        return ours;
    }

    // After CodeGen has made the C half: the extern fns the object calls
    // are used, so the C half declares them and their packages link.
    set<FnSpec *> declared;   // Extern fns the C half declares already.

    void Compile(CodeGen &cg) {
        if (!mod) return;
        mod->ours = &cg.foreign;
        declared.insert(cg.usedexterns.begin(), cg.usedexterns.end());
        for (auto sp : mod->externs)
            if (std::find(cg.usedexterns.begin(), cg.usedexterns.end(), sp) == cg.usedexterns.end())
                cg.usedexterns.push_back(sp);
    }

    // The C the object calls into: the shims, and a trampoline per extern
    // fn called. Appended to the C half, after everything it names.
    string Shims(CodeGen &cg) {
        if (!mod) return "";
        string s = "\n/* ---- for the tinygen object (docs/design/tinygen_backend.md) ---- */\n";
        for (auto &sh : mod->shims) s += sh.second;
        for (auto sp : mod->externs) {
            auto name = sp->sf->cname;
            if (!declared.count(sp) &&
                runtime_ext_text.find(cat(" ", name, "(")) == string_view::npos)
                s += cg.ExternProto(sp);
            auto ret = sp->rets.empty() ? string("void") : cg.CT(sp->rets[0]);
            string params, args;
            for (size_t i = 0; i < sp->argtypes.size(); i++) {
                Append(params, i ? ", " : "", cg.CT(sp->argtypes[i]), " a", i);
                Append(args, i ? ", " : "", "a", i);
            }
            Append(s, ret, " gs_tg_x_", name, "(", params.empty() ? "void" : params, ") { ",
                   sp->rets.empty() ? "" : "return ", name, "(", args, "); }\n");
        }
        return s;
    }

    bool Write(const string &path, string &err) {
        if (tyg_module_write(mod->m, path.c_str()) != 0) {
            err = tyg_error(mod->m) ? tyg_error(mod->m) : "?";
            return false;
        }
        return true;
    }
};

}  // namespace goose
#endif
