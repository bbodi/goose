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

// The shims that depend on the runtime alone, not on the program: the
// runtime.h helpers behind checked arithmetic, the `as` checks and the
// aborts, and the thread-locals the runtime keeps per thread program
// (gs_stks, gs_rf). The C half defines the ones a program uses; a whole
// program build links them all from the support object instead
// (TgSupportSource). "" for any other name.
inline string TgFixedShim(const string &name) {
    static const char *ops[] = { "add", "sub", "mul", "neg", "div", "mod" };
    static const IntStorage ints[] = { IS_I8, IS_I16, IS_I32, IS_I64, IS_U8, IS_U16, IS_U32, IS_U64 };
    for (auto op : ops)
        for (auto is : ints) {
            auto sfx = string(CodeGen::IntSfx(is));
            if (name != cat("gs_tg_", op, "_", sfx)) continue;
            auto ct = string(CodeGen::IntCT(is));
            if (string(op) == "neg")
                return cat(ct, " ", name, "(", ct, " a, const char *f, int l) { return gs_", op, "_",
                           sfx, "(a, f, l); }\n");
            return cat(ct, " ", name, "(", ct, " a, ", ct, " b, const char *f, int l) { return gs_", op,
                       "_", sfx, "(a, b, f, l); }\n");
        }
    if (name == "gs_tg_stks") return "void *gs_tg_stks(void) { return gs_stks; }\n";
    if (name == "gs_tg_stack_init")
        return "void gs_tg_stack_init(void *s) { gs_stack_init((gs_stack *)s); }\n";
    if (name == "gs_tg_rf") return "void *gs_tg_rf(void) { return &gs_rf; }\n";
    if (name == "gs_tg_rfpanic")
        return "void gs_tg_rfpanic(void) { gs_panic(\"unexpected long-distance return\"); }\n";
    if (name == "gs_tg_f2iwrap") return "int64_t gs_tg_f2iwrap(double d) { return gs_f2iwrap(d); }\n";
    if (name == "gs_tg_f2u")
        return "uint64_t gs_tg_f2u(double d, int s, const char *f, int l) "
               "{ (void)s; (void)f; (void)l; return GS_F2U(d, s, f, l); }\n";
    if (name == "gs_tg_f2i")
        return "int64_t gs_tg_f2i(double d, int s, int64_t lo, int64_t hi, const char *t, "
               "const char *f, int l) { (void)s; (void)lo; (void)hi; (void)t; (void)f; "
               "(void)l; return GS_F2I(d, s, lo, hi, t, f, l); }\n";
    if (name == "gs_tg_range_u")
        return "uint64_t gs_tg_range_u(uint64_t v, uint64_t hi, const char *t, const char *f, "
               "int l) { (void)hi; (void)t; (void)f; (void)l; "
               "return GS_RANGE_U(v, hi, t, f, l); }\n";
    if (name == "gs_tg_range")
        return "int64_t gs_tg_range(int64_t v, int64_t lo, int64_t hi, const char *t, "
               "const char *f, int l) { (void)lo; (void)hi; (void)t; (void)f; (void)l; "
               "return GS_RANGE(v, lo, hi, t, f, l); }\n";
    if (name == "gs_tg_slicefail")
        return "void gs_tg_slicefail(const char *f, int l) { gs_abort(GS_E_SLICE, f, l); }\n";
    if (name.rfind("gs_tg_abort_", 0) == 0) {
        auto what = name.substr(12);
        return cat("void ", name, "(const char *f, int l) { gs_abort(GS_E_", what, ", f, l); }\n");
    }
    return "";
}

// The extern fns of the runtime that runtime_ext.h defines as static
// functions in the program's own C (or under asm labels): a whole program
// build has no C half to hold their trampolines, so the support object
// holds one for each, as the C half would write it (TinyGenBackend::Shims).
struct TgSupportExtern {
    const char *name, *ret, *params, *args;
};
inline const vector<TgSupportExtern> &TgSupportExterns() {
    static const vector<TgSupportExtern> v = [] {
        vector<TgSupportExtern> r = {
            { "gs_bld_append", "void", "gs_rref a0, const void *a1, int64_t a2", "a0, a1, a2" },
            { "gs_find_any", "int64_t", "sl_u8 a0, const void *a1", "a0, a1" },
            { "gs_find_pair", "int64_t", "sl_u8 a0, const void *a1, int64_t a2, const void *a3",
              "a0, a1, a2, a3" },
        };
        static const char *unary[] = { "sqrt", "sin", "cos", "tan", "asin", "acos", "atan",
                                       "exp", "log", "log2", "log10" };
        static const char *binary[] = { "atan2", "pow" };
        static vector<string> names;   // Stable storage for the generated names.
        names.reserve(64);
        for (auto f : { false, true }) {
            for (auto u : unary) {
                names.push_back(cat("gs_", u, f ? "f" : ""));
                r.push_back({ names.back().c_str(), f ? "float" : "double", f ? "float a0" : "double a0",
                              "a0" });
            }
            for (auto b : binary) {
                names.push_back(cat("gs_", b, f ? "f" : ""));
                r.push_back({ names.back().c_str(), f ? "float" : "double",
                              f ? "float a0, float a1" : "double a0, double a1", "a0, a1" });
            }
        }
        return r;
    }();
    return v;
}

// What the support object defines beyond runtime.h and runtime_ext.h, which
// precede it: every program-independent shim (TgFixedShim; an abort shim
// per GS_E_ code of `runtimeh`'s), the trampolines of TgSupportExterns,
// the thread-local gs_rf a program with `return from` declares, and the
// start and end of a program whose main the tinygen object holds
// (TinyGenBackend::EmitMain), which set up and report the thread-locals
// kept here (gs_stks, gs_gl).
inline string TgSupportBody(string_view runtimeh) {
    string s = "\n/* ---- tinygen support (docs/design/tinygen_backend.md) ---- */\n"
               "static GS_TLS int32_t gs_rf;\n";
    static const char *ops[] = { "add", "sub", "mul", "neg", "div", "mod" };
    static const IntStorage ints[] = { IS_I8, IS_I16, IS_I32, IS_I64, IS_U8, IS_U16, IS_U32, IS_U64 };
    for (auto op : ops)
        for (auto is : ints) {
            // runtime.h's unsigned helpers take no file and line, and only
            // division and modulo of unsigned values go through a shim.
            if (IsUnsigned(is) && string(op) != "div" && string(op) != "mod") continue;
            s += TgFixedShim(cat("gs_tg_", op, "_", CodeGen::IntSfx(is)));
        }
    for (auto n : { "gs_tg_stks", "gs_tg_stack_init", "gs_tg_rf", "gs_tg_rfpanic", "gs_tg_f2iwrap", "gs_tg_f2u", "gs_tg_f2i",
                    "gs_tg_range_u", "gs_tg_range", "gs_tg_slicefail" })
        s += TgFixedShim(n);
    // The codes of the runtime's error enum, one per line: "    GS_E_POP,".
    set<string> codes;
    for (size_t pos = 0; (pos = runtimeh.find("\n    GS_E_", pos)) != string_view::npos;) {
        pos += 5;
        auto end = pos;
        while (end < runtimeh.size() && (isupper((unsigned char)runtimeh[end]) ||
                                         isdigit((unsigned char)runtimeh[end]) || runtimeh[end] == '_'))
            end++;
        auto code = string(runtimeh.substr(pos + 5, end - pos - 5));
        if (end < runtimeh.size() && (runtimeh[end] == ',' || runtimeh[end] == ' ') &&
            codes.insert(code).second)
            s += TgFixedShim(cat("gs_tg_abort_", code));
    }
    for (auto &x : TgSupportExterns())
        Append(s, x.ret, " gs_tg_x_", x.name, "(", x.params, ") { ",
               string(x.ret) == "void" ? "" : "return ", x.name, "(", x.args, "); }\n");
    s += "static uint8_t gs_tg_noglobals;\n"
         "void gs_tg_start(int argc, char **argv, int64_t mainstacks, int64_t mainregions,\n"
         "                 int64_t workerregions) {\n"
         "    gs_rt_init(argc, argv, mainstacks, mainregions, workerregions);\n"
         "    gs_gl = &gs_tg_noglobals;\n"
         "}\n"
         "void gs_tg_end(void) { gs_rt_stats(); }\n";
    return s;
}

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
    vector<FnSpec *> direct;    // Extern fns called directly (RuntimeApi).
    // The runtime's declarations (runtime.h, runtime_ext.h), for RuntimeApi.
    string_view rtdecls;

    // Whether the C function `name` is one the runtime object exports
    // (GS_API): a plain symbol, called directly without a trampoline.
    map<string, bool> rtapi;
    bool RuntimeApi(const string &name) {
        if (auto it = rtapi.find(name); it != rtapi.end()) return it->second;
        auto found = false;
        for (auto pos = rtdecls.find(cat(name, "(")); pos != string_view::npos && !found;
             pos = rtdecls.find(cat(name, "("), pos + 1)) {
            if (pos == 0 || (rtdecls[pos - 1] != ' ' && rtdecls[pos - 1] != '*')) continue;
            auto bol = rtdecls.rfind('\n', pos);
            bol = bol == string_view::npos ? 0 : bol + 1;
            found = rtdecls.substr(bol, 7) == "GS_API ";
        }
        return rtapi[name] = found;
    }

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
    // One of the shims that do not depend on the program (TgFixedShim).
    void NeedShim(const string &name) {
        auto text = TgFixedShim(name);
        assert(!text.empty());
        NeedShim(name, text);
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
            // A varint on its own is an i64, as the C holds it; in a bytes
            // value it is zigzag ULEB128 (FieldsOk, P::enc).
            case TY_INT: return true;
            case TY_FLT: case TY_BOOL: return true;
            case TY_REF:
                // An address of a fixed value, or of a variable-size or
                // resizable array it reads in place (RzOk): a fat reference
                // { hdr, stk } for a resizable one (gs_rref).
                // An optional one is the same address, null where empty.
                if (IsRel(t)) return depth > 0 && RelOk(t, depth);
                return (IsPlainRef(t) || OptRef(t)) &&
                       ((cg.IsFix(t->ref->sub) && TypeOk(t->ref->sub, depth + 1)) ||
                        ImgOk(t->ref->sub, depth + 1) || (IsPlainRef(t) && FoOk(t->ref->sub, depth + 1)));
            case TY_ARRAY:
                // T[k]: k elements, packed; a static-capacity T[..k], a
                // length beside them (LimOk).
                if (t->arr->akind == A_LIMITED) return LimOk(t, depth);
                return t->arr->akind == A_FIXED && cg.IsFix(t) && !cg.IsLargeFixed(t) &&
                       cg.ArrSize(t->arr) > 0 && TypeOk(t->arr->sub, depth + 1);
            case TY_SLICE:
                // { data, len }, over fixed elements (C.3), or over images
                // walked by their sizes.
                return cg.IsFix(t->sub) ? TypeOk(t->sub, depth + 1) : ImgOk(t->sub, depth + 1);
            case TY_STRUCT: {
                if (!cg.IsFix(t) || cg.IsLargeFixed(t)) return false;
                auto si = cg.SI(t);
                if (!si || EmptyLayout(si->st->fields)) return false;
                if (Reentered(t)) return true;   // Through an optional (OptRef).
                Open open { this, t };
                for (size_t i = 0; i < si->st->fields.size(); i++) {
                    if (si->st->fields[i].ispad) continue;
                    if (!TypeOk(si->ftypes[i], depth + 1)) return false;
                }
                return true;
            }
            case TY_ENUM: case TY_VARIANT: return AdtOk(t, depth);
            default: return false;
        }
    }
    // Why TypeOk refuses a type, as a class: what -v says beside the
    // type, so the reasons a sweep collects rank by what blocks them.
    string Blocker(TypeExpr *t, int depth = 0) {
        if (!t || depth > 32) return "unknown type";
        switch (t->kind) {
            case TY_INT: return "";
            case TY_FLT: case TY_BOOL: return "";
            case TY_REF:
                if (IsRel(t) && depth > 0 && RelOk(t, depth)) return "";
                if (!IsPlainRef(t) && !OptRef(t))
                    return t->ref->optional ? "optional relative reference" : "relative reference";
                if (ImgOk(t->ref->sub, depth + 1)) return "";
                return Blocker(t->ref->sub, depth + 1);
            case TY_SLICE:
                if (!cg.IsFix(t->sub)) return ImgOk(t->sub, depth + 1) ? "" : "variable-size elements";
                return Blocker(t->sub, depth + 1);
            case TY_ARRAY:
                switch (t->arr->akind) {
                    case A_LIMITED:
                        if (cg.ArrSize(t->arr) < 0) return RcOk(t, depth) ? "" : "runtime-capacity limited array";
                        if (cg.IsLargeFixed(t)) return "large fixed value";
                        if (cg.ArrSize(t->arr) == 0) return "limited array of capacity 0";
                        break;
                    case A_FIXED:
                        if (cg.IsFix(t) && cg.IsLargeFixed(t)) return "large fixed value";
                        break;
                    case A_VAR:
                        if (LenWidth(t) == 0) return "varint or signed length";
                        break;
                    default: break;
                }
                if (!cg.IsFix(t->arr->sub))
                    return VarImgOk(t, depth) || VeOk(t, depth) ? "" : "variable-size elements";
                {
                    auto b = Blocker(t->arr->sub, depth + 1);
                    if (!b.empty()) return b;
                }
                return t->arr->akind == A_FIXED || t->arr->akind == A_LIMITED ? "" : "variable-size value";
            case TY_STRUCT: {
                if (Name(t).rfind("dictionary", 0) == 0) return "dictionary";
                if (Name(t).rfind("vec", 0) == 0) return "SIMD vector";
                if (cg.IsFrameObj(t)) return FoOk(t, depth) ? "" : "frame object";
                if (!cg.IsFix(t)) {
                    if (VsOk(t, depth)) return "";
                    auto si = cg.SI(t);
                    for (size_t i = 0; i < si->st->fields.size(); i++) {
                        if (si->st->fields[i].ispad) continue;
                        auto b = Blocker(si->ftypes[i], depth + 1);
                        if (!b.empty() && b != "variable-size value") return cat("variable-size struct with ", b);
                    }
                    return "variable-size struct";
                }
                if (cg.IsLargeFixed(t)) return "large fixed value";
                auto si = cg.SI(t);
                if (!si || EmptyLayout(si->st->fields)) return "empty struct";
                if (Reentered(t)) return "";
                Open open { this, t };
                for (size_t i = 0; i < si->st->fields.size(); i++) {
                    if (si->st->fields[i].ispad) continue;
                    auto b = Blocker(si->ftypes[i], depth + 1);
                    if (!b.empty()) return cat("struct with ", b);
                }
                return "";
            }
            case TY_ENUM: case TY_VARIANT: return AdtBlocker(t, depth);
            case TY_FN: return "function type";
            default: return "unknown type";
        }
    }
    // A type's name with its Blocker class, for -v.
    string NameWhy(TypeExpr *t) {
        if (!t) return "?";
        auto b = Blocker(t);
        return b.empty() ? Name(t) : cat(Name(t), " [", b, "]");
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
    // A variable-size struct (VsOk) is an image too, read where it lies.
    bool ImgOk(TypeExpr *t, int depth = 0) {
        return RzOk(t, depth) || VsOk(t, depth) || VarImgOk(t, depth) || VmOk(t, depth) || VvOk(t, depth) ||
               RcOk(t, depth);
    }
    // A T[] whose elements are images themselves (u8[][], an array of
    // strings): its length prefix, then the elements back to back, each
    // walked by its size; never indexed (the checker forbids it).
    bool VarImgOk(TypeExpr *t, int depth = 0) {
        if (!t || t->kind != TY_ARRAY || t->arr->akind != A_VAR || depth > 32 || LenWidth(t) == 0) return false;
        auto e = t->arr->sub;
        return e && !cg.IsFix(e) && ImgOk(e, depth + 1);
    }
    // A resizable array whose elements are images (strings, variable-size
    // structs): their bytes back to back on its stack, each walked by its
    // size (CodeGen::SizeFn). The checker never lets one be indexed; it is
    // built by push and read by `for`, len and text.
    bool VeOk(TypeExpr *t, int depth = 0) {
        if (!t || t->kind != TY_ARRAY || depth > 32) return false;
        if (t->arr->akind != A_GROW && t->arr->akind != A_GROWSHRINK) return false;
        auto e = t->arr->sub;
        return e && !cg.IsFix(e) && ImgOk(e, depth + 1);
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
    // A by-value parameter of a type RzOk or VsOk holds.
    bool RzParam(TypeExpr *t) {
        return (t->kind == TY_ARRAY && (RzOk(t) || VarImgOk(t) || RcOk(t))) || VsOk(t) || VmOk(t) || VvOk(t);
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
            default: return IsRel(t) ? RelScal(t) : TYG_I64;   // A reference: an address.
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
    int64_t Size(TypeExpr *t) {
        if (IsScalar(t)) return ScalSize(Scal(t));
        // A payload-less variant as a value of its own: the gs_empty byte.
        if (t->kind == TY_VARIANT && !cg.FixedSize(t)) return 1;
        if (FoOk(t)) return FoLayout(t).tail + 16;
        return cg.FixedSize(t);
    }

    struct FieldAt {
        TypeExpr *t;
        int64_t off;
        string_view name;
    };
    vector<FieldAt> Fields(TypeExpr *t) {
        if (t->kind == TY_VARIANT) return VariantFields(t);
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
        if (IsRel(t)) return tyg_type_int(m, RelScal(t), 0);
        if (t->kind == TY_REF) return tyg_type_ptr(m);
        if (IsGrow(t)) return SliceAT();   // gs_rhdr { base, len }, as a slice.
        if (t->kind == TY_INT) return tyg_type_int(m, Scal(t), IsSigned(t));
        if (t->kind == TY_BOOL) return tyg_type_int(m, TYG_I8, 0);
        if (t->kind == TY_FLT) return tyg_type_scalar(m, Scal(t));
        if (t->kind == TY_SLICE) return SliceAT();
        auto key = cg.Mangle(t);
        if (auto it = atypes.find(key); it != atypes.end()) return it->second;
        if (IsLim(t)) return LimAT(t);
        if (t->kind == TY_ENUM) return atypes[key] = AdtAT(t);
        if (t->kind == TY_VARIANT && !cg.FixedSize(t)) {
            // struct { uint8_t gs_empty; }
            tyg_field f { tyg_type_int(m, TYG_I8, 0), 0 };
            return atypes[key] = tyg_type_record(m, 1, 1, &f, 1);
        }
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
                if (IsRel(t)) {
                    // The offset the slot holds, under the reference's name.
                    static const char *un[] = { "unsigned char", "unsigned short", "unsigned int",
                                                "unsigned long long" };
                    auto sz = ScalSize(Scal(t));
                    d = scalar(un[sz == 1 ? 0 : sz == 2 ? 1 : sz == 4 ? 2 : 3], TYG_DT_UNSIGNED);
                    break;
                }
                if (OptRef(t) && !cg.IsFatRef(t)) {
                    d = OptDT(t, name);
                    break;
                }
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
                if (IsLim(t)) return LimDT(t, rec);
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
                if (IsVs(t)) return VsDT(t, name);
                if (FoOk(t)) return FoDT(t, name);
                auto si = cg.SI(t);
                auto rec = tyg_dt_record(m, TYG_DT_STRUCT, name.c_str(), File(si->st->line.fileidx),
                                         si->st->line.line);
                dtypes[name] = rec;   // A field may refer back to it through a reference.
                for (auto &f : Fields(t))
                    tyg_dt_member(m, rec, string(f.name).c_str(), DT(f.t), f.off);
                tyg_dt_record_complete(m, rec, cg.FixedSize(t), 1);
                return rec;
            }
            case TY_ENUM: case TY_VARIANT: return IsVv(t) ? VvDT(t, name) : AdtDT(t, name);
            default: d = tyg_dt_void(m); break;
        }
        return dtypes[name] = d;
    }

    // ---- Limited arrays T[..k] -------------------------------------------------
    //
    // A static-capacity limited array (CodeGen::IsStaticLimited) is a fixed
    // value: the C half's struct { lenT len; T e[k]; }, packed, its length
    // in the smallest unsigned type that fits k (CodeGen::LenStore), then
    // k slots, the ones past the length unused. It is held wherever a
    // fixed array is: locals, fields, elements, parameters, results,
    // globals. A runtime-capacity T[..] is a variable-size value and is not.
    static bool IsLim(TypeExpr *t) { return t && t->kind == TY_ARRAY && t->arr->akind == A_LIMITED; }
    // A runtime-capacity T[..] is an image: [cap u32][len u32][cap slots].
    bool IsRc(TypeExpr *t) { return IsLim(t) && cg.ArrSize(t->arr) < 0; }
    bool RcOk(TypeExpr *t, int depth = 0) {
        return IsRc(t) && depth <= 32 && cg.IsFix(t->arr->sub) && TypeOk(t->arr->sub, depth + 1);
    }
    // A limited array this backend holds, of either kind.
    bool LimHeld(TypeExpr *t) { return TypeOk(t) || RcOk(t); }
    bool LimOk(TypeExpr *t, int depth) {
        return cg.IsStaticLimited(t) && !cg.IsLargeFixed(t) && cg.ArrSize(t->arr) > 0 &&
               TypeOk(t->arr->sub, depth + 1);
    }
    // The length field's bytes: where the slots start.
    int LimWidth(TypeExpr *t) { return IsRc(t) ? 8 : (int)CodeGen::IntSize(cg.LenStore(t->arr)); }
    tyg_scalar LimLenScal(TypeExpr *t) {
        if (IsRc(t)) return TYG_I32;
        switch (LimWidth(t)) {
            case 1: return TYG_I8;
            case 2: return TYG_I16;
            case 4: return TYG_I32;
            default: return TYG_I64;
        }
    }
    // For the ABI: a record of the length and an array of the k slots.
    const tyg_type *LimAT(TypeExpr *t) {
        auto key = cg.Mangle(t);
        if (auto it = atypes.find(key); it != atypes.end()) return it->second;
        auto w = LimWidth(t);
        tyg_field fs[2] = { { tyg_type_int(m, LimLenScal(t), 0), 0 },
                            { tyg_type_array(m, AT(t->arr->sub), cg.ArrSize(t->arr)), w } };
        return atypes[key] = tyg_type_record(m, cg.FixedSize(t), 1, fs, 2);
    }
    // For the debugger: the C half's layout under the Goose type's name,
    // `len` and the slots `e` (all k of them: tinygen has no array whose
    // count is another member, gap 3).
    const tyg_dtype *LimDT(TypeExpr *t, tyg_dtype *rec) {
        if (IsRc(t)) {
            // Its header; the slots' count is in the image.
            tyg_dt_member(m, rec, "cap", DT(ast.inttypes[IS_U32]), 0);
            tyg_dt_member(m, rec, "len", DT(ast.inttypes[IS_U32]), 4);
            tyg_dt_record_complete(m, rec, 8, 1);
            return rec;
        }
        auto w = LimWidth(t);
        auto lt = ast.inttypes[w == 1 ? IS_U8 : w == 2 ? IS_U16 : w == 4 ? IS_U32 : IS_U64];
        tyg_dt_member(m, rec, "len", DT(lt), 0);
        tyg_dt_member(m, rec, "e", tyg_dt_array(m, DT(t->arr->sub), cg.ArrSize(t->arr)), w);
        tyg_dt_record_complete(m, rec, cg.FixedSize(t), 1);
        return rec;
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

    // ---- Frame objects --------------------------------------------------------

    // A frame object (C.2, CodeGen::IsFrameObj): a struct whose last field
    // is a resizable, the C struct of its fixed fields, packed, with the
    // tail's header { base, len } behind them (EmitCFields), the tail's
    // elements on a data stack. Held where the fixed fields are ones this
    // backend holds and the tail is a resizable of fixed elements.
    struct FoLay {
        vector<int64_t> offs;   // Per fixed field.
        int64_t tail;           // The tail header's offset.
        int tailfi;
    };
    FoLay FoLayout(TypeExpr *t) {
        auto si = cg.SI(t);
        auto &fs = si->st->fields;
        auto n = fs.size();
        while (n > 0 && si->ftypes[n - 1] && cg.IsResz(si->ftypes[n - 1])) n--;
        vector<Field> pre(fs.begin(), fs.begin() + (long)n);
        vector<TypeExpr *> pret(si->ftypes.begin(), si->ftypes.begin() + (long)n);
        auto lo = cg.LayoutFields(pre, pret);
        FoLay fl;
        fl.offs.assign(lo.offs.begin(), lo.offs.end());
        fl.tail = lo.size;
        fl.tailfi = (int)n;
        return fl;
    }
    bool FoOk(TypeExpr *t, int depth = 0) {
        if (!t || t->kind != TY_STRUCT || depth > 32 || !cg.IsFrameObj(t)) return false;
        auto si = cg.SI(t);
        auto fl = FoLayout(t);
        if ((size_t)fl.tailfi + 1 != si->st->fields.size()) return false;
        auto tail = si->ftypes[(size_t)fl.tailfi];
        if (!IsGrow(tail) || !RzOk(tail, depth + 1)) return false;
        for (int i = 0; i < fl.tailfi; i++) {
            if (si->st->fields[(size_t)i].ispad) continue;
            auto ft = si->ftypes[(size_t)i];
            if (!cg.IsFix(ft) || !TypeOk(ft, depth + 1)) return false;
        }
        return true;
    }
    const tyg_dtype *FoDT(TypeExpr *t, const string &name) {
        auto si = cg.SI(t);
        auto fl = FoLayout(t);
        auto rec = tyg_dt_record(m, TYG_DT_STRUCT, name.c_str(), File(si->st->line.fileidx), si->st->line.line);
        dtypes[name] = rec;
        for (int i = 0; i <= fl.tailfi; i++) {
            auto &f = si->st->fields[(size_t)i];
            if (f.ispad) continue;
            auto off = i == fl.tailfi ? fl.tail : fl.offs[(size_t)i];
            tyg_dt_member(m, rec, string(f.name).c_str(), DT(si->ftypes[(size_t)i]), off);
        }
        tyg_dt_record_complete(m, rec, fl.tail + 16, 1);
        return rec;
    }

    // ---- Variable-size structs ----------------------------------------------

    // A struct with a variable-size field (bytes class, §6.1): one image of
    // its fields in declaration order, packed, each variable-size one (a
    // T[], or such a struct) inline at its own size, which a uint8_t *
    // points at, as a T[]'s does. Frame objects (a resizable field) are
    // another representation.
    bool IsVs(TypeExpr *t) { return t && t->kind == TY_STRUCT && !cg.IsFix(t) && !cg.IsFrameObj(t); }
    // An image: a T[] or such a struct.
    bool IsImg(TypeExpr *t) { return IsVarArr(t) || IsVs(t) || IsVm(t) || IsVv(t) || IsRc(t); }
    // One this backend holds: its fields fixed values it holds, T[]s of
    // fixed elements (RzOk), or such structs; no relative references.
    bool VsOk(TypeExpr *t, int depth = 0) {
        if (!IsVs(t) || depth > 32 || cg.SI(t)->sclass != SC_VARIABLE) return false;
        auto si = cg.SI(t);
        for (size_t i = 0; i < si->st->fields.size(); i++) {
            if (si->st->fields[i].ispad) continue;
            auto ft = si->ftypes[i];
            if (IsVarint(ft)) continue;
            if (cg.IsFix(ft) ? !TypeOk(ft, depth + 1)
                             : !((IsVarArr(ft) && (RzOk(ft, depth + 1) || VarImgOk(ft, depth + 1))) || VsOk(ft, depth + 1)))
                return false;
        }
        return true;
    }
    // The fewest bytes an image of t takes: its fields up to the first
    // variable-size one, and that one's least (a T[]'s prefix).
    int64_t VsMinSize(TypeExpr *t) {
        if (cg.IsFix(t)) return cg.FixedSize(t);
        if (IsVarint(t)) return 1;
        if (IsVarArr(t)) return LenWidth(t);
        auto si = cg.SI(t);
        int64_t off = 0;
        for (size_t i = 0; i < si->st->fields.size(); i++) {
            auto &f = si->st->fields[i];
            if (f.ispad) { off += f.padsize > 0 ? f.padsize : 0; continue; }
            if (!cg.IsFix(si->ftypes[i])) return off + VsMinSize(si->ftypes[i]);
            off += cg.FixedSize(si->ftypes[i]);
        }
        return off;
    }
    // Its debug type, under its Goose name: a record of its fields at their
    // offsets up to and including the first variable-size one (a T[]'s
    // `len` and `data`), where the offsets of those after it stop being
    // constants. A variable of it is a pointer to the record, as for a T[].
    const tyg_dtype *VsDT(TypeExpr *t, const string &name) {
        auto si = cg.SI(t);
        auto rec = tyg_dt_record(m, TYG_DT_STRUCT, name.c_str(), File(si->st->line.fileidx), si->st->line.line);
        dtypes[name] = rec;
        int64_t off = 0;
        for (size_t i = 0; i < si->st->fields.size(); i++) {
            auto &f = si->st->fields[i];
            if (f.ispad) { off += f.padsize > 0 ? f.padsize : 0; continue; }
            auto ft = si->ftypes[i];
            tyg_dt_member(m, rec, string(f.name).c_str(), DT(ft), off);
            if (!cg.IsFix(ft)) break;
            off += cg.FixedSize(ft);
        }
        tyg_dt_record_complete(m, rec, VsMinSize(t), 1);
        return rec;
    }

    // ---- ADTs ----------------------------------------------------------------
    //
    // As the C backend lays them out (CodeGen::CT, docs/implementation.md):
    // a fixed-mode ADT is the packed struct { tag; union { v_<Variant> } },
    // its tag the variant's declaration index in a u8 (a u16 past 256
    // variants), every non-empty variant's payload behind it, the size the
    // tag's plus the largest payload's; a variant type is a packed struct
    // of its payload's fields, and a payload-less one, as a value of its
    // own, C's one gs_empty byte. Variable-mode ADTs (`Shape..`) are not
    // held: each value is its own variant's size.

    EnumInst *EI(TypeExpr *t) { return t->kind == TY_VARIANT ? cg.EIVar(t) : cg.EIOf(t); }
    int VariantIdx(TypeExpr *vt) { return cg.EIVar(vt)->en->VariantIndex(vt->var->variant); }
    int64_t TagSize(TypeExpr *t) { return cg.TagSize(EI(t)->en); }
    tyg_scalar TagScal(TypeExpr *t) { return TagSize(t) == 1 ? TYG_I8 : TYG_I16; }

    // A fixed-mode ADT or a fixed variant whose payload fields it holds.
    bool AdtOk(TypeExpr *t, int depth) {
        if (t->kind == TY_ENUM && t->enu->varmode) return false;
        if (!cg.IsFix(t) || cg.IsLargeFixed(t)) return false;
        auto ei = EI(t);
        auto ok = [&](int vi) {
            for (size_t i = 0; i < ei->en->variants[vi].fields.size(); i++)
                if (!ei->en->variants[vi].fields[i].ispad && !TypeOk(ei->vftypes[vi][i], depth + 1))
                    return false;
            return true;
        };
        if (t->kind == TY_VARIANT) return ok(VariantIdx(t));
        for (size_t vi = 0; vi < ei->en->variants.size(); vi++)
            if (!ok((int)vi)) return false;
        return true;
    }
    string AdtBlocker(TypeExpr *t, int depth) {
        if (t->kind == TY_ENUM && t->enu->varmode) return VmOk(t, depth) ? "" : "variable-mode ADT";
        if (!cg.IsFix(t)) return VvOk(t, depth) ? "" : "variable-size ADT";
        if (cg.IsLargeFixed(t)) return "large fixed value";
        auto ei = EI(t);
        for (size_t vi = 0; vi < ei->en->variants.size(); vi++) {
            if (t->kind == TY_VARIANT && (int)vi != VariantIdx(t)) continue;
            for (size_t i = 0; i < ei->en->variants[vi].fields.size(); i++) {
                if (ei->en->variants[vi].fields[i].ispad) continue;
                auto b = Blocker(ei->vftypes[vi][i], depth + 1);
                // An ADT reached through its own payload is one class.
                if (!b.empty()) return b.rfind("ADT", 0) == 0 ? b : cat("ADT with ", b);
            }
        }
        return "";
    }
    vector<FieldAt> VariantFields(TypeExpr *vt) {
        auto ei = cg.EIVar(vt);
        auto vi = VariantIdx(vt);
        auto &v = ei->en->variants[vi];
        auto &lay = cg.VariantLayout(ei, vi);
        vector<FieldAt> fs;
        for (size_t i = 0; i < v.fields.size(); i++)
            if (!v.fields[i].ispad) fs.push_back({ ei->vftypes[vi][i], lay.offs[i], v.fields[i].name });
        return fs;
    }
    // The tag, and each non-empty variant's payload record behind it,
    // overlapping as the union's members do.
    const tyg_type *AdtAT(TypeExpr *t) {
        auto ei = cg.EIOf(t);
        vector<tyg_field> fs { { tyg_type_int(m, TagScal(t), 0), 0 } };
        for (size_t vi = 0; vi < ei->en->variants.size(); vi++)
            if (cg.VariantLayout(ei, (int)vi).size)
                fs.push_back({ AT(cg.VariantType(t, (int)vi)), TagSize(t) });
        return tyg_type_record(m, cg.FixedSize(t), 1, fs.data(), (int)fs.size());
    }
    // The debugger's view, the C's under Goose names: `Shape` a struct of
    // `tag`, an enum of the variants' names, and `u`, a union of the
    // payloads by variant name; `Shape.Circle` a struct of its fields.
    const tyg_dtype *AdtDT(TypeExpr *t, const string &name) {
        if (t->kind == TY_VARIANT) {
            auto fs = Fields(t);
            auto rec = tyg_dt_record(m, TYG_DT_STRUCT, name.c_str(), File(0), 0);
            dtypes[name] = rec;
            if (fs.empty()) tyg_dt_member(m, rec, "gs_empty", DT(ast.inttypes[IS_U8]), 0);
            for (auto &f : fs) tyg_dt_member(m, rec, string(f.name).c_str(), DT(f.t), f.off);
            tyg_dt_record_complete(m, rec, Size(t), 1);
            return rec;
        }
        auto ei = cg.EIOf(t);
        auto en = ei->en;
        auto rec = tyg_dt_record(m, TYG_DT_STRUCT, name.c_str(), File(en->line.fileidx), en->line.line);
        dtypes[name] = rec;
        auto tagbase = TagSize(t) == 1 ? tyg_dt_base(m, "unsigned char", 1, TYG_DT_UNSIGNED)
                                       : tyg_dt_base(m, "unsigned short", 2, TYG_DT_UNSIGNED);
        auto tag = tyg_dt_enum(m, cat(name, ".tag").c_str(), tagbase, File(en->line.fileidx), en->line.line);
        for (size_t vi = 0; vi < en->variants.size(); vi++)
            tyg_dt_enumerator(m, tag, string(en->variants[vi].name).c_str(), (int64_t)vi);
        tyg_dt_member(m, rec, "tag", tag, 0);
        auto full = IsVm(t) ? VmMaxSize(t) : cg.FixedSize(t);
        auto psize = full - TagSize(t);
        if (psize > 0) {
            auto u = tyg_dt_record(m, TYG_DT_UNION, nullptr, File(0), 0);
            for (size_t vi = 0; vi < en->variants.size(); vi++) {
                auto vt = cg.VariantType(t, (int)vi);
                if (!cg.IsFix(vt) || cg.VariantLayout(ei, (int)vi).size)
                    tyg_dt_member(m, u, string(en->variants[vi].name).c_str(), DT(vt), 0);
            }
            tyg_dt_record_complete(m, u, psize, 1);
            tyg_dt_member(m, rec, "u", u, TagSize(t));
        }
        tyg_dt_record_complete(m, rec, full, 1);
        return rec;
    }

    // ---- Variable-mode ADTs -------------------------------------------------

    // A variable-mode ADT (`Shape..`, §8.1) is a bytes value: its tag in
    // its storage integer, then the variant's payload fields, packed, each
    // value its own variant's size (CodeGen's GenStructLit for TY_ENUM).
    // Held where every variant's payload is a fixed value of fields held.
    static bool IsVm(TypeExpr *t) { return t && t->kind == TY_ENUM && t->enu->varmode; }
    static bool IsVarint(TypeExpr *t) { return t && t->kind == TY_INT && t->intstorage == IS_VARINT; }
    // A relative reference (§3.9): an offset of a fixed width in the slot,
    // from its pool's base less one where it is `in pool` (0 is null), else
    // from the slot itself. Held in a slot of an aggregate, the `in pool`
    // form only, its pointee a fixed value held; a value of one is the
    // address it decodes to, as any reference's.
    static bool IsRel(const TypeExpr *t) { return t && t->kind == TY_REF && t->ref->lenstorage >= 0; }
    bool RelOk(TypeExpr *t, int depth = 0) {
        if (!IsRel(t) || depth > 32) return false;
        auto w = t->ref->lenstorage;
        if (w != IS_U8 && w != IS_U16 && w != IS_U32 && w != IS_U64) return false;
        auto sub = t->ref->sub;
        return cg.IsFix(sub) && TypeOk(sub, depth + 1);
    }
    tyg_scalar RelScal(TypeExpr *t) {
        switch (t->ref->lenstorage) {
            case IS_U8: return TYG_I8;
            case IS_U16: return TYG_I16;
            case IS_U32: return TYG_I32;
            default: return TYG_I64;
        }
    }
    bool VmOk(TypeExpr *t, int depth = 0) {
        if (!IsVm(t) || depth > 32) return false;
        auto ei = cg.EIOf(t);
        for (size_t vi = 0; vi < ei->en->variants.size(); vi++)
            if (!FieldsOk(ei->en->variants[vi].fields, ei->vftypes[vi], depth + 1)) return false;
        return true;
    }
    // A run of fields this backend reads and builds: fixed ones it holds,
    // T[]s and variable-size structs it holds as images.
    bool FieldsOk(const vector<Field> &fields, const vector<TypeExpr *> &ftypes, int depth) {
        for (size_t i = 0; i < fields.size(); i++) {
            if (fields[i].ispad) continue;
            auto ft = ftypes[i];
            if (IsVarint(ft)) continue;
            if (cg.IsFix(ft) ? !TypeOk(ft, depth + 1)
                             : !((IsVarArr(ft) && (RzOk(ft, depth + 1) || VarImgOk(ft, depth + 1))) || VsOk(ft, depth + 1)))
                return false;
        }
        return true;
    }
    // A variant type whose payload has a variable-size field (`Msg.Text`
    // with a u8[]): an image of its fields, as a variable-size struct is.
    bool IsVv(TypeExpr *t) { return t && t->kind == TY_VARIANT && !cg.IsFix(t); }
    bool VvOk(TypeExpr *t, int depth = 0) {
        if (!IsVv(t) || depth > 32) return false;
        auto ei = cg.EIVar(t);
        auto vi = ei->en->VariantIndex(t->var->variant);
        return FieldsOk(ei->en->variants[(size_t)vi].fields, ei->vftypes[(size_t)vi], depth + 1);
    }
    const tyg_dtype *VvDT(TypeExpr *t, const string &name) {
        auto ei = cg.EIVar(t);
        auto vi = (size_t)ei->en->VariantIndex(t->var->variant);
        auto rec = tyg_dt_record(m, TYG_DT_STRUCT, name.c_str(), File(0), 0);
        dtypes[name] = rec;
        int64_t off = 0;
        for (size_t i = 0; i < ei->en->variants[vi].fields.size(); i++) {
            auto &f = ei->en->variants[vi].fields[i];
            if (f.ispad) { off += f.padsize > 0 ? f.padsize : 0; continue; }
            auto ft = ei->vftypes[vi][i];
            tyg_dt_member(m, rec, string(f.name).c_str(), DT(ft), off);
            if (!cg.IsFix(ft)) { off += IsVarArr(ft) ? LenWidth(ft) : 0; break; }
            off += cg.FixedSize(ft);
        }
        tyg_dt_record_complete(m, rec, off, 1);
        return rec;
    }
    // The largest value: the tag and the largest payload.
    int64_t VmMaxSize(TypeExpr *t) {
        auto ei = cg.EIOf(t);
        int64_t m = 0;
        for (size_t vi = 0; vi < ei->en->variants.size(); vi++) {
            auto vt = cg.VariantType(t, (int)vi);
            if (cg.IsFix(vt)) m = std::max(m, cg.FixedSize(vt));
        }
        return TagSize(t) + m;
    }

    // ---- Optionals ---------------------------------------------------------

    // An optional reference T? (§3.8) as the C backend has it: the address
    // a plain T& would be, 0 for null; for a resizable, the fat reference
    // gs_rref { hdr, stk } with a null hdr. Relative optionals stay in C.
    static bool OptRef(const TypeExpr *t) { return IsOptional(t) && t->ref->lenstorage < 0; }
    // The structs TypeOk and Blocker are inside of: a struct reached again
    // (through an optional link, `struct Node { next: Node? }`) holds if
    // the rest of it does.
    vector<TypeExpr *> tyopen;
    bool Reentered(TypeExpr *t) {
        for (auto o : tyopen)
            if (cg.TEq(o, t)) return true;
        return false;
    }
    struct Open {
        TgModule *mod;
        Open(TgModule *_mod, TypeExpr *t) : mod(_mod) { mod->tyopen.push_back(t); }
        ~Open() { mod->tyopen.pop_back(); }
    };
    // The debug type of a T?: a typedef under its Goose name over the
    // pointer the C half has (`Node *`), so lldb shows `(Node?) n`.
    const tyg_dtype *OptDT(TypeExpr *t, const string &name) {
        return tyg_dt_typedef(m, name.c_str(), tyg_dt_pointer(m, DT(t->ref->sub)), File(0), 0);
    }
    // Whether a struct reaches a value of its own type through its fields
    // (an optional link): its text would recurse, which the C half's
    // render functions do at run time.
    bool Recursive(TypeExpr *t) {
        set<string> seen;
        function<bool(TypeExpr *, int)> walk = [&](TypeExpr *x, int depth) -> bool {
            if (!x || depth > 32) return true;
            switch (x->kind) {
                case TY_REF: return walk(x->ref->sub, depth + 1);
                case TY_SLICE: return walk(x->sub, depth + 1);
                case TY_ARRAY: return walk(x->arr->sub, depth + 1);
                case TY_STRUCT: {
                    if (depth > 0 && cg.TEq(x, t)) return true;
                    if (!seen.insert(cg.Mangle(x)).second) return false;
                    auto si = cg.SI(x);
                    if (!si) return false;
                    for (size_t i = 0; i < si->st->fields.size(); i++)
                        if (!si->st->fields[i].ispad && walk(si->ftypes[i], depth + 1)) return true;
                    return false;
                }
                default: return false;
            }
        };
        return walk(t, 0);
    }

    // ---- Globals -----------------------------------------------------------

    // A global this backend reaches: a fixed value it holds, not a pool's.
    // In a mixed build every global stays the C half's (gs_globals_t,
    // initialized by gs_init_globals as for any build), and is reached
    // through an accessor the C half exports, which returns its address in
    // the running thread's instance (GS_GL; Global); a whole program's
    // object defines the accessors and the globals itself
    // (TinyGenBackend::EmitGlobalStorage). A `let` of a const type
    // with a compile-time initializer, which the C makes static data
    // (CodeGen::EmitGlobalDecls), is static data in this object too, read
    // where it lies (StaticGlobal).
    // A resizable or T[] global of fixed elements (RzOk) lives on a
    // dedicated stack of its own (CodeGen::gstks), which a second accessor
    // gives (StackAccessor): its header, or its image's address, in the
    // instance, and that stack beside it.
    bool GlobalOk(VarDef *d) {
        if (!d->isglobal || cg.PrefVar(d) || !cg.gnames.count(d)) return false;
        if (d->reusable) return IsGrow(d->type) && RzOk(d->type) && cg.gstks.count(d) && cg.gpools.count(d);
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
    // A global pool's freelist accessors: its header's, its stack's.
    map<const VarDef *, pair<string, string>> flaccessors;
    pair<string, string> PoolAccessors(VarDef *d) {
        auto it = flaccessors.find(d);
        auto n = flaccessors.size();
        auto r = it != flaccessors.end() ? it->second
                                         : (flaccessors[d] = { cat("gs_tg_gf", n), cat("gs_tg_gfs", n) });
        NeedShim(r.first, cat("void *", r.first, "(void) { return (void *)&(", cg.gpools[d].first, "); }\n"));
        NeedShim(r.second, cat("void *", r.second, "(void) { return (void *)", cg.gpools[d].second, "; }\n"));
        return r;
    }
    // The `return from` channels' accessors (gs_tg_lret_<tid>_<i>, and
    // gs_tg_fdst_, gs_tg_fval_), with the size of each channel.
    map<string, int64_t> channels;
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
    // holds (FvOk) come after the declared parameters. A `return from`
    // travels outside the signature, in the C half's thread-locals; this
    // backend defines a target of one whose results are all fixed (their
    // channels are plain thread-locals, no destination stacks).
    bool SigOk(FnSpec *sp, bool define, string &why) {
        auto &si = cg.sinfo[sp];
        if (sp->sf->isextern) { why = "extern"; return false; }
        for (auto fv : si.freevars)
            if (!FvOk(fv)) { why = cat("free variable of type ", fv->type ? NameWhy(fv->type) : "?"); return false; }
        if (define && cg.fromids.count(sp))
            for (auto r : sp->rets)
                if (!RetOk(r)) { why = cat("return from target with result type ", NameWhy(r)); return false; }
        for (auto r : sp->rets)
            if (!RetOk(r)) { why = cat("result type ", NameWhy(r)); return false; }
        for (size_t i = 0; i < sp->argtypes.size(); i++)
            if (cg.IsPoolParam(sp, i) || !(TypeOk(sp->argtypes[i]) || RzParam(sp->argtypes[i]))) {
                why = cat("parameter type ", NameWhy(sp->argtypes[i]));
                return false;
            }
        return true;
    }
    // A result this backend holds: a fixed value, or a resizable array or
    // a T[] image of fixed elements, built at the caller's destination
    // stack.
    bool RetOk(TypeExpr *t) {
        return TypeOk(t) || ((IsGrow(t) || IsVarArr(t)) && RzOk(t)) || VsOk(t) || VmOk(t) || VvOk(t) || RcOk(t) ||
               VarImgOk(t) || VeOk(t);
    }
    // A free variable of a nested function (§7.5), as SigParams passes it:
    // a fixed variable by its address, a T[] by its image's address, and
    // a resizable by its header's address and then its stack. Pools and
    // frame objects are not held.
    bool FvOk(const VarDef *fv) {
        if (!fv->type || fv->reusable || cg.PrefVar(fv)) return false;
        return TypeOk(fv->type) || ((IsGrow(fv->type) || IsVarArr(fv->type)) && RzOk(fv->type)) || VsOk(fv->type) ||
               RcOk(fv->type) ||
               VmOk(fv->type) || VvOk(fv->type) || VarImgOk(fv->type) || VeOk(fv->type);
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
            } else if (IsImg(sp->rets[i])) {
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
        tyg_local res = 0;        // Where a break's value goes, if wanted,
        TypeExpr *rt = nullptr;   // of this type.
        bool block = false;       // A block, which continue passes by.
    };
    vector<LoopTargets> loops;
    // Each result's channel (CodeGen::SigParams), its parameters kept in
    // locals: an out-pointer in `a` for a fixed result other than the C
    // return value; a destination stack in `a` and the count's
    // out-parameter in `b` for a resizable one.
    struct Chan {
        tyg_local a = 0, b = 0;
        tyg_local top0 = 0;   // A destination's top on entry (DstTop0).
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
    // A reusable pool's freelist (§5.4): its header { base, len } and the
    // slot holding its stack, by the pool's variable.
    map<const VarDef *, pair<tyg_local, tyg_local>> poolfl;

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
        bool enc = false;   // A varint in a bytes value: zigzag ULEB128 (ZigRead).
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
            M.NeedShim("gs_tg_stks");
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
        if (live && sp && sp->incycle && callee->incycle && cycle(sp) == cycle(callee))
            No(l, "data stack held across a call into the recursive cycle");
        stackcalls.push_back({ callee, live, l });
        return tyg_bin(p, TYG_ADD, SpVal(l), tyg_iconst(p, TYG_I64, live));
    }

    // Appends x, of the element type et, at the top of stk to the
    // resizable whose count is at len.
    void PushRaw(tyg_val stk, tyg_addr len, TypeExpr *et, const V &x) {
        if (!cg.IsFix(et)) No(Line {}, "an element of variable size");
        auto top = Top(stk);
        Store(P { tyg_at_val(top, 0), et }, x);
        SetTop(stk, tyg_bin(p, TYG_ADD, top, tyg_iconst(p, TYG_I64, cg.FixedSize(et))));
        tyg_store(p, TYG_I64, len, tyg_bin(p, TYG_ADD, tyg_load(p, TYG_I64, len, 0), tyg_iconst(p, TYG_I64, 1)), 0);
    }
    // Copies n elements of et from elems to the top of stk, adding them to
    // the count at len. The source may lie on stk itself, below its top.
    void CopyRun(tyg_val stk, tyg_addr len, TypeExpr *et, tyg_val elems, tyg_val n) {
        if (!cg.IsFix(et)) No(Line {}, "a copy of elements of variable size");
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
        if (auto ie = Is<IfExpr>(n); ie && ie->elseb) {
            // Each arm builds the array at the same top.
            BuildIf(ie, l, [&](Node *v) { GenRz(v, stk, hdr, t, l); });
            return;
        }
        if (auto b = Is<Block>(n); b && b->tail) {
            BuildArm(b, l, [&](Node *v) { GenRz(v, stk, hdr, t, l); });
            return;
        }
        if (auto c = Is<Call>(n); c && c->fvbody && c->fvbody->tail) {
            BuildFv(c, l, [&](Node *v) { GenRz(v, stk, hdr, t, l); });
            return;
        }
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
        auto pl = LimCallResult(n) ? LimCallPlace(n) : Place(n);
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
    bool IsImg(TypeExpr *t) { return M.IsImg(t); }

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
        if (TgModule::IsVarint(t)) {
            // A varint field: its value zigzag ULEB128 at the top.
            if (!n || from) No(l, "varint without a value");
            auto top = Top(stk);
            auto x = GenAs(n, M.ast.inttypes[IS_I64]);
            if (Ended()) return top;
            SetTop(stk, tyg_bin(p, TYG_ADD, top, ZigWrite(top, x.v)));
            return top;
        }
        if (auto ie = n && !from ? Is<IfExpr>(n) : nullptr; ie && ie->elseb) {
            // Each arm builds its value at the same top (CodeGen's
            // GenConstruct of an if): the image starts where the top was.
            auto start = tyg_local_new(p, 8, 8);
            tyg_store(p, TYG_I64, tyg_at_local(start, 0), Top(stk), 0);
            BuildIf(ie, l, [&](Node *v) { GenImg(v, stk, t, l); });
            return tyg_load(p, TYG_I64, tyg_at_local(start, 0), 0);
        }
        if (auto b = n && !from ? Is<Block>(n) : nullptr; b && b->tail) {
            auto start = tyg_local_new(p, 8, 8);
            tyg_store(p, TYG_I64, tyg_at_local(start, 0), Top(stk), 0);
            BuildArm(b, l, [&](Node *v) { GenImg(v, stk, t, l); });
            return tyg_load(p, TYG_I64, tyg_at_local(start, 0), 0);
        }
        if (auto c = n && !from ? Is<Call>(n) : nullptr; c && c->fvbody && c->fvbody->tail) {
            auto start = tyg_local_new(p, 8, 8);
            tyg_store(p, TYG_I64, tyg_at_local(start, 0), Top(stk), 0);
            BuildFv(c, l, [&](Node *v) { GenImg(v, stk, t, l); });
            return tyg_load(p, TYG_I64, tyg_at_local(start, 0), 0);
        }
        if (M.IsVm(t)) {
            if (!n || from) No(l, cat("value of type ", M.NameWhy(t)));
            return GenVm(n, stk, t, l);
        }
        if (M.IsVv(t)) {
            if (!n || from) No(l, cat("value of type ", M.NameWhy(t)));
            return GenVv(n, stk, t, l);
        }
        if (M.IsVs(t)) {
            if (!n || from) No(l, cat("value of type ", M.NameWhy(t)));
            return GenVs(n, stk, t, l);
        }
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
        if (M.VarImgOk(t)) return GenImgOfImgs(n, stk, t, l, from, start);
        if (M.IsRc(t)) {
            GenRc(n, stk, t, l, from, at());
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

    // A T[..] built at the top of stk, at `at` (CodeGen::GenArrayLit):
    // [..cap] its capacity and no elements, the slots left as they are;
    // another T[..] copied whole, capacity and all; any other array or
    // literal its elements, the capacity their count.
    void GenRc(Node *n, tyg_val stk, TypeExpr *t, Line l, const View *from, tyg_val at) {
        auto et = t->arr->sub;
        auto esz = cg.FixedSize(et);
        P pl { tyg_at_val(at, 0), t };
        auto al = n && !from ? Is<ArrayLit>(n) : nullptr;
        if (al && al->capexpr) {
            auto cap = Index64(al->capexpr);
            if (Ended()) return;
            AbortUnless(tyg_cmp(p, TYG_ULE, cap, tyg_iconst(p, TYG_I64, 0xffffffffll)), "CAPRANGE", l);
            tyg_store(p, TYG_I32, pl.a, tyg_convert(p, TYG_TRUNC, TYG_I32, cap), 0);
            LimSetLen(pl, tyg_iconst(p, TYG_I64, 0));
            SetTop(stk, tyg_bin(p, TYG_ADD, at, tyg_bin(p, TYG_ADD, tyg_iconst(p, TYG_I64, 8),
                                                         tyg_bin(p, TYG_MUL, cap, tyg_iconst(p, TYG_I64, esz)))));
            return;
        }
        if (n && !from && !al && !Is<StrLit>(n) && !LimCallResult(n) && n->exprtype && cg.TEq(n->exprtype, t)) {
            auto src = Deref(Place(n));
            if (Ended()) return;
            ImgCopy(stk, tyg_addr_of(p, src.a), t);
            return;
        }
        // The elements behind an 8-byte header, built as a resizable's
        // are; then the header, capacity and length both their count.
        SetTop(stk, tyg_bin(p, TYG_ADD, at, tyg_iconst(p, TYG_I64, 8)));
        auto hdr = tyg_at_local(tyg_local_new(p, 16, 8), 0);
        auto len = hdr;
        len.offset += 8;
        tyg_store(p, TYG_I64, hdr, Top(stk), 0);
        tyg_store(p, TYG_I64, len, tyg_iconst(p, TYG_I64, 0), 0);
        if (from) {
            if (cg.Mangle(from->elem) != cg.Mangle(et)) No(l, "array of another element type");
            CopyRun(stk, len, et, from->elems, from->len);
        } else if (al || Is<StrLit>(n)) {
            GenRz(n, stk, hdr, t, l);
        } else if (n) {
            if (auto c = Is<Call>(n); c && c->builtin == B_COPY) n = c->ArgNodes()[0];
            auto v = ViewOf(LimCallResult(n) ? LimCallPlace(n) : Place(n), l);
            if (Ended()) return;
            if (cg.Mangle(v.elem) != cg.Mangle(et)) No(l, "array of another element type");
            CopyRun(stk, len, et, v.elems, v.len);
        }
        if (Ended()) return;
        auto cnt = tyg_load(p, TYG_I64, len, 0);
        AbortUnless(tyg_cmp(p, TYG_ULE, cnt, tyg_iconst(p, TYG_I64, 0xffffffffll)), "CAPRANGE", l);
        tyg_store(p, TYG_I32, pl.a, tyg_convert(p, TYG_TRUNC, TYG_I32, cnt), 0);
        LimSetLen(pl, cnt);
    }

    // A T[] of images built at the top of stk (its start in `start`): the
    // length prefix, then a literal's elements each built in place behind
    // the last, or another such array's elements copied as one run of
    // bytes, then the prefix patched with the count.
    tyg_val GenImgOfImgs(Node *n, tyg_val stk, TypeExpr *t, Line l, const View *from, tyg_local start) {
        auto at = [&] { return tyg_load(p, TYG_I64, tyg_at_local(start, 0), 0); };
        auto et = t->arr->sub;
        SetTop(stk, tyg_bin(p, TYG_ADD, at(), tyg_iconst(p, TYG_I64, TgModule::LenWidth(t))));
        auto cnt = tyg_local_new(p, 8, 8);
        tyg_store(p, TYG_I64, tyg_at_local(cnt, 0), tyg_iconst(p, TYG_I64, 0), 0);
        View v {};
        auto al = n ? Is<ArrayLit>(n) : nullptr;
        if (al && !al->fillval && !al->capexpr) {
            for (auto e : al->elems) {
                GenImg(e, stk, et, l);
                if (Ended()) return at();
                tyg_store(p, TYG_I64, tyg_at_local(cnt, 0),
                          tyg_bin(p, TYG_ADD, tyg_load(p, TYG_I64, tyg_at_local(cnt, 0), 0), tyg_iconst(p, TYG_I64, 1)), 0);
            }
        } else {
            if (from) v = *from;
            else if (n) v = ViewOf(Place(n), l);
            else No(l, "array of images without a value");
            if (Ended()) return at();
            if (cg.Mangle(v.elem) != cg.Mangle(et)) No(l, "array of another element type");
            auto bytes = tyg_bin(p, TYG_SUB, RunEnd(v), v.elems);
            auto top = Top(stk);
            tyg_move_dyn(p, top, v.elems, bytes);
            SetTop(stk, tyg_bin(p, TYG_ADD, top, bytes));
            tyg_store(p, TYG_I64, tyg_at_local(cnt, 0), v.len, 0);
        }
        PatchPrefix(at(), t, tyg_load(p, TYG_I64, tyg_at_local(cnt, 0), 0));
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
        while (IsAddrRef(x.t) && want->kind != TY_REF) x = Load(RefTarget(x), l);
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
        if (LimAdapts(x, want)) return LimFrom(x, want, l);
        if (x.t->kind == TY_VARIANT && want->kind == TY_ENUM && x.mem) return AdaptToAdt(x, want, l);
        if (OptFit(x, want)) return x;
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
        if (pl.enc) {
            x.v = ZigRead(tyg_addr_of(p, pl.a));
            return x;
        }
        if (TgModule::IsRel(pl.t)) {
            x.v = RelLoad(pl, l);
            return x;
        }
        if (M.IsScalar(pl.t)) {
            x.v = tyg_load(p, M.Scal(pl.t), pl.a, 0);
            return x;
        }
        if (!M.TypeOk(pl.t) && M.ImgOk(pl.t)) {
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
        if (pl.enc) No(Line {}, "a store into a varint in a bytes value");
        if (TgModule::IsRel(pl.t)) {
            RelStore(pl, x.v, Line {});
            return;
        }
        // A scalar without a value comes from code after a return or break,
        // which tinygen keeps in a block that is never placed.
        if (M.IsScalar(pl.t) && !x.v) return;
        if (!M.IsScalar(pl.t) && !M.TypeOk(pl.t)) No(Line {}, cat("store of a ", M.Name(pl.t)));
        if (M.IsScalar(pl.t)) tyg_store(p, M.Scal(pl.t), pl.a, x.v, 0);
        else tyg_move(p, pl.a, x.a, M.Size(pl.t));
    }

    // ---- Places --------------------------------------------------------------

    // What the reference at pl refers to: a fat reference's header, with
    // its stack.
    P Step(const P &pl) {
        if (TgModule::IsRel(pl.t)) return P { tyg_at_val(RelLoad(pl, Line {}), 0), pl.t->ref->sub };
        auto ptr = tyg_load(p, TYG_I64, pl.a, 0);
        P r { tyg_at_val(ptr, 0), pl.t->ref->sub };
        if (cg.IsFatRef(pl.t)) {
            auto a8 = pl.a;
            a8.offset += 8;
            r.stk = tyg_load(p, TYG_I64, a8, 0);
        }
        return r;
    }
    // Through references to what they refer to: an optional one is
    // narrowed wherever the checker lets a path through it (§3.8).
    P Deref(P pl) {
        while (IsAddrRef(pl.t) || TgModule::IsRel(pl.t)) pl = Step(pl);
        return pl;
    }

    // ---- Relative references (§3.9) ----------------------------------------

    // What an offset measures from (CodeGen::RelOrigin): the slot itself,
    // or for an `in pool` reference the pool's base less one, so that 0
    // stays null (PoolBase).
    tyg_val RelOrigin(const P &pl, Line l) {
        auto pool = pl.t->ref->pool;
        if (!pool) return tyg_addr_of(p, pl.a);
        auto pp = VarPlace(const_cast<VarDef *>(pool), l);
        auto base = tyg_load(p, TYG_I64, pp.a, 0);
        return tyg_bin(p, TYG_SUB, base, tyg_iconst(p, TYG_I64, 1));
    }
    // The address the slot at pl decodes to, null for a 0 offset.
    tyg_val RelLoad(const P &pl, Line l) {
        auto s = M.RelScal(pl.t);
        // Signed from the slot, unsigned from a pool (CodeGen::RelParts).
        auto off = tyg_load(p, s, pl.a, 0);
        if (s != TYG_I64) off = tyg_convert(p, pl.t->ref->pool ? TYG_ZEXT : TYG_SEXT, TYG_I64, off);
        auto at = tyg_bin(p, TYG_ADD, RelOrigin(pl, l), off);
        if (!pl.t->ref->optional) return at;
        return tyg_select(p, tyg_cmp(p, TYG_EQ, off, tyg_iconst(p, TYG_I64, 0)), tyg_iconst(p, TYG_I64, 0), at);
    }
    // The address v as the slot's offset, range-checked (EmitRelStoreAt).
    void RelStore(const P &pl, tyg_val v, Line l) {
        auto s = M.RelScal(pl.t);
        auto pool = pl.t->ref->pool;
        auto off = tyg_bin(p, TYG_SUB, v, RelOrigin(pl, l));
        if (pl.t->ref->optional) {
            auto isnull = tyg_cmp(p, TYG_EQ, v, tyg_iconst(p, TYG_I64, 0));
            off = tyg_select(p, isnull, tyg_iconst(p, TYG_I64, 0), off);
            // A link to its own slot would read back as null (RelOffset).
            if (!pool)
                AbortUnless(tyg_bin(p, TYG_OR, isnull, tyg_cmp(p, TYG_NE, off, tyg_iconst(p, TYG_I64, 0))),
                            "RELNULL", l);
        }
        auto bits = TgModule::ScalSize(s) * 8;
        if (bits < 64 && pool)
            AbortUnless(tyg_cmp(p, TYG_ULE, off, tyg_iconst(p, TYG_I64, (int64_t)((1ull << bits) - 1))), "RELOFF", l);
        if (bits < 64 && !pool) {
            auto lo = tyg_cmp(p, TYG_SGE, off, tyg_iconst(p, TYG_I64, -(int64_t)(1ull << (bits - 1))));
            auto hi = tyg_cmp(p, TYG_SLT, off, tyg_iconst(p, TYG_I64, (int64_t)(1ull << (bits - 1))));
            AbortUnless(tyg_bin(p, TYG_AND, lo, hi), "RELOFF", l);
        }
        tyg_store(p, s, pl.a, s == TYG_I64 ? off : tyg_convert(p, TYG_TRUNC, s, off), 0);
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
            if (M.VsOk(base.t)) return VsFieldPlace(base, d->fieldidx);
            if (M.FoOk(base.t)) return FoFieldPlace(base, d->fieldidx, n->line);
            if (M.VvOk(base.t)) {
                auto ei = cg.EIVar(base.t);
                auto vi = (size_t)ei->en->VariantIndex(base.t->var->variant);
                auto ft = ei->vftypes[vi][(size_t)d->fieldidx];
                P r { tyg_at_val(VvField(base.t, tyg_addr_of(p, base.a), d->fieldidx), 0), ft };
                r.enc = TgModule::IsVarint(ft);
                return r;
            }
            if (base.t->kind == TY_VARIANT && M.TypeOk(base.t)) return VariantField(base, d->fieldidx);
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
        if (!M.GlobalOk(d)) No(l, cat("global of type ", M.NameWhy(d->type)));
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
    // A literal of a fixed struct with a non-null self-relative link: the
    // C builds it where it lies, whose slots the offsets measure from.
    bool SelfRelLit(Node *n) {
        auto sl = Is<StructLit>(n);
        if (!sl || !sl->sinst || sl->variant || !sl->exprtype || sl->exprtype->kind != TY_STRUCT ||
            !M.TypeOk(sl->exprtype))
            return false;
        auto si = cg.SI(sl->exprtype);
        for (size_t i = 0; i < si->st->fields.size(); i++) {
            if (si->st->fields[i].ispad) continue;
            auto ft = si->ftypes[i];
            auto init = sl->InitFor((int)i);
            if (init && TgModule::IsRel(ft) && !ft->ref->pool && !Is<NullLit>(init)) return true;
        }
        return false;
    }
    // A struct literal SelfRelInto can build.
    bool SelfRelLitOk(Node *n) {
        auto sl = Is<StructLit>(n);
        return sl && sl->sinst && !sl->variant && sl->exprtype && sl->exprtype->kind == TY_STRUCT &&
               M.TypeOk(sl->exprtype);
    }
    // The value a literal being built in place starts at, which `self` in
    // its fields refers to (§3.9), or 0.
    tyg_val selfbase = 0;
    // Such a literal built at pl: zero bytes, then its fields where they lie.
    void SelfRelInto(const P &pl, StructLit *sl) {
        auto t = sl->exprtype;
        tyg_zero(p, pl.a, M.Size(t));
        auto outer = selfbase;
        selfbase = tyg_addr_of(p, pl.a);
        struct Back {
            tyg_val &at, was;
            ~Back() { at = was; }
        } back { selfbase, outer };
        auto si = cg.SI(t);
        auto &lay = cg.StructLayout(si);
        for (size_t i = 0; i < si->st->fields.size() && !Ended(); i++) {
            if (si->st->fields[i].ispad) continue;
            auto init = sl->InitFor((int)i);
            if (!init) continue;
            auto a = pl.a;
            a.offset += lay.offs[i];
            Into(P { a, si->ftypes[i] }, init);
        }
    }
    void Into(const P &pl, Node *n) {
        if (SelfRelLit(n) && cg.TEq(n->exprtype, pl.t)) {
            SelfRelInto(pl, Is<StructLit>(n));
            return;
        }
        auto x = GenAs(n, pl.t);
        if (M.IsScalar(pl.t) || x.mem) Store(pl, x);
        else if (!Ended()) No(n->line, "aggregate value");
    }

    // ---- Expressions -------------------------------------------------------

    V Gen(Node *n) {
        auto x = Gen1(n);
        // After a return or break the rest is unreachable, but the code using
        // this value is still built: it needs a value, not none.
        if (Ended() && !x.v && !x.mem && n->exprtype && M.IsScalar(n->exprtype)) return Dummy(n->exprtype);
        if (Ended() || !x.t || !n->exprtype || n->exprtype->kind == TY_VOID) return x;
        return Fit(x, n->exprtype, n->line);
    }
    V GenAs(Node *n, TypeExpr *t) {
        if (t && t->kind == TY_SLICE) return GenSliceAs(n, t);
        if (t && t->kind == TY_REF && Is<NullLit>(n)) return Null(t, n->line);
        if (t && t->kind == TY_REF && cg.IsFatRef(t) && n->exprtype && (IsGrow(n->exprtype) || M.FoOk(n->exprtype))) {
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
        if (t && IsAddrRef(t) && n->exprtype && IsImg(n->exprtype) && M.ImgOk(t->ref->sub)) {
            // A T[] (or a variable-size struct) where a reference to one is
            // wanted, plain or optional: its image's address.
            auto pl = Deref(Place(n));
            V x;
            x.t = t;
            x.v = Ended() ? tyg_iconst(p, TYG_I64, 0) : tyg_addr_of(p, pl.a);
            return x;
        }
        return Fit(Gen(n), t, n->line);
    }
    tyg_val Truth(Node *n) {
        if (OptCond(n)) return OptTest(n);
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
            case NK_NullLit: return Null(n->exprtype, n->line);
            case NK_SelfRef: {
                // `self` in a literal built where it lies: its start.
                if (!selfbase) No(n->line, "self outside a literal built in place");
                V x;
                x.t = n->exprtype;
                x.v = selfbase;
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
                if (d->member == B_CAP) return GenCap(d);
                if (d->variantconst) return GenVariantConst(d);
                if (!d->IsField()) No(n->line, "property or variant constant");
                return Load(Place(n), n->line);
            }
            case NK_Index: return Load(Place(n), n->line);
            case NK_ArrayLit: return GenArrayLit(static_cast<ArrayLit *>(n));
            case NK_SliceExpr: return GenSlice(static_cast<SliceExpr *>(n));
            case NK_StrLit: {
                // A string literal as a u8[:] (its bytes, read-only).
                auto t = n->exprtype;
                if (TgModule::IsLim(t)) return LimStr(static_cast<StrLit *>(n));
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
            case NK_EarlyBlock: return GenEarly(static_cast<EarlyBlock *>(n), false);
            case NK_LoopExpr: return GenLoopX(static_cast<LoopExpr *>(n), false);
            case NK_MatchExpr: return GenMatch(static_cast<MatchExpr *>(n), false);
            case NK_Return: case NK_Break: case NK_Continue: case NK_VarDecl: case NK_Assign:
            case NK_IncDec: case NK_While: case NK_ForLoop:
                Stmt(n);
                return Dummy(n->exprtype && n->exprtype->kind != TY_VOID ? n->exprtype : nullptr);
            default:
                No(n->line, n->nkind == NK_MatchExpr ? string("match expression")
                            : n->nkind == NK_EarlyBlock ? string("block expression")
                            : cat("expression kind ", (int)n->nkind));
        }
    }

    V GenUnary(Unary *u) {
        auto et = u->exprtype;
        if (u->op == T_BITAND) {
            if (et && et->kind == TY_REF && cg.IsFatRef(et)) {
                // gs_rref: the header's address and its stack.
                auto pl = Deref(Place(u->child));
                if ((!IsGrow(pl.t) && !M.FoOk(pl.t)) || !pl.stk) No(u->line, "reference to an array of this kind");
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
                if (!IsPlainRef(et) && !M.OptRef(et) && !(TgModule::IsRel(et) && M.RelOk(et, 1)))
                    No(u->line, "reference kind");
                auto pl = Deref(Place(u->child));
                V x;
                x.t = et;
                x.v = tyg_addr_of(p, pl.a);
                return x;
            }
            return Load(Deref(Place(u->child)), u->line);
        }
        if (u->op == T_NOT && OptCond(u->child)) {
            V x;
            x.t = M.ast.booltype;
            x.v = tyg_cmp(p, TYG_EQ, OptTest(u->child), tyg_iconst(p, TYG_I8, 0));
            return x;
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
        auto name = cat("gs_tg_", op, "_", CodeGen::IntSfx(t->intstorage));
        M.NeedShim(name);
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
        if (op == T_DOTEQ || op == T_DOTNEQ) return GenRefEq(b);
        if (Is<NullLit>(b->left) || Is<NullLit>(b->right)) {
            if (op != T_EQ && op != T_NEQ) No(b->line, "null operand");
            return GenRefEq(b);
        }
        if (op == T_EQ || op == T_NEQ) {
            auto arr = [](TypeExpr *t) {
                while (t && IsPlainRef(t)) t = t->ref->sub;
                return t && (t->kind == TY_SLICE || t->kind == TY_ARRAY) ? t : nullptr;
            };
            if (arr(b->left->exprtype) && arr(b->right->exprtype)) return GenArrayEq(b);
        }
        auto lt = cg.OperandT(b->left->exprtype);
        if (!M.IsScalar(lt) || lt->kind == TY_REF) No(b->line, cat("operand type ", M.NameWhy(lt)));
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
                M.NeedShim("gs_tg_f2iwrap");
                r = CallC("gs_tg_f2iwrap", { f64 }, { d }, IT(TYG_I64));
            } else if (is == IS_U64) {
                M.NeedShim("gs_tg_f2u");
                r = CallC("gs_tg_f2u", { f64, IT(TYG_I32), tyg_type_ptr(m), IT(TYG_I32) },
                          { d, tyg_iconst(p, TYG_I32, M.Scal(st) == TYG_F32), FileArg(c->line),
                            LineArg(c->line) },
                          IT(TYG_I64, false));
            } else {
                M.NeedShim("gs_tg_f2i");
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
            M.NeedShim("gs_tg_range_u");
            r = CallC("gs_tg_range_u",
                      { IT(TYG_I64, false), IT(TYG_I64, false), tyg_type_ptr(m), tyg_type_ptr(m),
                        IT(TYG_I32) },
                      { x.v, tyg_iconst(p, TYG_I64, IntRange(is).second), TypeNameArg(is),
                        FileArg(c->line), LineArg(c->line) },
                      IT(TYG_I64, false));
            y.v = Conv(r, M.ast.inttypes[IS_U64], tt, c->line);
            return y;
        }
        M.NeedShim("gs_tg_range");
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
        if (t->kind == TY_VARIANT || t->kind == TY_ENUM) return GenVariantLit(sl);
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
            // A self-relative link measures from the slot it lies in, which a
            // literal built here and copied out of is not.
            auto ft = si->ftypes[i];
            if (TgModule::IsRel(ft) && !ft->ref->pool && !Is<NullLit>(init))
                No(sl->line, "self-relative link in a literal");
            Into(P { tyg_at_local(tmp, lay.offs[i]), ft }, init);
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
        if (TgModule::IsLim(pl.t) && M.LimHeld(pl.t)) return LimView(pl);
        if (IsGrow(pl.t) && M.VeOk(pl.t)) {
            // Elements of variable size: walked from the first (StepElem).
            auto a8 = pl.a;
            a8.offset += 8;
            return { tyg_load(p, TYG_I64, pl.a, 0), tyg_load(p, TYG_I64, a8, 0), pl.t->arr->sub, -1 };
        }
        if (IsArrayKind(pl.t, A_VAR) && (M.RzOk(pl.t) || M.VarImgOk(pl.t))) {
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
        if (!cg.IsFix(v.elem)) No(Line {}, "an element of variable size by its index");
        auto esz = cg.FixedSize(v.elem);
        auto off = tyg_bin(p, TYG_MUL, i, tyg_iconst(p, TYG_I64, esz));
        return tyg_bin(p, TYG_ADD, v.elems, off);
    }

    // The element after the one at `at`: a fixed size on, or past its
    // image's bytes.
    tyg_val StepElem(const View &v, tyg_val at) {
        if (cg.IsFix(v.elem)) return tyg_bin(p, TYG_ADD, at, tyg_iconst(p, TYG_I64, cg.FixedSize(v.elem)));
        return tyg_bin(p, TYG_ADD, at, ImgSize(v.elem, at));
    }

    // The end of a view's elements: past the last, each walked by its size
    // where they vary (CodeGen's SizeFn).
    tyg_val RunEnd(const View &v) {
        if (cg.IsFix(v.elem))
            return tyg_bin(p, TYG_ADD, v.elems, tyg_bin(p, TYG_MUL, v.len, tyg_iconst(p, TYG_I64, cg.FixedSize(v.elem))));
        auto cl = tyg_local_new(p, 8, 8), il = tyg_local_new(p, 8, 8);
        tyg_store(p, TYG_I64, tyg_at_local(cl, 0), v.elems, 0);
        tyg_store(p, TYG_I64, tyg_at_local(il, 0), tyg_iconst(p, TYG_I64, 0), 0);
        auto head = tyg_block_new(p), body = tyg_block_new(p), done = tyg_block_new(p);
        tyg_jump(p, head);
        tyg_block_set(p, head);
        auto i = tyg_load(p, TYG_I64, tyg_at_local(il, 0), 0);
        tyg_branch(p, tyg_cmp(p, TYG_SLT, i, v.len), body, done);
        tyg_block_set(p, body);
        auto c = tyg_load(p, TYG_I64, tyg_at_local(cl, 0), 0);
        tyg_store(p, TYG_I64, tyg_at_local(cl, 0), tyg_bin(p, TYG_ADD, c, ImgSize(v.elem, c)), 0);
        tyg_store(p, TYG_I64, tyg_at_local(il, 0), tyg_bin(p, TYG_ADD, i, tyg_iconst(p, TYG_I64, 1)), 0);
        tyg_jump(p, head);
        tyg_block_set(p, done);
        return tyg_load(p, TYG_I64, tyg_at_local(cl, 0), 0);
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
        while (ot && IsAddrRef(ot)) ot = ot->ref->sub;
        if (!ot || !(M.TypeOk(ot) || M.RzOk(ot) || M.VeOk(ot) || M.VarImgOk(ot) || M.RcOk(ot)) ||
            (ot->kind != TY_ARRAY && ot->kind != TY_SLICE))
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
        if (auto al = Is<ArrayLit>(n); al && !al->fillval && !al->capexpr && cg.IsFix(t->sub)) {
            // A literal at a slice destination: its elements in a temporary
            // of their own, viewed.
            auto et = t->sub;
            auto esz = cg.FixedSize(et);
            auto k = (int64_t)al->elems.size();
            auto tmp = tyg_local_new(p, std::max<int64_t>(k * esz, 1), 8);
            for (int64_t i = 0; i < k; i++) {
                Into(P { tyg_at_local(tmp, i * esz), et }, al->elems[(size_t)i]);
                if (Ended()) return Dummy(t);
            }
            return SliceValue(t, tyg_addr_of(p, tyg_at_local(tmp, 0)), tyg_iconst(p, TYG_I64, k));
        }
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
            M.NeedShim("gs_tg_slicefail");
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
        if (TgModule::IsLim(t)) return LimLit(al);
        if (!t || t->kind != TY_ARRAY || t->arr->akind != A_FIXED || !M.TypeOk(t) || al->capexpr)
            No(al->line, "array literal of this type");
        auto k = cg.ArrSize(t->arr);
        auto et = t->arr->sub;
        auto esz = cg.FixedSize(et);
        auto tmp = Temp(t);
        tyg_zero(p, tyg_at_local(tmp, 0), M.Size(t));
        if (al->fillval) {
            if (SelfRelLit(al->fillval)) No(al->line, "fill of a value linking to itself");
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

    // ---- Limited arrays T[..k] ---------------------------------------------
    //
    // The C half's struct { lenT len; T e[k]; } (TgModule::LimOk), read and
    // changed where it lies, with the C backend's checks, order and abort
    // codes (codegen_builtins.h, CodeGen::CopyIntoLimited): a push, an
    // append, a resize past the capacity aborts with GS_E_CAPACITY. Any
    // other array or slice of the element type where one is wanted is
    // copied in under that check (§4.2). Slots past the length are zero
    // where this backend makes a new value, as the C's `{0}` makes them.

    // The length at pl, as an i64.
    tyg_val LimLen(const P &pl) {
        auto s = M.LimLenScal(pl.t);
        auto n = tyg_load(p, s, LimLenAt(pl), 0);
        return s == TYG_I64 ? n : tyg_convert(p, TYG_ZEXT, TYG_I64, n);
    }
    // Stores n, an i64 within the capacity, as the length at pl.
    void LimSetLen(const P &pl, tyg_val n) {
        auto s = M.LimLenScal(pl.t);
        tyg_store(p, s, LimLenAt(pl), s == TYG_I64 ? n : tyg_convert(p, TYG_TRUNC, s, n), 0);
    }
    tyg_addr LimLenAt(const P &pl) {
        auto a = pl.a;
        if (M.IsRc(pl.t)) a.offset += 4;
        return a;
    }
    int64_t LimCap(TypeExpr *t) { return cg.ArrSize(t->arr); }
    // The capacity at pl, as an i64: the type's, or a T[..]'s own.
    tyg_val LimCapOf(const P &pl) {
        if (!M.IsRc(pl.t)) return tyg_iconst(p, TYG_I64, LimCap(pl.t));
        return tyg_convert(p, TYG_ZEXT, TYG_I64, tyg_load(p, TYG_I32, pl.a, 0));
    }
    View LimView(const P &pl) {
        auto elems = tyg_bin(p, TYG_ADD, tyg_addr_of(p, pl.a), tyg_iconst(p, TYG_I64, M.LimWidth(pl.t)));
        return { elems, LimLen(pl), pl.t->arr->sub, -1 };
    }
    // A new value of type t in a temporary: zero bytes, an empty array.
    P LimTemp(TypeExpr *t) {
        P pl { tyg_at_local(Temp(t), 0), t };
        tyg_zero(p, pl.a, M.Size(t));
        return pl;
    }
    V LimValue(const P &pl) {
        V x;
        x.t = pl.t;
        x.mem = true;
        x.a = pl.a;
        return x;
    }
    // .cap: the type's.
    V GenCap(Dot *d) {
        auto ot = d->obj->exprtype;
        while (ot && IsPlainRef(ot)) ot = ot->ref->sub;
        if (!TgModule::IsLim(ot) || !M.LimHeld(ot)) No(d->line, "cap of this type");
        V x;
        x.t = M.ast.inttypes[IS_I64];
        if (M.IsRc(ot)) {
            x.v = LimCapOf(Deref(Place(d->obj)));
            return x;
        }
        if (!Is<Ident>(d->obj) && !Is<Dot>(d->obj)) Gen(d->obj);   // What it does, it does.
        x.v = tyg_iconst(p, TYG_I64, LimCap(ot));
        return x;
    }
    // Whether x, an array or slice of another representation, is copied
    // into a limited array where one of type want is wanted.
    bool LimAdapts(const V &x, TypeExpr *want) {
        if (!TgModule::IsLim(want) || !M.TypeOk(want) || !x.mem || !x.t) return false;
        if (x.t->kind != TY_ARRAY && x.t->kind != TY_SLICE) return false;
        return !cg.TEq(x.t, want);
    }
    // The count of v, then the check against want's capacity, then the
    // elements copied into dst and its length set (CopyIntoLimited). The
    // count and elements are read before anything is written, and the
    // elements move as memmove moves them.
    void LimCopyInto(const P &dst, const View &v, Line l) {
        auto et = dst.t->arr->sub;
        if (cg.Mangle(v.elem) != cg.Mangle(et)) No(l, "array of another element type");
        auto nl = tyg_local_new(p, 8, 8);
        tyg_store(p, TYG_I64, tyg_at_local(nl, 0), v.len, 0);
        auto n = [&] { return tyg_load(p, TYG_I64, tyg_at_local(nl, 0), 0); };
        AbortUnless(tyg_cmp(p, TYG_SLE, n(), LimCapOf(dst)), "CAPACITY", l);
        auto to = LimView(dst).elems;
        tyg_move_dyn(p, to, v.elems, tyg_bin(p, TYG_MUL, n(), tyg_iconst(p, TYG_I64, cg.FixedSize(et))));
        LimSetLen(dst, n());
    }
    V LimFrom(const V &x, TypeExpr *want, Line l) {
        auto v = ViewOf(P { x.a, x.t, x.stk }, l);
        auto dst = LimTemp(want);
        LimCopyInto(dst, v, l);
        return LimValue(dst);
    }
    // A string literal typed as a u8[..k].
    V LimStr(StrLit *s) {
        auto t = s->exprtype;
        if (!M.TypeOk(t) || !IsU8(t->arr->sub) || (int64_t)s->val.size() > LimCap(t))
            No(s->line, "string of this type");
        auto dst = LimTemp(t);
        if (!s->val.empty())
            tyg_copy(p, tyg_at_val(LimView(dst).elems, 0), tyg_at_sym(M.Bytes(s->val), 0),
                     (int64_t)s->val.size());
        LimSetLen(dst, tyg_iconst(p, TYG_I64, (int64_t)s->val.size()));
        return LimValue(dst);
    }
    // [a, b], [v; n] and [..c] typed as a T[..k]: the elements in order
    // into the first slots, the length their count.
    V LimLit(ArrayLit *al) {
        auto t = al->exprtype;
        if (!M.TypeOk(t)) No(al->line, cat("array literal of type ", M.NameWhy(t)));
        auto et = t->arr->sub;
        auto esz = cg.FixedSize(et);
        auto dst = LimTemp(t);
        int64_t count = 0;
        if (al->capexpr) {
            if (al->fillval || !al->elems.empty()) No(al->line, "array literal of this kind");
        } else if (al->fillval) {
            auto il = Is<IntLit>(al->fillcount);
            if (!il || il->val < 0 || il->val > LimCap(t)) No(al->line, "fill count");
            count = il->val;
            auto one = Temp(et);
            Into(P { tyg_at_local(one, 0), et }, al->fillval);
            for (int64_t i = 0; i < count && !Ended(); i++) {
                auto a = dst.a;
                a.offset += M.LimWidth(t) + i * esz;
                tyg_copy(p, a, tyg_at_local(one, 0), esz);
            }
            if (count > 64) No(al->line, "fill count");
        } else {
            count = (int64_t)al->elems.size();
            if (count > LimCap(t)) No(al->line, "array literal length");
            for (int64_t i = 0; i < count; i++) {
                auto a = dst.a;
                a.offset += M.LimWidth(t) + i * esz;
                Into(P { a, et }, al->elems[(size_t)i]);
            }
        }
        if (!Ended()) LimSetLen(dst, tyg_iconst(p, TYG_I64, count));
        return LimValue(dst);
    }
    // A call returning a T[..k] that the checker typed as the array built
    // from it (a T[], a resizable): its result where it lies, to copy from.
    bool LimCallResult(Node *n) {
        auto c = Is<Call>(n);
        if (!c || c->builtin >= 0 || !c->spec || c->spec->rets.empty() || !n->exprtype) return false;
        auto rt = c->spec->rets[0];
        return ((rt->kind == TY_ARRAY &&
                 (rt->arr->akind == A_LIMITED || rt->arr->akind == A_FIXED || IsVarArr(rt) || IsGrow(rt))) ||
                rt->kind == TY_SLICE) &&
               !cg.TEq(n->exprtype, rt);
    }
    P LimCallPlace(Node *n) {
        auto x = Gen1(n);
        if (Ended()) return P { x.a, x.t };
        if (!x.mem || !x.t || (x.t->kind != TY_ARRAY && x.t->kind != TY_SLICE)) No(n->line, "array result of this kind");
        return P { x.a, x.t };
    }
    // v.push(x) (EmitPush): the receiver, then the element, then the
    // length read and checked; the slot written, the length raised. Its
    // value is the slot's address.
    V LimPush(Call *c, const P &pl) {
        if (!M.LimHeld(pl.t)) No(c->line, cat("push to ", M.NameWhy(pl.t)));
        auto et = pl.t->arr->sub;
        auto x = GenAs(c->ArgNodes()[1], et);
        if (Ended()) return Dummy(c->exprtype);
        if (!M.IsScalar(et) && !x.mem) No(c->line, "aggregate value");
        auto v = LimView(pl);
        auto nl = tyg_local_new(p, 8, 8);
        tyg_store(p, TYG_I64, tyg_at_local(nl, 0), v.len, 0);
        auto n = [&] { return tyg_load(p, TYG_I64, tyg_at_local(nl, 0), 0); };
        AbortUnless(tyg_cmp(p, TYG_SLT, n(), LimCapOf(pl)), "CAPACITY", c->line);
        auto at = ElemAddr(LimView(pl), n());
        Store(P { tyg_at_val(at, 0), et }, x);
        LimSetLen(pl, tyg_bin(p, TYG_ADD, n(), tyg_iconst(p, TYG_I64, 1)));
        V r;
        r.t = c->exprtype && c->exprtype->kind == TY_REF ? c->exprtype : nullptr;
        if (r.t) r.v = at;
        return r;
    }
    // pool.alloc_index(x), pool.alloc_ref(x) and pool.free(i) on a reusable
    // pool (CodeGen::EmitAlloc, B_FREE): a slot from the freelist, or a new
    // one at the top that joins the array once its element is written.
    V GenPool(Call *c) {
        auto an = c->ArgNodes();
        auto recv = an[0];
        if (auto u = Is<Unary>(recv); u && u->op == T_BITAND) recv = u->child;
        auto id = Is<Ident>(recv);
        auto vd = id ? id->vdef : nullptr;
        if (!vd || !(poolfl.count(vd) || (vd->isglobal && vd->reusable))) No(c->line, "pool of this kind");
        auto pl = VarPlace(vd, c->line);
        tyg_addr fl;
        tyg_val flstk;
        if (vd->isglobal) {
            auto [h, st] = M.PoolAccessors(vd);
            fl = tyg_at_val(tyg_load(p, TYG_I64, tyg_at_local(AccessorLocal(h), 0), 0), 0);
            flstk = tyg_load(p, TYG_I64, tyg_at_local(AccessorLocal(st), 0), 0);
        } else {
            fl = tyg_at_local(poolfl[vd].first, 0);
            flstk = tyg_load(p, TYG_I64, tyg_at_local(poolfl[vd].second, 0), 0);
        }
        auto fllen = fl;
        fllen.offset += 8;
        auto len = pl.a;
        len.offset += 8;
        auto elem = pl.t->arr->sub;
        auto esz = cg.FixedSize(elem);
        if (c->builtin == B_FREE) {
            auto i = Index64(an[1]);
            if (Ended()) return V {};
            BoundsCheck(i, tyg_load(p, TYG_I64, len, 0), c->line);
            auto top = Top(flstk);
            tyg_store(p, TYG_I64, tyg_at_val(top, 0), i, 0);
            SetTop(flstk, tyg_bin(p, TYG_ADD, top, tyg_iconst(p, TYG_I64, 8)));
            tyg_store(p, TYG_I64, fllen, tyg_bin(p, TYG_ADD, tyg_load(p, TYG_I64, fllen, 0), tyg_iconst(p, TYG_I64, 1)), 0);
            return V {};
        }
        // The value first, then the slot (the checker keeps the array from
        // growing into a fresh one meanwhile); a literal with a self-relative
        // link is built once the slot is known (CodeGen's atslot).
        auto atslot = Is<StructLit>(an[1]) && cg.HasRelRef(elem) && SelfRelLitOk(an[1]) &&
                      cg.TEq(an[1]->exprtype, elem);
        V x;
        if (!atslot) {
            x = GenAs(an[1], elem);
            if (Ended()) return Dummy(c->exprtype);
            if (!M.IsScalar(elem) && !x.mem) No(c->line, "aggregate value");
        }
        auto il = tyg_local_new(p, 8, 8);
        auto pop = tyg_block_new(p), fresh = tyg_block_new(p), got = tyg_block_new(p);
        tyg_branch(p, tyg_cmp(p, TYG_SGT, tyg_load(p, TYG_I64, fllen, 0), tyg_iconst(p, TYG_I64, 0)), pop, fresh);
        tyg_block_set(p, pop);
        tyg_store(p, TYG_I64, fllen, tyg_bin(p, TYG_SUB, tyg_load(p, TYG_I64, fllen, 0), tyg_iconst(p, TYG_I64, 1)), 0);
        auto ftop = tyg_bin(p, TYG_SUB, Top(flstk), tyg_iconst(p, TYG_I64, 8));
        SetTop(flstk, ftop);
        tyg_store(p, TYG_I64, tyg_at_local(il, 0), tyg_load(p, TYG_I64, tyg_at_val(ftop, 0), 0), 0);
        tyg_jump(p, got);
        tyg_block_set(p, fresh);
        tyg_store(p, TYG_I64, tyg_at_local(il, 0), tyg_load(p, TYG_I64, len, 0), 0);
        tyg_jump(p, got);
        tyg_block_set(p, got);
        auto idx = [&] { return tyg_load(p, TYG_I64, tyg_at_local(il, 0), 0); };
        auto base = tyg_load(p, TYG_I64, pl.a, 0);
        auto e = tyg_bin(p, TYG_ADD, base, tyg_bin(p, TYG_MUL, idx(), tyg_iconst(p, TYG_I64, esz)));
        if (atslot) SelfRelInto(P { tyg_at_val(e, 0), elem }, Is<StructLit>(an[1]));
        else Store(P { tyg_at_val(e, 0), elem }, x);
        auto grow = tyg_block_new(p), done = tyg_block_new(p);
        tyg_branch(p, tyg_cmp(p, TYG_EQ, idx(), tyg_load(p, TYG_I64, len, 0)), grow, done);
        tyg_block_set(p, grow);
        tyg_store(p, TYG_I64, len, tyg_bin(p, TYG_ADD, tyg_load(p, TYG_I64, len, 0), tyg_iconst(p, TYG_I64, 1)), 0);
        SetTop(pl.stk, tyg_bin(p, TYG_ADD, Top(pl.stk), tyg_iconst(p, TYG_I64, esz)));
        tyg_jump(p, done);
        tyg_block_set(p, done);
        V r;
        r.t = c->exprtype;
        if (c->builtin == B_ALLOC_INDEX) {
            r.v = Conv(idx(), M.ast.inttypes[IS_I64], c->exprtype, c->line);
        } else {
            auto base2 = tyg_load(p, TYG_I64, pl.a, 0);
            r.v = tyg_bin(p, TYG_ADD, base2, tyg_bin(p, TYG_MUL, idx(), tyg_iconst(p, TYG_I64, esz)));
        }
        return r;
    }

    // v.append(x) (EmitAppend): the receiver, then the source's elements
    // (a string literal's bytes, or an array's, a slice's, a literal's,
    // evaluated whole), then the length read and the capacity checked,
    // the elements copied after it.
    V LimAppend(Call *c, const P &pl) {
        if (!M.LimHeld(pl.t)) No(c->line, cat("append to ", M.NameWhy(pl.t)));
        auto et = pl.t->arr->sub;
        auto src = c->ArgNodes()[1];
        if (auto cc = Is<Call>(src); cc && cc->builtin == B_COPY && cc->ArgNodes().size() == 1)
            src = cc->ArgNodes()[0];
        View v;
        if (auto s = Is<StrLit>(src)) {
            if (!IsU8(et)) No(c->line, "string of this type");
            v = { tyg_addr_of(p, tyg_at_sym(M.Bytes(s->val), 0)),
                  tyg_iconst(p, TYG_I64, (int64_t)s->val.size()), et, -1 };
        } else {
            if (Is<Call>(src) && src->exprtype && (IsGrow(src->exprtype) || IsVarArr(src->exprtype)))
                No(c->line, "append of this call");
            auto from = Place(src);
            if (Ended()) return V {};
            v = ViewOf(from, c->line);
        }
        if (cg.Mangle(v.elem) != cg.Mangle(et)) No(c->line, "append of another element type");
        auto nl = tyg_local_new(p, 8, 8);
        tyg_store(p, TYG_I64, tyg_at_local(nl, 0), v.len, 0);
        auto n = [&] { return tyg_load(p, TYG_I64, tyg_at_local(nl, 0), 0); };
        auto ol = tyg_local_new(p, 8, 8);
        tyg_store(p, TYG_I64, tyg_at_local(ol, 0), LimLen(pl), 0);
        auto o = [&] { return tyg_load(p, TYG_I64, tyg_at_local(ol, 0), 0); };
        auto total = [&] { return tyg_bin(p, TYG_ADD, o(), n()); };
        AbortUnless(tyg_cmp(p, TYG_SLE, total(), LimCapOf(pl)), "CAPACITY", c->line);
        tyg_move_dyn(p, ElemAddr(LimView(pl), o()), v.elems,
                     tyg_bin(p, TYG_MUL, n(), tyg_iconst(p, TYG_I64, cg.FixedSize(et))));
        LimSetLen(pl, total());
        return V {};
    }
    // pop, pop_n, resize and clear (codegen_builtins.h): the receiver,
    // the arguments, the checks; only the length moves.
    V LimShrink(Call *c, const P &pl) {
        if (!M.LimHeld(pl.t)) No(c->line, cat(BuiltinByKind(c->builtin).name, " of ", M.NameWhy(pl.t)));
        auto an = c->ArgNodes();
        auto et = pl.t->arr->sub;
        auto i64 = [&](int64_t v) { return tyg_iconst(p, TYG_I64, v); };
        switch (c->builtin) {
            case B_POP: {
                auto nl = tyg_bin(p, TYG_SUB, LimLen(pl), i64(1));
                AbortUnless(tyg_cmp(p, TYG_SGE, nl, i64(0)), "POP", c->line);
                LimSetLen(pl, nl);
                return Load(P { tyg_at_val(ElemAddr(LimView(pl), LimLen(pl)), 0), et }, c->line);
            }
            case B_POP_N: {
                auto nn = Index64(an[1]);
                if (Ended()) return V {};
                auto ol = LimLen(pl);
                AbortUnless(tyg_cmp(p, TYG_ULE, nn, ol), "POPN", c->line);
                LimSetLen(pl, tyg_bin(p, TYG_SUB, ol, nn));
                return V {};
            }
            case B_CLEAR:
                LimSetLen(pl, i64(0));
                return V {};
            default: break;
        }
        // resize(n) / resize(n, fill): the count, the fill, then the length.
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
        tyg_store(p, TYG_I64, tyg_at_local(ol, 0), LimLen(pl), 0);
        auto o = [&] { return tyg_load(p, TYG_I64, tyg_at_local(ol, 0), 0); };
        AbortUnless(tyg_cmp(p, TYG_SGE, n(), i64(0)), "RESIZENEG", c->line);
        auto shrink = tyg_block_new(p), notshrink = tyg_block_new(p), grow = tyg_block_new(p),
             done = tyg_block_new(p);
        tyg_branch(p, tyg_cmp(p, TYG_SLT, n(), o()), shrink, notshrink);
        tyg_block_set(p, shrink);
        LimSetLen(pl, n());
        tyg_jump(p, done);
        tyg_block_set(p, notshrink);
        tyg_branch(p, tyg_cmp(p, TYG_SGT, n(), o()), grow, done);
        tyg_block_set(p, grow);
        if (an.size() <= 2) {
            Abort("RESIZEFILL", c->line);
        } else {
            AbortUnless(tyg_cmp(p, TYG_SLE, n(), LimCapOf(pl)), "CAPACITY", c->line);
            auto il = tyg_local_new(p, 8, 8);
            tyg_store(p, TYG_I64, tyg_at_local(il, 0), o(), 0);
            auto i = [&] { return tyg_load(p, TYG_I64, tyg_at_local(il, 0), 0); };
            auto head = tyg_block_new(p), body = tyg_block_new(p), out = tyg_block_new(p);
            tyg_jump(p, head);
            tyg_block_set(p, head);
            tyg_branch(p, tyg_cmp(p, TYG_SLT, i(), n()), body, out);
            tyg_block_set(p, body);
            Store(P { tyg_at_val(ElemAddr(LimView(pl), i()), 0), et }, fv);
            tyg_store(p, TYG_I64, tyg_at_local(il, 0), tyg_bin(p, TYG_ADD, i(), i64(1)), 0);
            tyg_jump(p, head);
            tyg_block_set(p, out);
            LimSetLen(pl, n());
            tyg_jump(p, done);
        }
        tyg_block_set(p, done);
        return V {};
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

    // default<T>() of zero bytes: a scalar's, or a fixed array's of
    // such elements (the checker's per-element default).
    static bool ZeroDefault(Node *n) {
        auto c = Is<Call>(n);
        if (!c || c->builtin != B_DEFAULT || c->rettypes.size() != 1) return false;
        auto t = c->rettypes[0];
        if (!c->defaultinit)
            return t->kind == TY_INT || t->kind == TY_FLT || t->kind == TY_BOOL || TgModule::OptRef(t);
        return t->kind == TY_ARRAY && t->arr->akind == A_FIXED && ZeroDefault(c->defaultinit);
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
        if (c->builtin == B_INDEX_OF && c->ArgNodes().size() == 2) {
            // The checker proved the reference is an element of this very
            // array (§3.3): the distance in whole elements, unchecked.
            auto an = c->ArgNodes();
            auto v = ViewOf(Place(an[0]), c->line);
            if (!cg.IsFix(v.elem)) No(c->line, "index_of of elements of variable size");
            auto r = Gen(an[1]);
            if (Ended()) return Dummy(c->exprtype);
            auto d = tyg_bin(p, TYG_SUB, r.v, v.elems);
            auto ix = tyg_bin(p, TYG_SDIV, d, tyg_iconst(p, TYG_I64, std::max<int64_t>(cg.FixedSize(v.elem), 1)));
            V x;
            x.t = c->exprtype;
            x.v = Conv(ix, M.ast.inttypes[IS_I64], c->exprtype, c->line);
            return x;
        }
        if ((c->builtin == B_ALLOC_INDEX || c->builtin == B_ALLOC_REF || c->builtin == B_FREE) &&
            c->ArgNodes().size() == 2)
            return GenPool(c);
        if (c->builtin == B_COPY && c->ArgNodes().size() == 1 && cg.IsFix(c->rettypes[0]) &&
            M.TypeOk(c->rettypes[0])) {
            // A fresh copy of a fixed value; at a slice destination, a view
            // of the copy, not of its source (CodeGen's B_COPY).
            auto rt = c->rettypes[0];
            auto x = GenAs(c->ArgNodes()[0], rt);
            if (Ended()) return Dummy(c->exprtype);
            if (M.IsScalar(rt)) return x;
            auto tmp = Temp(rt);
            tyg_copy(p, tyg_at_local(tmp, 0), x.a, M.Size(rt));
            if (c->exprtype->kind == TY_SLICE) {
                auto v = ViewOf(P { tyg_at_local(tmp, 0), rt }, c->line);
                return SliceValue(c->exprtype, v.elems, v.len);
            }
            V y;
            y.t = rt;
            y.mem = true;
            y.a = tyg_at_local(tmp, 0);
            return y;
        }
        if (c->builtin == B_BYTES_OF && c->ArgNodes().size() == 1) {
            // The element region of an array or slice as a u8[:]
            // (CodeGen::PayloadOf), for fixed elements their count times
            // their size.
            auto a = c->ArgNodes()[0];
            auto rt = a->exprtype;
            while (rt && IsAddrRef(rt)) rt = rt->ref->sub;
            if (!rt || (rt->kind != TY_ARRAY && rt->kind != TY_SLICE)) No(c->line, "bytes_of of this type");
            auto elem = rt->kind == TY_SLICE ? rt->sub : rt->arr->sub;
            if (!cg.IsFix(elem)) No(c->line, "bytes_of of variable-size elements");
            auto v = ViewOf(Place(a), c->line);
            if (Ended()) return Dummy(c->exprtype);
            auto n = tyg_bin(p, TYG_MUL, v.len, tyg_iconst(p, TYG_I64, cg.FixedSize(elem)));
            return SliceValue(c->exprtype, v.elems, n);
        }
        if (ZeroDefault(c) && M.TypeOk(c->rettypes[0])) {
            // Zero bytes (CodeGen::EmitDefaultInto).
            auto t = c->rettypes[0];
            if (M.IsScalar(t)) return Dummy(t);
            V x;
            x.t = t;
            x.mem = true;
            x.a = tyg_at_local(Temp(t), 0);
            tyg_zero(p, x.a, M.Size(t));
            return x;
        }
        if (c->builtin == B_DEFAULT && c->rettypes.size() == 1 && cg.IsFix(c->rettypes[0]) &&
            M.TypeOk(c->rettypes[0])) {
            // A fixed value from its type's defaults (CodeGen's B_DEFAULT):
            // the default initializer, per element for a fixed array.
            auto t = c->rettypes[0];
            auto tmp = Temp(t);
            P pl { tyg_at_local(tmp, 0), t };
            tyg_zero(p, pl.a, M.Size(t));
            if (c->defaultinit && t->kind == TY_ARRAY && t->arr->akind == A_FIXED) {
                auto et = t->arr->sub;
                auto il = tyg_local_new(p, 8, 8);
                tyg_store(p, TYG_I64, tyg_at_local(il, 0), tyg_iconst(p, TYG_I64, 0), 0);
                auto head = tyg_block_new(p), body = tyg_block_new(p), done = tyg_block_new(p);
                tyg_jump(p, head);
                tyg_block_set(p, head);
                auto i = tyg_load(p, TYG_I64, tyg_at_local(il, 0), 0);
                tyg_branch(p, tyg_cmp(p, TYG_SLT, i, tyg_iconst(p, TYG_I64, cg.ArrSize(t->arr))), body, done);
                tyg_block_set(p, body);
                auto at = tyg_bin(p, TYG_ADD, tyg_addr_of(p, pl.a),
                                  tyg_bin(p, TYG_MUL, i, tyg_iconst(p, TYG_I64, cg.FixedSize(et))));
                Into(P { tyg_at_val(at, 0), et }, c->defaultinit);
                if (!Ended()) {
                    auto i2 = tyg_load(p, TYG_I64, tyg_at_local(il, 0), 0);
                    tyg_store(p, TYG_I64, tyg_at_local(il, 0), tyg_bin(p, TYG_ADD, i2, tyg_iconst(p, TYG_I64, 1)), 0);
                    tyg_jump(p, head);
                }
                tyg_block_set(p, done);
            } else if (c->defaultinit) {
                Into(pl, c->defaultinit);
            }
            if (Ended()) return Dummy(t);
            if (M.IsScalar(t)) return Load(pl, c->line);
            V x;
            x.t = t;
            x.mem = true;
            x.a = pl.a;
            return x;
        }
        if (c->builtin >= 0) No(c->line, cat("builtin ", BuiltinByKind(c->builtin).name));
        if (c->fvbody) return GenFvCall(c);
        if (!c->dispatch.empty()) return GenDispatch(c);
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
        FvArgs(c, ci, args);
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
            if (IsImg(r.t)) {
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
        RfCheck(callee);
        for (size_t i = 0; i < rets.size(); i++) {
            if ((int)i == ci.cret) lastrets.push_back(x);
            else if (M.IsScalar(rets[i].t)) lastrets.push_back(Load(P { rets[i].a, rets[i].t }, c->line));
            else lastrets.push_back(rets[i]);
        }
        return lastrets.empty() ? V {} : lastrets[0];
    }

    void FvArgs(Call *c, CodeGen::SpecInfo &ci, vector<tyg_arg> &args) {
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
    }

    // A call of a function value (§7.6; CodeGen::EmitFvCall): the checked
    // body instance spliced in here, its parameters ordinary locals bound
    // to the arguments in order, in a scope of its own. Its value is a
    // copy (§4.1), so that a slice of it never points into a variable the
    // body names.
    V GenFvCall(Call *c) {
        auto et = c->fvbody->exprtype;
        auto wantsval = !cg.IsVoidT(et);
        if (wantsval && !M.TypeOk(et) && IsImg(et) && M.ImgOk(et)) {
            // An image: built at the top of a temporary's stack.
            auto stk = Stk(AllocStk(false, c->line), c->line);
            V x;
            x.t = et;
            x.mem = true;
            x.a = tyg_at_val(GenImg(c, stk, et, c->line), 0);
            return x;
        }
        if (wantsval && !M.TypeOk(et) && IsGrow(et) && M.RzOk(et)) {
            auto x = RzTemp(et, c->line);
            GenRz(c, x.stk, x.a, et, c->line);
            return x;
        }
        if (wantsval && !M.TypeOk(et)) No(c->line, cat("function value of type ", M.Name(et)));
        if (c->fvparams.size() > c->args.size()) No(c->line, "function value parameters");
        if (Ended()) return Dummy(et);
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
        if (TgModule::IsLim(pl.t)) return LimPush(c, pl);
        if (IsGrow(pl.t) && M.VeOk(pl.t) && pl.stk) {
            // An image element: built straight at the top of the array's own
            // stack, behind the elements before it, then counted.
            auto la = pl.a;
            la.offset += 8;
            auto at = GenImg(an[1], pl.stk, pl.t->arr->sub, c->line);
            if (Ended()) return Dummy(c->exprtype);
            tyg_store(p, TYG_I64, la, tyg_bin(p, TYG_ADD, tyg_load(p, TYG_I64, la, 0), tyg_iconst(p, TYG_I64, 1)), 0);
            V r;
            r.t = c->exprtype && c->exprtype->kind == TY_REF ? c->exprtype : nullptr;
            if (r.t) r.v = at;
            return r;
        }
        if (!IsGrow(pl.t) || !M.RzOk(pl.t) || !pl.stk) No(c->line, "push to an array of this kind");
        auto et = pl.t->arr->sub;
        if (SelfRelLit(an[1]) && cg.TEq(an[1]->exprtype, et)) {
            // Built in its slot at the top, then counted.
            auto slot = Top(pl.stk);
            auto sv = tyg_local_new(p, 8, 8);
            tyg_store(p, TYG_I64, tyg_at_local(sv, 0), slot, 0);
            SelfRelInto(P { tyg_at_val(slot, 0), et }, Is<StructLit>(an[1]));
            if (Ended()) return Dummy(c->exprtype);
            auto at = tyg_load(p, TYG_I64, tyg_at_local(sv, 0), 0);
            SetTop(pl.stk, tyg_bin(p, TYG_ADD, at, tyg_iconst(p, TYG_I64, cg.FixedSize(et))));
            auto la = pl.a;
            la.offset += 8;
            tyg_store(p, TYG_I64, la, tyg_bin(p, TYG_ADD, tyg_load(p, TYG_I64, la, 0), tyg_iconst(p, TYG_I64, 1)), 0);
            V r;
            r.t = c->exprtype && c->exprtype->kind == TY_REF ? c->exprtype : nullptr;
            if (r.t) r.v = at;
            return r;
        }
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
        if (TgModule::IsLim(pl.t)) return LimAppend(c, pl);
        if (!IsGrow(pl.t) || !M.RzOk(pl.t) || !pl.stk) No(c->line, "append to an array of this kind");
        AppendSrc(pl, an[1], c->line);
        return V {};
    }
    // The elements of src added to the resizable at pl.
    void AppendSrc(const P &pl, Node *src, Line l) {
        auto et = pl.t->arr->sub;
        auto len = pl.a;
        len.offset += 8;
        if (auto ie = Is<IfExpr>(src); ie && ie->elseb) {
            BuildIf(ie, l, [&](Node *v) { AppendSrc(pl, v, l); });
            return;
        }
        if (auto cc = Is<Call>(src); cc && cc->builtin == B_COPY && cc->ArgNodes().size() == 1)
            src = cc->ArgNodes()[0];
        auto st = src->exprtype;
        if (Is<Call>(src) && st && IsGrow(st) && !Is<Call>(src)->spec) No(l, "append of this call");
        if (auto cc = Is<Call>(src); cc && cc->builtin < 0 && st && IsGrow(st)) {
            auto tmp = tyg_local_new(p, 16, 8);
            GenRz(src, pl.stk, tyg_at_local(tmp, 0), st, l);
            if (Ended()) return;
            tyg_store(p, TYG_I64, len,
                      tyg_bin(p, TYG_ADD, tyg_load(p, TYG_I64, len, 0),
                              tyg_load(p, TYG_I64, tyg_at_local(tmp, 8), 0)), 0);
            return;
        }
        if (auto s = Is<StrLit>(src)) {
            if (!IsU8(et)) No(l, "string of this type");
            CopyRun(pl.stk, len, et, tyg_addr_of(p, tyg_at_sym(M.Bytes(s->val), 0)),
                    tyg_iconst(p, TYG_I64, (int64_t)s->val.size()));
            return;
        }
        // A literal is a fixed array, all of it evaluated before any of it
        // is copied, as in the C.
        auto from = Place(src);
        if (Ended()) return;
        auto v = ViewOf(from, l);
        if (cg.Mangle(v.elem) != cg.Mangle(et)) No(l, "append of another element type");
        CopyRun(pl.stk, len, et, v.elems, v.len);
        return;
    }
    static bool IsGrow(TypeExpr *t) { return TgModule::IsGrow(t); }

    // The text of an[from..] appended at the top of stk to the u8
    // resizable whose count is at len (EmitStr, EmitFormatInto), each
    // argument evaluated and written in turn: a scalar formatted by the
    // runtime's gs_fmt_* straight at the top, a u8 array or slice copied,
    // anything else rendered structurally (RenderInto).
    // The call whose user format overloads the value being rendered uses
    // (CodeGen::FmtContext), or none.
    Call *fmtc = nullptr;

    // A type's name as text renders it: the checked type the C renders is
    // the value's, which a `const` variable's place does not drop.
    static string TextName(TypeExpr *t) {
        auto s = TgModule::Name(t);
        if (t->cq && s.rfind("const ", 0) == 0) s = s.substr(6);
        return s;
    }

    // A user `format` overload rendering the value at pl (EmitUserFormat):
    // handed the builder as a gs_rref -- its header, the { base, len } that
    // len is in, and its stack -- and the value as the overload takes it.
    void UserFormat(tyg_val stk, tyg_addr len, P pl, FnSpec *sp, Line l) {
        string why;
        if (!M.SigOk(sp, false, why)) No(l, cat("format overload: ", why));
        auto &ci = cg.sinfo[sp];
        if (!ci.freevars.empty()) No(l, "format overload with free variables");
        auto pt = sp->argtypes[1];
        if (cg.IsLargeFixed(pt)) No(l, "format overload of a large value");
        auto hdr = len;
        hdr.offset -= 8;
        auto r = tyg_local_new(p, 16, 8);
        tyg_store(p, TYG_I64, tyg_at_local(r, 0), tyg_addr_of(p, hdr), 0);
        tyg_store(p, TYG_I64, tyg_at_local(r, 8), stk, 0);
        vector<tyg_arg> args;
        tyg_arg out {};
        out.mem = tyg_at_local(r, 0);
        args.push_back(out);
        tyg_arg v {};
        if (pl.enc || TgModule::IsVarint(pl.t)) {
            // An i64 read out of varint storage, which holds no i64: the
            // overload is handed a temporary holding its value (RefArg).
            auto i64t = M.ast.inttypes[IS_I64];
            auto tmp = Temp(i64t);
            tyg_store(p, TYG_I64, tyg_at_local(tmp, 0), Load(pl, l).v, 0);
            pl = P { tyg_at_local(tmp, 0), i64t };
        }
        if (pt->kind == TY_REF && cg.IsFatRef(pt)) {
            // A resizable by reference: gs_rref, its header's address and
            // its stack.
            if (!pl.stk) No(l, "format overload taking a resizable without its stack");
            auto fr = tyg_local_new(p, 16, 8);
            tyg_store(p, TYG_I64, tyg_at_local(fr, 0), tyg_addr_of(p, pl.a), 0);
            tyg_store(p, TYG_I64, tyg_at_local(fr, 8), pl.stk, 0);
            v.mem = tyg_at_local(fr, 0);
        } else if (pt->kind == TY_REF) {
            v.val = tyg_addr_of(p, pl.a);
        } else if (M.IsScalar(pt)) {
            v.val = Load(P { pl.a, pt }, l).v;
        } else if (M.RzParam(pt)) {
            No(l, "format overload taking a variable-size value");
        } else {
            v.mem = pl.a;
        }
        args.push_back(v);
        if (ci.needssp) {
            tyg_arg s {};
            s.val = CalleeSp(sp, l);
            args.push_back(s);
        }
        auto sym = M.Sym(ci.cname, TYG_SYM_FUNC | TYG_SYM_GLOBAL);
        Result(sp, ci.cret, M.Sig(sp), tyg_direct(sym), args);
        RfCheck(sp);
    }

    void TextInto(tyg_val stk, tyg_addr len, Call *c, const vector<Node *> &an, size_t from) {
        auto outer = fmtc;
        struct Restore {
            Call *&at, *was;
            ~Restore() { at = was; }
        } restore { fmtc, outer };
        for (size_t i = from; i < an.size(); i++) {
            auto a = an[i];
            fmtc = cg.FmtContext(c, a);
            auto bt = a->exprtype;
            while (bt && IsPlainRef(bt)) bt = bt->ref->sub;
            if (fmtc && TgModule::IsVarint(bt)) {
                // A varint read out of its storage is an i64 (§3.6): an i64
                // overload is handed a temporary holding its value.
                auto i64t = M.ast.inttypes[IS_I64];
                if (auto sp = cg.FmtSpecFor(fmtc, i64t)) {
                    auto x = GenAs(a, i64t);
                    if (Ended()) return;
                    auto tmp = Temp(i64t);
                    tyg_store(p, TYG_I64, tyg_at_local(tmp, 0), x.v, 0);
                    UserFormat(stk, len, P { tyg_at_local(tmp, 0), i64t }, sp, a->line);
                    continue;
                }
            }
            if (fmtc && bt && cg.FmtSpecFor(fmtc, bt)) {
                // The overload renders it, whatever its type. A function
                // value's reference result is spliced in as a value, where
                // the overload must get the storage it names.
                if (auto fc = Is<Call>(a); fc && fc->fvbody && a->exprtype && a->exprtype->kind == TY_REF)
                    No(a->line, "format overload of a function value's reference result");
                // A call's reference result is rendered where it points, as
                // a reference variable is, not as the value the checker
                // decayed it to.
                P pl;
                auto fc = Is<Call>(a);
                if (fc && fc->builtin < 0 && fc->spec && !fc->spec->rets.empty() &&
                    fc->spec->rets[0]->kind == TY_REF && IsPlainRef(fc->spec->rets[0])) {
                    auto x = Gen1(a);
                    if (Ended()) return;
                    pl = RefTarget(x);
                } else {
                    pl = Place(a);
                }
                if (Ended()) return;
                if (pl.enc || TgModule::IsVarint(pl.t)) {
                    // The overload found for the argument's checked type,
                    // an i64, which a varint place's own type is not.
                    UserFormat(stk, len, pl, cg.FmtSpecFor(fmtc, bt), a->line);
                    continue;
                }
                RenderInto(stk, len, pl, false, a->line);
                continue;
            }
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
            if (!ot || !M.IsScalar(ot) || ot->kind == TY_REF) {
                auto pl = Place(a);
                if (Ended()) return;
                RenderInto(stk, len, pl, false, a->line);
                continue;
            }
            auto x = GenAs(a, ot).v;
            if (Ended()) return;
            FmtScalar(stk, len, ot, x);
        }
    }

    // A scalar x of type t formatted at the top of stk by the runtime's
    // gs_fmt_*, its bytes added to the count at len.
    void FmtScalar(tyg_val stk, tyg_addr len, TypeExpr *t, tyg_val x) {
        tyg_val n;
        if (t->kind == TY_INT && t->intstorage == IS_U64) {
            n = CallC("gs_fmt_u64", { tyg_type_ptr(m), IT(TYG_I64, false) }, { Top(stk), x }, IT(TYG_I64));
        } else if (t->kind == TY_INT) {
            x = Conv(x, t, M.ast.inttypes[IS_I64], Line {});
            n = CallC("gs_fmt_i64", { tyg_type_ptr(m), IT(TYG_I64) }, { Top(stk), x }, IT(TYG_I64));
        } else if (t->kind == TY_FLT && M.Scal(t) == TYG_F32) {
            n = CallC("gs_fmt_f32", { tyg_type_ptr(m), tyg_type_scalar(m, TYG_F32) }, { Top(stk), x },
                      IT(TYG_I64));
        } else if (t->kind == TY_FLT) {
            n = CallC("gs_fmt_f64", { tyg_type_ptr(m), tyg_type_scalar(m, TYG_F64) }, { Top(stk), x },
                      IT(TYG_I64));
        } else {
            n = CallC("gs_fmt_bool", { tyg_type_ptr(m), IT(TYG_I64) },
                      { Top(stk), tyg_convert(p, TYG_ZEXT, TYG_I64, x) }, IT(TYG_I64));
        }
        SetTop(stk, tyg_bin(p, TYG_ADD, Top(stk), n));
        tyg_store(p, TYG_I64, len, tyg_bin(p, TYG_ADD, tyg_load(p, TYG_I64, len, 0), n), 0);
    }
    // Literal text appended as FmtScalar appends.
    void TextLit(tyg_val stk, tyg_addr len, const string &text) {
        CopyRun(stk, len, M.ast.inttypes[IS_U8], tyg_addr_of(p, tyg_at_sym(M.Bytes(text), 0)),
                tyg_iconst(p, TYG_I64, (int64_t)text.size()));
    }

    // The value at pl rendered structurally, as CodeGen::RenderLoc renders
    // it: through references to what they refer to, a scalar by gs_fmt_*,
    // u8 elements as their bytes (gs_fmt_quoted's quoted form when nested
    // in a larger value), other arrays and slices as `[a, b]` with their
    // elements and count read once, a struct as `Name { a, b }`. The types
    // held are fixed or arrays of fixed elements, none reaching itself
    // through references, so no render functions are needed.
    void RenderInto(tyg_val stk, tyg_addr len, P pl, bool nested, Line l, int depth = 0) {
        if (depth > 32) No(l, "text of a type this deep");
        while (IsPlainRef(pl.t)) pl = Step(pl);
        auto t = pl.t;
        if (fmtc) {
            if (auto sp = cg.FmtSpecFor(fmtc, t)) {
                UserFormat(stk, len, pl, sp, l);
                return;
            }
        }
        if (M.OptRef(t)) {
            RenderOpt(stk, len, pl, nested, l, depth);
            return;
        }
        switch (t->kind) {
            case TY_INT:
                if (t->intstorage == IS_VARINT) {
                    FmtScalar(stk, len, M.ast.inttypes[IS_I64], Load(pl, l).v);
                    return;
                }
                [[fallthrough]];
            case TY_FLT: case TY_BOOL:
                FmtScalar(stk, len, t, Load(pl, l).v);
                return;
            case TY_STRUCT: {
                if (M.VsOk(t)) {
                    VsRender(stk, len, pl, l, depth);
                    return;
                }
                if (M.FoOk(t)) {
                    auto si = cg.SI(t);
                    TextLit(stk, len, cat(TextName(t), " { "));
                    auto first = true;
                    for (size_t i = 0; i < si->st->fields.size(); i++) {
                        if (si->st->fields[i].ispad) continue;
                        if (!first) TextLit(stk, len, ", ");
                        first = false;
                        RenderInto(stk, len, FoFieldPlace(pl, (int)i, l), true, l, depth + 1);
                    }
                    TextLit(stk, len, " }");
                    return;
                }
                if (!M.TypeOk(t)) break;
                auto si = cg.SI(t);
                auto &lay = cg.StructLayout(si);
                TextLit(stk, len, cat(TextName(t), " { "));
                auto first = true;
                for (size_t i = 0; i < si->st->fields.size(); i++) {
                    if (si->st->fields[i].ispad) continue;
                    if (!first) TextLit(stk, len, ", ");
                    first = false;
                    auto a = pl.a;
                    a.offset += lay.offs[i];
                    RenderInto(stk, len, P { a, si->ftypes[i] }, true, l, depth + 1);
                }
                TextLit(stk, len, " }");
                return;
            }
            case TY_ARRAY: case TY_SLICE: {
                auto v = ViewOf(pl, l);
                if (IsU8(v.elem)) {
                    if (!nested) {
                        CopyRun(stk, len, v.elem, v.elems, v.len);
                        return;
                    }
                    auto n = CallC("gs_fmt_quoted", { tyg_type_ptr(m), tyg_type_ptr(m), IT(TYG_I64) },
                                   { Top(stk), v.elems, v.len }, IT(TYG_I64));
                    SetTop(stk, tyg_bin(p, TYG_ADD, Top(stk), n));
                    tyg_store(p, TYG_I64, len, tyg_bin(p, TYG_ADD, tyg_load(p, TYG_I64, len, 0), n), 0);
                    return;
                }
                TextLit(stk, len, "[");
                auto il = tyg_local_new(p, 8, 8);
                auto i = [&] { return tyg_load(p, TYG_I64, tyg_at_local(il, 0), 0); };
                tyg_store(p, TYG_I64, tyg_at_local(il, 0), tyg_iconst(p, TYG_I64, 0), 0);
                // The element's address, walked on from the first.
                auto cl = tyg_local_new(p, 8, 8);
                auto cur = [&] { return tyg_load(p, TYG_I64, tyg_at_local(cl, 0), 0); };
                tyg_store(p, TYG_I64, tyg_at_local(cl, 0), v.elems, 0);
                auto head = tyg_block_new(p), body = tyg_block_new(p), sep = tyg_block_new(p),
                     elem = tyg_block_new(p), out = tyg_block_new(p);
                tyg_jump(p, head);
                tyg_block_set(p, head);
                tyg_branch(p, tyg_cmp(p, TYG_SLT, i(), v.len), body, out);
                tyg_block_set(p, body);
                tyg_branch(p, tyg_cmp(p, TYG_NE, i(), tyg_iconst(p, TYG_I64, 0)), sep, elem);
                tyg_block_set(p, sep);
                TextLit(stk, len, ", ");
                tyg_jump(p, elem);
                tyg_block_set(p, elem);
                RenderInto(stk, len, P { tyg_at_val(cur(), 0), v.elem }, true, l, depth + 1);
                tyg_store(p, TYG_I64, tyg_at_local(cl, 0), StepElem(v, cur()), 0);
                tyg_store(p, TYG_I64, tyg_at_local(il, 0), tyg_bin(p, TYG_ADD, i(), tyg_iconst(p, TYG_I64, 1)), 0);
                tyg_jump(p, head);
                tyg_block_set(p, out);
                TextLit(stk, len, "]");
                return;
            }
            default: break;
        }
        No(l, cat("text of type ", M.NameWhy(t)));
    }

    // gs_abort(GS_E_<what>) with the Goose file and line, through a shim
    // (the codes are the C half's enum); the block ends there.
    void Abort(const string &what, Line l) {
        auto name = cat("gs_tg_abort_", what);
        M.NeedShim(name);
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
        if (TgModule::IsLim(pl.t)) return LimShrink(c, pl);
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
        if (TgModule::IsLim(pl.t) && M.LimHeld(pl.t) && IsU8(pl.t->arr->sub)) {
            // Into a static-capacity u8[..k] (CodeGen::AppendBytes): the
            // text rendered into a builder of the statement's, then its
            // bytes appended under the capacity check.
            auto b = RzTemp(cg.GrowU8(), c->line);
            auto blen = b.a;
            blen.offset += 8;
            tyg_store(p, TYG_I64, b.a, Top(b.stk), 0);
            tyg_store(p, TYG_I64, blen, tyg_iconst(p, TYG_I64, 0), 0);
            TextInto(b.stk, blen, c, an, 1);
            if (Ended()) return V {};
            auto n = tyg_load(p, TYG_I64, blen, 0);
            auto ol = LimLen(pl);
            AbortUnless(tyg_cmp(p, TYG_SLE, tyg_bin(p, TYG_ADD, ol, n), LimCapOf(pl)),
                        "CAPACITY", c->line);
            auto v = LimView(pl);
            tyg_copy_dyn(p, tyg_bin(p, TYG_ADD, v.elems, LimLen(pl)), tyg_load(p, TYG_I64, b.a, 0), n);
            LimSetLen(pl, tyg_bin(p, TYG_ADD, LimLen(pl), n));
            return V {};
        }
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
        // its own of the same extern. A function the runtime object
        // exports needs none.
        auto direct = M.RuntimeApi(callee->sf->cname);
        auto &list = direct ? M.direct : M.externs;
        auto known = false;
        for (auto e : list) known = known || e->sf->cname == callee->sf->cname;
        if (!known) list.push_back(callee);
        if (Ended()) return Dummy(callee->rets.empty() ? nullptr : callee->rets[0]);
        auto sym = M.Sym(direct ? callee->sf->cname : cat("gs_tg_x_", callee->sf->cname),
                         TYG_SYM_FUNC | TYG_SYM_GLOBAL);
        return Result(callee, callee->rets.empty() ? -1 : 0, M.ExternSig(callee), tyg_direct(sym), args);
    }

    // print(...) (§3.7): every argument is evaluated before any text is
    // written, as the C backend renders the whole line first.
    V GenPrint(Call *c) {
        // A value rendered structurally: the whole line is rendered into a
        // builder on a stack of the statement's and written at once, as
        // the C backend writes every line (B_PRINT).
        auto simple = [&](Node *a) {
            if (Is<StrLit>(a)) return true;
            auto bt = a->exprtype;
            while (bt && IsPlainRef(bt)) bt = bt->ref->sub;
            auto fc = cg.FmtContext(c, a);
            if (bt && cg.FmtSpecFor(fc, bt)) return false;
            if (TgModule::IsVarint(bt) && cg.FmtSpecFor(fc, M.ast.inttypes[IS_I64])) return false;
            if (bt && ((bt->kind == TY_ARRAY && IsU8(bt->arr->sub)) || (bt->kind == TY_SLICE && IsU8(bt->sub))))
                return true;
            auto ot = cg.OperandT(a->exprtype);
            return ot && M.IsScalar(ot) && ot->kind != TY_REF;
        };
        auto an = c->ArgNodes();
        if (!std::all_of(an.begin(), an.end(), simple)) {
            auto b = RzTemp(cg.GrowU8(), c->line);
            auto len = b.a;
            len.offset += 8;
            tyg_store(p, TYG_I64, b.a, Top(b.stk), 0);
            tyg_store(p, TYG_I64, len, tyg_iconst(p, TYG_I64, 0), 0);
            TextInto(b.stk, len, c, an, 0);
            if (Ended()) return V {};
            CallC("gs_out_bytes", { tyg_type_ptr(m), IT(TYG_I64) },
                  { tyg_load(p, TYG_I64, b.a, 0), tyg_load(p, TYG_I64, len, 0) }, nullptr);
            CallC("gs_out_nl", {}, {}, nullptr);
            return V {};
        }
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
            if (!ot || !M.IsScalar(ot) || ot->kind == TY_REF) No(a->line, cat("print of type ", M.NameWhy(ot)));
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

    // ---- Optionals -----------------------------------------------------------

    // Optionals (§3.8) as the C backend lowers them: a T? is the address a
    // T& would be, 0 for null (a resizable's fat reference with a null
    // hdr), so a narrowed one is used as that reference, and a test reads
    // the address alone (Binary::CgX, GenTruth).

    // A reference that is an address: a plain one, or an optional one
    // used where the checker has narrowed it (a path through it, a value
    // read through it), which is the same address.
    static bool IsAddrRef(const TypeExpr *t) { return IsPlainRef(t) || TgModule::OptRef(t); }
    // A fat reference where one differing only in optionality is wanted
    // (T& to T?, a narrowed T? to T&): the same two words.
    bool OptFit(V &x, TypeExpr *want) {
        if (x.t->kind != TY_REF || want->kind != TY_REF || !cg.IsFatRef(x.t) || !cg.IsFatRef(want) ||
            x.t->ref->lenstorage >= 0 || want->ref->lenstorage >= 0 || !cg.TEq(x.t->ref->sub, want->ref->sub))
            return false;
        x.t = want;
        return true;
    }
    // The null of an optional (or of the checker's bare null type).
    V Null(TypeExpr *t, Line l) {
        V x;
        x.t = t;
        if (t && t->kind == TY_REF && cg.IsFatRef(t)) {
            auto tmp = tyg_local_new(p, 16, 8);
            tyg_zero(p, tyg_at_local(tmp, 0), 16);
            x.mem = true;
            x.a = tyg_at_local(tmp, 0);
            return x;
        }
        if (!t || t->kind != TY_REF || (t->ref->lenstorage >= 0 && !M.RelOk(t, 1))) No(l, "null of this type");
        x.v = tyg_iconst(p, TYG_I64, 0);
        return x;
    }
    // A reference value's address: a fat reference's hdr.
    tyg_val Addr(const V &x, Line l) {
        if (!x.t || x.t->kind != TY_REF || (x.t->ref->lenstorage >= 0 && !M.RelOk(x.t, 1))) No(l, "address of this value");
        if (x.mem) return tyg_load(p, TYG_I64, x.a, 0);
        return x.v;
    }
    // A reference as a test or identity sees it: a variable's own value,
    // even where it is narrowed; null; anything else's value.
    tyg_val RefAddr(Node *n) {
        if (Is<NullLit>(n)) return tyg_iconst(p, TYG_I64, 0);
        if (auto id = Is<Ident>(n); id && id->vdef && id->vdef->type && id->vdef->type->kind == TY_REF) {
            auto x = Load(VarPlace(id->vdef, n->line), n->line);
            return Ended() ? tyg_iconst(p, TYG_I64, 0) : Addr(x, n->line);
        }
        auto x = Gen(n);
        return Ended() ? tyg_iconst(p, TYG_I64, 0) : Addr(x, n->line);
    }
    // Whether a condition is an optional's null test: an optional
    // variable (narrowed already or not), or a value of an optional type.
    bool OptCond(Node *n) {
        if (auto id = Is<Ident>(n); id && id->vdef && id->vdef->type && IsOptional(id->vdef->type)) return true;
        return n->exprtype && IsOptional(n->exprtype);
    }
    tyg_val OptTest(Node *n) { return tyg_cmp(p, TYG_NE, RefAddr(n), tyg_iconst(p, TYG_I64, 0)); }
    // `a == null`, `a != null`, and identity `a .== b`, `a .!= b`: the
    // addresses, left first.
    V GenRefEq(Binary *b) {
        auto l = RefAddr(b->left);
        auto r = RefAddr(b->right);
        V x;
        x.t = M.ast.booltype;
        x.v = tyg_cmp(p, b->op == T_EQ || b->op == T_DOTEQ ? TYG_EQ : TYG_NE, l, r);
        return x;
    }
    // An optional's text (CodeGen::RenderLoc): `null`, or its pointee's.
    // A pointee that reaches its own type (a list) is rendered by the C
    // half's recursive render functions; its function stays in C.
    void RenderOpt(tyg_val stk, tyg_addr len, const P &pl, bool nested, Line l, int depth) {
        if (M.Recursive(pl.t->ref->sub)) No(l, cat("text of type ", M.Name(pl.t), " [recursive]"));
        auto none = tyg_block_new(p), some = tyg_block_new(p), done = tyg_block_new(p);
        tyg_branch(p, tyg_cmp(p, TYG_EQ, tyg_load(p, TYG_I64, pl.a, 0), tyg_iconst(p, TYG_I64, 0)), none, some);
        tyg_block_set(p, none);
        TextLit(stk, len, "null");
        tyg_jump(p, done);
        tyg_block_set(p, some);
        RenderInto(stk, len, Step(pl), nested, l, depth + 1);
        Jump(done);
        tyg_block_set(p, done);
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
                if (x.t && (IsGrow(x.t) || IsImg(x.t))) No(b->tail->line, "array value of a block");
            }
        }
        CloseScope();
        tyg_scope_end(p);
        return x;
    }

    // ---- Varints in bytes values (§3.6: ULEB128, values zigzag) --------------

    // The ULEB128 at `at`, unsigned.
    tyg_val UlebRead(tyg_val at) {
        auto v = tyg_local_new(p, 8, 8), sh = tyg_local_new(p, 8, 8), q = tyg_local_new(p, 8, 8);
        tyg_store(p, TYG_I64, tyg_at_local(v, 0), tyg_iconst(p, TYG_I64, 0), 0);
        tyg_store(p, TYG_I64, tyg_at_local(sh, 0), tyg_iconst(p, TYG_I64, 0), 0);
        tyg_store(p, TYG_I64, tyg_at_local(q, 0), at, 0);
        auto head = tyg_block_new(p), more = tyg_block_new(p), done = tyg_block_new(p);
        tyg_jump(p, head);
        tyg_block_set(p, head);
        auto qa = tyg_load(p, TYG_I64, tyg_at_local(q, 0), 0);
        auto b = tyg_convert(p, TYG_ZEXT, TYG_I64, tyg_load(p, TYG_I8, tyg_at_val(qa, 0), 0));
        auto bits = tyg_bin(p, TYG_SHL, tyg_bin(p, TYG_AND, b, tyg_iconst(p, TYG_I64, 0x7f)),
                            tyg_load(p, TYG_I64, tyg_at_local(sh, 0), 0));
        tyg_store(p, TYG_I64, tyg_at_local(v, 0), tyg_bin(p, TYG_OR, tyg_load(p, TYG_I64, tyg_at_local(v, 0), 0), bits), 0);
        tyg_store(p, TYG_I64, tyg_at_local(q, 0), tyg_bin(p, TYG_ADD, qa, tyg_iconst(p, TYG_I64, 1)), 0);
        tyg_branch(p, tyg_cmp(p, TYG_NE, tyg_bin(p, TYG_AND, b, tyg_iconst(p, TYG_I64, 0x80)), tyg_iconst(p, TYG_I64, 0)),
                   more, done);
        tyg_block_set(p, more);
        tyg_store(p, TYG_I64, tyg_at_local(sh, 0),
                  tyg_bin(p, TYG_ADD, tyg_load(p, TYG_I64, tyg_at_local(sh, 0), 0), tyg_iconst(p, TYG_I64, 7)), 0);
        tyg_jump(p, head);
        tyg_block_set(p, done);
        return tyg_load(p, TYG_I64, tyg_at_local(v, 0), 0);
    }
    // A varint value: (u >> 1) ^ -(u & 1) (gs_zig_read).
    tyg_val ZigRead(tyg_val at) {
        auto u = UlebRead(at);
        auto half = tyg_bin(p, TYG_LSHR, u, tyg_iconst(p, TYG_I64, 1));
        auto sign = tyg_un(p, TYG_NEG, tyg_bin(p, TYG_AND, u, tyg_iconst(p, TYG_I64, 1)));
        return tyg_bin(p, TYG_XOR, half, sign);
    }
    // The bytes of the ULEB128 at `at` (gs_uleb_size).
    tyg_val UlebSize(tyg_val at) {
        auto q = tyg_local_new(p, 8, 8);
        tyg_store(p, TYG_I64, tyg_at_local(q, 0), at, 0);
        auto head = tyg_block_new(p), more = tyg_block_new(p), done = tyg_block_new(p);
        tyg_jump(p, head);
        tyg_block_set(p, head);
        auto qa = tyg_load(p, TYG_I64, tyg_at_local(q, 0), 0);
        auto b = tyg_convert(p, TYG_ZEXT, TYG_I64, tyg_load(p, TYG_I8, tyg_at_val(qa, 0), 0));
        tyg_store(p, TYG_I64, tyg_at_local(q, 0), tyg_bin(p, TYG_ADD, qa, tyg_iconst(p, TYG_I64, 1)), 0);
        tyg_branch(p, tyg_cmp(p, TYG_NE, tyg_bin(p, TYG_AND, b, tyg_iconst(p, TYG_I64, 0x80)), tyg_iconst(p, TYG_I64, 0)),
                   more, done);
        tyg_block_set(p, more);
        tyg_jump(p, head);
        tyg_block_set(p, done);
        return tyg_bin(p, TYG_SUB, tyg_load(p, TYG_I64, tyg_at_local(q, 0), 0), at);
    }
    // Writes the varint value v at `at`, zigzag ULEB128 (gs_zig_write); its
    // byte count.
    tyg_val ZigWrite(tyg_val at, tyg_val v) {
        auto u = tyg_local_new(p, 8, 8), q = tyg_local_new(p, 8, 8);
        tyg_store(p, TYG_I64, tyg_at_local(u, 0),
                  tyg_bin(p, TYG_XOR, tyg_bin(p, TYG_SHL, v, tyg_iconst(p, TYG_I64, 1)),
                          tyg_bin(p, TYG_ASHR, v, tyg_iconst(p, TYG_I64, 63))), 0);
        tyg_store(p, TYG_I64, tyg_at_local(q, 0), at, 0);
        auto head = tyg_block_new(p), more = tyg_block_new(p), last = tyg_block_new(p), done = tyg_block_new(p);
        tyg_jump(p, head);
        tyg_block_set(p, head);
        auto uv = tyg_load(p, TYG_I64, tyg_at_local(u, 0), 0);
        auto b = tyg_bin(p, TYG_AND, uv, tyg_iconst(p, TYG_I64, 0x7f));
        auto rest = tyg_bin(p, TYG_LSHR, uv, tyg_iconst(p, TYG_I64, 7));
        tyg_store(p, TYG_I64, tyg_at_local(u, 0), rest, 0);
        auto qa = tyg_load(p, TYG_I64, tyg_at_local(q, 0), 0);
        tyg_store(p, TYG_I64, tyg_at_local(q, 0), tyg_bin(p, TYG_ADD, qa, tyg_iconst(p, TYG_I64, 1)), 0);
        tyg_branch(p, tyg_cmp(p, TYG_NE, rest, tyg_iconst(p, TYG_I64, 0)), more, last);
        tyg_block_set(p, more);
        tyg_store(p, TYG_I8, tyg_at_val(qa, 0),
                  tyg_convert(p, TYG_TRUNC, TYG_I8, tyg_bin(p, TYG_OR, b, tyg_iconst(p, TYG_I64, 0x80))), 0);
        tyg_jump(p, head);
        tyg_block_set(p, last);
        tyg_store(p, TYG_I8, tyg_at_val(qa, 0), tyg_convert(p, TYG_TRUNC, TYG_I8, b), 0);
        tyg_jump(p, done);
        tyg_block_set(p, done);
        return tyg_bin(p, TYG_SUB, tyg_load(p, TYG_I64, tyg_at_local(q, 0), 0), at);
    }

    // An arm whose value is an image built at the top of stk: its
    // statements, then its tail built there, in a scope of its own.
    void ImgArm(Node *n, tyg_val stk, TypeExpr *t, Line l) {
        BuildArm(n, l, [&](Node *v) { GenImg(v, stk, t, l); });
    }
    // The same for any value built in place by `build`.
    template <class F> void BuildArm(Node *n, Line l, F build) {
        if (auto b = Is<Block>(n)) {
            tyg_scope_begin(p);
            OpenScope(false);
            for (auto st : b->stmts) Stmt(st);
            if (b->tail && !Ended()) {
                Open();
                Loc(b->tail->line);
                build(b->tail);
            } else if (!b->tail && !Ended()) {
                No(l, "arm without a value");
            }
            CloseScope();
            tyg_scope_end(p);
            return;
        }
        OpenScope(true);
        build(n);
        CloseScope();
    }
    // A function value's call whose value is built in place: its
    // parameters bound in a scope of its own, then its body as an arm.
    template <class F> void BuildFv(Call *c, Line l, F build) {
        if (c->fvparams.size() > c->args.size()) No(c->line, "function value parameters");
        tyg_scope_begin(p);
        OpenScope(false);
        for (size_t i = 0; i < c->fvparams.size(); i++) {
            Declare(c->fvparams[i], c->args[i]->line);
            Init(c->fvparams[i], c->args[i], c->args[i]->line);
        }
        if (!Ended()) BuildArm(c->fvbody, l, build);
        CloseScope();
        tyg_scope_end(p);
    }
    // An if with an else whose arms each build a value in place.
    template <class F> void BuildIf(IfExpr *ie, Line l, F build) {
        auto c = Truth(ie->cond);
        auto tb = tyg_block_new(p), eb = tyg_block_new(p), done = tyg_block_new(p);
        tyg_branch(p, c, tb, eb);
        tyg_block_set(p, tb);
        BuildArm(ie->thenb, l, build);
        Jump(done);
        tyg_block_set(p, eb);
        BuildArm(ie->elseb, l, build);
        Jump(done);
        tyg_block_set(p, done);
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
        if (x.t && (IsGrow(x.t) || IsImg(x.t))) No(n->line, "array value of an arm");
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

    // A value a loop, block or match delivers, if wanted: of a type held.
    bool Valued(Node *n, bool discard) {
        auto et = n->exprtype;
        if (discard || !et || et->kind == TY_VOID) return false;
        if (!M.TypeOk(et)) No(n->line, cat("value of type ", M.NameWhy(et)));
        return true;
    }

    // `block { ... }` (EarlyBlock::CgAny): its tail's value, or a break's.
    V GenEarly(EarlyBlock *e, bool discard) {
        auto valued = Valued(e, discard);
        auto et = e->exprtype;
        tyg_local res = valued ? Temp(et) : 0;
        auto done = tyg_block_new(p);
        loops.push_back({ done, done, sscopes.size(), res, valued ? et : nullptr, true });
        auto x = GenBlock(e->body, !valued);
        loops.pop_back();
        if (valued && !Ended() && x.t) Store(P { tyg_at_local(res, 0), et }, Fit(x, et, e->line));
        Jump(done);
        tyg_block_set(p, done);
        if (!valued) return V {};
        return Load(P { tyg_at_local(res, 0), et }, e->line);
    }

    // `loop { ... }`, its value a break's.
    V GenLoopX(LoopExpr *lp, bool discard) {
        auto valued = std::any_of(lp->breaks.begin(), lp->breaks.end(), [](Break *b) { return b->val; }) &&
                      Valued(lp, discard);
        auto et = lp->exprtype;
        tyg_local res = valued ? Temp(et) : 0;
        auto body = tyg_block_new(p), done = tyg_block_new(p);
        tyg_jump(p, body);
        tyg_block_set(p, body);
        loops.push_back({ done, body, sscopes.size(), res, valued ? et : nullptr });
        GenBlock(lp->body, true);
        loops.pop_back();
        Jump(body);
        tyg_block_set(p, done);
        if (!valued) return V {};
        return Load(P { tyg_at_local(res, 0), et }, lp->line);
    }

    // An integer match (MatchExpr::CgAny): the scrutinee evaluated once,
    // then the arms tested in order, each listed value or range at the
    // scrutinee's type, unsigned for an unsigned one. ADTs are not held.
    V GenMatch(MatchExpr *me, bool discard) {
        if (auto adt = AdtOf(me->scrutinee->exprtype)) return GenAdtMatch(me, adt, discard);
        auto ot = cg.OperandT(me->scrutinee->exprtype);
        if (!ot || ot->kind != TY_INT || ot->intstorage == IS_VARINT)
            No(me->line, cat("match over type ", ot ? M.NameWhy(ot) : string("?")));
        auto valued = Valued(me, discard);
        auto et = me->exprtype;
        tyg_local res = valued ? Temp(et) : 0;
        auto uns = IsUnsigned(ot->intstorage);
        auto x0 = Conv(GenAs(me->scrutinee, ot).v, ot, M.ast.inttypes[IS_I64], me->line);
        if (Ended()) return valued ? Dummy(et) : V {};
        auto xl = tyg_local_new(p, 8, 8);
        tyg_store(p, TYG_I64, tyg_at_local(xl, 0), x0, 0);
        auto x = [&] { return tyg_load(p, TYG_I64, tyg_at_local(xl, 0), 0); };
        auto k = [&](int64_t v) { return tyg_iconst(p, TYG_I64, v); };
        auto done = tyg_block_new(p);
        auto arm = [&](Node *body) {
            auto v = GenArm(body, !valued);
            if (valued && !Ended() && v.t) Store(P { tyg_at_local(res, 0), et }, Fit(v, et, body->line));
            Jump(done);
        };
        auto wild = false;
        for (auto &a : me->arms) {
            if (a.binder) No(me->line, "match arm with a binding");
            if (a.pat.kind == P_WILDCARD) {
                arm(a.body);
                wild = true;
                break;
            }
            auto hit = tyg_block_new(p), next = tyg_block_new(p);
            for (size_t i = 0; i < a.ranges.size(); i++) {
                auto &r = a.ranges[i];
                auto miss = i + 1 < a.ranges.size() ? tyg_block_new(p) : next;
                if (r.lo == r.hi) {
                    tyg_branch(p, tyg_cmp(p, TYG_EQ, x(), k(r.lo)), hit, miss);
                } else {
                    auto upper = tyg_block_new(p);
                    tyg_branch(p, tyg_cmp(p, uns ? TYG_UGE : TYG_SGE, x(), k(r.lo)), upper, miss);
                    tyg_block_set(p, upper);
                    tyg_branch(p, tyg_cmp(p, uns ? TYG_ULE : TYG_SLE, x(), k(r.hi)), hit, miss);
                }
                if (miss != next) tyg_block_set(p, miss);
            }
            if (a.ranges.empty()) tyg_jump(p, next);
            tyg_block_set(p, hit);
            arm(a.body);
            tyg_block_set(p, next);
        }
        if (!wild) Jump(done);
        tyg_block_set(p, done);
        if (!valued) return V {};
        return Load(P { tyg_at_local(res, 0), et }, me->line);
    }

    // ---- ADTs ------------------------------------------------------------------
    //
    // Fixed-mode ADTs and variants as the C backend builds and reads them
    // (TgModule's ADT section has the layout): literals and constants,
    // a variant adapted to its ADT (CodeGen::AdaptToFixed), payload
    // fields, `match` (MatchExpr::CgAny) and case-function dispatch
    // (CodeGen::EmitDispatch), each a chain of tag compares in variant
    // order (tinygen has no switch), with GS_UNREACHABLE's abort past the
    // last under --debug.

    // The ADT a scrutinee is, or refers to, if a fixed-mode one.
    static TypeExpr *AdtOf(TypeExpr *t) {
        if (t && IsPlainRef(t)) t = t->ref->sub;
        return t && t->kind == TY_ENUM ? t : nullptr;
    }
    V MemTemp(TypeExpr *t, bool zero) {
        V x;
        x.t = t;
        x.mem = true;
        x.a = tyg_at_local(Temp(t), 0);
        if (zero) tyg_zero(p, x.a, M.Size(t));
        return x;
    }
    void StoreTag(tyg_addr a, TypeExpr *adt, int vi) {
        tyg_store(p, M.TagScal(adt), a, tyg_iconst(p, M.TagScal(adt), vi), 0);
    }
    // The tag at a, widened.
    tyg_val LoadTag(tyg_addr a, TypeExpr *adt) {
        return tyg_convert(p, TYG_ZEXT, TYG_I64, tyg_load(p, M.TagScal(adt), a, 0));
    }
    // A variant's value as its fixed-mode ADT: the variant's tag, the
    // payload behind it.
    V AdaptToAdt(const V &x, TypeExpr *adt, Line l) {
        if (adt->enu->varmode || !M.TypeOk(adt) || M.EI(x.t) != M.EI(adt)) No(l, "ADT adaptation");
        auto r = MemTemp(adt, true);
        StoreTag(r.a, adt, M.VariantIdx(x.t));
        if (cg.FixedSize(x.t)) {
            auto pa = r.a;
            pa.offset += M.TagSize(adt);
            tyg_copy(p, pa, x.a, cg.FixedSize(x.t));
        }
        return r;
    }
    // Shape.Circle { r: 1.0 }, as the variant or as its ADT.
    V GenVariantLit(StructLit *sl) {
        auto t = sl->exprtype;
        if (!M.TypeOk(t) || (t->kind == TY_ENUM && !sl->variant)) No(sl->line, "literal of this type");
        auto ei = M.EI(t);
        auto vi = t->kind == TY_VARIANT ? M.VariantIdx(t) : ei->en->VariantIndex(sl->variant);
        auto x = MemTemp(t, true);
        int64_t base = 0;
        if (t->kind == TY_ENUM) {
            StoreTag(x.a, t, vi);
            base = M.TagSize(t);
        }
        auto &v = ei->en->variants[vi];
        auto &lay = cg.VariantLayout(ei, vi);
        for (size_t i = 0; i < v.fields.size(); i++) {
            if (v.fields[i].ispad) continue;
            auto init = sl->InitFor((int)i);
            if (!init) continue;
            auto a = x.a;
            a.offset += base + lay.offs[i];
            Into(P { a, ei->vftypes[vi][i] }, init);
        }
        return x;
    }
    // A payload-less variant constant (Dot::CgX).
    V GenVariantConst(Dot *d) {
        auto t = d->exprtype;
        if (!M.TypeOk(t)) No(d->line, cat("variant constant of type ", M.NameWhy(t)));
        auto x = MemTemp(t, true);
        if (t->kind == TY_ENUM) StoreTag(x.a, t, d->einst->en->VariantIndex(d->variantconst));
        return x;
    }
    // A payload field of a variant at base.
    P VariantField(const P &base, int fieldidx) {
        auto ei = cg.EIVar(base.t);
        auto vi = M.VariantIdx(base.t);
        auto a = base.a;
        a.offset += cg.VariantLayout(ei, vi).offs[fieldidx];
        return P { a, ei->vftypes[vi][fieldidx] };
    }
    // Past the last variant: a corrupt tag, which --debug reports
    // (GS_UNREACHABLE) and a release build assumes away.
    void BadTag(Line l) {
        if (M.checks) Abort("TAG", l);
        else tyg_unreachable(p);
    }
    // The ADT value a match or dispatch reads: a copy in memory of this
    // frame's (the C's sv), its tag read once, kept in a local.
    struct Scrut {
        V v;
        TypeExpr *adt;
        tyg_local tag;
    };
    Scrut AdtScrutinee(Node *n, TypeExpr *adt) {
        if (M.VmOk(adt)) {
            // A variable-mode value is read where it lies: its tag first,
            // its payload behind it, as a fixed-mode one's.
            auto pl = Deref(Place(n));
            Scrut s { V {}, adt, tyg_local_new(p, 8, 8) };
            s.v.t = adt;
            s.v.mem = true;
            s.v.a = pl.a;
            if (Ended()) s.v.a = tyg_at_local(Temp(M.ast.inttypes[IS_I64]), 0);
            tyg_store(p, TYG_I64, tyg_at_local(s.tag, 0), LoadTag(s.v.a, adt), 0);
            return s;
        }
        if (!M.TypeOk(adt)) No(n->line, cat("match over type ", M.NameWhy(adt)));
        Scrut s { GenAs(n, adt), adt, tyg_local_new(p, 8, 8) };
        if (Ended()) s.v = MemTemp(adt, false);
        tyg_store(p, TYG_I64, tyg_at_local(s.tag, 0), LoadTag(s.v.a, adt), 0);
        return s;
    }
    // Branches to `hit` if the scrutinee's tag is variant vi, else on.
    void TagIs(const Scrut &s, int vi, tyg_block hit) {
        auto next = tyg_block_new(p);
        auto tag = tyg_load(p, TYG_I64, tyg_at_local(s.tag, 0), 0);
        tyg_branch(p, tyg_cmp(p, TYG_EQ, tag, tyg_iconst(p, TYG_I64, vi)), hit, next);
        tyg_block_set(p, next);
    }
    // Variant vi's payload, copied out of the scrutinee into `to`.
    void CopyPayload(const Scrut &s, int vi, tyg_addr to) {
        auto vt = cg.VariantType(s.adt, vi);
        auto sz = cg.FixedSize(vt);
        if (!sz) {
            tyg_zero(p, to, 1);
            return;
        }
        auto from = s.v.a;
        from.offset += M.TagSize(s.adt);
        tyg_copy(p, to, from, sz);
    }

    V GenAdtMatch(MatchExpr *me, TypeExpr *adt, bool discard) {
        auto valued = Valued(me, discard);
        auto et = me->exprtype;
        tyg_local res = valued ? Temp(et) : 0;
        auto s = AdtScrutinee(me->scrutinee, adt);
        auto done = tyg_block_new(p);
        auto wild = false;
        for (auto &a : me->arms) {
            auto hit = tyg_block_new(p);
            if (a.pat.kind == P_WILDCARD) {
                wild = true;
                tyg_jump(p, hit);
            } else {
                for (auto v : a.variants) TagIs(s, adt->enu->en->VariantIndex(v), hit);
            }
            auto next = tyg_block_new(p);
            if (!wild) tyg_jump(p, next);
            tyg_block_set(p, hit);
            tyg_scope_begin(p);
            // The arm's own stack scope: a binder's stack is taken and given
            // back on this arm's path only.
            OpenScope(false);
            if (a.binder) {
                auto vi = adt->enu->en->VariantIndex(a.variants[0]);
                auto vt = cg.VariantType(adt, vi);
                auto pay = s.v.a;
                pay.offset += M.TagSize(adt);
                if (a.pat.byref) {
                    // Variable-mode payloads only (§8.1): its address.
                    if (!M.IsVm(adt)) No(me->line, "match arm binding by reference");
                    auto slot = tyg_local_new(p, 8, 8);
                    tyg_store(p, TYG_I64, tyg_at_local(slot, 0), tyg_addr_of(p, pay), 0);
                    locals[a.binder] = slot;
                    if (!IsAddrRef(a.binder->type)) indirect.insert(a.binder);
                } else if (!cg.IsFix(vt)) {
                    // A variable payload, copied onto the binder's own stack.
                    Declare(a.binder, me->line);
                    auto stk = tyg_load(p, TYG_I64, tyg_at_local(rzstk[a.binder], 0), 0);
                    tyg_store(p, TYG_I64, tyg_at_local(locals[a.binder], 0), ImgCopy(stk, tyg_addr_of(p, pay), vt), 0);
                } else {
                    Declare(a.binder, me->line);
                    CopyPayload(s, vi, tyg_at_local(locals[a.binder], 0));
                }
            }
            Loc(a.body->line);   // A breakpoint on the arm's line stops here, bound.
            auto v = GenArm(a.body, !valued);
            if (valued && !Ended() && v.t) Store(P { tyg_at_local(res, 0), et }, Fit(v, et, a.body->line));
            CloseScope();
            tyg_scope_end(p);
            Jump(done);
            tyg_block_set(p, next);
            if (wild) break;
        }
        if (!wild) BadTag(me->line);
        else tyg_unreachable(p);
        tyg_block_set(p, done);
        if (!valued) return V {};
        return Load(P { tyg_at_local(res, 0), et }, me->line);
    }

    // A case-function call dispatched on its ADT argument's tag (§8.2):
    // every argument evaluated once, in order, then the variant's case
    // called with its payload by value and the others shared.
    V GenDispatch(Call *c) {
        auto sp0 = c->dispatch[0];
        auto pos = c->dispatcharg;
        auto an = c->ArgNodes();
        if (an.size() < sp0->argtypes.size()) No(c->line, "argument count");
        an.resize(sp0->argtypes.size());
        auto adt = AdtOf(an[pos]->exprtype);
        if (!adt) No(c->line, "dispatch over this type");
        for (auto sp : c->dispatch) {
            string why;
            if (!M.SigOk(sp, false, why)) No(c->line, cat("call to ", sp->sf->name, ": ", why));
            if (sp->rets.size() > 1 || (sp->rets.size() == 1 && cg.sinfo[sp].cret != 0))
                No(c->line, "dispatch with results through out-pointers");
            for (size_t i = 0; i < sp->argtypes.size(); i++)
                if (M.RzParam(sp->argtypes[i])) No(c->line, "dispatch with an array by value");
        }
        // The shared arguments, each kept in a local of its own: the arms
        // are blocks of their own.
        Scrut s {};
        vector<tyg_local> shared(an.size());
        for (size_t i = 0; i < an.size(); i++) {
            if ((int)i == pos) {
                s = AdtScrutinee(an[i], adt);
                continue;
            }
            auto pt = sp0->argtypes[i];
            if (!M.TypeOk(pt)) No(an[i]->line, "argument of this type");
            auto x = GenAs(an[i], pt);
            shared[i] = Temp(pt);
            if (Ended()) continue;
            if (M.IsScalar(pt)) tyg_store(p, M.Scal(pt), tyg_at_local(shared[i], 0), x.v, 0);
            else tyg_copy(p, tyg_at_local(shared[i], 0), x.a, M.Size(pt));
        }
        auto rt = sp0->rets.empty() ? nullptr : sp0->rets[0];
        tyg_local res = rt ? Temp(rt) : 0;
        lastrets.clear();
        if (Ended()) {
            if (rt) lastrets.push_back(Dummy(rt));
            return Dummy(rt);
        }
        auto done = tyg_block_new(p);
        auto &variants = adt->enu->en->variants;
        vector<tyg_block> arms;
        for (size_t vi = 0; vi < variants.size(); vi++) {
            arms.push_back(tyg_block_new(p));
            TagIs(s, (int)vi, arms.back());
        }
        BadTag(c->line);
        for (size_t vi = 0; vi < variants.size(); vi++) {
            tyg_block_set(p, arms[vi]);
            auto sp = c->dispatch[vi];
            auto &ci = cg.sinfo[sp];
            vector<tyg_arg> args;
            for (size_t i = 0; i < an.size(); i++) {
                auto pt = sp->argtypes[i];
                tyg_arg a {};
                if ((int)i == pos) {
                    if (pt->kind != TY_VARIANT) No(c->line, "dispatch to a case of this kind");
                    auto t = Temp(pt);
                    CopyPayload(s, (int)vi, tyg_at_local(t, 0));
                    a.mem = tyg_at_local(t, 0);
                } else if (M.IsScalar(pt)) {
                    a.val = tyg_load(p, M.Scal(pt), tyg_at_local(shared[i], 0), 0);
                } else {
                    a.mem = tyg_at_local(shared[i], 0);
                }
                args.push_back(a);
            }
            FvArgs(c, ci, args);
            if (ci.needssp) {
                tyg_arg sa {};
                sa.val = CalleeSp(sp, c->line);
                args.push_back(sa);
            }
            auto sym = M.Sym(ci.cname, TYG_SYM_FUNC | TYG_SYM_GLOBAL);
            auto x = Result(sp, ci.cret, M.Sig(sp), tyg_direct(sym), args);
            if (rt) Store(P { tyg_at_local(res, 0), rt }, x);
            RfCheck(sp);
            Jump(done);
        }
        tyg_block_set(p, done);
        if (!rt) return V {};
        auto x = Load(P { tyg_at_local(res, 0), rt }, c->line);
        lastrets.push_back(x);
        return x;
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
            case NK_LoopExpr: GenLoopX(static_cast<LoopExpr *>(n), true); return;
            case NK_EarlyBlock: GenEarly(static_cast<EarlyBlock *>(n), true); return;
            case NK_MatchExpr: GenMatch(static_cast<MatchExpr *>(n), true); return;
            case NK_ForLoop: return StmtFor(static_cast<ForLoop *>(n));
            case NK_Return: {
                auto r = static_cast<Return *>(n);
                if (!sp) No(n->line, "return in a global initializer");
                if (r->target != sp->sf) return GenFromReturn(r);
                return DoReturn(r->vals, r->line);
            }
            case NK_Break: {
                // To the innermost loop or block (CodeGen::GenBreakPath):
                // its value first, then the stacks of the scopes left.
                auto b = static_cast<Break *>(n);
                if (loops.empty()) No(n->line, "break target");
                auto t = loops.back();
                if (b->val) {
                    if (t.rt) {
                        auto x = GenAs(b->val, t.rt);
                        if (!M.IsScalar(t.rt) && !x.mem && !Ended()) No(n->line, "aggregate value");
                        Store(P { tyg_at_local(t.res, 0), t.rt }, x);
                    } else {
                        Stmt1(b->val);
                    }
                    if (Ended()) return;
                }
                RestoreFrom(t.depth);
                tyg_jump(p, t.brk);
                return;
            }
            case NK_Continue: {
                auto i = loops.size();
                while (i > 0 && loops[i - 1].block) i--;
                if (i == 0) No(n->line, "continue target");
                RestoreFrom(loops[i - 1].depth);
                tyg_jump(p, loops[i - 1].cnt);
                return;
            }
            case NK_FnDecl: case NK_StructDecl: case NK_EnumDecl: case NK_AliasDecl: return;
            case NK_Block: GenBlock(static_cast<Block *>(n), true); return;
            case NK_IfExpr: GenIf(static_cast<IfExpr *>(n), true); return;
            default: Gen(n); return;
        }
    }

    // A nonfixed result built at the top of dst, from val or from x (a
    // forwarded call's value): a T[]'s image, or a resizable's elements,
    // whose count is stored at len.
    void BuildAt(Node *val, V *x, tyg_val dst, TypeExpr *t, tyg_addr len, Line l) {
        if (IsImg(t)) {
            if (!x) {
                GenImg(val, dst, t, l);
            } else if (M.IsVs(t) || M.IsVm(t)) {
                VsCopy(dst, *x, t, l);
            } else {
                auto v = ViewOf(P { x->a, x->t, x->stk }, l);
                GenImg(nullptr, dst, t, l, &v);
            }
            return;
        }
        auto hdr = tyg_at_local(tyg_local_new(p, 16, 8), 0);
        auto hlen = hdr;
        hlen.offset += 8;
        if (!x) {
            GenRz(val, dst, hdr, t, l);
        } else {
            auto v = ViewOf(P { x->a, x->t, x->stk }, l);
            tyg_store(p, TYG_I64, hdr, Top(dst), 0);
            tyg_store(p, TYG_I64, hlen, tyg_iconst(p, TYG_I64, 0), 0);
            CopyRun(dst, hlen, t->arr->sub, v.elems, v.len);
        }
        if (Ended()) return;
        tyg_store(p, TYG_I64, len, tyg_load(p, TYG_I64, hlen, 0), 0);
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
            Leave();
            tyg_ret(p, 0);
            return;
        }
        if (rets.size() == 1 && cret == 0) {
            auto x = GenAs(vals.at(0), rets[0]);
            if (Ended()) return;
            if (!M.IsScalar(rets[0]) && !x.mem) No(l, "aggregate value");
            Leave();
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
            if (IsImg(rets[i]) || IsGrow(rets[i])) {
                auto dst = tyg_load(p, TYG_I64, tyg_at_local(chans[i].a, 0), 0);
                SetTop(dst, tyg_load(p, TYG_I64, tyg_at_local(chans[i].top0, 0), 0));
                auto rl = IsGrow(rets[i]) ? tyg_at_val(tyg_load(p, TYG_I64, tyg_at_local(chans[i].b, 0), 0), 0)
                                          : tyg_addr {};
                BuildAt(xs.empty() ? vals[i] : nullptr, xs.empty() ? nullptr : &xs[i], dst, rets[i], rl, l);
                if (Ended()) return;
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
        Leave();
        if (cret < 0) tyg_ret(p, 0);
        else if (M.IsScalar(rets[cret])) tyg_ret(p, tyg_load(p, M.Scal(rets[cret]), first.a, 0));
        else tyg_ret_mem(p, first.a);
    }

    // ---- return from (§7.9) ---------------------------------------------------
    //
    // As the C half does it (CodeGen::GenFromReturn, EmitRfCheck), so that
    // a return passes through frames of both halves: the values go into
    // the target's thread-local channels (gs_lret_<tid>_<i>), the
    // discriminant gs_rf is set to the target's id, and every frame on
    // the way returns at once, its stacks restored; after a call that can
    // propagate one, a nonzero gs_rf is caught by its target (cleared, the
    // values returned as its own) or passed on. Both thread-locals are the
    // C half's, reached through accessors it exports (gap 6), read once
    // per invocation as the globals' are.
    tyg_val RfAddr() {
        M.NeedShim("gs_tg_rf");
        return tyg_load(p, TYG_I64, tyg_at_local(AccessorLocal("gs_tg_rf"), 0), 0);
    }
    // A target's channel for result i: gs_lret_ (a fixed value, or a
    // resizable's count), gs_fdst_ (a nonfixed one's destination stack)
    // or gs_fval_ (where its value starts there).
    tyg_addr ChanAt(FnSpec *t, size_t i, const char *kind) {
        cg.EnsureFromChannels(t);
        auto tid = cg.fromids[t];
        auto name = cat("gs_tg_", kind, "_", tid, "_", i);
        M.NeedShim(name, cat("void *", name, "(void) { return (void *)&gs_", kind, "_", tid, "_", i, "; }\n"));
        auto rt = t->rets[i];
        M.channels[name] = string(kind) == "lret" && !IsGrow(rt) ? M.Size(rt) : 8;
        return tyg_at_val(tyg_load(p, TYG_I64, tyg_at_local(AccessorLocal(name), 0), 0), 0);
    }
    tyg_addr LretAt(FnSpec *t, size_t i) { return ChanAt(t, i, "lret"); }
    // A target's saves of its gs_fdst_ channels, restored as it returns.
    vector<pair<tyg_addr, tyg_local>> fdstsaves;
    void Leave() {
        RestoreFrom(0);
        for (auto &[at, sav] : fdstsaves) tyg_store(p, TYG_I64, at, tyg_load(p, TYG_I64, tyg_at_local(sav, 0), 0), 0);
    }
    // Every frame on a propagation's way out returns at once: its stacks
    // restored, a dummy C return value (CodeGen::PropagateReturn).
    void Propagate() {
        Leave();
        auto cret = cg.sinfo[sp].cret;
        if (cret < 0) {
            tyg_ret(p, 0);
        } else if (M.IsScalar(sp->rets[cret])) {
            tyg_ret(p, Dummy(sp->rets[cret]).v);
        } else {
            auto t = Temp(sp->rets[cret]);
            tyg_zero(p, tyg_at_local(t, 0), M.Size(sp->rets[cret]));
            tyg_ret_mem(p, tyg_at_local(t, 0));
        }
    }
    void GenFromReturn(Return *r) {
        auto t = r->targetspec;
        auto l = r->line;
        if (!sp || !t || !cg.fromids.count(t) || !cg.sinfo[sp].hasrf) No(l, "return from");
        auto &rets = t->rets;
        for (auto rt : rets)
            if (!M.RetOk(rt)) No(l, cat("return from with a result of type ", M.Name(rt)));
        auto &vals = r->vals;
        // A nonfixed value lands at its channel's top, which the catch
        // moves down to where the target's caller expects it.
        for (size_t i = 0; i < rets.size(); i++)
            if (IsImg(rets[i]) || IsGrow(rets[i]))
                tyg_store(p, TYG_I64, ChanAt(t, i, "fval"),
                          Top(tyg_load(p, TYG_I64, ChanAt(t, i, "fdst"), 0)), 0);
        vector<V> xs;
        if (vals.size() == 1 && rets.size() > 1) xs = GenCallAll(vals[0], rets.size());
        else if (vals.size() != rets.size()) No(l, "result count");
        for (size_t i = 0; i < rets.size(); i++) {
            if (IsImg(rets[i]) || IsGrow(rets[i])) {
                auto dst = tyg_load(p, TYG_I64, ChanAt(t, i, "fdst"), 0);
                BuildAt(xs.empty() ? vals[i] : nullptr, xs.empty() ? nullptr : &xs[i], dst, rets[i],
                        IsGrow(rets[i]) ? LretAt(t, i) : tyg_addr {}, l);
                if (Ended()) return;
                continue;
            }
            auto x = xs.empty() ? GenAs(vals[i], rets[i]) : Fit(xs[i], rets[i], l);
            if (Ended()) return;
            if (!M.IsScalar(rets[i]) && !x.mem) No(l, "aggregate value");
            Store(P { LretAt(t, i), rets[i] }, x);
        }
        tyg_store(p, TYG_I32, tyg_at_val(RfAddr(), 0), tyg_iconst(p, TYG_I32, cg.fromids[t]), 0);
        Propagate();
    }
    // After a call that can propagate a return (CodeGen::EmitRfCheck).
    void RfCheck(FnSpec *callee) {
        if (!cg.sinfo[callee].hasrf || cg.norfcheck || Ended()) return;
        auto rfa = RfAddr();
        auto rf = tyg_load(p, TYG_I32, tyg_at_val(rfa, 0), 0);
        auto on = tyg_block_new(p), cont = tyg_block_new(p);
        tyg_branch(p, tyg_cmp(p, TYG_NE, rf, tyg_iconst(p, TYG_I32, 0)), on, cont);
        tyg_block_set(p, on);
        auto elsewhere = [&] {
            if (sp && cg.sinfo[sp].hasrf) {
                Propagate();
            } else {
                M.NeedShim("gs_tg_rfpanic");
                CallC("gs_tg_rfpanic", {}, {}, nullptr);
                tyg_unreachable(p);
            }
        };
        if (sp && callee->needs.count(sp)) {
            auto tid = cg.fromids[sp];
            auto here = tyg_block_new(p), other = tyg_block_new(p);
            tyg_branch(p, tyg_cmp(p, TYG_EQ, rf, tyg_iconst(p, TYG_I32, tid)), here, other);
            tyg_block_set(p, here);
            // The propagation ends here: "nothing in flight" again, and
            // the values in the channels returned as this frame's own.
            tyg_store(p, TYG_I32, tyg_at_val(rfa, 0), tyg_iconst(p, TYG_I32, 0), 0);
            auto &rets = sp->rets;
            auto cret = cg.sinfo[sp].cret;
            for (size_t i = 0; i < rets.size(); i++) {
                if ((int)i == cret) continue;
                if (IsImg(rets[i]) || IsGrow(rets[i])) {
                    // The value is on our destination, behind anything the
                    // calls it unwound had built there (CodeGen::LandValue).
                    auto dst = tyg_load(p, TYG_I64, tyg_at_local(chans[i].a, 0), 0);
                    auto top0 = tyg_load(p, TYG_I64, tyg_at_local(chans[i].top0, 0), 0);
                    auto start = tyg_load(p, TYG_I64, ChanAt(sp, i, "fval"), 0);
                    auto n = tyg_bin(p, TYG_SUB, Top(dst), start);
                    tyg_move_dyn(p, top0, start, n);
                    SetTop(dst, tyg_bin(p, TYG_ADD, top0, n));
                    if (IsGrow(rets[i])) {
                        auto rl = tyg_load(p, TYG_I64, tyg_at_local(chans[i].b, 0), 0);
                        tyg_store(p, TYG_I64, tyg_at_val(rl, 0), tyg_load(p, TYG_I64, LretAt(sp, i), 0), 0);
                    }
                    continue;
                }
                auto ptr = tyg_load(p, TYG_I64, tyg_at_local(chans[i].a, 0), 0);
                Store(P { tyg_at_val(ptr, 0), rets[i] }, Load(P { LretAt(sp, i), rets[i] }, Line {}));
            }
            Leave();
            if (cret < 0) tyg_ret(p, 0);
            else if (M.IsScalar(rets[cret])) tyg_ret(p, Load(P { LretAt(sp, cret), rets[cret] }, Line {}).v);
            else tyg_ret_mem(p, LretAt(sp, cret));
            tyg_block_set(p, other);
        }
        elsewhere();
        tyg_block_set(p, cont);
    }

    void Declare(VarDef *d, Line l) {
        if (IsGrow(d->type) && (M.RzOk(d->type) || M.VeOk(d->type)) && !cg.PrefVar(d)) {
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
            if (d->reusable) {
                // Its freelist: free slot indices on a stack of their own.
                auto flstk = Stk(AllocStk(true, l), l);
                auto fl = tyg_local_new(p, 16, 8);
                auto fsl = tyg_local_new(p, 8, 8);
                tyg_store(p, TYG_I64, tyg_at_local(fsl, 0), flstk, 0);
                tyg_store(p, TYG_I64, tyg_at_local(fl, 0), Top(flstk), 0);
                tyg_store(p, TYG_I64, tyg_at_local(fl, 8), tyg_iconst(p, TYG_I64, 0), 0);
                poolfl[d] = { fl, fsl };
            }
            return;
        }
        if (M.FoOk(d->type) && !cg.PrefVar(d)) {
            // A frame object local: its C struct here, its tail's elements
            // on a stack of its own, the tail empty.
            auto stk = Stk(AllocStk(true, l), l);
            auto fo = tyg_local_new(p, M.Size(d->type), 8);
            auto sl = tyg_local_new(p, 8, 8);
            tyg_store(p, TYG_I64, tyg_at_local(sl, 0), stk, 0);
            tyg_zero(p, tyg_at_local(fo, 0), M.Size(d->type));
            auto tail = M.FoLayout(d->type).tail;
            tyg_store(p, TYG_I64, tyg_at_local(fo, tail), Top(stk), 0);
            locals[d] = fo;
            rzstk[d] = sl;
            tyg_debug_var(p, string(d->name).c_str(), M.DT(d->type), tyg_at_local(fo, 0),
                          M.File(d->line.fileidx), d->line.line);
            return;
        }
        if (((IsVarArr(d->type) && M.RzOk(d->type)) || M.VsOk(d->type) || M.VmOk(d->type) || M.VvOk(d->type) ||
             M.RcOk(d->type)) &&
            !cg.PrefVar(d)) {
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
        if (!M.TypeOk(d->type) || cg.PrefVar(d)) No(l, cat("variable type of ", d->name, ": ", M.NameWhy(d->type)));
        auto loc = Temp(d->type);
        locals[d] = loc;
        tyg_debug_var(p, string(d->name).c_str(), M.DT(d->type), tyg_at_local(loc, 0),
                      M.File(d->line.fileidx), d->line.line);
    }

    void StmtVarDecl(VarDecl *vd) {
        if (vd->isglobal) No(vd->line, "declaration kind");
        if (vd->reusable)
            for (auto d : vd->defs)
                if (!IsGrow(d->type) || !M.RzOk(d->type) || cg.PrefVar(d)) No(vd->line, "reusable pool of this kind");
        if (vd->defs.size() > 1 && vd->inits.size() == 1) {
            // let a, b = f(): the locals, then the call into them.
            for (auto d : vd->defs) Declare(d, vd->line);
            auto xs = GenCallAll(vd->inits[0], vd->defs.size());
            for (size_t i = 0; i < xs.size(); i++) {
                auto d = vd->defs[i];
                if ((M.IsVs(d->type) || M.IsVm(d->type)) && !Ended()) {
                    auto stk = tyg_load(p, TYG_I64, tyg_at_local(rzstk[d], 0), 0);
                    tyg_store(p, TYG_I64, tyg_at_local(locals[d], 0), VsCopy(stk, xs[i], d->type, vd->line), 0);
                    continue;
                }
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
        if (IsImg(d->type)) {
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
        if (M.FoOk(d->type)) {
            if (init) GenFo(init, P { pl.a, d->type, tyg_load(p, TYG_I64, tyg_at_local(rzstk[d], 0), 0) }, l);
            return;
        }
        if (!init) tyg_zero(p, pl.a, M.Size(d->type));
        else Into(pl, init);
    }

    void StmtAssign(Assign *a) {
        auto pl = Place(a->lval);
        if (a->pointee) pl = Deref(pl);
        if (a->op == T_DOTASSIGN) {
            // A reference re-pointed (CodeGen::GenRebind): the slot holding
            // it takes the new address, an optional's null too. Relative
            // references and pooled ones are not held.
            auto rel = TgModule::IsRel(pl.t) && M.RelOk(pl.t, 1);
            if (!rel && (!(IsPlainRef(pl.t) || M.OptRef(pl.t)) || !M.TypeOk(pl.t)))
                No(a->line, cat("rebind of type ", M.NameWhy(pl.t)));
            auto x = GenAs(a->rhs, pl.t);
            if (!M.IsScalar(pl.t) && !x.mem && !Ended()) No(a->line, "aggregate value");
            Store(pl, x);
            return;
        }
        if (a->op == T_ASSIGN && IsGrow(pl.t)) {
            // A whole resizable (Assign::CgStmt): its stack's top back to
            // where its elements start, then the new ones built there.
            if (!M.RzOk(pl.t) || !pl.stk) No(a->line, "assignment to an array of this kind");
            SetTop(pl.stk, tyg_load(p, TYG_I64, pl.a, 0));
            GenRz(a->rhs, pl.stk, pl.a, pl.t, a->line);
            return;
        }
        if (a->op == T_ASSIGN && IsAddrRef(pl.t) && M.IsRc(Deref(pl).t)) pl = Deref(pl);
        if (a->op == T_ASSIGN && M.IsRc(pl.t)) {
            // A T[..] (Assign::CgStmt): the value as one, then its length
            // and elements copied in under the destination's capacity.
            if (!M.RcOk(pl.t)) No(a->line, "assignment to an array of this kind");
            // A place's elements are read where they lie, as any array's
            // or slice's the checker typed as the destination's type.
            View v;
            auto rhs = a->rhs;
            if (auto c = Is<Call>(rhs); c && c->builtin == B_COPY) rhs = c->ArgNodes()[0];
            if (Is<Ident>(rhs) || Is<Dot>(rhs) || Is<Index>(rhs)) {
                v = ViewOf(Place(rhs), a->line);
            } else {
                auto stk = Stk(AllocStk(false, a->line), a->line);
                v = LimView(P { tyg_at_val(GenImg(a->rhs, stk, pl.t, a->line), 0), pl.t });
            }
            if (Ended()) return;
            if (cg.Mangle(v.elem) != cg.Mangle(pl.t->arr->sub)) No(a->line, "array of another element type");
            auto nl = tyg_local_new(p, 8, 8);
            tyg_store(p, TYG_I64, tyg_at_local(nl, 0), v.len, 0);
            auto n = [&] { return tyg_load(p, TYG_I64, tyg_at_local(nl, 0), 0); };
            AbortUnless(tyg_cmp(p, TYG_SLE, n(), LimCapOf(pl)), "CAPACITY", a->line);
            auto esz = tyg_iconst(p, TYG_I64, cg.FixedSize(pl.t->arr->sub));
            tyg_move_dyn(p, LimView(pl).elems, v.elems, tyg_bin(p, TYG_MUL, n(), esz));
            LimSetLen(pl, n());
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
        // Elements of variable size are walked with a cursor, each one's
        // bytes on from the last (StepElem); the loop variable is the
        // element's address, as a T[] local is its image's.
        auto itt = f->iter->exprtype;
        while (itt && IsAddrRef(itt)) itt = itt->ref->sub;
        TypeExpr *ielem = !itt ? nullptr : itt->kind == TY_ARRAY ? itt->arr->sub
                        : itt->kind == TY_SLICE ? itt->sub : nullptr;
        auto walk = ielem && !cg.IsFix(ielem);
        tyg_local cl = 0;
        tyg_scope_begin(p);
        if (f->vdef && walk) {
            auto loc = tyg_local_new(p, 8, 8);
            locals[f->vdef] = loc;
            // A reference variable holds the address itself; a value one is
            // read through it, as a T[] local is.
            if (!IsAddrRef(f->vdef->type)) indirect.insert(f->vdef);
            tyg_debug_var(p, string(f->vdef->name).c_str(), tyg_dt_pointer(m, M.DT(ielem)), tyg_at_local(loc, 0),
                          M.File(f->vdef->line.fileidx), f->vdef->line.line);
        } else if (f->vdef) {
            Declare(f->vdef, f->line);
        }
        if (f->idxdef) Declare(f->idxdef, f->line);
        auto head = tyg_block_new(p), body = tyg_block_new(p), step = tyg_block_new(p),
             done = tyg_block_new(p);
        if (walk) {
            cl = tyg_local_new(p, 8, 8);
            tyg_store(p, TYG_I64, tyg_at_local(cl, 0), ViewOf(at(), f->line).elems, 0);
        }
        tyg_jump(p, head);
        tyg_block_set(p, head);
        Loc(f->line);
        auto v = ViewOf(at(), f->line);
        auto i = tyg_load(p, TYG_I64, tyg_at_local(gi, 0), 0);
        tyg_branch(p, tyg_cmp(p, TYG_SLT, i, v.len), body, done);
        tyg_block_set(p, body);
        auto ea = walk ? tyg_load(p, TYG_I64, tyg_at_local(cl, 0), 0) : ElemAddr(v, i);
        if (f->vdef && walk) {
            tyg_store(p, TYG_I64, tyg_at_local(locals[f->vdef], 0), ea, 0);
        } else if (f->vdef) {
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
        if (walk) {
            auto c0 = tyg_load(p, TYG_I64, tyg_at_local(cl, 0), 0);
            tyg_store(p, TYG_I64, tyg_at_local(cl, 0), tyg_bin(p, TYG_ADD, c0, ImgSize(ielem, c0)), 0);
        }
        tyg_jump(p, head);
        tyg_block_set(p, done);
        tyg_scope_end(p);
    }

    // ---- Frame objects ---------------------------------------------------------

    // A fixed field at its offset in the C struct; the tail, a resizable
    // whose header is the struct's last member, on the frame object's stack.
    P FoFieldPlace(const P &base, int fi, Line l) {
        auto fl = M.FoLayout(base.t);
        auto si = cg.SI(base.t);
        auto a = base.a;
        if (fi == fl.tailfi) {
            if (!base.stk) No(l, "frame object without its stack");
            a.offset += fl.tail;
            return P { a, si->ftypes[(size_t)fi], base.stk };
        }
        a.offset += fl.offs[(size_t)fi];
        return P { a, si->ftypes[(size_t)fi] };
    }
    // A frame object built at pl, its tail on pl.stk: a literal's fields
    // where they lie, the tail built in place (CodeGen's GenConstruct).
    void GenFo(Node *n, P pl, Line l) {
        auto sl = Is<StructLit>(n);
        if (!sl || !sl->sinst || sl->variant) No(l, cat("value of type ", M.NameWhy(pl.t)));
        auto si = cg.SI(pl.t);
        auto fl = M.FoLayout(pl.t);
        for (size_t i = 0; i < si->st->fields.size(); i++) {
            if (si->st->fields[i].ispad) continue;
            auto init = sl->InitFor((int)i);
            auto fp = FoFieldPlace(pl, (int)i, l);
            if ((int)i == fl.tailfi) {
                if (init) GenRz(init, pl.stk, fp.a, fp.t, l);
                else {
                    tyg_store(p, TYG_I64, fp.a, Top(pl.stk), 0);
                    auto len = fp.a;
                    len.offset += 8;
                    tyg_store(p, TYG_I64, len, tyg_iconst(p, TYG_I64, 0), 0);
                }
                continue;
            }
            if (init) Into(fp, init);
            else tyg_zero(p, fp.a, M.Size(fp.t));
            if (Ended()) return;
        }
    }

    // ---- Variable-size structs -------------------------------------------------

    // A variable-size struct the module holds (TgModule::VsOk) is a bytes
    // value as a T[] is (§6.1): its locals, by-value parameters, results,
    // free variables and temporaries are a T[]'s -- an image on a stack,
    // a uint8_t * at its start (Declare, Emit, GenCall, DoReturn) -- and
    // what differs is here: its size and its fields' addresses walk the
    // variable-size fields before them, and it is built field by field.

    // The bytes of the image of type t at `at` (CodeGen::SizeX).
    tyg_val ImgSize(TypeExpr *t, tyg_val at) {
        if (cg.IsFix(t)) return tyg_iconst(p, TYG_I64, cg.FixedSize(t));
        if (TgModule::IsVarint(t)) return UlebSize(at);
        if (M.IsVv(t)) return tyg_bin(p, TYG_SUB, VvField(t, at, -1), at);
        if (M.IsVm(t)) {
            // The tag, and its variant's payload: a variable one walked,
            // only on its own tag's path.
            auto ei = cg.EIOf(t);
            auto ts = M.TagSize(t);
            auto res = tyg_local_new(p, 8, 8);
            auto tag = tyg_local_new(p, 8, 8);
            tyg_store(p, TYG_I64, tyg_at_local(tag, 0), LoadTag(tyg_at_val(at, 0), t), 0);
            auto done = tyg_block_new(p);
            for (size_t vi = 0; vi < ei->en->variants.size(); vi++) {
                auto vt = cg.VariantType(t, (int)vi);
                auto hit = tyg_block_new(p), next = tyg_block_new(p);
                tyg_branch(p, tyg_cmp(p, TYG_EQ, tyg_load(p, TYG_I64, tyg_at_local(tag, 0), 0),
                                      tyg_iconst(p, TYG_I64, (int64_t)vi)), hit, next);
                tyg_block_set(p, hit);
                auto pay = tyg_bin(p, TYG_ADD, at, tyg_iconst(p, TYG_I64, ts));
                auto ps = cg.IsFix(vt) ? tyg_iconst(p, TYG_I64, cg.FixedSize(vt)) : ImgSize(vt, pay);
                tyg_store(p, TYG_I64, tyg_at_local(res, 0), tyg_bin(p, TYG_ADD, ps, tyg_iconst(p, TYG_I64, ts)), 0);
                tyg_jump(p, done);
                tyg_block_set(p, next);
            }
            tyg_store(p, TYG_I64, tyg_at_local(res, 0), tyg_iconst(p, TYG_I64, ts), 0);
            tyg_jump(p, done);
            tyg_block_set(p, done);
            return tyg_load(p, TYG_I64, tyg_at_local(res, 0), 0);
        }
        if (M.IsRc(t)) {
            auto cap = LimCapOf(P { tyg_at_val(at, 0), t });
            return tyg_bin(p, TYG_ADD, tyg_bin(p, TYG_MUL, cap, tyg_iconst(p, TYG_I64, cg.FixedSize(t->arr->sub))),
                           tyg_iconst(p, TYG_I64, 8));
        }
        if (IsVarArr(t)) {
            auto v = ViewOf(P { tyg_at_val(at, 0), t }, Line {});
            if (!cg.IsFix(t->arr->sub)) return tyg_bin(p, TYG_SUB, RunEnd(v), at);
            auto bytes = tyg_bin(p, TYG_MUL, v.len, tyg_iconst(p, TYG_I64, cg.FixedSize(t->arr->sub)));
            return tyg_bin(p, TYG_ADD, bytes, tyg_iconst(p, TYG_I64, TgModule::LenWidth(t)));
        }
        return tyg_bin(p, TYG_SUB, VsField(t, at, -1), at);
    }
    // The address of field fi of the image of the struct t at `at`, or its
    // end where fi < 0 (CodeGen::FieldPtr): a constant offset until a
    // variable-size field intervenes, then a cursor past each one.
    tyg_val VsField(TypeExpr *t, tyg_val at, int fi) {
        auto si = cg.SI(t);
        return RunField(si->st->fields, si->ftypes, at, fi);
    }
    // A variable-size variant's field: its payload's run walked.
    tyg_val VvField(TypeExpr *t, tyg_val at, int fi) {
        auto ei = cg.EIVar(t);
        auto vi = (size_t)ei->en->VariantIndex(t->var->variant);
        return RunField(ei->en->variants[vi].fields, ei->vftypes[vi], at, fi);
    }
    // The same over any run of fields: a struct's, a variant's payload.
    tyg_val RunField(const vector<Field> &fields, const vector<TypeExpr *> &ftypes, tyg_val at, int fi) {
        auto n = fi < 0 ? (int)fields.size() : fi;
        int64_t off = 0;
        auto cur = at;
        for (int i = 0; i < n; i++) {
            auto &f = fields[(size_t)i];
            if (f.ispad) { off += f.padsize > 0 ? f.padsize : 0; continue; }
            auto ft = ftypes[(size_t)i];
            if (cg.IsFix(ft)) { off += cg.FixedSize(ft); continue; }
            if (off) cur = tyg_bin(p, TYG_ADD, cur, tyg_iconst(p, TYG_I64, off));
            off = 0;
            cur = tyg_bin(p, TYG_ADD, cur, ImgSize(ft, cur));
        }
        return off ? tyg_bin(p, TYG_ADD, cur, tyg_iconst(p, TYG_I64, off)) : cur;
    }
    P VsFieldPlace(const P &base, int fi) {
        auto si = cg.SI(base.t);
        auto ft = si->ftypes[(size_t)fi];
        P r { tyg_at_val(VsField(base.t, tyg_addr_of(p, base.a), fi), 0), ft };
        r.enc = TgModule::IsVarint(ft);
        return r;
    }

    // Copies the image of type t at src to the top of stk; its new address.
    // The source may lie on stk itself, below its top.
    tyg_val ImgCopy(tyg_val stk, tyg_val src, TypeExpr *t) {
        auto top = Top(stk);
        auto size = ImgSize(t, src);
        tyg_move_dyn(p, top, src, size);
        SetTop(stk, tyg_bin(p, TYG_ADD, top, size));
        return top;
    }
    // A result of a call delivered elsewhere (a multi-value one), copied.
    tyg_val VsCopy(tyg_val stk, const V &x, TypeExpr *t, Line l) {
        if (!x.mem || !x.t || !cg.TEq(x.t, t)) No(l, cat("value of type ", M.NameWhy(t)));
        return ImgCopy(stk, tyg_addr_of(p, x.a), t);
    }

    // Builds n's value, of the variable-size struct t, at the top of stk
    // (CodeGen::GenConstruct); its address. A literal field by field, a
    // call returning one handed stk as its destination, anything else (a
    // variable, a field, a pointee) copied whole.
    // Builds n's value, of the variable-mode ADT t, at the top of stk; its
    // address. A variant literal as its tag then its payload fields, a call
    // returning one handed stk, anything else copied whole.
    tyg_val GenVm(Node *n, tyg_val stk, TypeExpr *t, Line l) {
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
        if (auto sl = Is<StructLit>(n); sl && sl->variant && !cg.IsFix(cg.VariantType(t, cg.EIOf(t)->en->VariantIndex(sl->variant)))) {
            // A variable payload: the tag, then its fields built in turn.
            auto ei = cg.EIOf(t);
            auto vi = (size_t)ei->en->VariantIndex(sl->variant);
            auto base = at();
            SetTop(stk, tyg_bin(p, TYG_ADD, base, tyg_iconst(p, TYG_I64, M.TagSize(t))));
            StoreTag(tyg_at_val(base, 0), t, (int)vi);
            RunLit(sl, stk, ei->en->variants[vi].fields, ei->vftypes[vi]);
            return at();
        }
        if (auto sl = Is<StructLit>(n); sl && sl->variant) {
            auto ei = cg.EIOf(t);
            auto vi = ei->en->VariantIndex(sl->variant);
            auto vt = cg.VariantType(t, vi);
            auto ts = M.TagSize(t);
            auto size = ts + cg.FixedSize(vt);
            auto base = at();
            SetTop(stk, tyg_bin(p, TYG_ADD, base, tyg_iconst(p, TYG_I64, size)));
            tyg_zero(p, tyg_at_val(base, 0), size);
            StoreTag(tyg_at_val(base, 0), t, vi);
            auto &lay = cg.VariantLayout(ei, vi);
            for (size_t i = 0; i < ei->en->variants[(size_t)vi].fields.size(); i++) {
                if (ei->en->variants[(size_t)vi].fields[i].ispad) continue;
                auto init = sl->InitFor((int)i);
                if (!init) continue;
                Into(P { tyg_at_val(at(), ts + lay.offs[i]), ei->vftypes[(size_t)vi][i] }, init);
                if (Ended()) return at();
            }
            return at();
        }
        auto pl = Deref(Place(n));
        if (Ended()) return at();
        if (!cg.TEq(pl.t, t)) No(l, cat("value of type ", M.NameWhy(t)));
        return ImgCopy(stk, tyg_addr_of(p, pl.a), t);
    }

    // A variable-size variant's value at the top of stk: a literal's fields
    // in turn, a call's built there, anything else copied whole.
    tyg_val GenVv(Node *n, tyg_val stk, TypeExpr *t, Line l) {
        auto start = tyg_local_new(p, 8, 8);
        tyg_store(p, TYG_I64, tyg_at_local(start, 0), Top(stk), 0);
        auto at = [&] { return tyg_load(p, TYG_I64, tyg_at_local(start, 0), 0); };
        if (auto sl = Is<StructLit>(n); sl && cg.TEq(sl->exprtype, t)) {
            auto ei = cg.EIVar(t);
            auto vi = (size_t)ei->en->VariantIndex(t->var->variant);
            RunLit(sl, stk, ei->en->variants[vi].fields, ei->vftypes[vi]);
            return at();
        }
        if (auto c = Is<Call>(n); c && c->builtin < 0 && c->spec && !c->spec->sf->isextern &&
                                   !c->spec->rets.empty() && cg.TEq(c->spec->rets[0], t)) {
            imgdst = stk;
            GenCall(c);
            imgdst = 0;
            return at();
        }
        auto pl = Deref(Place(n));
        if (Ended()) return at();
        if (!cg.TEq(pl.t, t)) No(l, cat("value of type ", M.NameWhy(pl.t), " as ", M.Name(t)));
        ImgCopy(stk, tyg_addr_of(p, pl.a), t);
        return at();
    }

    tyg_val GenVs(Node *n, tyg_val stk, TypeExpr *t, Line l) {
        auto start = tyg_local_new(p, 8, 8);
        tyg_store(p, TYG_I64, tyg_at_local(start, 0), Top(stk), 0);
        auto at = [&] { return tyg_load(p, TYG_I64, tyg_at_local(start, 0), 0); };
        if (auto c = Is<Call>(n); c && c->builtin == B_COPY && c->ArgNodes().size() == 1) n = c->ArgNodes()[0];
        if (auto sl = Is<StructLit>(n); sl && sl->sinst && !sl->variant && cg.TEq(sl->exprtype, t)) {
            VsLit(sl, stk, t);
            return at();
        }
        if (auto c = Is<Call>(n); c && c->builtin < 0 && c->spec && !c->spec->sf->isextern &&
                                   !c->spec->rets.empty() && cg.TEq(c->spec->rets[0], t)) {
            imgdst = stk;
            GenCall(c);
            imgdst = 0;
            return at();
        }
        auto pl = Deref(Place(n));
        if (Ended()) return at();
        if (!cg.TEq(pl.t, t)) No(l, cat("value of type ", M.NameWhy(pl.t), " as ", M.Name(t)));
        ImgCopy(stk, tyg_addr_of(p, pl.a), t);
        return at();
    }
    // A literal's fields in layout order (CodeGen::GenFieldInits), each
    // built at the top in turn: a fixed one evaluated, then stored; a
    // variable-size one built there; explicit padding zeroed.
    void VsLit(StructLit *sl, tyg_val stk, TypeExpr *t) {
        auto si = cg.SI(t);
        RunLit(sl, stk, si->st->fields, si->ftypes);
    }
    void RunLit(StructLit *sl, tyg_val stk, const vector<Field> &fields, const vector<TypeExpr *> &ftypes) {
        auto zero = [&](int64_t n) {
            auto top = Top(stk);
            tyg_zero(p, tyg_at_val(top, 0), n);
            SetTop(stk, tyg_bin(p, TYG_ADD, top, tyg_iconst(p, TYG_I64, n)));
        };
        for (size_t i = 0; i < fields.size() && !Ended(); i++) {
            auto &f = fields[i];
            if (f.ispad) {
                if (f.padsize > 0) zero(f.padsize);
                continue;
            }
            auto ft = ftypes[i];
            auto init = sl->InitFor((int)i);
            if (!cg.IsFix(ft)) {
                if (!init) No(sl->line, "omitted variable-size field");
                GenImg(init, stk, ft, init->line);
                continue;
            }
            if (!init) {
                zero(cg.FixedSize(ft));
                continue;
            }
            auto x = GenAs(init, ft);
            if (Ended()) return;
            if (!M.IsScalar(ft) && !x.mem) No(init->line, "aggregate value");
            auto top = Top(stk);
            Store(P { tyg_at_val(top, 0), ft }, x);
            SetTop(stk, tyg_bin(p, TYG_ADD, top, tyg_iconst(p, TYG_I64, cg.FixedSize(ft))));
        }
    }

    // `Name { a, b }` (CodeGen::RenderLoc), each field where the walk finds
    // it.
    void VsRender(tyg_val stk, tyg_addr len, const P &pl, Line l, int depth) {
        auto si = cg.SI(pl.t);
        TextLit(stk, len, cat(TextName(pl.t), " { "));
        auto first = true;
        for (size_t i = 0; i < si->st->fields.size(); i++) {
            if (si->st->fields[i].ispad) continue;
            if (!first) TextLit(stk, len, ", ");
            first = false;
            RenderInto(stk, len, VsFieldPlace(pl, (int)i), true, l, depth + 1);
        }
        TextLit(stk, len, " }");
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
        // The C parameter: a by-value T[>..] takes two. The free variables
        // follow the declared parameters; their entries end the prologue
        // for the debugger as the parameters' do (tyg_debug_var_ref,
        // tyg_debug_param).
        int pi = 0;
        for (auto t : sp->argtypes) pi += M.RzParam(t) && IsGrow(t) ? 2 : 1;
        auto param = [&] {
            auto loc = tyg_local_new(p, 8, 8);
            tyg_store(p, TYG_I64, tyg_at_local(loc, 0), tyg_param(p, pi++), 0);
            return loc;
        };
        // Free variables (§7.5), each the address of the variable in the
        // frame it lives in (a T[]'s image, a resizable's header, with its
        // stack), read through as a by-value T[] parameter is. The debugger
        // sees the variable itself under its name, reached through the
        // address (tyg_debug_var_ref), where the C half shows a pointer.
        for (auto fv : cg.sinfo[sp].freevars) {
            auto loc = param();
            locals[fv] = loc;
            indirect.insert(fv);
            tyg_debug_var_ref(p, string(fv->name).c_str(), M.DT(fv->type), tyg_at_local(loc, 0),
                              M.File(fv->line.fileidx), fv->line.line);
            if (IsGrow(fv->type)) rzstk[fv] = param();
        }
        int afterfv = pi;
        pi = 0;
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
        pi = afterfv;
        chans.resize(sp->rets.size());
        for (size_t i = 0; i < sp->rets.size(); i++) {
            if (IsGrow(sp->rets[i])) {
                chans[i].a = param();
                chans[i].b = param();
            } else if (M.IsImg(sp->rets[i]) || (int)i != cret) {
                chans[i].a = param();
            }
        }
        if (cg.sinfo[sp].needssp) spl = param();
        // Each destination's top on entry: a return builds its result
        // there, also one nested in another return's value that has
        // started building (`return str("x", if c { return "y"; } ...)`).
        for (size_t i = 0; i < sp->rets.size(); i++) {
            if (!IsGrow(sp->rets[i]) && !IsImg(sp->rets[i])) continue;
            chans[i].top0 = tyg_local_new(p, 8, 8);
            tyg_store(p, TYG_I64, tyg_at_local(chans[i].top0, 0),
                      Top(tyg_load(p, TYG_I64, tyg_at_local(chans[i].a, 0), 0)), 0);
        }
        gblk = tyg_block_new(p);
        gstart = tyg_block_new(p);
        tyg_jump(p, gblk);
        tyg_block_set(p, gstart);
        if (cg.fromids.count(sp)) {
            // A `return from` target: its destinations are where values
            // in flight to it are built (CodeGen::EmitSpec).
            for (size_t i = 0; i < sp->rets.size(); i++) {
                if (!IsImg(sp->rets[i]) && !IsGrow(sp->rets[i])) continue;
                auto at = ChanAt(sp, i, "fdst");
                auto sav = tyg_local_new(p, 8, 8);
                tyg_store(p, TYG_I64, tyg_at_local(sav, 0), tyg_load(p, TYG_I64, at, 0), 0);
                tyg_store(p, TYG_I64, at, tyg_load(p, TYG_I64, tyg_at_local(chans[i].a, 0), 0), 0);
                fdstsaves.emplace_back(at, sav);
            }
        }
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
        Finish(sp->sf->line);
    }

    // The entry's block (gblk): the addresses of the globals the body
    // names, and the stack block, then on to the body (gstart).
    void Finish(Line line) {
        tyg_block_set(p, gblk);
        Loc(line);
        if (stksl) tyg_store(p, TYG_I64, tyg_at_local(stksl, 0), CallC("gs_tg_stks", {}, {}, tyg_type_ptr(m)), 0);
        for (auto &[acc, loc] : gaddrs)
            tyg_store(p, TYG_I64, tyg_at_local(loc, 0), CallC(acc, {}, {}, tyg_type_ptr(m)), 0);
        tyg_jump(p, gstart);
        if (tyg_proc_end(p) != 0) {
            p = nullptr;
            No(line, cat("tinygen: ", tyg_error(m) ? tyg_error(m) : "?"));
        }
        p = nullptr;
    }

    // gs_init_globals as a procedure of the object, for a whole program
    // build (CodeGen::EmitGlobalInit): each initializer in declaration
    // order with gs_sp fixed at 0, a resizable or T[] global built on its
    // dedicated stack, reserved first (gs_stack_init). No sp: what refers
    // to the function's own results (return, return from) is unsupported.
    void EmitInit(tyg_sym sym, Line line) {
        p = tyg_proc_begin(m, sym, tyg_sig_new(m, nullptr, nullptr, 0, 0));
        if (!p) No(line, cat("tyg_proc_begin: ", tyg_error(m) ? tyg_error(m) : "?"));
        tyg_debug_proc(p, "gs_init_globals", M.File(line.fileidx), line.line,
                       tyg_dt_func(m, tyg_dt_void(m), nullptr, 0, 0));
        Loc(line);
        spl = tyg_local_new(p, 8, 8);
        tyg_store(p, TYG_I64, tyg_at_local(spl, 0), tyg_iconst(p, TYG_I64, 0), 0);
        gblk = tyg_block_new(p);
        gstart = tyg_block_new(p);
        tyg_jump(p, gblk);
        tyg_block_set(p, gstart);
        OpenScope(false);
        for (auto g : M.ast.globals) {
            if (g->inits.empty()) continue;
            if (g->defs.size() != g->inits.size()) No(g->line, "global initializer of several results");
            OpenScope(true);
            for (size_t i = 0; i < g->defs.size() && !Ended(); i++) {
                auto d = g->defs[i];
                auto init = g->inits[i];
                if (cg.gstatic.count(d)) continue;   // Static data.
                Loc(init->line);
                if (!M.GlobalOk(d) || cg.IsFrameObj(d->type) ||
                    (d->type->kind == TY_REF && d->type->ref->lenstorage >= 0))
                    No(d->line, "global of this kind");
                if (IsGrow(d->type) || IsVarArr(d->type)) {
                    auto at = tyg_load(p, TYG_I64, tyg_at_local(AccessorLocal(M.Accessor(d)), 0), 0);
                    auto stk = tyg_load(p, TYG_I64, tyg_at_local(AccessorLocal(M.StackAccessor(d)), 0), 0);
                    M.NeedShim("gs_tg_stack_init");
                    CallC("gs_tg_stack_init", { tyg_type_ptr(m) }, { stk }, nullptr);
                    if (IsGrow(d->type)) {
                        // CodeGen::InitGlobalStack: an empty array, then
                        // its elements.
                        tyg_store(p, TYG_I64, tyg_at_val(at, 0), Top(stk), 0);
                        tyg_store(p, TYG_I64, tyg_at_val(at, 8), tyg_iconst(p, TYG_I64, 0), 0);
                        GenRz(init, stk, tyg_at_val(at, 0), d->type, init->line);
                        if (d->reusable && !Ended()) {
                            // Its freelist, empty, on a stack of its own.
                            auto [h, st] = M.PoolAccessors(d);
                            auto fl = tyg_load(p, TYG_I64, tyg_at_local(AccessorLocal(h), 0), 0);
                            auto flstk = tyg_load(p, TYG_I64, tyg_at_local(AccessorLocal(st), 0), 0);
                            CallC("gs_tg_stack_init", { tyg_type_ptr(m) }, { flstk }, nullptr);
                            tyg_store(p, TYG_I64, tyg_at_val(fl, 0), Top(flstk), 0);
                            tyg_store(p, TYG_I64, tyg_at_val(fl, 8), tyg_iconst(p, TYG_I64, 0), 0);
                        }
                    } else {
                        auto img = GenImg(init, stk, d->type, init->line);
                        if (!Ended()) tyg_store(p, TYG_I64, tyg_at_val(at, 0), img, 0);
                    }
                    continue;
                }
                Into(GlobalPlace(d, init->line), init);
            }
            CloseScope();
        }
        CloseScope();
        if (!Ended()) tyg_ret(p, 0);
        Finish(line);
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
    string runtime_decls;   // runtime.h and runtime_ext.h (TgModule::RuntimeApi).
    // Functions left to C whatever they hold (--tinygen-skip): what tests
    // calls across the two halves with.
    vector<string> skip;

    TinyGenBackend(Ast &_ast, bool _checks, const string &_source, string_view _ext,
                   string _decls)
        : ast(_ast), checks(_checks), source(_source), runtime_ext_text(_ext),
          runtime_decls(std::move(_decls)) {}

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
        auto nshims = mod->shims.size(), nexterns = mod->externs.size(), ndirect = mod->direct.size();
        TgFn fn(*mod, sp);
        try {
            fn.Emit();
        } catch (TgUnsupported &u) {
            if (fn.p) tyg_proc_abort(fn.p);
            mod->shims.resize(nshims);
            mod->externs.resize(nexterns);
            mod->direct.resize(ndirect);
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
        mod->rtdecls = runtime_decls;
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
        for (auto list : { &mod->externs, &mod->direct })
            for (auto sp : *list)
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

    // Whether the object can be the whole program, with no C half: every
    // live specialization compiled, nothing asked of the C half that the
    // support object (TgSupportBody) or the object itself does not hold --
    // globals it can lay out and initialize (gs_init_globals built here),
    // `return from` channels of fixed types, extern fns the runtime
    // exports or the support object has trampolines for -- and a plain
    // main without threads. Else why not.
    bool WholeProgram(CodeGen &cg, string &why) {
        auto mainsf = ast.MainFunction();
        auto mainsp = mainsf && !mainsf->specs.empty() ? mainsf->specs[0] : nullptr;
        if (!mod) why = "nothing compiled";
        else if (!left.empty()) why = cat(left.size(), " function(s) left to C");
        else if (cg.library) why = "a library";
        else if (!mainsp || !cg.sinfo.count(mainsp) || !mainsp->params.empty() ||
                 !cg.sinfo[mainsp].freevars.empty())
            why = "no plain main";
        else if (cg.usesthreads || !cg.queues.empty()) why = "threads";
        if (!why.empty()) return false;
        for (auto sf : ast.functions)
            if (sf->isexport) return why = "export fns", false;
        for (auto sp : mod->externs) {
            auto found = false;
            for (auto &x : TgSupportExterns()) found = found || sp->sf->cname == x.name;
            if (!found) return why = cat("extern ", sp->sf->cname), false;
        }
        if (!GlobalsOk(cg, why)) return false;
        // A gs_init_globals built but not used stays in the object,
        // unreferenced, with what it asked of the C half.
        if (!cg.noglobalinit && !BuildInit(cg, why)) return false;
        for (auto &sh : mod->shims)
            if (TgFixedShim(sh.first).empty() && !OwnAccessor(sh.first)) {
                initsym = TYG_NO_SYM;
                return why = cat("C half's ", sh.first), false;
            }
        EmitGlobalStorage(cg);
        return true;
    }

    // ---- Globals of a whole program ------------------------------------------
    //
    // With no C half the object holds the globals: each member of the C's
    // instance block (gs_globals_t) a zero-filled data symbol of its own,
    // under its Goose name for the debugger, a resizable's or T[]'s
    // dedicated stack another beside it; the static data the C makes of a
    // `let` is the object's already (TgModule::StaticGlobal). A whole
    // program has no threads, so main's instance is the only one, as
    // GS_GL is &gs_globals_main in the C. The functions compiled reach a
    // global through its accessor (gs_tg_g<i>, gs_tg_gs<i>) as in a mixed
    // build, which the object now defines, and gs_init_globals is a
    // procedure of the object (TgFn::EmitInit) that main calls.

    // Whether every global is one the object can hold: else why not.
    bool GlobalsOk(CodeGen &cg, string &why) {
        for (auto g : ast.globals)
            for (auto d : g->defs) {
                if (cg.gstatic.count(d)) {
                    // Static data reached through an accessor: its bytes
                    // were not laid out (StaticBytes).
                    if (mod->accessors.count(d)) return why = cat("global ", d->name, " as static data"), false;
                    continue;
                }
                if (!mod->GlobalOk(d) || cg.IsFrameObj(d->type) ||
                    (d->type->kind == TY_REF && d->type->ref->lenstorage >= 0))
                    return why = cat("global ", d->name, " of this kind"), false;
            }
        return true;
    }

    // Whether a program-specific shim is one the object defines itself:
    // a global's accessor, or a `return from` channel's of a fixed type.
    bool OwnAccessor(const string &name) {
        if (auto it = mod->channels.find(name); it != mod->channels.end())
            return true;
        for (auto &[d, n] : mod->accessors)
            if (n == name) return true;
        for (auto &[d, n] : mod->stkaccessors)
            if (n == name) return true;
        for (auto &[d, n] : mod->flaccessors)
            if (n.first == name || n.second == name) return true;
        return false;
    }

    // gs_init_globals into the object, if its initializers compile and
    // need no more data stacks than the C's (stackneed[nullptr], which
    // gs_rt_init's counts were made with). Else why not.
    tyg_sym initsym = TYG_NO_SYM;
    bool BuildInit(CodeGen &cg, string &why) {
        auto nshims = mod->shims.size(), nexterns = mod->externs.size(), ndirect = mod->direct.size();
        auto sym = mod->Sym("gs_tg_init_globals", TYG_SYM_FUNC);
        TgFn fn(*mod, nullptr);
        auto undo = [&](const string &w) {
            if (fn.p) tyg_proc_abort(fn.p);
            mod->shims.resize(nshims);
            mod->externs.resize(nexterns);
            mod->direct.resize(ndirect);
            why = w;
            return false;
        };
        Line line;
        for (auto g : ast.globals)
            if (!g->inits.empty()) {
                line = g->line;
                break;
            }
        try {
            fn.EmitInit(sym, line);
        } catch (TgUnsupported &u) {
            return undo(cat("global initializer: ", u.why, u.line.line > 0 ? cat(" (line ", u.line.line, ")") : ""));
        }
        int64_t need = fn.maxown;
        for (auto &c : fn.stackcalls) need = std::max(need, c.offset + cg.stackneed[c.callee]);
        if (need > cg.stackneed[nullptr]) return why = "global initializers' data stacks", false;
        // Extern fns: as for any function of a whole program.
        for (auto sp : mod->externs) {
            auto found = false;
            for (auto &x : TgSupportExterns()) found = found || sp->sf->cname == x.name;
            if (!found) return why = cat("extern ", sp->sf->cname), false;
        }
        initsym = sym;
        return true;
    }

    // The globals' storage, their debug entries, and the accessors the
    // functions call, each returning its global's address.
    void EmitGlobalStorage(CodeGen &cg) {
        auto m = mod->m;
        map<const VarDef *, tyg_sym> gsyms, ssyms;
        map<const VarDef *, pair<tyg_sym, tyg_sym>> flsyms;
        auto def = [&](const string &name, int64_t size, int64_t align, VarDef *d, const tyg_dtype *dt) {
            auto sym = tyg_symbol(m, cat("gs_g_", name).c_str(), 0);
            tyg_data(m, sym, TYG_SECTION_BSS, nullptr, std::max<int64_t>(size, 1), align);
            if (dt) tyg_debug_global(m, string(d->name).c_str(), dt, sym, mod->File(d->line.fileidx), d->line.line);
            return sym;
        };
        for (auto g : ast.globals)
            for (auto d : g->defs) {
                if (cg.gstatic.count(d)) continue;
                auto name = cg.gnames[d].substr(7);   // "GS_GL->name"
                if (TgModule::IsGrow(d->type)) {
                    gsyms[d] = def(name, 16, 8, d, mod->DT(d->type));
                } else if (TgModule::IsVarArr(d->type)) {
                    gsyms[d] = def(name, 8, 8, d, tyg_dt_pointer(m, mod->DT(d->type)));
                } else {
                    auto sz = mod->Size(d->type);
                    int64_t align = 1;
                    while (align < 8 && align * 2 <= sz) align *= 2;
                    gsyms[d] = def(name, sz, align, d, mod->DT(d->type));
                }
                if (cg.gstks.count(d)) ssyms[d] = def(cat("gs_gstk_", name), 8, 8, d, nullptr);
                if (cg.gpools.count(d)) {
                    flsyms[d] = { def(cat(name, "_fl"), 16, 8, d, nullptr),
                                  def(cat("gs_gstk_", name, "_fl"), 8, 8, d, nullptr) };
                }
            }
        auto accessor = [&](const string &name, tyg_sym target) {
            auto p = tyg_proc_begin(m, mod->Sym(name, TYG_SYM_FUNC | TYG_SYM_GLOBAL),
                                    tyg_sig_new(m, tyg_type_ptr(m), nullptr, 0, 0));
            tyg_ret(p, tyg_addr_of(p, tyg_at_sym(target, 0)));
            if (tyg_proc_end(p) != 0)
                throw CompileError { cat("tinygen: ", name, ": ", tyg_error(m) ? tyg_error(m) : "?") };
        };
        for (auto &[d, n] : mod->accessors) accessor(n, gsyms.at(d));
        for (auto &[d, n] : mod->stkaccessors) accessor(n, ssyms.at(d));
        for (auto &[d, n] : mod->flaccessors) {
            accessor(n.first, flsyms.at(d).first);
            accessor(n.second, flsyms.at(d).second);
        }
        // The `return from` channels, which the C half keeps thread-local:
        // a whole program has one thread, so plain data (gap 6).
        for (auto &sh : mod->shims) {
            auto it = mod->channels.find(sh.first);
            if (it == mod->channels.end()) continue;
            auto sym = tyg_symbol(m, cat("gs_", sh.first.substr(6)).c_str(), 0);   // gs_lret_<tid>_<i>
            tyg_data(m, sym, TYG_SECTION_BSS, nullptr, std::max<int64_t>(it->second, 1), 8);
            accessor(sh.first, sym);
        }
    }

    // The C main of a whole program build, which the C half otherwise
    // writes (CodeGen::EmitProgramInit, EmitMain): the runtime started
    // with the program's stack counts, the globals initialized, fn main()
    // run, the stack report.
    // In the debugger it is `main` at fn main()'s line.
    void EmitMain(CodeGen &cg) {
        auto m = mod->m;
        auto mainsp = ast.MainFunction()->specs[0];
        auto i32 = tyg_type_int(m, TYG_I32, 1), i64 = tyg_type_int(m, TYG_I64, 1);
        auto ptr = tyg_type_ptr(m);
        const tyg_type *ps[] = { i32, ptr };
        auto p = tyg_proc_begin(m, mod->Sym("main", TYG_SYM_FUNC | TYG_SYM_GLOBAL),
                                tyg_sig_new(m, i32, ps, 2, 0));
        auto line = mainsp->sf->line;
        auto file = mod->File(line.fileidx);
        auto dint = tyg_dt_base(m, "int", 4, TYG_DT_SIGNED);
        const tyg_dtype *pdt[] = { dint, tyg_dt_pointer(m, tyg_dt_void(m)) };
        tyg_debug_proc(p, "main", file, line.line, tyg_dt_func(m, dint, pdt, 2, 0));
        tyg_loc(p, file, line.line, 0);
        auto call = [&](tyg_sig *sig, const string &name, vector<tyg_val> args) {
            vector<tyg_arg> as;
            for (auto a : args) {
                tyg_arg x {};
                x.val = a;
                as.push_back(x);
            }
            tyg_call(p, sig, tyg_direct(mod->Sym(name, TYG_SYM_FUNC | TYG_SYM_GLOBAL)), as.data(),
                     (int)as.size(), nullptr, nullptr);
        };
        const tyg_type *sps[] = { i32, ptr, i64, i64, i64 };
        call(tyg_sig_new(m, nullptr, sps, 5, 0), "gs_tg_start",
             { tyg_param(p, 0), tyg_param(p, 1), tyg_iconst(p, TYG_I64, cg.rtinit[0]),
               tyg_iconst(p, TYG_I64, cg.rtinit[1]), tyg_iconst(p, TYG_I64, cg.rtinit[2]) });
        if (initsym != TYG_NO_SYM) {
            tyg_call(p, tyg_sig_new(m, nullptr, nullptr, 0, 0), tyg_direct(initsym), nullptr, 0, nullptr,
                     nullptr);
        }
        vector<tyg_val> margs;
        if (cg.sinfo[mainsp].needssp) margs.push_back(tyg_iconst(p, TYG_I64, 0));
        call(mod->Sig(mainsp), cg.sinfo[mainsp].cname, margs);
        call(tyg_sig_new(m, nullptr, nullptr, 0, 0), "gs_tg_end", {});
        tyg_ret(p, tyg_iconst(p, TYG_I32, 0));
        if (tyg_proc_end(p) != 0)
            throw CompileError { cat("tinygen: main: ", tyg_error(m) ? tyg_error(m) : "?") };
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
