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

    static string Name(TypeExpr *t) {
        string s;
        t->Dump(s);
        return s;
    }
    static bool IsSigned(TypeExpr *t) { return t->kind == TY_INT && !IsUnsigned(t->intstorage); }

    // A type this backend holds: a machine integer, a float, a bool, a plain
    // reference to a fixed value it holds, or a fixed struct of those.
    bool TypeOk(TypeExpr *t, int depth = 0) {
        if (!t || depth > 32) return false;
        switch (t->kind) {
            case TY_INT: return t->intstorage != IS_VARINT;
            case TY_FLT: case TY_BOOL: return true;
            case TY_REF:
                // An address of a fixed value, or of a variable-size or
                // resizable array it reads in place (RzOk): a fat reference
                // { hdr, stk } for a resizable one (gs_rref).
                return IsPlainRef(t) && ((cg.IsFix(t->ref->sub) && TypeOk(t->ref->sub, depth + 1)) ||
                                         RzOk(t->ref->sub, depth + 1));
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
                     (t->kind == TY_REF && !cg.IsFatRef(t)));
    }

    // A variable-size or resizable array of fixed elements, read and grown
    // where it lies but never a slot's value: T[] / T[uN], a length prefix
    // and then the elements, an image a uint8_t * points at (§6.1); and
    // T[>..] / T[>..<], a gs_rhdr { base, len } with the elements on a data
    // stack, whose top they end at. Held behind a reference or as a
    // by-value parameter (the C passes the image's address, or the header
    // and its stack).
    bool RzOk(TypeExpr *t, int depth = 0) {
        if (!t || t->kind != TY_ARRAY || depth > 32) return false;
        auto k = t->arr->akind;
        if (k == A_VAR) {
            if (LenWidth(t) == 0) return false;
        } else if (k != A_GROW && k != A_GROWSHRINK) {
            return false;
        }
        return cg.IsFix(t->arr->sub) && TypeOk(t->arr->sub, depth + 1);
    }
    static bool IsGrow(TypeExpr *t) {
        return t && t->kind == TY_ARRAY && (t->arr->akind == A_GROW || t->arr->akind == A_GROWSHRINK);
    }
    // A variable-size T[]: an image, its length prefix then its elements.
    static bool IsVarArr(TypeExpr *t) { return t && t->kind == TY_ARRAY && t->arr->akind == A_VAR; }
    // A T[] length prefix's bytes: u32 by default, else its unsigned
    // storage's; 0 for a varint or a signed one.
    static int LenWidth(TypeExpr *t) {
        switch (t->arr->lenstorage) {
            case -1: case IS_U32: return 4;
            case IS_U8: return 1;
            case IS_U16: return 2;
            case IS_U64: return 8;
            default: return 0;
        }
    }
    // A by-value parameter of a type RzOk holds.
    bool RzParam(TypeExpr *t) { return t->kind == TY_ARRAY && RzOk(t); }
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
        if (t->kind == TY_REF && cg.IsFatRef(t)) return PairAT();
        if (t->kind == TY_REF) return tyg_type_ptr(m);
        if (IsGrow(t)) return SliceAT();   // gs_rhdr { base, len }, as a slice.
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
    // A fat reference, gs_rref: the header's address and the stack's.
    const tyg_type *pairat = nullptr;
    const tyg_type *PairAT() {
        if (pairat) return pairat;
        tyg_field fs[2] = { { tyg_type_ptr(m), 0 }, { tyg_type_ptr(m), 8 } };
        return pairat = tyg_type_record(m, 16, 1, fs, 2);
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
            case TY_REF: {
                if (!cg.IsFatRef(t)) {
                    d = tyg_dt_pointer(m, DT(t->ref->sub));
                    break;
                }
                // gs_rref, under the reference's Goose name: the header and
                // the stack its elements grow on.
                auto rec = tyg_dt_record(m, TYG_DT_STRUCT, name.c_str(), File(0), 0);
                dtypes[name] = rec;
                tyg_dt_member(m, rec, "hdr", tyg_dt_pointer(m, DT(t->ref->sub)), 0);
                tyg_dt_member(m, rec, "stk", tyg_dt_pointer(m, StackDT()), 8);
                tyg_dt_record_complete(m, rec, 16, 1);
                return rec;
            }
            case TY_ARRAY: {
                if (t->arr->akind == A_FIXED) {
                    d = tyg_dt_array(m, DT(t->arr->sub), cg.ArrSize(t->arr));
                    break;
                }
                auto rec = tyg_dt_record(m, TYG_DT_STRUCT, name.c_str(), File(0), 0);
                dtypes[name] = rec;
                if (t->arr->akind == A_VAR) {
                    // The image: its length prefix, then the elements, as
                    // many as it says (an array of unknown size).
                    auto w = LenWidth(t);
                    auto lt = ast.inttypes[w == 1 ? IS_U8 : w == 2 ? IS_U16 : w == 8 ? IS_U64 : IS_U32];
                    tyg_dt_member(m, rec, "len", DT(lt), 0);
                    tyg_dt_member(m, rec, "data", tyg_dt_array(m, DT(t->arr->sub), -1), w);
                    tyg_dt_record_complete(m, rec, w, 1);
                } else {
                    // gs_rhdr: where the elements start, and their count.
                    tyg_dt_member(m, rec, "base", tyg_dt_pointer(m, DT(t->arr->sub)), 0);
                    tyg_dt_member(m, rec, "len", DT(ast.inttypes[IS_I64]), 8);
                    tyg_dt_record_complete(m, rec, 16, 1);
                }
                return rec;
            }
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

    // gs_stack { top }: where the next element goes.
    const tyg_dtype *stackdt = nullptr;
    const tyg_dtype *StackDT() {
        if (stackdt) return stackdt;
        auto rec = tyg_dt_record(m, TYG_DT_STRUCT, "gs_stack", File(0), 0);
        stackdt = rec;
        tyg_dt_member(m, rec, "top", tyg_dt_pointer(m, DT(ast.inttypes[IS_U8])), 0);
        tyg_dt_record_complete(m, rec, 8, 1);
        return rec;
    }

    // ---- Globals -----------------------------------------------------------

    // A global this backend reaches: a fixed value it holds, not a pool's.
    // Every global stays the C half's (gs_globals_t, initialized by
    // gs_init_globals as for any build), and is reached through an
    // accessor the C half exports, which returns its address in the
    // running thread's instance (GS_GL; Global). A `let` of a const type
    // with a compile-time initializer, which the C makes static data
    // (CodeGen::EmitGlobalDecls), is static data in this object too, read
    // where it lies (StaticGlobal).
    // A resizable or T[] global of fixed elements (RzOk) lives on a
    // dedicated stack of its own (CodeGen::gstks), which a second accessor
    // gives (StackAccessor): its header, or its image's address, in the
    // instance, and that stack beside it.
    bool GlobalOk(VarDef *d) {
        if (!d->isglobal || d->reusable || cg.PrefVar(d) || !cg.gnames.count(d)) return false;
        if (TypeOk(d->type)) return true;
        return (IsGrow(d->type) || IsVarArr(d->type)) && RzOk(d->type) && cg.gstks.count(d);
    }

    map<const VarDef *, string> accessors;
    string Accessor(VarDef *d) {
        auto it = accessors.find(d);
        auto name = it != accessors.end() ? it->second
                                          : (accessors[d] = cat("gs_tg_g", accessors.size()));
        NeedShim(name, cat("void *", name, "(void) { return (void *)&(", cg.gnames[d], "); }\n"));
        return name;
    }

    map<const VarDef *, string> stkaccessors;
    string StackAccessor(VarDef *d) {
        auto it = stkaccessors.find(d);
        auto name = it != stkaccessors.end() ? it->second
                                             : (stkaccessors[d] = cat("gs_tg_gs", stkaccessors.size()));
        NeedShim(name, cat("void *", name, "(void) { return (void *)", cg.gstks[d], "; }\n"));
        return name;
    }

    // The static copy of a global the C makes static data, with its debug
    // entry under its Goose name; TYG_NO_SYM for any other global, or one
    // whose initializer this backend does not lay out.
    map<const VarDef *, tyg_sym> gdata;
    tyg_sym StaticGlobal(VarDef *d) {
        if (auto it = gdata.find(d); it != gdata.end()) return it->second;
        auto init = cg.gstatic.find(d);
        vector<uint8_t> bytes((size_t)max<int64_t>(Size(d->type), 1), 0);
        if (init == cg.gstatic.end() || !StaticBytes(init->second, d->type, bytes, 0))
            return gdata[d] = TYG_NO_SYM;
        auto sym = tyg_symbol(m, cat("gs_tg_d", gdata.size()).c_str(), 0);
        int64_t align = 1;
        while (align < 8 && align * 2 <= (int64_t)bytes.size()) align *= 2;
        tyg_data(m, sym, TYG_SECTION_RODATA, bytes.data(), (int64_t)bytes.size(), align);
        tyg_debug_global(m, string(d->name).c_str(), DT(d->type), sym, File(d->line.fileidx),
                         d->line.line);
        return gdata[d] = sym;
    }

    // n's value of type t as the bytes at `off`: what CodeGen::StaticInitX
    // spells as a C initializer, laid out as the packed C type holds it.
    bool StaticBytes(Node *n, TypeExpr *t, vector<uint8_t> &out, int64_t off) {
        if (!n || !t) return false;
        auto put = [&](const void *v, int64_t size) {
            if (off + size > (int64_t)out.size()) return false;
            memcpy(out.data() + off, v, (size_t)size);
            return true;
        };
        if (auto id = Is<Ident>(n)) {
            auto it = id->vdef ? cg.gstatic.find(id->vdef) : cg.gstatic.end();
            if (it == cg.gstatic.end() ||
                !(cg.TEq(id->vdef->type, t) || (t->kind == TY_INT && id->vdef->type->kind == TY_INT)))
                return false;
            return StaticBytes(it->second, t, out, off);
        }
        switch (t->kind) {
            case TY_INT: {
                auto i = Is<IntLit>(n);
                if (!i || t->intstorage == IS_VARINT) return false;
                auto v = (uint64_t)i->val;   // Little-endian: the low bytes.
                return put(&v, ScalSize(Scal(t)));
            }
            case TY_FLT: {
                auto f = Is<FltLit>(n);
                if (!f) return false;
                if (Scal(t) == TYG_F32) {
                    auto v = (float)f->val;
                    return put(&v, 4);
                }
                auto v = (double)f->val;
                return put(&v, 8);
            }
            case TY_BOOL: {
                auto b = Is<BoolLit>(n);
                if (!b) return false;
                uint8_t v = b->val ? 1 : 0;
                return put(&v, 1);
            }
            case TY_STRUCT: {
                auto sl = Is<StructLit>(n);
                auto si = cg.SI(t);
                if (!sl || sl->sinst != si) return false;
                auto &lay = cg.StructLayout(si);
                for (size_t fi = 0; fi < si->st->fields.size(); fi++) {
                    if (si->st->fields[fi].ispad) continue;
                    if (!StaticBytes(sl->InitFor((int)fi), si->ftypes[fi], out, off + lay.offs[fi]))
                        return false;
                }
                return true;
            }
            case TY_ARRAY: {
                if (t->arr->akind != A_FIXED) return false;
                auto k = cg.ArrSize(t->arr);
                auto et = t->arr->sub;
                auto esz = cg.FixedSize(et);
                if (auto str = Is<StrLit>(n)) {
                    if (!IsU8(et) || (int64_t)str->val.size() != k) return false;
                    return put(str->val.data(), k);
                }
                auto al = Is<ArrayLit>(n);
                if (!al) return false;
                if (al->fillval) {
                    for (int64_t e = 0; e < k; e++)
                        if (!StaticBytes(al->fillval, et, out, off + e * esz)) return false;
                    return true;
                }
                if ((int64_t)al->elems.size() != k) return false;
                for (int64_t e = 0; e < k; e++)
                    if (!StaticBytes(al->elems[(size_t)e], et, out, off + e * esz)) return false;
                return true;
            }
            default: return false;
        }
    }

    // ---- Signatures --------------------------------------------------------

    // Whether a call to sp, or sp's own definition (`define`), has a C
    // interface this backend can match (CodeGen::SigParams): declared
    // parameters it holds, then a channel per result -- the C return value
    // for the first fixed one (cret), an out-pointer `T *gs_r<i>` for every
    // other fixed one, a destination stack and count out-parameter
    // (`gs_stack *gs_dst<i>, int64_t *gs_rl<i>`) for a resizable one, and
    // a destination stack alone for a T[] image (built there whole) --
    // then gs_sp, where the C gives it one. Free variables of a type it
    // holds (FvOk) come after the declared parameters; no `return from`
    // channel.
    bool SigOk(FnSpec *sp, bool define, string &why) {
        (void)define;
        auto &si = cg.sinfo[sp];
        if (sp->sf->isextern) { why = "extern"; return false; }
        for (auto fv : si.freevars)
            if (!FvOk(fv)) { why = cat("free variable of type ", fv->type ? Name(fv->type) : "?"); return false; }
        if (si.hasrf || cg.fromids.count(sp)) { why = "return from"; return false; }
        for (auto r : sp->rets)
            if (!RetOk(r)) { why = cat("result type ", Name(r)); return false; }
        for (size_t i = 0; i < sp->argtypes.size(); i++)
            if (cg.IsPoolParam(sp, i) || !(TypeOk(sp->argtypes[i]) || RzParam(sp->argtypes[i]))) {
                why = cat("parameter type ", Name(sp->argtypes[i]));
                return false;
            }
        return true;
    }
    // A result this backend holds: a fixed value, or a resizable array or
    // a T[] image of fixed elements, built at the caller's destination
    // stack.
    bool RetOk(TypeExpr *t) { return TypeOk(t) || ((IsGrow(t) || IsVarArr(t)) && RzOk(t)); }
    // A free variable of a nested function (§7.5), as SigParams passes it:
    // a fixed variable by its address, a T[] by its image's address, and
    // a resizable by its header's address and then its stack. Pools and
    // frame objects are not held.
    bool FvOk(const VarDef *fv) {
        if (!fv->type || fv->reusable || cg.PrefVar(fv)) return false;
        return TypeOk(fv->type) || ((IsGrow(fv->type) || IsVarArr(fv->type)) && RzOk(fv->type));
    }

    tyg_sig *Sig(FnSpec *sp) {
        if (auto it = sigs.find(sp); it != sigs.end()) return it->second;
        auto &si = cg.sinfo[sp];
        vector<const tyg_type *> ps;
        for (auto t : sp->argtypes) {
            // A by-value T[] is its image's address; a by-value T[>..] its
            // header, then its stack (CodeGen::SigParams).
            if (RzParam(t) && !IsGrow(t)) {
                ps.push_back(tyg_type_ptr(m));
                continue;
            }
            ps.push_back(AT(t));
            if (RzParam(t)) ps.push_back(tyg_type_ptr(m));
        }
        for (auto fv : si.freevars) {
            ps.push_back(tyg_type_ptr(m));
            if (IsGrow(fv->type)) ps.push_back(tyg_type_ptr(m));   // its stack
        }
        for (size_t i = 0; i < sp->rets.size(); i++) {
            if (IsGrow(sp->rets[i])) {
                ps.push_back(tyg_type_ptr(m));   // gs_dst<i>
                ps.push_back(tyg_type_ptr(m));   // gs_rl<i>
            } else if (IsVarArr(sp->rets[i])) {
                ps.push_back(tyg_type_ptr(m));   // gs_dst<i>: the image at its top
            } else if ((int)i != si.cret) {
                ps.push_back(tyg_type_ptr(m));   // gs_r<i>
            }
        }
        if (!sp->sf->isextern && si.needssp) ps.push_back(tyg_type_int(m, TYG_I64, 1));
        const tyg_type *ret = si.cret < 0 ? nullptr : AT(sp->rets[si.cret]);
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
        size_t depth;   // The stack scopes open outside the body (sscopes).
    };
    vector<LoopTargets> loops;
    // Each result's channel (CodeGen::SigParams), its parameters kept in
    // locals: an out-pointer in `a` for a fixed result other than the C
    // return value; a destination stack in `a` and the count's
    // out-parameter in `b` for a resizable one.
    struct Chan {
        tyg_local a = 0, b = 0;
    };
    vector<Chan> chans;
    // Data stacks (§6.2), as the C backend uses them: gs_sp in a local,
    // the thread's stack block (gs_stks) read once per invocation (gblk);
    // the body's own stacks are GS(gs_sp + k), k counted up as locals and
    // temporaries take one (AllocStk) and down as their scopes end, and a
    // callee is handed gs_sp + the indices in use. Every scope restores
    // the tops of the stacks it took, on every way out of it, from marks
    // saved as each was taken (CodeGen::CScope).
    tyg_local spl = 0, stksl = 0;
    int live = 0, maxown = 0;
    vector<CodeGen::StackCall> stackcalls;
    struct Mark {
        int k;
        tyg_local top;
    };
    struct SScope {
        bool stmt;
        vector<Mark> marks;
    };
    vector<SScope> sscopes;
    // The globals of the C half's instance this body names, each with the
    // local holding its address. The accessors that give them are called
    // once per invocation, in a block the entry block jumps to (gblk),
    // filled once the body is done: a body runs on one thread, whose
    // instance does not change under it.
    vector<pair<string, tyg_local>> gaddrs;   // By accessor.
    tyg_block gblk = 0, gstart = 0;
    // By-value T[] parameters, whose locals hold their images' addresses;
    // and by-value T[>..] ones, whose locals hold their headers, with the
    // local holding each one's stack.
    set<const VarDef *> indirect;
    map<const VarDef *, tyg_local> rzstk;

    TgFn(TgModule &_M, FnSpec *_sp) : M(_M), cg(_M.cg), sp(_sp), m(_M.m) {}

    [[noreturn]] static void No(Line l, const string &why) { throw TgUnsupported { l, why }; }

    // A value: a scalar in a virtual register, or an aggregate in memory.
    struct V {
        tyg_val v = 0;
        tyg_addr a {};
        bool mem = false;
        TypeExpr *t = nullptr;
        tyg_val stk = 0;   // A resizable's stack, its header at a.
    };
    // A place: memory holding a value of type t, and for a resizable
    // array the stack its elements grow on, where known (else 0).
    struct P {
        tyg_addr a;
        TypeExpr *t;
        tyg_val stk = 0;
    };

    // The elements of an array or slice and their count (ViewOf).
    struct View {
        tyg_val elems, len;
        TypeExpr *elem;
        int64_t k;   // A fixed array's length, else -1.
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

    // ---- Data stacks ---------------------------------------------------------

    tyg_val SpVal(Line l) {
        if (!spl) No(l, "data stack without gs_sp");
        return tyg_load(p, TYG_I64, tyg_at_local(spl, 0), 0);
    }
    // GS(gs_sp + k): the gs_stack (one pointer, its top) at that index.
    tyg_val Stk(int k, Line l) {
        auto sp0 = SpVal(l);
        if (!stksl) {
            stksl = tyg_local_new(p, 8, 8);
            M.NeedShim("gs_tg_stks", "void *gs_tg_stks(void) { return gs_stks; }\n");
        }
        auto base = tyg_load(p, TYG_I64, tyg_at_local(stksl, 0), 0);
        auto idx = tyg_bin(p, TYG_ADD, sp0, tyg_iconst(p, TYG_I64, k));
        return tyg_bin(p, TYG_ADD, base, tyg_bin(p, TYG_MUL, idx, tyg_iconst(p, TYG_I64, 8)));
    }
    tyg_val Top(tyg_val stk) { return tyg_load(p, TYG_I64, tyg_at_val(stk, 0), 0); }
    void SetTop(tyg_val stk, tyg_val top) { tyg_store(p, TYG_I64, tyg_at_val(stk, 0), top, 0); }

    // The next free index, for a local (in the innermost scope that is not
    // a statement's, AllocStk(forlocal) in the C) or a temporary (in the
    // statement's), its top saved to restore.
    int AllocStk(bool forlocal, Line l) {
        auto k = live++;
        maxown = std::max(maxown, live);
        auto mark = tyg_local_new(p, 8, 8);
        tyg_store(p, TYG_I64, tyg_at_local(mark, 0), Top(Stk(k, l)), 0);
        auto i = sscopes.size() - 1;
        while (forlocal && i > 0 && sscopes[i].stmt) i--;
        sscopes[i].marks.push_back({ k, mark });
        return k;
    }
    void Restore(const SScope &sc) {
        for (size_t i = sc.marks.size(); i-- > 0;)
            SetTop(Stk(sc.marks[i].k, Line {}), tyg_load(p, TYG_I64, tyg_at_local(sc.marks[i].top, 0), 0));
    }
    // The restores of every scope from `depth` in, innermost first: a
    // break's, a continue's, a return's (from 0).
    void RestoreFrom(size_t depth) {
        for (size_t i = sscopes.size(); i-- > depth;) Restore(sscopes[i]);
    }
    void OpenScope(bool stmt) { sscopes.push_back({ stmt, {} }); }
    void CloseScope() {
        auto sc = std::move(sscopes.back());
        sscopes.pop_back();
        if (!Ended()) Restore(sc);
        live -= (int)sc.marks.size();
    }

    // A call handed gs_sp + the indices in use: none into the recursive
    // cycle this function is in (§7.8; CodeGen::NoStackAcrossCycleCall),
    // which the C backend's allocation guarantees and this one's may not.
    tyg_val CalleeSp(FnSpec *callee, Line l) {
        auto cycle = [](FnSpec *s) {
            while (s->cyclelink) s = s->cyclelink;
            return s;
        };
        if (live && sp->incycle && callee->incycle && cycle(sp) == cycle(callee))
            No(l, "data stack held across a call into the recursive cycle");
        stackcalls.push_back({ callee, live, l });
        return tyg_bin(p, TYG_ADD, SpVal(l), tyg_iconst(p, TYG_I64, live));
    }

    // Appends x, of the element type et, at the top of stk to the
    // resizable whose count is at len.
    void PushRaw(tyg_val stk, tyg_addr len, TypeExpr *et, const V &x) {
        auto top = Top(stk);
        Store(P { tyg_at_val(top, 0), et }, x);
        SetTop(stk, tyg_bin(p, TYG_ADD, top, tyg_iconst(p, TYG_I64, cg.FixedSize(et))));
        tyg_store(p, TYG_I64, len, tyg_bin(p, TYG_ADD, tyg_load(p, TYG_I64, len, 0), tyg_iconst(p, TYG_I64, 1)), 0);
    }
    // Copies n elements of et from elems to the top of stk, adding them to
    // the count at len. The source may lie on stk itself, below its top.
    void CopyRun(tyg_val stk, tyg_addr len, TypeExpr *et, tyg_val elems, tyg_val n) {
        auto top = Top(stk);
        auto bytes = tyg_bin(p, TYG_MUL, n, tyg_iconst(p, TYG_I64, cg.FixedSize(et)));
        tyg_move_dyn(p, top, elems, bytes);
        SetTop(stk, tyg_bin(p, TYG_ADD, top, bytes));
        tyg_store(p, TYG_I64, len, tyg_bin(p, TYG_ADD, tyg_load(p, TYG_I64, len, 0), n), 0);
    }

    // Builds n's value, a resizable array of type t, at the top of stk,
    // its header at hdr (CodeGen::GenConstruct): a literal's elements one
    // by one, a call's at the destination it is handed, anything else (an
    // array, a slice, copy(x)) as a copy of its elements.
    void GenRz(Node *n, tyg_val stk, tyg_addr hdr, TypeExpr *t, Line l) {
        auto et = t->arr->sub;
        if (auto c = Is<Call>(n); c && c->builtin == B_COPY && c->ArgNodes().size() == 1)
            n = c->ArgNodes()[0];
        auto len = hdr;
        len.offset += 8;
        auto begin = [&] {
            tyg_store(p, TYG_I64, hdr, Top(stk), 0);
            tyg_store(p, TYG_I64, len, tyg_iconst(p, TYG_I64, 0), 0);
        };
        if (auto al = Is<ArrayLit>(n)) {
            if (al->fillval || al->capexpr) No(l, "array literal of this kind");
            begin();
            for (auto e : al->elems) {
                auto x = GenAs(e, et);
                if (Ended()) return;
                if (!M.IsScalar(et) && !x.mem) No(e->line, "aggregate value");
                PushRaw(stk, len, et, x);
            }
            return;
        }
        if (auto c = Is<Call>(n); c && c->builtin == B_STR) {
            if (!IsU8(et)) No(l, "str of this type");
            begin();
            auto an = c->ArgNodes();
            TextInto(stk, len, c, an, 0);
            return;
        }
        if (auto c = Is<Call>(n); c && c->builtin < 0 && c->spec && !c->spec->sf->isextern &&
                                   !c->spec->rets.empty() && IsGrow(c->spec->rets[0])) {
            if (cg.Mangle(c->spec->rets[0]->arr->sub) != cg.Mangle(et)) No(l, "result of another element type");
            RzDst d { stk, hdr };
            rzdst = &d;
            GenCall(c);
            rzdst = nullptr;
            return;
        }
        if (auto s = Is<StrLit>(n)) {
            if (!IsU8(et)) No(l, "string of this type");
            begin();
            CopyRun(stk, len, et, tyg_addr_of(p, tyg_at_sym(M.Bytes(s->val), 0)),
                    tyg_iconst(p, TYG_I64, (int64_t)s->val.size()));
            return;
        }
        auto pl = Place(n);
        if (Ended()) return;
        auto v = ViewOf(pl, l);
        if (cg.Mangle(v.elem) != cg.Mangle(et)) No(l, "array of another element type");
        begin();
        CopyRun(stk, len, et, v.elems, v.len);
    }
    // Where GenCall builds a call's first result, a resizable, instead of
    // a temporary of its own: a local's stack, a return destination.
    struct RzDst {
        tyg_val stk;
        tyg_addr hdr;
    };
    const RzDst *rzdst = nullptr;
    // Where GenCall builds a call's first result, a T[] image: a local's
    // stack, a return destination.
    tyg_val imgdst = 0;
    static bool IsVarArr(TypeExpr *t) { return TgModule::IsVarArr(t); }

    // A T[] length prefix (EmitPrefixPatch): n, checked against the
    // prefix's range as the C checks it, stored at pre.
    void PatchPrefix(tyg_val pre, TypeExpr *t, tyg_val n) {
        auto w = TgModule::LenWidth(t);
        auto s = w == 1 ? TYG_I8 : w == 2 ? TYG_I16 : w == 4 ? TYG_I32 : TYG_I64;
        if (w < 8) {
            auto ok = tyg_block_new(p), bad = tyg_block_new(p);
            auto max = (int64_t)((1ull << (8 * w)) - 1);
            tyg_branch(p, tyg_cmp(p, TYG_ULE, n, tyg_iconst(p, TYG_I64, max)), ok, bad);
            tyg_block_set(p, bad);
            string msg = "array length exceeds storage range";
            msg.push_back('\0');
            CallC("gs_panic", { tyg_type_ptr(m) }, { tyg_addr_of(p, tyg_at_sym(M.Bytes(msg), 0)) }, nullptr);
            tyg_unreachable(p);
            tyg_block_set(p, ok);
            n = tyg_convert(p, TYG_TRUNC, s, n);
        }
        tyg_store(p, s, tyg_at_val(pre, 0), n, 0);
    }
    // Builds a T[] image of type t at the top of stk (CodeGen's variable
    // array construction): n's value -- a call returning one is handed
    // stk as its destination, anything else is built as a resizable's
    // elements (GenRz) behind a prefix patched once they are counted --
    // or the elements of `from`, or none. Its address.
    tyg_val GenImg(Node *n, tyg_val stk, TypeExpr *t, Line l, const View *from = nullptr) {
        auto start = tyg_local_new(p, 8, 8);
        tyg_store(p, TYG_I64, tyg_at_local(start, 0), Top(stk), 0);
        auto at = [&] { return tyg_load(p, TYG_I64, tyg_at_local(start, 0), 0); };
        if (auto c = Is<Call>(n); c && c->builtin < 0 && c->spec && !c->spec->sf->isextern &&
                                   !c->spec->rets.empty() && cg.Mangle(c->spec->rets[0]) == cg.Mangle(t)) {
            imgdst = stk;
            GenCall(c);
            imgdst = 0;
            return at();
        }
        SetTop(stk, tyg_bin(p, TYG_ADD, at(), tyg_iconst(p, TYG_I64, TgModule::LenWidth(t))));
        auto hdr = tyg_at_local(tyg_local_new(p, 16, 8), 0);
        auto len = hdr;
        len.offset += 8;
        if (n) {
            GenRz(n, stk, hdr, t, l);
        } else {
            tyg_store(p, TYG_I64, hdr, Top(stk), 0);
            tyg_store(p, TYG_I64, len, tyg_iconst(p, TYG_I64, 0), 0);
            if (from) {
                if (cg.Mangle(from->elem) != cg.Mangle(t->arr->sub)) No(l, "array of another element type");
                CopyRun(stk, len, t->arr->sub, from->elems, from->len);
            }
        }
        if (Ended()) return at();
        PatchPrefix(at(), t, tyg_load(p, TYG_I64, len, 0));
        return at();
    }

    // A resizable temporary: a stack of the statement's, its header.
    V RzTemp(TypeExpr *t, Line l) {
        V x;
        x.t = t;
        x.mem = true;
        x.stk = Stk(AllocStk(false, l), l);
        x.a = tyg_at_local(tyg_local_new(p, 16, 8), 0);
        return x;
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
        while (IsPlainRef(x.t) && want->kind != TY_REF) x = Load(RefTarget(x), l);
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
        if (!M.TypeOk(pl.t) && M.RzOk(pl.t)) {
            // A T[] or a resizable is read where it lies (its uses copy it).
            x.mem = true;
            x.a = pl.a;
            x.stk = pl.stk;
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
        if (!M.IsScalar(pl.t) && !M.TypeOk(pl.t)) No(Line {}, cat("store of a ", M.Name(pl.t)));
        if (M.IsScalar(pl.t)) tyg_store(p, M.Scal(pl.t), pl.a, x.v, 0);
        else tyg_move(p, pl.a, x.a, M.Size(pl.t));
    }

    // ---- Places --------------------------------------------------------------

    // What the reference at pl refers to: a fat reference's header, with
    // its stack.
    P Step(const P &pl) {
        auto ptr = tyg_load(p, TYG_I64, pl.a, 0);
        P r { tyg_at_val(ptr, 0), pl.t->ref->sub };
        if (cg.IsFatRef(pl.t)) {
            auto a8 = pl.a;
            a8.offset += 8;
            r.stk = tyg_load(p, TYG_I64, a8, 0);
        }
        return r;
    }
    P Deref(P pl) {
        while (IsPlainRef(pl.t)) pl = Step(pl);
        return pl;
    }
    // What a reference value refers to.
    P RefTarget(const V &x) {
        if (x.mem) return Step(P { x.a, x.t });
        return P { tyg_at_val(x.v, 0), x.t->ref->sub };
    }

    P Place(Node *n) {
        if (auto id = Is<Ident>(n)) {
            if (!id->vdef) No(n->line, "function as a value");
            return VarPlace(id->vdef, n->line);
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
        if (auto se = Is<SliceExpr>(n)) {
            // A view, whatever array the checker typed it as.
            auto x = GenSlice(se);
            return P { x.a, x.t };
        }
        // Any other value: in memory of its own.
        auto x = Gen(n);
        if (x.mem) return P { x.a, x.t, x.stk };
        if (!x.t || x.t->kind == TY_VOID) No(n->line, "place of no value");
        auto t = Temp(x.t);
        P pl { tyg_at_local(t, 0), x.t };
        Store(pl, x);
        return pl;
    }

    // A variable: a local or parameter of this frame, one it was handed
    // (a free variable, read through its address), or a global.
    P VarPlace(VarDef *vd, Line l) {
        auto it = locals.find(vd);
        if (it == locals.end() && vd->isglobal) return GlobalPlace(vd, l);
        if (it == locals.end()) No(l, "variable from outside");
        if (indirect.count(vd)) {
            P pl { tyg_at_val(tyg_load(p, TYG_I64, tyg_at_local(it->second, 0), 0), 0), vd->type };
            // A captured resizable: its header's address, its stack.
            if (auto s = rzstk.find(vd); s != rzstk.end() && IsGrow(vd->type))
                pl.stk = tyg_load(p, TYG_I64, tyg_at_local(s->second, 0), 0);
            return pl;
        }
        P pl { tyg_at_local(it->second, 0), vd->type };
        if (auto s = rzstk.find(vd); s != rzstk.end())
            pl.stk = tyg_load(p, TYG_I64, tyg_at_local(s->second, 0), 0);
        return pl;
    }

    P GlobalPlace(VarDef *d, Line l) {
        if (!M.GlobalOk(d)) No(l, "global of this kind");
        auto s = M.TypeOk(d->type) ? M.StaticGlobal(d) : TYG_NO_SYM;
        if (s != TYG_NO_SYM) return P { tyg_at_sym(s, 0), d->type };
        // The shims now, so that an abandoned body takes them back.
        auto at = tyg_load(p, TYG_I64, tyg_at_local(AccessorLocal(M.Accessor(d)), 0), 0);
        if (IsVarArr(d->type)) {
            // A T[]: the instance holds its image's address.
            return P { tyg_at_val(tyg_load(p, TYG_I64, tyg_at_val(at, 0), 0), 0), d->type };
        }
        P pl { tyg_at_val(at, 0), d->type };
        if (IsGrow(d->type))
            pl.stk = tyg_load(p, TYG_I64, tyg_at_local(AccessorLocal(M.StackAccessor(d)), 0), 0);
        return pl;
    }
    // The local an accessor's result is kept in, read once per invocation.
    tyg_local AccessorLocal(const string &name) {
        for (auto &[g, at] : gaddrs)
            if (g == name) return at;
        auto loc = tyg_local_new(p, 8, 8);
        gaddrs.emplace_back(name, loc);
        return loc;
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
        if (t && t->kind == TY_REF && cg.IsFatRef(t) && n->exprtype && IsGrow(n->exprtype)) {
            // A resizable where a reference to one is wanted: gs_rref, its
            // header's address and its stack.
            auto pl = Deref(Place(n));
            if (Ended()) return Dummy(t);
            if (!pl.stk) No(n->line, "reference to an array of this kind");
            auto tmp = tyg_local_new(p, 16, 8);
            tyg_store(p, TYG_I64, tyg_at_local(tmp, 0), tyg_addr_of(p, pl.a), 0);
            tyg_store(p, TYG_I64, tyg_at_local(tmp, 8), pl.stk, 0);
            V x;
            x.t = t;
            x.mem = true;
            x.a = tyg_at_local(tmp, 0);
            return x;
        }
        if (t && IsPlainRef(t) && n->exprtype && IsVarArr(n->exprtype) && M.RzOk(t->ref->sub)) {
            // A T[] where a reference to one is wanted: its image's address.
            auto pl = Deref(Place(n));
            V x;
            x.t = t;
            x.v = Ended() ? tyg_iconst(p, TYG_I64, 0) : tyg_addr_of(p, pl.a);
            return x;
        }
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
            if (et && et->kind == TY_REF && cg.IsFatRef(et)) {
                // gs_rref: the header's address and its stack.
                auto pl = Deref(Place(u->child));
                if (!IsGrow(pl.t) || !pl.stk) No(u->line, "reference to an array of this kind");
                auto tmp = tyg_local_new(p, 16, 8);
                tyg_store(p, TYG_I64, tyg_at_local(tmp, 0), tyg_addr_of(p, pl.a), 0);
                tyg_store(p, TYG_I64, tyg_at_local(tmp, 8), pl.stk, 0);
                V x;
                x.t = et;
                x.mem = true;
                x.a = tyg_at_local(tmp, 0);
                return x;
            }
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
            OpenScope(true);   // Its temporaries exist only when it runs.
            auto r = Truth(b->right);
            CloseScope();
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
        if (op == T_EQ || op == T_NEQ) {
            auto arr = [](TypeExpr *t) {
                while (t && IsPlainRef(t)) t = t->ref->sub;
                return t && (t->kind == TY_SLICE || t->kind == TY_ARRAY) ? t : nullptr;
            };
            if (arr(b->left->exprtype) && arr(b->right->exprtype)) return GenArrayEq(b);
        }
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
    // a == b of arrays or slices (GenRangeEq, §4.5): the lengths, then
    // the elements' bytes where that is their equality (integers, bools).
    V GenArrayEq(Binary *b) {
        auto l = ViewOf(Place(b->left), b->line);
        auto r = ViewOf(Place(b->right), b->line);
        auto et = l.elem;
        if (cg.Mangle(et) != cg.Mangle(r.elem) || !(et->kind == TY_INT || et->kind == TY_BOOL) ||
            et->intstorage == IS_VARINT)
            No(b->line, "array comparison of this element type");
        auto res = tyg_local_new(p, 1, 1);
        auto same = tyg_cmp(p, TYG_EQ, l.len, r.len);
        tyg_store(p, TYG_I8, tyg_at_local(res, 0), same, 0);
        auto cmp = tyg_block_new(p), done = tyg_block_new(p);
        tyg_branch(p, same, cmp, done);
        tyg_block_set(p, cmp);
        auto bytes = tyg_bin(p, TYG_MUL, l.len, tyg_iconst(p, TYG_I64, cg.FixedSize(et)));
        auto d = CallC("memcmp", { tyg_type_ptr(m), tyg_type_ptr(m), IT(TYG_I64, false) },
                       { l.elems, r.elems, bytes }, IT(TYG_I32));
        tyg_store(p, TYG_I8, tyg_at_local(res, 0), tyg_cmp(p, TYG_EQ, d, tyg_iconst(p, TYG_I32, 0)), 0);
        tyg_jump(p, done);
        tyg_block_set(p, done);
        V x;
        x.t = M.ast.booltype;
        x.v = tyg_load(p, TYG_I8, tyg_at_local(res, 0), 0);
        if (b->op == T_NEQ) x.v = tyg_cmp(p, TYG_EQ, x.v, tyg_iconst(p, TYG_I8, 0));
        return x;
    }

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
    View ViewOf(P pl, Line l) {
        pl = Deref(pl);
        if (pl.t->kind == TY_ARRAY && pl.t->arr->akind == A_FIXED && M.TypeOk(pl.t)) {
            auto k = cg.ArrSize(pl.t->arr);
            return { tyg_addr_of(p, pl.a), tyg_iconst(p, TYG_I64, k), pl.t->arr->sub, k };
        }
        if ((pl.t->kind == TY_SLICE && M.TypeOk(pl.t)) || (IsGrow(pl.t) && M.RzOk(pl.t))) {
            // { data, len }, and gs_rhdr { base, len } alike.
            auto a8 = pl.a;
            a8.offset += 8;
            auto elem = pl.t->kind == TY_SLICE ? pl.t->sub : pl.t->arr->sub;
            return { tyg_load(p, TYG_I64, pl.a, 0), tyg_load(p, TYG_I64, a8, 0), elem, -1 };
        }
        if (IsArrayKind(pl.t, A_VAR) && M.RzOk(pl.t)) {
            // A T[] image: the length prefix, the elements behind it.
            auto w = TgModule::LenWidth(pl.t);
            auto s = w == 1 ? TYG_I8 : w == 2 ? TYG_I16 : w == 4 ? TYG_I32 : TYG_I64;
            auto n = tyg_load(p, s, pl.a, 0);
            if (s != TYG_I64) n = tyg_convert(p, TYG_ZEXT, TYG_I64, n);
            auto base = tyg_addr_of(p, pl.a);
            return { tyg_bin(p, TYG_ADD, base, tyg_iconst(p, TYG_I64, w)), n, pl.t->arr->sub, -1 };
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
        if (!ot || !(M.TypeOk(ot) || M.RzOk(ot)) || (ot->kind != TY_ARRAY && ot->kind != TY_SLICE))
            No(n->line, "len of this type");
        tyg_val len;
        if (ot->kind == TY_ARRAY && ot->arr->akind == A_FIXED) {
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
            if (x.t && IsPlainRef(x.t)) pl = Deref(RefTarget(x));
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
        // A slice the checker typed as the array it builds (a T[] result)
        // is a view all the same, which that array is built from.
        if (t && t->kind == TY_ARRAY && t->arr->akind != A_FIXED) t = M.ast.SliceOf(t->arr->sub, se->line);
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
        if (!M.TypeOk(pt)) No(n->line, "argument of this type");
        tyg_arg a {};
        auto x = GenAs(n, pt);
        if (M.IsScalar(pt)) a.val = x.v;
        else a.mem = x.a;
        return a;
    }

    // Every result of the latest call (GenCall): the first as the call's
    // value, the others where the callee wrote them through out-pointers.
    vector<V> lastrets;

    // The call, and its C return value: rets[cret], none where cret < 0.
    V Result(FnSpec *callee, int cret, tyg_sig *sig, tyg_callee ce, vector<tyg_arg> &args) {
        V x;
        if (cret < 0) {
            tyg_call(p, sig, ce, args.data(), (int)args.size(), nullptr, nullptr);
            return x;
        }
        auto rt = callee->rets[cret];
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
        auto dst = rzdst;   // For this call only, not for its arguments'.
        rzdst = nullptr;
        auto idst = imgdst;
        imgdst = 0;
        if (c->builtin == B_PRINT) return GenPrint(c);
        if (c->builtin == B_LEN && c->ArgNodes().size() == 1) return GenLen(c->ArgNodes()[0], c);
        if (c->builtin == B_PUSH && c->ArgNodes().size() == 2) return GenPush(c);
        if (c->builtin == B_APPEND && c->ArgNodes().size() == 2) return GenAppend(c);
        if (c->builtin == B_STR) return GenStr(c);
        if (c->builtin == B_POP || c->builtin == B_POP_N || c->builtin == B_RESIZE || c->builtin == B_CLEAR)
            return GenShrink(c);
        if (c->builtin == B_ASSERT && c->ArgNodes().size() == 1) {
            auto ok = tyg_block_new(p), bad = tyg_block_new(p);
            tyg_branch(p, Truth(c->ArgNodes()[0]), ok, bad);
            tyg_block_set(p, bad);
            Abort("ASSERT", c->line);
            tyg_block_set(p, ok);
            return V {};
        }
        if (c->builtin == B_ABORT && c->ArgNodes().size() == 1) {
            auto v = ViewOf(Place(c->ArgNodes()[0]), c->line);
            if (Ended()) return V {};
            CallC("gs_abort_msg", { tyg_type_ptr(m), IT(TYG_I64), tyg_type_ptr(m), IT(TYG_I32) },
                  { v.elems, v.len, FileArg(c->line), LineArg(c->line) }, nullptr);
            return V {};
        }
        if (c->builtin == B_EXIT && c->ArgNodes().size() == 1) {
            auto x = GenAs(c->ArgNodes()[0], M.ast.inttypes[IS_I64]);
            if (Ended()) return V {};
            CallC("gs_exit", { IT(TYG_I64) }, { x.v }, nullptr);
            return V {};
        }
        if (c->builtin == B_FORMAT && c->ArgNodes().size() >= 1) return GenFormat(c);
        if (c->builtin >= 0) No(c->line, cat("builtin ", BuiltinByKind(c->builtin).name));
        if (c->fvbody) return GenFvCall(c);
        if (!c->dispatch.empty()) No(c->line, "call kind: dispatch");
        if (!c->spec) No(c->line, "call kind");
        auto callee = c->spec;
        auto an = c->ArgNodes();
        if (an.size() < callee->argtypes.size()) No(c->line, "argument count");
        an.resize(callee->argtypes.size());
        if (callee->sf->isextern) return GenExternCall(c, callee, an);
        string why;
        if (!M.SigOk(callee, false, why)) No(c->line, cat("call to ", callee->sf->name, ": ", why));
        auto &ci = cg.sinfo[callee];
        vector<tyg_arg> args;
        for (size_t i = 0; i < an.size(); i++) {
            auto pt = callee->argtypes[i];
            if (M.RzParam(pt)) {
                // A by-value resizable: a copy of its own, on a stack of the
                // statement's, as its header and its stack.
                if (!IsGrow(pt)) {
                    // A T[]: an image of its own, on a stack of the
                    // statement's, by its address.
                    auto stk = Stk(AllocStk(false, an[i]->line), an[i]->line);
                    tyg_arg a {};
                    a.val = GenImg(an[i], stk, pt, an[i]->line);
                    args.push_back(a);
                    continue;
                }
                auto x = RzTemp(pt, an[i]->line);
                GenRz(an[i], x.stk, x.a, pt, an[i]->line);
                tyg_arg h {}, k {};
                h.mem = x.a;
                k.val = x.stk;
                args.push_back(h);
                args.push_back(k);
                continue;
            }
            args.push_back(Arg(an[i], pt));
        }
        // The callee's free variables (CodeGen::EmitFvArg): what this call
        // passes for each (Call::FreeVarArg), a variable of this frame or
        // one this function was handed, by its address.
        for (auto fv : ci.freevars) {
            auto v = c->FreeVarArg(fv);
            auto pl = VarPlace(v, c->line);
            tyg_arg a {};
            a.val = tyg_addr_of(p, pl.a);
            args.push_back(a);
            if (IsGrow(v->type)) {
                if (!pl.stk) No(c->line, "free variable without its stack");
                tyg_arg k {};
                k.val = pl.stk;
                args.push_back(k);
            }
        }
        lastrets.clear();
        if (Ended()) {
            for (auto r : callee->rets) lastrets.push_back(Dummy(r));
            return Dummy(callee->rets.empty() ? nullptr : callee->rets[0]);
        }
        // A channel per result after the declared arguments (C.3): a
        // resizable's destination stack -- a temporary's, or the one this
        // call builds at -- and the count's address; a temporary's address
        // for a fixed result the C does not return.
        vector<V> rets(callee->rets.size());
        for (size_t i = 0; i < callee->rets.size(); i++) {
            auto &r = rets[i];
            r.t = callee->rets[i];
            if (IsVarArr(r.t)) {
                // A T[]: built whole at the top of a temporary's stack, or
                // of the one this call builds at.
                auto stk = i == 0 && idst ? idst : Stk(AllocStk(false, c->line), c->line);
                r.mem = true;
                r.a = tyg_at_val(Top(stk), 0);
                tyg_arg s {};
                s.val = stk;
                args.push_back(s);
                continue;
            }
            if (IsGrow(r.t)) {
                if (i == 0 && dst) {
                    r.mem = true;
                    r.stk = dst->stk;
                    r.a = dst->hdr;
                } else {
                    r = RzTemp(r.t, c->line);
                }
                auto len = r.a;
                len.offset += 8;
                tyg_store(p, TYG_I64, r.a, Top(r.stk), 0);
                tyg_store(p, TYG_I64, len, tyg_iconst(p, TYG_I64, 0), 0);
                tyg_arg s {}, n {};
                s.val = r.stk;
                n.val = tyg_addr_of(p, len);
                args.push_back(s);
                args.push_back(n);
                continue;
            }
            if ((int)i == ci.cret) continue;
            auto t = Temp(r.t);
            r.a = tyg_at_local(t, 0);
            tyg_arg a {};
            a.val = tyg_addr_of(p, r.a);
            args.push_back(a);
            if (!M.IsScalar(r.t)) r.mem = true;
        }
        if (ci.needssp) {
            tyg_arg s {};
            s.val = CalleeSp(callee, c->line);
            args.push_back(s);
        }
        auto sym = M.Sym(ci.cname, TYG_SYM_FUNC | TYG_SYM_GLOBAL);
        auto x = Result(callee, ci.cret, M.Sig(callee), tyg_direct(sym), args);
        for (size_t i = 0; i < rets.size(); i++) {
            if ((int)i == ci.cret) lastrets.push_back(x);
            else if (M.IsScalar(rets[i].t)) lastrets.push_back(Load(P { rets[i].a, rets[i].t }, c->line));
            else lastrets.push_back(rets[i]);
        }
        return lastrets.empty() ? V {} : lastrets[0];
    }

    // A call of a function value (§7.6; CodeGen::EmitFvCall): the checked
    // body instance spliced in here, its parameters ordinary locals bound
    // to the arguments in order, in a scope of its own. Its value is a
    // copy (§4.1), so that a slice of it never points into a variable the
    // body names.
    V GenFvCall(Call *c) {
        auto et = c->fvbody->exprtype;
        auto wantsval = !cg.IsVoidT(et);
        if (wantsval && !M.TypeOk(et)) No(c->line, cat("function value of type ", M.Name(et)));
        if (c->fvparams.size() > c->args.size()) No(c->line, "function value parameters");
        tyg_scope_begin(p);
        OpenScope(false);
        for (size_t i = 0; i < c->fvparams.size(); i++) {
            Declare(c->fvparams[i], c->args[i]->line);
            Init(c->fvparams[i], c->args[i], c->args[i]->line);
        }
        auto x = GenBlock(c->fvbody, !wantsval);
        if (wantsval && x.mem && !Ended()) {
            auto t = Temp(x.t);
            P pl { tyg_at_local(t, 0), x.t };
            Store(pl, x);
            x.a = pl.a;
        }
        CloseScope();
        tyg_scope_end(p);
        if (!wantsval) return V {};
        if (Ended()) return Dummy(et);
        return x;
    }

    // v.push(x) for a fixed element (EmitPush): the receiver, then the
    // element, which may grow the array itself, then its slot claimed at
    // the stack's top, where the elements end, and the count raised. Its
    // value is the element's address.
    V GenPush(Call *c) {
        auto an = c->ArgNodes();
        auto pl = Deref(Place(an[0]));
        if (!IsGrow(pl.t) || !M.RzOk(pl.t) || !pl.stk) No(c->line, "push to an array of this kind");
        auto et = pl.t->arr->sub;
        auto x = GenAs(an[1], et);
        if (Ended()) return Dummy(c->exprtype);
        if (!M.IsScalar(et) && !x.mem) No(c->line, "aggregate value");
        auto top = Top(pl.stk);
        auto la = pl.a;
        la.offset += 8;
        PushRaw(pl.stk, la, et, x);
        V r;
        r.t = c->exprtype && c->exprtype->kind == TY_REF ? c->exprtype : nullptr;
        if (r.t) r.v = top;
        return r;
    }

    // v.append(x) (EmitAppend): a call's resizable result built straight
    // at v's top, its count added; anything else holding elements (an
    // array, a slice, a string literal, copy(x)) copied there.
    V GenAppend(Call *c) {
        auto an = c->ArgNodes();
        auto pl = Deref(Place(an[0]));
        if (!IsGrow(pl.t) || !M.RzOk(pl.t) || !pl.stk) No(c->line, "append to an array of this kind");
        auto et = pl.t->arr->sub;
        auto len = pl.a;
        len.offset += 8;
        auto src = an[1];
        if (auto cc = Is<Call>(src); cc && cc->builtin == B_COPY && cc->ArgNodes().size() == 1)
            src = cc->ArgNodes()[0];
        auto st = src->exprtype;
        if (Is<Call>(src) && st && IsGrow(st) && !Is<Call>(src)->spec) No(c->line, "append of this call");
        if (auto cc = Is<Call>(src); cc && cc->builtin < 0 && st && IsGrow(st)) {
            auto tmp = tyg_local_new(p, 16, 8);
            GenRz(src, pl.stk, tyg_at_local(tmp, 0), st, c->line);
            if (Ended()) return V {};
            tyg_store(p, TYG_I64, len,
                      tyg_bin(p, TYG_ADD, tyg_load(p, TYG_I64, len, 0),
                              tyg_load(p, TYG_I64, tyg_at_local(tmp, 8), 0)), 0);
            return V {};
        }
        if (auto s = Is<StrLit>(src)) {
            if (!IsU8(et)) No(c->line, "string of this type");
            CopyRun(pl.stk, len, et, tyg_addr_of(p, tyg_at_sym(M.Bytes(s->val), 0)),
                    tyg_iconst(p, TYG_I64, (int64_t)s->val.size()));
            return V {};
        }
        // A literal is a fixed array, all of it evaluated before any of it
        // is copied, as in the C.
        auto from = Place(src);
        if (Ended()) return V {};
        auto v = ViewOf(from, c->line);
        if (cg.Mangle(v.elem) != cg.Mangle(et)) No(c->line, "append of another element type");
        CopyRun(pl.stk, len, et, v.elems, v.len);
        return V {};
    }
    static bool IsGrow(TypeExpr *t) { return TgModule::IsGrow(t); }

    // The text of an[from..] appended at the top of stk to the u8
    // resizable whose count is at len (EmitStr, EmitFormatInto), each
    // argument evaluated and written in turn: a scalar formatted by the
    // runtime's gs_fmt_* straight at the top, a u8 array or slice copied.
    // Anything else renders structurally in the C half (not yet here).
    void TextInto(tyg_val stk, tyg_addr len, Call *c, const vector<Node *> &an, size_t from) {
        if (!c->fmtspecs.empty()) No(c->line, "text with format overloads");
        for (size_t i = from; i < an.size(); i++) {
            auto a = an[i];
            auto bt = a->exprtype;
            while (bt && IsPlainRef(bt)) bt = bt->ref->sub;
            if (bt && ((bt->kind == TY_ARRAY && IsU8(bt->arr->sub)) || (bt->kind == TY_SLICE && IsU8(bt->sub)))) {
                if (auto s = Is<StrLit>(a)) {
                    CopyRun(stk, len, bt->kind == TY_ARRAY ? bt->arr->sub : bt->sub,
                            tyg_addr_of(p, tyg_at_sym(M.Bytes(s->val), 0)),
                            tyg_iconst(p, TYG_I64, (int64_t)s->val.size()));
                    continue;
                }
                auto v = ViewOf(Place(a), a->line);
                if (Ended()) return;
                CopyRun(stk, len, v.elem, v.elems, v.len);
                continue;
            }
            auto ot = cg.OperandT(a->exprtype);
            if (!ot || !M.IsScalar(ot) || ot->kind == TY_REF) No(a->line, "text of this type");
            tyg_val n;
            if (ot->kind == TY_INT && ot->intstorage == IS_U64) {
                auto x = GenAs(a, ot).v;
                if (Ended()) return;
                n = CallC("gs_fmt_u64", { tyg_type_ptr(m), IT(TYG_I64, false) }, { Top(stk), x }, IT(TYG_I64));
            } else if (ot->kind == TY_INT) {
                auto x = GenAs(a, M.ast.inttypes[IS_I64]).v;
                if (Ended()) return;
                n = CallC("gs_fmt_i64", { tyg_type_ptr(m), IT(TYG_I64) }, { Top(stk), x }, IT(TYG_I64));
            } else if (ot->kind == TY_FLT && M.Scal(ot) == TYG_F32) {
                auto x = GenAs(a, ot).v;
                if (Ended()) return;
                n = CallC("gs_fmt_f32", { tyg_type_ptr(m), tyg_type_scalar(m, TYG_F32) }, { Top(stk), x },
                          IT(TYG_I64));
            } else if (ot->kind == TY_FLT) {
                auto x = GenAs(a, ot).v;
                if (Ended()) return;
                n = CallC("gs_fmt_f64", { tyg_type_ptr(m), tyg_type_scalar(m, TYG_F64) }, { Top(stk), x },
                          IT(TYG_I64));
            } else {
                auto x = GenAs(a, ot).v;
                if (Ended()) return;
                n = CallC("gs_fmt_bool", { tyg_type_ptr(m), IT(TYG_I64) },
                          { Top(stk), tyg_convert(p, TYG_ZEXT, TYG_I64, x) }, IT(TYG_I64));
            }
            SetTop(stk, tyg_bin(p, TYG_ADD, Top(stk), n));
            tyg_store(p, TYG_I64, len, tyg_bin(p, TYG_ADD, tyg_load(p, TYG_I64, len, 0), n), 0);
        }
    }

    // gs_abort(GS_E_<what>) with the Goose file and line, through a shim
    // (the codes are the C half's enum); the block ends there.
    void Abort(const string &what, Line l) {
        auto name = cat("gs_tg_abort_", what);
        M.NeedShim(name, cat("void ", name, "(const char *f, int l) { gs_abort(GS_E_", what, ", f, l); }\n"));
        CallC(name, { tyg_type_ptr(m), IT(TYG_I32) }, { FileArg(l), LineArg(l) }, nullptr);
        tyg_unreachable(p);
    }
    // An abort unless `cond`, then on.
    void AbortUnless(tyg_val cond, const string &what, Line l) {
        auto ok = tyg_block_new(p), bad = tyg_block_new(p);
        tyg_branch(p, cond, ok, bad);
        tyg_block_set(p, bad);
        Abort(what, l);
        tyg_block_set(p, ok);
    }

    // pop, pop_n, resize and clear of a resizable (codegen_builtins.h): the
    // receiver, then the arguments, then the checks; the elements end at
    // the stack's top, which moves with the count.
    V GenShrink(Call *c) {
        auto an = c->ArgNodes();
        auto pl = Deref(Place(an[0]));
        if (!IsGrow(pl.t) || !M.RzOk(pl.t) || !pl.stk)
            No(c->line, cat(BuiltinByKind(c->builtin).name, " of an array of this kind"));
        auto et = pl.t->arr->sub;
        auto esz = cg.FixedSize(et);
        auto len = pl.a;
        len.offset += 8;
        auto i64 = [&](int64_t v) { return tyg_iconst(p, TYG_I64, v); };
        auto setlen = [&](tyg_val n) { tyg_store(p, TYG_I64, len, n, 0); };
        auto at = [&](tyg_val n) {   // The address of element n.
            return tyg_bin(p, TYG_ADD, tyg_load(p, TYG_I64, pl.a, 0), tyg_bin(p, TYG_MUL, n, i64(esz)));
        };
        switch (c->builtin) {
            case B_POP: {
                auto nl = tyg_bin(p, TYG_SUB, tyg_load(p, TYG_I64, len, 0), i64(1));
                AbortUnless(tyg_cmp(p, TYG_SGE, nl, i64(0)), "POP", c->line);
                setlen(nl);
                auto top = tyg_bin(p, TYG_SUB, Top(pl.stk), i64(esz));
                SetTop(pl.stk, top);
                return Load(P { tyg_at_val(top, 0), et }, c->line);
            }
            case B_POP_N: {
                auto nn = Index64(an[1]);
                if (Ended()) return V {};
                auto ol = tyg_load(p, TYG_I64, len, 0);
                AbortUnless(tyg_cmp(p, TYG_ULE, nn, ol), "POPN", c->line);
                setlen(tyg_bin(p, TYG_SUB, ol, nn));
                SetTop(pl.stk, tyg_bin(p, TYG_SUB, Top(pl.stk), tyg_bin(p, TYG_MUL, nn, i64(esz))));
                return V {};
            }
            case B_CLEAR:
                SetTop(pl.stk, tyg_load(p, TYG_I64, pl.a, 0));
                setlen(i64(0));
                return V {};
            default: break;
        }
        // resize(n) / resize(n, fill): shrink to n, or grow by copies of
        // fill, which is evaluated before the length is read.
        auto nn = Index64(an[1]);
        V fv;
        if (an.size() > 2) {
            fv = GenAs(an[2], et);
            if (!M.IsScalar(et) && !fv.mem && !Ended()) No(c->line, "aggregate value");
        }
        if (Ended()) return V {};
        auto nl = tyg_local_new(p, 8, 8);
        tyg_store(p, TYG_I64, tyg_at_local(nl, 0), nn, 0);
        auto n = [&] { return tyg_load(p, TYG_I64, tyg_at_local(nl, 0), 0); };
        auto ol = tyg_local_new(p, 8, 8);
        tyg_store(p, TYG_I64, tyg_at_local(ol, 0), tyg_load(p, TYG_I64, len, 0), 0);
        auto o = [&] { return tyg_load(p, TYG_I64, tyg_at_local(ol, 0), 0); };
        AbortUnless(tyg_cmp(p, TYG_SGE, n(), i64(0)), "RESIZENEG", c->line);
        auto shrink = tyg_block_new(p), notshrink = tyg_block_new(p), grow = tyg_block_new(p),
             done = tyg_block_new(p);
        tyg_branch(p, tyg_cmp(p, TYG_SLT, n(), o()), shrink, notshrink);
        tyg_block_set(p, shrink);
        setlen(n());
        SetTop(pl.stk, at(n()));
        tyg_jump(p, done);
        tyg_block_set(p, notshrink);
        tyg_branch(p, tyg_cmp(p, TYG_SGT, n(), o()), grow, done);
        tyg_block_set(p, grow);
        if (an.size() <= 2) {
            Abort("RESIZEFILL", c->line);
        } else {
            // The new slots through a cursor, the top written once after.
            auto cur = tyg_local_new(p, 8, 8);
            tyg_store(p, TYG_I64, tyg_at_local(cur, 0), Top(pl.stk), 0);
            auto head = tyg_block_new(p), body = tyg_block_new(p), out = tyg_block_new(p);
            tyg_jump(p, head);
            tyg_block_set(p, head);
            auto cv = tyg_load(p, TYG_I64, tyg_at_local(cur, 0), 0);
            tyg_branch(p, tyg_cmp(p, TYG_ULT, cv, at(n())), body, out);
            tyg_block_set(p, body);
            Store(P { tyg_at_val(cv, 0), et }, fv);
            tyg_store(p, TYG_I64, tyg_at_local(cur, 0), tyg_bin(p, TYG_ADD, cv, i64(esz)), 0);
            tyg_jump(p, head);
            tyg_block_set(p, out);
            SetTop(pl.stk, tyg_load(p, TYG_I64, tyg_at_local(cur, 0), 0));
            setlen(n());
            tyg_jump(p, done);
        }
        tyg_block_set(p, done);
        return V {};
    }

    // str(a, b, ...): a fresh u8[>..] on a stack of the statement's (or
    // where GenRz builds it); format(out, a, b, ...): appended to out.
    V GenStr(Call *c) {
        auto t = c->exprtype;
        auto slice = t && t->kind == TY_SLICE && IsU8(t->sub);
        if (!slice && (!t || !IsGrow(t) || !IsU8(t->arr->sub))) No(c->line, cat("str of type ", t ? M.Name(t) : "?"));
        auto x = RzTemp(slice ? cg.GrowU8() : t, c->line);
        GenRz(c, x.stk, x.a, x.t, c->line);
        if (!slice || Ended()) return x;
        // Where the checker typed it as the slice it is passed as: a view.
        auto len = x.a;
        len.offset += 8;
        return SliceValue(t, tyg_load(p, TYG_I64, x.a, 0), tyg_load(p, TYG_I64, len, 0));
    }
    V GenFormat(Call *c) {
        auto an = c->ArgNodes();
        auto pl = Deref(Place(an[0]));
        if (!IsGrow(pl.t) || !M.RzOk(pl.t) || !pl.stk || !IsU8(pl.t->arr->sub))
            No(c->line, "format into an array of this kind");
        auto len = pl.a;
        len.offset += 8;
        TextInto(pl.stk, len, c, an, 1);
        return V {};
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
        return Result(callee, callee->rets.empty() ? -1 : 0, M.ExternSig(callee), tyg_direct(sym), args);
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
            auto bt = t;
            while (bt && IsPlainRef(bt)) bt = bt->ref->sub;
            if (bt && ((bt->kind == TY_ARRAY && IsU8(bt->arr->sub)) || (bt->kind == TY_SLICE && IsU8(bt->sub)))) {
                // A u8 array or slice prints as its bytes (SimpleText).
                auto v = ViewOf(Place(a), a->line);
                outs.push_back({ "gs_out_bytes", { v.elems, v.len }, { tyg_type_ptr(m), IT(TYG_I64) } });
                continue;
            }
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
    // A block's value is its tail's (none in statement position); a
    // function body's tail with a result is returned (tailret).
    V GenBlock(Block *b, bool discard, bool tailret = false) {
        tyg_scope_begin(p);
        OpenScope(false);
        for (auto s : b->stmts) Stmt(s);
        V x;
        if (b->tail) {
            if (tailret) {
                Open();
                Loc(b->tail->line);
                DoReturn({ b->tail }, b->tail->line);
            } else if (discard) {
                Stmt(b->tail);
            } else {
                Open();
                Loc(b->tail->line);
                x = Gen(b->tail);
                if (x.t && (IsGrow(x.t) || IsVarArr(x.t))) No(b->tail->line, "array value of a block");
            }
        }
        CloseScope();
        tyg_scope_end(p);
        return x;
    }

    // An arm runs or not: what it takes on a stack is its own.
    V GenArm(Node *n, bool discard) {
        if (auto b = Is<Block>(n)) return GenBlock(b, discard);
        if (discard) {
            Stmt(n);
            return V {};
        }
        OpenScope(true);
        auto x = Gen(n);
        if (x.t && (IsGrow(x.t) || IsVarArr(x.t))) No(n->line, "array value of an arm");
        CloseScope();
        return x;
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

    // A statement's temporaries' stacks are free again at its end.
    void Stmt(Node *n) {
        Open();
        Loc(n->line);
        OpenScope(true);
        Stmt1(n);
        CloseScope();
    }
    void Stmt1(Node *n) {
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
                OpenScope(true);
                auto c = Truth(w->cond);
                CloseScope();
                tyg_branch(p, c, body, done);
                tyg_block_set(p, body);
                loops.push_back({ done, head, sscopes.size() });
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
                loops.push_back({ done, body, sscopes.size() });
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
                return DoReturn(r->vals, r->line);
            }
            case NK_Break: {
                auto b = static_cast<Break *>(n);
                if (b->val) No(n->line, "break with a value");
                if (loops.empty()) No(n->line, "break target");
                RestoreFrom(loops.back().depth);
                tyg_jump(p, loops.back().brk);
                return;
            }
            case NK_Continue:
                if (loops.empty()) No(n->line, "continue target");
                RestoreFrom(loops.back().depth);
                tyg_jump(p, loops.back().cnt);
                return;
            case NK_FnDecl: case NK_StructDecl: case NK_EnumDecl: case NK_AliasDecl: return;
            case NK_Block: GenBlock(static_cast<Block *>(n), true); return;
            case NK_IfExpr: GenIf(static_cast<IfExpr *>(n), true); return;
            default: Gen(n); return;
        }
    }

    // return, return x, return a, b (each result in turn, the ones the C
    // does not return written through their channels as they are made, as
    // the C does), or return f() forwarding all of a call's results (§7.1).
    // A resizable result is built at its destination stack, its count
    // written through its out-parameter. Then every scope's stacks are
    // restored, and the C return value returned.
    void DoReturn(const vector<Node *> &vals, Line l) {
        auto &rets = sp->rets;
        auto cret = cg.sinfo[sp].cret;
        if (rets.empty()) {
            for (auto v : vals) Gen(v);
            if (Ended()) return;
            RestoreFrom(0);
            tyg_ret(p, 0);
            return;
        }
        if (rets.size() == 1 && cret == 0) {
            auto x = GenAs(vals.at(0), rets[0]);
            if (Ended()) return;
            if (!M.IsScalar(rets[0]) && !x.mem) No(l, "aggregate value");
            RestoreFrom(0);
            if (x.mem) tyg_ret_mem(p, x.a);
            else tyg_ret(p, x.v);
            return;
        }
        vector<V> xs;
        if (vals.size() == 1 && rets.size() > 1) xs = GenCallAll(vals[0], rets.size());
        else if (vals.size() != rets.size()) No(l, "result count");
        P first {};
        if (cret >= 0) first = P { tyg_at_local(Temp(rets[cret]), 0), rets[cret] };
        for (size_t i = 0; i < rets.size(); i++) {
            if (IsVarArr(rets[i])) {
                // A T[]: its image built at the destination.
                auto dst = tyg_load(p, TYG_I64, tyg_at_local(chans[i].a, 0), 0);
                if (xs.empty()) {
                    GenImg(vals[i], dst, rets[i], l);
                } else {
                    auto v = ViewOf(P { xs[i].a, xs[i].t, xs[i].stk }, l);
                    GenImg(nullptr, dst, rets[i], l, &v);
                }
                if (Ended()) return;
                continue;
            }
            if (IsGrow(rets[i])) {
                auto dst = tyg_load(p, TYG_I64, tyg_at_local(chans[i].a, 0), 0);
                auto hdr = tyg_at_local(tyg_local_new(p, 16, 8), 0);
                if (xs.empty()) {
                    GenRz(vals[i], dst, hdr, rets[i], l);
                } else {
                    auto v = ViewOf(P { xs[i].a, xs[i].t, xs[i].stk }, l);
                    tyg_store(p, TYG_I64, hdr, Top(dst), 0);
                    auto len = hdr;
                    len.offset += 8;
                    tyg_store(p, TYG_I64, len, tyg_iconst(p, TYG_I64, 0), 0);
                    CopyRun(dst, len, rets[i]->arr->sub, v.elems, v.len);
                }
                if (Ended()) return;
                auto len = hdr;
                len.offset += 8;
                auto rl = tyg_load(p, TYG_I64, tyg_at_local(chans[i].b, 0), 0);
                tyg_store(p, TYG_I64, tyg_at_val(rl, 0), tyg_load(p, TYG_I64, len, 0), 0);
                continue;
            }
            auto x = xs.empty() ? GenAs(vals[i], rets[i]) : Fit(xs[i], rets[i], l);
            if (Ended()) return;
            if (!M.IsScalar(rets[i]) && !x.mem) No(l, "aggregate value");
            if ((int)i == cret) {
                Store(first, x);
                continue;
            }
            auto ptr = tyg_load(p, TYG_I64, tyg_at_local(chans[i].a, 0), 0);
            Store(P { tyg_at_val(ptr, 0), rets[i] }, x);
        }
        RestoreFrom(0);
        if (cret < 0) tyg_ret(p, 0);
        else if (M.IsScalar(rets[cret])) tyg_ret(p, tyg_load(p, M.Scal(rets[cret]), first.a, 0));
        else tyg_ret_mem(p, first.a);
    }

    void Declare(VarDef *d, Line l) {
        if (IsGrow(d->type) && M.RzOk(d->type) && !cg.PrefVar(d)) {
            // A resizable local: its header, and a stack of its own, empty.
            auto stk = Stk(AllocStk(true, l), l);
            auto hdr = tyg_local_new(p, 16, 8);
            auto sl = tyg_local_new(p, 8, 8);
            tyg_store(p, TYG_I64, tyg_at_local(sl, 0), stk, 0);
            tyg_store(p, TYG_I64, tyg_at_local(hdr, 0), Top(stk), 0);
            tyg_store(p, TYG_I64, tyg_at_local(hdr, 8), tyg_iconst(p, TYG_I64, 0), 0);
            locals[d] = hdr;
            rzstk[d] = sl;
            tyg_debug_var(p, string(d->name).c_str(), M.DT(d->type), tyg_at_local(hdr, 0),
                          M.File(d->line.fileidx), d->line.line);
            return;
        }
        if (IsVarArr(d->type) && M.RzOk(d->type) && !cg.PrefVar(d)) {
            // A T[] local: its image's address, the image on a stack of its
            // own (built by StmtVarDecl).
            auto stk = Stk(AllocStk(true, l), l);
            auto sl = tyg_local_new(p, 8, 8);
            auto loc = tyg_local_new(p, 8, 8);
            tyg_store(p, TYG_I64, tyg_at_local(sl, 0), stk, 0);
            tyg_store(p, TYG_I64, tyg_at_local(loc, 0), Top(stk), 0);
            locals[d] = loc;
            indirect.insert(d);
            rzstk[d] = sl;
            tyg_debug_var(p, string(d->name).c_str(), tyg_dt_pointer(m, M.DT(d->type)), tyg_at_local(loc, 0),
                          M.File(d->line.fileidx), d->line.line);
            return;
        }
        if (!M.TypeOk(d->type) || cg.PrefVar(d)) No(l, cat("variable type of ", d->name, ": ", M.Name(d->type)));
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
                if (IsVarArr(d->type) && !Ended()) {
                    auto v = ViewOf(P { xs[i].a, xs[i].t, xs[i].stk }, vd->line);
                    auto stk = tyg_load(p, TYG_I64, tyg_at_local(rzstk[d], 0), 0);
                    auto img = GenImg(nullptr, stk, d->type, vd->line, &v);
                    tyg_store(p, TYG_I64, tyg_at_local(locals[d], 0), img, 0);
                    continue;
                }
                if (IsGrow(d->type) && !Ended()) {
                    // A resizable result, copied from its temporary onto
                    // the local's own stack.
                    if (!IsGrow(xs[i].t) || cg.Mangle(xs[i].t->arr->sub) != cg.Mangle(d->type->arr->sub))
                        No(vd->line, "array result of another type");
                    auto v = ViewOf(P { xs[i].a, xs[i].t, xs[i].stk }, vd->line);
                    auto stk = tyg_load(p, TYG_I64, tyg_at_local(rzstk[d], 0), 0);
                    CopyRun(stk, tyg_at_local(locals[d], 8), d->type->arr->sub, v.elems, v.len);
                    continue;
                }
                auto x = Fit(xs[i], d->type, vd->line);
                if (!M.IsScalar(d->type) && !x.mem && !Ended()) No(vd->line, "aggregate value");
                Store(P { tyg_at_local(locals[d], 0), d->type }, x);
            }
            return;
        }
        if (!vd->inits.empty() && vd->inits.size() != vd->defs.size()) No(vd->line, "declaration kind");
        // Every local first, so that a resizable one's stack lies under
        // what its initializer takes for itself.
        for (auto d : vd->defs) Declare(d, vd->line);
        for (size_t i = 0; i < vd->defs.size(); i++)
            Init(vd->defs[i], vd->inits.empty() ? nullptr : vd->inits[i], vd->line);
    }
    // A declared local's first value: init's, or zero bytes (an empty
    // array) where there is none.
    void Init(VarDef *d, Node *init, Line l) {
        P pl { tyg_at_local(locals[d], 0), d->type };
        if (IsVarArr(d->type)) {
            // Built on its own stack.
            auto stk = tyg_load(p, TYG_I64, tyg_at_local(rzstk[d], 0), 0);
            auto img = GenImg(init, stk, d->type, l);
            if (!Ended()) tyg_store(p, TYG_I64, pl.a, img, 0);
            return;
        }
        if (IsGrow(d->type)) {
            // Built in place, on its own stack.
            if (init) GenRz(init, tyg_load(p, TYG_I64, tyg_at_local(rzstk[d], 0), 0), pl.a, d->type, l);
            return;
        }
        if (!init) tyg_zero(p, pl.a, M.Size(d->type));
        else Into(pl, init);
    }

    void StmtAssign(Assign *a) {
        auto pl = Place(a->lval);
        if (a->pointee) pl = Deref(pl);
        if (a->op == T_DOTASSIGN) No(a->line, "rebind");
        if (a->op == T_ASSIGN && IsGrow(pl.t)) {
            // A whole resizable (Assign::CgStmt): its stack's top back to
            // where its elements start, then the new ones built there.
            if (!M.RzOk(pl.t) || !pl.stk) No(a->line, "assignment to an array of this kind");
            SetTop(pl.stk, tyg_load(p, TYG_I64, pl.a, 0));
            GenRz(a->rhs, pl.stk, pl.a, pl.t, a->line);
            return;
        }
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
        loops.push_back({ done, step, sscopes.size() });
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
        loops.push_back({ done, step, sscopes.size() });
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
        for (auto t : sp->argtypes) {
            if (M.RzParam(t) && !IsGrow(t)) pdt.push_back(tyg_dt_pointer(m, M.DT(t)));
            else pdt.push_back(M.DT(t));
            if (M.RzParam(t) && IsGrow(t)) pdt.push_back(tyg_dt_pointer(m, M.StackDT()));
        }
        for (auto fv : cg.sinfo[sp].freevars) {
            pdt.push_back(tyg_dt_pointer(m, M.DT(fv->type)));
            if (IsGrow(fv->type)) pdt.push_back(tyg_dt_pointer(m, M.StackDT()));
        }
        auto cret = cg.sinfo[sp].cret;
        auto fdt = tyg_dt_func(m, cret < 0 ? tyg_dt_void(m) : M.DT(sp->rets[cret]), pdt.data(),
                               (int)pdt.size(), 0);
        tyg_debug_proc(p, string(sp->sf->qname).c_str(), file, sp->sf->line.line, fdt);
        Loc(sp->sf->line);
        int pi = 0;   // The C parameter: a by-value T[>..] takes two.
        for (size_t i = 0; i < sp->params.size(); i++) {
            auto vd = sp->params[i];
            auto t = sp->argtypes[i];
            auto pv = tyg_param(p, pi++);
            if (M.RzParam(t) && !IsGrow(t)) {
                // A by-value T[]: its image's address, the image a copy the
                // caller made.
                auto loc = tyg_local_new(p, 8, 8);
                locals[vd] = loc;
                indirect.insert(vd);
                tyg_store(p, TYG_I64, tyg_at_local(loc, 0), pv, 0);
                tyg_debug_param(p, string(vd->name).c_str(), tyg_dt_pointer(m, M.DT(t)),
                                tyg_at_local(loc, 0), M.File(vd->line.fileidx), vd->line.line);
                continue;
            }
            auto loc = M.RzParam(t) ? tyg_local_new(p, 16, 8) : Temp(t);
            locals[vd] = loc;
            if (M.IsScalar(t)) tyg_store(p, M.Scal(t), tyg_at_local(loc, 0), pv, 0);
            else tyg_copy(p, tyg_at_local(loc, 0), tyg_at_val(pv, 0), M.RzParam(t) ? 16 : M.Size(t));
            tyg_debug_param(p, string(vd->name).c_str(), M.DT(t), tyg_at_local(loc, 0),
                            M.File(vd->line.fileidx), vd->line.line);
            if (M.RzParam(t)) {
                // A by-value T[>..]: its header, then its stack.
                auto sl = tyg_local_new(p, 8, 8);
                tyg_store(p, TYG_I64, tyg_at_local(sl, 0), tyg_param(p, pi++), 0);
                rzstk[vd] = sl;
            }
        }
        auto param = [&] {
            auto loc = tyg_local_new(p, 8, 8);
            tyg_store(p, TYG_I64, tyg_at_local(loc, 0), tyg_param(p, pi++), 0);
            return loc;
        };
        // Free variables (§7.5), each the address of the variable in the
        // frame it lives in (a T[]'s image, a resizable's header, with its
        // stack), read through as a by-value T[] parameter is. The debugger
        // sees each as a pointer under the variable's name, as the C has it.
        for (auto fv : cg.sinfo[sp].freevars) {
            auto loc = param();
            locals[fv] = loc;
            indirect.insert(fv);
            tyg_debug_param(p, string(fv->name).c_str(), tyg_dt_pointer(m, M.DT(fv->type)),
                            tyg_at_local(loc, 0), M.File(fv->line.fileidx), fv->line.line);
            if (IsGrow(fv->type)) rzstk[fv] = param();
        }
        chans.resize(sp->rets.size());
        for (size_t i = 0; i < sp->rets.size(); i++) {
            if (IsGrow(sp->rets[i])) {
                chans[i].a = param();
                chans[i].b = param();
            } else if (TgModule::IsVarArr(sp->rets[i]) || (int)i != cret) {
                chans[i].a = param();
            }
        }
        if (cg.sinfo[sp].needssp) spl = param();
        gblk = tyg_block_new(p);
        gstart = tyg_block_new(p);
        tyg_jump(p, gblk);
        tyg_block_set(p, gstart);
        // The tail of a body with a result is returned as the C returns it
        // (EmitSpec): unless it is an `if` without an `else`, or no value.
        auto body = sp->body;
        auto tail = body->tail;
        auto tailret = !sp->rets.empty() && tail && tail->exprtype && tail->exprtype->kind != TY_VOID &&
                       !(Is<IfExpr>(tail) && !Is<IfExpr>(tail)->elseb) && !Is<Return>(tail) &&
                       !Is<Break>(tail) && !Is<Continue>(tail);
        GenBlock(body, true, tailret);
        if (!Ended()) {
            // Control cannot reach the end of a function with a result
            // (the checker's rule); the C emits GS_UNREACHABLE.
            if (sp->rets.empty()) tyg_ret(p, 0);
            else tyg_trap(p);
        }
        // The addresses of the globals the body names, and the stack block.
        tyg_block_set(p, gblk);
        Loc(sp->sf->line);
        if (stksl) tyg_store(p, TYG_I64, tyg_at_local(stksl, 0), CallC("gs_tg_stks", {}, {}, tyg_type_ptr(m)), 0);
        for (auto &[acc, loc] : gaddrs)
            tyg_store(p, TYG_I64, tyg_at_local(loc, 0), CallC(acc, {}, {}, tyg_type_ptr(m)), 0);
        tyg_jump(p, gstart);
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

    // Builds sp into the module. A construct this backend does not
    // support anywhere in the body abandons the procedure (tyg_proc_abort),
    // and what it asked of the module for the C half (shims, trampolines) is
    // taken back, so that the whole body goes to C. "" when it compiled,
    // else why not. A body that compiled tells the C half's stack
    // accounting (codegen_stacks.h) the stacks it owns and what it hands
    // its callees, as one the C backend emits does.
    string Build(CodeGen &cg, FnSpec *sp) {
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
        cg.stackown[sp] = fn.maxown;
        cg.stackcalls[sp] = fn.stackcalls;
        return "";
    }

    // The specializations this backend takes: every one whose body
    // compiles. Each is built straight into the module; one that does not
    // compile is left to C. The two halves call each other with gs_sp as
    // two C functions do, so what one takes does not depend on the other.
    set<FnSpec *> Choose(CodeGen &cg, const vector<FnSpec *> &specs) override {
        set<FnSpec *> ours;
        mod = make_unique<TgModule>(ast, cg, checks, source);
        if (!mod->m) {
            for (auto sp : specs) left.emplace_back(sp, "tinygen has no module for this target");
            mod.reset();
            return {};
        }
        for (auto sp : specs) {
            string w = std::find(skip.begin(), skip.end(), sp->sf->qname) != skip.end()
                           ? string("--tinygen-skip")
                           : Build(cg, sp);
            if (w.empty()) {
                ours.insert(sp);
                taken.push_back(sp);
            } else {
                left.emplace_back(sp, w);
            }
        }
        return ours;
    }

    // After CodeGen has made the C half: the extern fns the object calls
    // are used, so the C half declares them and their packages link.
    set<FnSpec *> declared;   // Extern fns the C half declares already.

    void Compile(CodeGen &cg) {
        if (!mod) return;
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
