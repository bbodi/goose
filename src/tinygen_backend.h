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
    // gs_thread_spawn is the runtime.h helper of a program with threads
    // (GS_NEED_THREADS); the support object has its own (TgSupportBody).
    if (name == "gs_tg_thread_spawn")
        return "int64_t gs_tg_thread_spawn(void (*e)(uint8_t *), const void *a, int64_t n) "
               "{ return gs_thread_spawn(e, a, n); }\n";
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
               "#ifdef _MSC_VER\n#define GS_TG_TLS __declspec(thread)\n#else\n#define GS_TG_TLS __thread\n#endif\n"
               "static GS_TG_TLS int32_t gs_rf;\n";
    static const char *ops[] = { "add", "sub", "mul", "neg", "div", "mod" };
    static const IntStorage ints[] = { IS_I8, IS_I16, IS_I32, IS_I64, IS_U8, IS_U16, IS_U32, IS_U64 };
    for (auto op : ops)
        for (auto is : ints) {
            // runtime.h's unsigned helpers take no file and line, and only
            // division and modulo of unsigned values go through a shim.
            if (IsUnsigned(is) && string(op) != "div" && string(op) != "mod") continue;
            s += TgFixedShim(cat("gs_tg_", op, "_", CodeGen::IntSfx(is)));
        }
    for (auto n : { "gs_tg_stack_init", "gs_tg_rf", "gs_tg_rfpanic", "gs_tg_f2iwrap", "gs_tg_f2u", "gs_tg_f2i",
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
    // The thread's block of data stacks (gs_stks) is thread-local only in a
    // program built with GS_NEED_THREADS, which this object is not, so a
    // thread-local of its own holds the block the procedures reach; a
    // worker's thunk opens one and its end frees it (the runtime's
    // gs_stack_block and gs_thread_run).
    s += "static GS_TG_TLS gs_stack *gs_tg_tstks;\n"
         "void *gs_tg_stks(void) { return gs_tg_tstks; }\n"
         "void gs_tg_stack_block(int64_t n) {\n"
         "    gs_tg_tstks = (gs_stack *)calloc((size_t)(n > 0 ? n : 1), sizeof(gs_stack));\n"
         "    if (!gs_tg_tstks) gs_panic(\"out of memory allocating stack block\");\n"
         "    for (int64_t i = 0; i < n; i++) gs_stack_init(&gs_tg_tstks[i]);\n"
         "}\n"
         "static void gs_tg_thread_run(void (*entry)(uint8_t *), uint8_t *args) {\n"
         "    entry(args);\n"
         "    free(gs_tg_tstks);\n"
         "    gs_tg_tstks = NULL;\n"
         "}\n"
         "int64_t gs_tg_thread_spawn(void (*entry)(uint8_t *), const void *args, int64_t size) {\n"
         "    return gs_thread_start(gs_tg_thread_run, entry, args, size);\n"
         "}\n";
    s += "static uint8_t gs_tg_noglobals;\n"
         "void gs_tg_start(int argc, char **argv, int64_t mainstacks, int64_t mainregions,\n"
         "                 int64_t workerregions) {\n"
         "    gs_rt_init(argc, argv, mainstacks, mainregions, workerregions);\n"
         "    gs_tg_tstks = gs_stks;\n"
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
    // The queues and worker thunks the procedures name, each through an
    // accessor returning its address: the C half's static (a shim), or the
    // object's own in a whole program. Queue accessor -> the C queue's name.
    map<string, string> queueacc;
    map<string, FnSpec *> thunkacc;
    // The runtime's declarations (runtime.h, runtime_ext.h), for RuntimeApi.
    string_view rtdecls;

    // Whether the C function `name` is one the runtime object exports
    // (GS_API): a plain symbol, called directly without a trampoline.
    map<string, bool> rtapi;
    bool RuntimeApi(const string &name) {
        if (auto it = rtapi.find(name); it != rtapi.end()) return it->second;
        // The C library's math functions are plain symbols the linker
        // resolves (never macros, unlike isnan), so no trampoline either.
        static const set<string> libm = [] {
            set<string> r;
            for (auto f : { "acos", "asin", "atan", "atan2", "cbrt", "ceil", "copysign", "cos", "cosh",
                            "erf", "exp", "exp2", "expm1", "fabs", "fdim", "floor", "fma", "fmax",
                            "fmin", "fmod", "hypot", "lgamma", "log", "log10", "log1p", "log2",
                            "nearbyint", "pow", "remainder", "rint", "round", "sin", "sinh", "sqrt",
                            "tan", "tanh", "tgamma", "trunc" }) {
                r.insert(f);
                r.insert(cat(f, "f"));
            }
            return r;
        }();
        if (libm.count(name)) return rtapi[name] = true;
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
    // The accessor of the queue for elements of type t (CodeGen::QueueFor),
    // which returns the gs_queue's address.
    string QueueAccessor(TypeExpr *t) {
        auto q = cg.QueueFor(t);
        auto name = cat("gs_tg_q_", q);
        // The shim again for each use: a function abandoned after it took
        // the shim back (TinyGenBackend::Build).
        queueacc[name] = q;
        NeedShim(name, cat("void *", name, "(void) { return &", q, "; }\n"));
        return name;
    }
    // The accessor of a worker's entry thunk (CodeGen::EnsureThreadThunk),
    // which returns the thunk's address.
    string ThunkAccessor(FnSpec *sp) {
        auto thunk = cg.EnsureThreadThunk(sp);
        auto name = cat("gs_tg_t_", thunk);
        thunkacc[name] = sp;
        NeedShim(name, cat("void *", name, "(void) { return (void *)", thunk, "; }\n"));
        return name;
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
                // A relative reference outside a slot (a local, a parameter,
                // a global) holds the offset too: of the `in pool` form, or
                // a self-relative one only null reaches (§3.9).
                if (IsRel(t)) return !IsVr(t) && RelOk(t, depth);   // The varint width is an image (ImgOk).
                return (IsPlainRef(t) || OptRef(t)) &&
                       ((cg.IsFix(t->ref->sub) && TypeOk(t->ref->sub, depth + 1)) || IsVarint(t->ref->sub) ||
                        ImgOk(t->ref->sub, depth + 1) || ZeroOk(t->ref->sub, depth + 1) ||
                        ((IsPlainRef(t) || OptRef(t)) && (FoOk(t->ref->sub, depth + 1) || RsOk(t->ref->sub, depth + 1))) ||
                        (IsPlainRef(t) && VeOk(t->ref->sub, depth + 1)));
            case TY_ARRAY:
                // T[k]: k elements, packed; a static-capacity T[..k], a
                // length beside them (LimOk).
                if (ZeroArr(t)) return depth == 0 && ZeroOk(t, depth);
                if (t->arr->akind == A_LIMITED) return LimOk(t, depth);
                return t->arr->akind == A_FIXED && cg.IsFix(t) &&
                       cg.ArrSize(t->arr) > 0 && TypeOk(t->arr->sub, depth + 1);
            case TY_SLICE:
                // { data, len }, over fixed elements (C.3), or over images
                // walked by their sizes.
                return cg.IsFix(t->sub) ? TypeOk(t->sub, depth + 1) : ImgOk(t->sub, depth + 1) || IsVarint(t->sub);
            case TY_STRUCT: {
                if (!cg.IsFix(t)) return false;
                auto si = cg.SI(t);
                if (!si) return false;
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
                if (!cg.IsFix(t->sub)) return ImgOk(t->sub, depth + 1) || IsVarint(t->sub) ? "" : "variable-size elements";
                return Blocker(t->sub, depth + 1);
            case TY_ARRAY:
                switch (t->arr->akind) {
                    case A_LIMITED:
                        if (cg.ArrSize(t->arr) < 0) return RcOk(t, depth) ? "" : "runtime-capacity limited array";
                        break;
                    case A_FIXED:
                        break;
                    case A_VAR:
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
                if (IsRb(t)) return RbOk(t, depth) ? "" : "resizable struct";
                if (IsRs(t)) {
                    if (RsOk(t, depth)) return "";
                    auto si = cg.SI(t);
                    for (size_t i = 0; i < si->st->fields.size(); i++) {
                        if (si->st->fields[i].ispad) continue;
                        auto ft = si->ftypes[i];
                        if (cg.IsResz(ft) && (int)i == LastRealField(si->st->fields)) {
                            if (!(IsGrow(ft) && RzOk(ft)) && !FoOk(ft)) return cat("resizable struct with tail ", Name(ft));
                            continue;
                        }
                        auto b = Blocker(ft, depth + 1);
                        if (!b.empty() && b != "variable-size value") return cat("resizable struct with ", b);
                    }
                    return "resizable struct";
                }
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
                auto si = cg.SI(t);
                if (!si) return "empty struct";
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
        } else if (k != A_GROW && k != A_GROWSHRINK) {
            return false;
        }
        return cg.IsFix(t->arr->sub) && TypeOk(t->arr->sub, depth + 1);
    }
    // A variable-size struct (VsOk) is an image too, read where it lies.
    bool ImgOk(TypeExpr *t, int depth = 0) {
        return RzOk(t, depth) || VsOk(t, depth) || VrOk(t, depth) || VarImgOk(t, depth) || VmOk(t, depth) || VvOk(t, depth) ||
               RcOk(t, depth) || RbOk(t, depth);
    }
    // A T[] whose elements are images themselves (u8[][], an array of
    // strings): its length prefix, then the elements back to back, each
    // walked by its size; never indexed (the checker forbids it).
    bool VarImgOk(TypeExpr *t, int depth = 0) {
        if (!t || t->kind != TY_ARRAY || t->arr->akind != A_VAR || depth > 32) return false;
        auto e = t->arr->sub;
        return e && !cg.IsFix(e) && (ImgOk(e, depth + 1) || IsVarint(e));
    }
    // A resizable array whose elements are images (strings, variable-size
    // structs): their bytes back to back on its stack, each walked by its
    // size (CodeGen::SizeFn). The checker never lets one be indexed; it is
    // built by push and read by `for`, len and text.
    bool VeOk(TypeExpr *t, int depth = 0) {
        if (!t || t->kind != TY_ARRAY || depth > 32) return false;
        if (t->arr->akind != A_GROW && t->arr->akind != A_GROWSHRINK) return false;
        auto e = t->arr->sub;
        return e && !cg.IsFix(e) && (ImgOk(e, depth + 1) || IsVarint(e));
    }
    static bool IsGrow(TypeExpr *t) {
        return t && t->kind == TY_ARRAY && (t->arr->akind == A_GROW || t->arr->akind == A_GROWSHRINK);
    }
    // A variable-size T[]: an image, its length prefix then its elements.
    static bool IsVarArr(TypeExpr *t) { return t && t->kind == TY_ARRAY && t->arr->akind == A_VAR; }
    // A T[] length prefix's bytes: u32 by default, else its storage's;
    // 0 for a varint (ULEB128, CodeGen::EmitLenStore).
    static int LenWidth(TypeExpr *t) {
        switch (t->arr->lenstorage) {
            case -1: case IS_U32: case IS_I32: return 4;
            case IS_U8: case IS_I8: return 1;
            case IS_U16: case IS_I16: return 2;
            case IS_U64: case IS_I64: return 8;
            default: return 0;
        }
    }
    // The bytes reserved for it while the elements are built: a varint's
    // one, moved up behind a longer count (CodeGen::PrefixBytes).
    static int PrefixBytes(TypeExpr *t) { return LenWidth(t) ? LenWidth(t) : 1; }
    static bool SignedLen(TypeExpr *t) {
        auto ls = t->arr->lenstorage;
        return ls == IS_I8 || ls == IS_I16 || ls == IS_I32 || ls == IS_I64;
    }
    // A by-value parameter of a type RzOk or VsOk holds.
    bool RzParam(TypeExpr *t) {
        return (t->kind == TY_ARRAY && (RzOk(t) || VarImgOk(t) || RcOk(t) || VeOk(t))) || VsOk(t) || VrOk(t) || VmOk(t) ||
               VvOk(t) || FoOk(t) || RsOk(t) || RbOk(t);
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
        if (IsVr(t)) return 16;   // A temporary for the value: up to ten bytes of varint.
        if (IsRb(t)) return 16;   // Its header.
        if (IsScalar(t)) return ScalSize(Scal(t));
        // A payload-less variant as a value of its own: the gs_empty byte.
        if (t->kind == TY_VARIANT && !cg.FixedSize(t)) return 1;
        if (ZeroArr(t)) return cg.FixedSize(t) + cg.FixedSize(t->arr->sub);
        if (FoOk(t)) return FoLayout(t).tail + FoTailSize(t);
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
        if (IsRel(t) && !IsVr(t)) return tyg_type_int(m, RelScal(t), 0);
        if (t->kind == TY_REF) return tyg_type_ptr(m);   // A varint-width relative reference: its image.
        if (IsGrow(t) || IsRs(t) || IsRb(t)) return SliceAT();   // gs_rhdr { base, len }, as a slice.
        if (t->kind == TY_STRUCT && cg.IsFrameObj(t)) return FoAT(t);
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
            tyg_field f { tyg_type_array(m, AT(t->arr->sub), std::max<int64_t>(cg.ArrSize(t->arr), 1)), 0 };
            return atypes[key] = tyg_type_record(m, Size(t), 1, &f, 1);
        }
        vector<tyg_field> fs;
        for (auto &f : Fields(t)) fs.push_back({ AT(f.t), f.off });
        return atypes[key] = tyg_type_record(m, cg.FixedSize(t), 1, fs.data(), (int)fs.size());
    }

    // A frame object's C struct: its fixed fields, then the tail's header.
    const tyg_type *FoAT(TypeExpr *t) {
        auto key = cg.Mangle(t);
        if (auto it = atypes.find(key); it != atypes.end()) return it->second;
        auto si = cg.SI(t);
        auto fl = FoLayout(t);
        vector<tyg_field> fs;
        for (int i = 0; i < fl.tailfi; i++) {
            if (si->st->fields[(size_t)i].ispad) continue;
            fs.push_back({ AT(si->ftypes[(size_t)i]), fl.offs[(size_t)i] });
        }
        fs.push_back({ AT(si->ftypes[(size_t)fl.tailfi]), fl.tail });
        return atypes[key] = tyg_type_record(m, fl.tail + FoTailSize(t), 1, fs.data(), (int)fs.size());
    }
    // A resizable value: a T[>..]'s header, or a frame object, with a stack.
    bool Resz(TypeExpr *t) { return IsGrow(t) || IsRs(t) || IsRb(t) || (t && t->kind == TY_STRUCT && cg.IsFrameObj(t)); }

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
                    auto sz = IsVr(t) ? 1 : ScalSize(Scal(t));   // A varint's first byte.
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
                    d = tyg_dt_array(m, DT(t->arr->sub), std::max<int64_t>(cg.ArrSize(t->arr), 1));
                    break;
                }
                auto rec = tyg_dt_record(m, TYG_DT_STRUCT, name.c_str(), File(0), 0);
                dtypes[name] = rec;
                if (IsLim(t)) return LimDT(t, rec);
                if (t->arr->akind == A_VAR) {
                    // The image: its length prefix, then the elements, as
                    // many as it says (an array of unknown size).
                    auto w = PrefixBytes(t);
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
                if (IsRb(t)) return RbDT(name);
                if (IsVs(t)) return VsDT(t, name);
                if (IsRs(t)) return RsDT(t, name);
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
            case TY_ENUM: case TY_VARIANT:
                if (IsRb(t)) return RbDT(name);
                return IsVv(t) ? VvDT(t, name) : AdtDT(t, name);
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
    // A T[0] or T[..0] (§3.4): C gives the one element its struct needs, so
    // as a value of its own it is a slot's worth bigger than its layout, and
    // only that (a parameter, a result, a local, a global, behind a
    // reference) is held: a field or an element's layout has no such slot.
    bool ZeroArr(TypeExpr *t) {
        return t && t->kind == TY_ARRAY && (t->arr->akind == A_FIXED || cg.IsStaticLimited(t)) &&
               cg.ArrSize(t->arr) == 0;
    }
    bool ZeroOk(TypeExpr *t, int depth) {
        return ZeroArr(t) && cg.IsFix(t) && depth <= 32 && TypeOk(t->arr->sub, depth + 1);
    }
    bool LimOk(TypeExpr *t, int depth) {
        return cg.IsStaticLimited(t) && cg.ArrSize(t->arr) > 0 &&
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
                            { tyg_type_array(m, AT(t->arr->sub), std::max<int64_t>(cg.ArrSize(t->arr), 1)), w } };
        return atypes[key] = tyg_type_record(m, Size(t), 1, fs, 2);
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
        tyg_dt_member(m, rec, "e", tyg_dt_array(m, DT(t->arr->sub), std::max<int64_t>(cg.ArrSize(t->arr), 1)), w);
        tyg_dt_record_complete(m, rec, Size(t), 1);
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
        if (Reentered(t)) return true;   // Through an optional reference.
        Open open { this, t };
        auto si = cg.SI(t);
        auto fl = FoLayout(t);
        if ((size_t)fl.tailfi + 1 != si->st->fields.size()) return false;
        auto tail = si->ftypes[(size_t)fl.tailfi];
        if (!(IsGrow(tail) && (RzOk(tail, depth + 1) || VeOk(tail, depth + 1))) && !FoOk(tail, depth + 1)) return false;
        for (int i = 0; i < fl.tailfi; i++) {
            if (si->st->fields[(size_t)i].ispad) continue;
            auto ft = si->ftypes[(size_t)i];
            if (!cg.IsFix(ft) || !TypeOk(ft, depth + 1)) return false;
        }
        return true;
    }
    // The tail's bytes: a resizable's header, or a nested frame object's
    // struct, which ends in its own tail.
    int64_t FoTailSize(TypeExpr *t) {
        auto tail = cg.SI(t)->ftypes[(size_t)FoLayout(t).tailfi];
        return IsGrow(tail) ? 16 : Size(tail);
    }
    // Where the innermost tail's header lies in the struct.
    int64_t FoHdrOff(TypeExpr *t) {
        auto fl = FoLayout(t);
        auto tail = cg.SI(t)->ftypes[(size_t)fl.tailfi];
        return IsGrow(tail) ? fl.tail : fl.tail + FoHdrOff(tail);
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
        tyg_dt_record_complete(m, rec, fl.tail + FoTailSize(t), 1);
        return rec;
    }

    // ---- Variable-size structs ----------------------------------------------

    // A struct with a variable-size field (bytes class, §6.1): one image of
    // its fields in declaration order, packed, each variable-size one (a
    // T[], or such a struct) inline at its own size, which a uint8_t *
    // points at, as a T[]'s does. Frame objects (a resizable field) are
    // another representation.
    bool IsVs(TypeExpr *t) { return t && t->kind == TY_STRUCT && cg.Cls(t) == SC_VARIABLE; }
    // A resizable struct that is not a frame object: its fields before the
    // tail are not all fixed, so they are bytes as a variable-size struct's
    // are, and the tail's elements follow them on the data stack. The value
    // is a gs_rhdr { base, len } as a resizable's is, base the start of
    // the image, len the tail's count (CodeGen::FieldLocAt, BytesLoc).
    bool IsRs(TypeExpr *t) { return t && t->kind == TY_STRUCT && cg.IsResz(t) && !cg.IsFrameObj(t) && !IsRb(t); }
    // One this backend holds: the fields before the tail as a variable-size
    // struct's (FieldsOk), the tail a resizable of fixed elements.
    bool RsOk(TypeExpr *t, int depth = 0) {
        if (!IsRs(t) || depth > 32) return false;
        if (Reentered(t)) return true;   // Through an optional reference.
        Open open { this, t };
        auto si = cg.SI(t);
        auto last = LastRealField(si->st->fields);
        if (last < 0) return false;
        vector<Field> pre(si->st->fields.begin(), si->st->fields.begin() + last);
        vector<TypeExpr *> pret(si->ftypes.begin(), si->ftypes.begin() + last);
        auto tail = si->ftypes[(size_t)last];
        return ((IsGrow(tail) && RzOk(tail, depth + 1)) || FoOk(tail, depth + 1)) && FieldsOk(pre, pret, depth + 1);
    }
    // The tail's elements' type: a resizable's, or a frame object's innermost.
    TypeExpr *RsElem(TypeExpr *t) {
        auto si = cg.SI(t);
        auto tail = si->ftypes[(size_t)LastRealField(si->st->fields)];
        return IsGrow(tail) ? tail->arr->sub : cg.FoTailArr(tail)->arr->sub;
    }
    const tyg_dtype *RsDT(TypeExpr *t, const string &name) {
        auto si = cg.SI(t);
        auto rec = tyg_dt_record(m, TYG_DT_STRUCT, name.c_str(), File(si->st->line.fileidx), si->st->line.line);
        dtypes[name] = rec;
        tyg_dt_member(m, rec, "base", tyg_dt_pointer(m, DT(ast.inttypes[IS_U8])), 0);
        tyg_dt_member(m, rec, "len", DT(ast.inttypes[IS_I64]), 8);
        tyg_dt_record_complete(m, rec, 16, 1);
        return rec;
    }
    // An image: a T[] or such a struct.
    // (A resizable ADT is a header and a stack, IsRb, not an image.)
    bool IsImg(TypeExpr *t) {
        return IsVarArr(t) || IsVs(t) || ((IsVm(t) || IsVv(t)) && !IsRb(t)) || IsRc(t) || IsVr(t);
    }
    // One this backend holds: its fields fixed values it holds, T[]s of
    // fixed elements (RzOk), or such structs; no relative references.
    bool VsOk(TypeExpr *t, int depth = 0) {
        if (!IsVs(t) || depth > 32 || cg.SI(t)->sclass != SC_VARIABLE) return false;
        if (Reentered(t)) return true;   // Through an optional reference.
        Open open { this, t };
        auto si = cg.SI(t);
        for (size_t i = 0; i < si->st->fields.size(); i++) {
            if (si->st->fields[i].ispad) continue;
            auto ft = si->ftypes[i];
            if (IsVarint(ft)) continue;
            if (IsVr(ft)) {
                if (!VrOk(ft, depth + 1)) return false;
                continue;
            }
            if (cg.IsFix(ft) ? !TypeOk(ft, depth + 1)
                             : !((IsVarArr(ft) && (RzOk(ft, depth + 1) || VarImgOk(ft, depth + 1))) || RcOk(ft, depth + 1) ||
                                 VsOk(ft, depth + 1) || VmOk(ft, depth + 1)))
                return false;
        }
        return true;
    }
    // The fewest bytes an image of t takes: its fields up to the first
    // variable-size one, and that one's least (a T[]'s prefix).
    int64_t VsMinSize(TypeExpr *t) {
        if (cg.IsFix(t)) return cg.FixedSize(t);
        if (IsVarint(t) || IsVr(t)) return 1;
        if (IsVarArr(t)) return PrefixBytes(t);
        if (IsRc(t)) return 8;   // cap and len.
        if (IsVm(t)) return TagSize(t);
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
        if (!cg.IsFix(t)) return false;
        if (Reentered(t)) return true;   // Through a reference or a slice.
        Open open { this, t };
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
        if (IsRb(t)) return RbOk(t, depth) ? "" : "resizable ADT";
        if (t->kind == TY_ENUM && t->enu->varmode) return VmOk(t, depth) ? "" : "variable-mode ADT";
        if (!cg.IsFix(t)) return VvOk(t, depth) ? "" : "variable-size ADT";
        if (Reentered(t)) return "";
        Open open { this, t };
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
        if (w != IS_U8 && w != IS_U16 && w != IS_U32 && w != IS_U64 && w != IS_VARINT) return false;
        return RelSubOk(t, depth);
    }
    // The varint width (`T&<varint in pool>`, `T&<varint>`): its offset is
    // ULEB128 (zigzag when self-relative), so the value is variable-size,
    // a bytes value a uint8_t * points at, as a T[] is (SC_VARIABLE).
    static bool IsVr(const TypeExpr *t) { return IsRel(t) && t->ref->lenstorage == IS_VARINT; }
    bool VrOk(TypeExpr *t, int depth = 0) { return IsVr(t) && depth <= 32 && RelSubOk(t, depth); }
    bool RelSubOk(TypeExpr *t, int depth) {
        auto sub = t->ref->sub;
        if (cg.IsFix(sub)) return TypeOk(sub, depth + 1);
        // An image it points at, which may hold such links to its own type.
        if (relimg.count(sub)) return true;
        relimg.insert(sub);
        auto ok = ImgOk(sub, depth + 1);
        relimg.erase(sub);
        return ok;
    }
    set<TypeExpr *> relimg;
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
        if (Reentered(t)) return true;   // Through an optional reference.
        Open open { this, t };
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
            if (IsVr(ft)) {
                if (!VrOk(ft, depth + 1)) return false;
                continue;
            }
            if (cg.IsFix(ft) ? !TypeOk(ft, depth + 1)
                             : !((IsVarArr(ft) && (RzOk(ft, depth + 1) || VarImgOk(ft, depth + 1))) || RcOk(ft, depth + 1) ||
                                 VsOk(ft, depth + 1) || VmOk(ft, depth + 1)))
                return false;
        }
        return true;
    }
    // A variant type whose payload has a variable-size field (`Msg.Text`
    // with a u8[]): an image of its fields, as a variable-size struct is.
    bool IsVv(TypeExpr *t) { return t && t->kind == TY_VARIANT && !cg.IsFix(t); }
    bool VvOk(TypeExpr *t, int depth = 0) {
        if (!IsVv(t) || depth > 32) return false;
        if (Reentered(t)) return true;   // Through an optional reference.
        Open open { this, t };
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
            if (!cg.IsFix(ft)) { off += IsVarArr(ft) ? PrefixBytes(ft) : 0; break; }
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

    // ---- Resizable ADTs ------------------------------------------------------

    // A resizable-class ADT (`Log..` with a variant ending in a T[>..]) or
    // one of its resizable variants (`Value.A`): a gs_rhdr { base, len } and
    // a data stack, as a T[>..] is, but the bytes at base are the value's
    // own layout: its tag, for the ADT, then the variant's fixed fields,
    // packed, then the tail's elements, `len` of them (CodeGen::GenStructLit,
    // VariantBehindTag). A fixed variant of such an ADT has no tail: its
    // header's count is zero. The C copies, binds and dispatches none of the
    // resizable payloads.
    bool IsRb(TypeExpr *t) {
        if (!t) return false;
        if (t->kind == TY_ENUM) return t->enu->varmode && cg.IsResz(t);
        // A struct that is resizable and no frame object: a fixed prefix,
        // and a tail that is such a value (a resizable ADT).
        if (t->kind == TY_STRUCT) {
            if (!cg.IsResz(t) || cg.IsFrameObj(t) || !cg.SI(t)) return false;
            auto si = cg.SI(t);
            auto last = LastRealField(si->st->fields);
            return last >= 0 && si->ftypes[(size_t)last]->kind != TY_ARRAY && IsRb(si->ftypes[(size_t)last]);
        }
        return t->kind == TY_VARIANT && cg.IsResz(t);
    }
    // A resizable struct's fields: the fixed ones' offsets, the tail's.
    struct RbSt {
        vector<int64_t> offs;
        int64_t pre = 0;   // The bytes before the tail.
        int tailfi = -1;
        bool ok = true;
    };
    RbSt RbStShape(TypeExpr *t) {
        RbSt sh;
        auto si = cg.SI(t);
        auto &fs = si->st->fields;
        auto last = LastRealField(fs);
        int64_t off = 0;
        for (size_t i = 0; i < fs.size(); i++) {
            sh.offs.push_back(off);
            if (fs[i].ispad) {
                if (fs[i].padsize < 0) sh.ok = false;
                else off += fs[i].padsize;
                continue;
            }
            if ((int)i == last) {
                sh.tailfi = (int)i;
                break;
            }
            if (!cg.IsFix(si->ftypes[i])) {
                sh.ok = false;
                break;
            }
            off += cg.FixedSize(si->ftypes[i]);
        }
        sh.pre = off;
        return sh;
    }
    bool RbStOk(TypeExpr *t, int depth) {
        auto sh = RbStShape(t);
        if (!sh.ok || sh.tailfi < 0) return false;
        auto si = cg.SI(t);
        for (int i = 0; i < sh.tailfi; i++) {
            if (si->st->fields[(size_t)i].ispad) continue;
            if (!TypeOk(si->ftypes[(size_t)i], depth + 1)) return false;
        }
        auto tail = si->ftypes[(size_t)sh.tailfi];
        return tail->kind != TY_ARRAY && IsRb(tail) && RbOk(tail, depth + 1);
    }
    // Where a variant's fields lie behind its tag, and its tail.
    struct RbShape {
        vector<int64_t> offs;        // Per field, from the payload's start.
        int64_t pre = 0;             // The payload's bytes before its tail's elements.
        TypeExpr *elem = nullptr;    // The tail's element type, null without one.
        int tailfi = -1;
        bool img = false;            // A variable-size field: an image, walked by its size (VvOk).
        bool fo = false;             // The tail is a frame object, its fields bytes ahead of its elements.
        bool ok = true;
    };
    RbShape RbShapeOf(EnumInst *ei, int vi) {
        RbShape sh;
        auto &fs = ei->en->variants[(size_t)vi].fields;
        int64_t off = 0;
        for (size_t i = 0; i < fs.size(); i++) {
            sh.offs.push_back(off);
            if (fs[i].ispad) {
                if (fs[i].padsize < 0) sh.ok = false;   // A bare pad aligns here, inserts nothing there.
                else off += fs[i].padsize;
                continue;
            }
            auto ft = ei->vftypes[(size_t)vi][i];
            if (cg.IsResz(ft)) {
                if ((int)i != LastRealField(fs)) sh.ok = false;
                else if (IsGrow(ft)) sh.elem = ft->arr->sub, sh.tailfi = (int)i;
                else if (cg.IsFrameObj(ft)) {
                    // A frame object (C.2) as bytes (CodeGen::FoBytes): its fixed
                    // fields, then its innermost tail's elements.
                    sh.elem = cg.FoTailArr(ft)->arr->sub, sh.tailfi = (int)i, sh.fo = true;
                    off += cg.FoBytesPrefix(ft);
                } else sh.ok = false;
                break;
            }
            if (!cg.IsFix(ft)) {   // A variable-size field: a size to walk, no offset.
                sh.img = true;
                for (auto j = i + 1; j < fs.size(); j++)
                    if (!fs[j].ispad && cg.IsResz(ei->vftypes[(size_t)vi][j])) sh.ok = false;
                break;
            }
            off += cg.FixedSize(ft);
        }
        sh.pre = off;
        return sh;
    }
    // The variant vi of the ADT or variant type t, which a resizable ADT
    // holds: a fixed one, a variable-size one (an image, no tail), or one
    // ending in a T[>..].
    bool RbVarOk(TypeExpr *t, int vi, int depth) {
        auto ei = EI(t);
        auto sh = RbShapeOf(ei, vi);
        if (!sh.ok) return false;
        if (sh.img) return VvOk(t->kind == TY_VARIANT ? t : cg.VariantType(t, vi), depth + 1);
        auto &fs = ei->en->variants[(size_t)vi].fields;
        for (size_t i = 0; i < fs.size(); i++) {
            if (fs[i].ispad) continue;
            auto ft = ei->vftypes[(size_t)vi][i];
            if ((int)i == sh.tailfi) {
                if (sh.fo ? !(FoOk(ft, depth + 1) && RzOk(cg.FoTailArr(ft), depth + 1)) : !RzOk(ft, depth + 1)) return false;
                continue;
            }
            if (!cg.IsFix(ft) || !TypeOk(ft, depth + 1)) return false;
        }
        return true;
    }
    bool RbOk(TypeExpr *t, int depth = 0) {
        if (!IsRb(t) || depth > 32) return false;
        if (Reentered(t)) return true;   // Through an optional reference.
        Open open { this, t };
        if (t->kind == TY_STRUCT) return RbStOk(t, depth);
        auto ei = EI(t);
        if (t->kind == TY_VARIANT) return RbVarOk(t, VariantIdx(t), depth);
        for (size_t vi = 0; vi < ei->en->variants.size(); vi++)
            if (!RbVarOk(t, (int)vi, depth)) return false;
        return true;
    }
    // Its header, under its Goose name: where the value starts, and the
    // count of its tail's elements.
    const tyg_dtype *RbDT(const string &name) {
        auto rec = tyg_dt_record(m, TYG_DT_STRUCT, name.c_str(), File(0), 0);
        dtypes[name] = rec;
        tyg_dt_member(m, rec, "base", tyg_dt_pointer(m, DT(ast.inttypes[IS_U8])), 0);
        tyg_dt_member(m, rec, "len", DT(ast.inttypes[IS_I64]), 8);
        tyg_dt_record_complete(m, rec, 16, 1);
        return rec;
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
    // Whether a struct or ADT reaches a value of its own type through its
    // fields (an optional link, a slice): its text would recurse, which the C half's
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
                case TY_ENUM: case TY_VARIANT: {
                    if (depth > 0 && cg.TEq(x, t)) return true;
                    if (!seen.insert(cg.Mangle(x)).second) return false;
                    auto ei = EI(x);
                    for (size_t vi = 0; vi < ei->en->variants.size(); vi++) {
                        if (x->kind == TY_VARIANT && (int)vi != VariantIdx(x)) continue;
                        auto &v = ei->en->variants[vi];
                        for (size_t i = 0; i < v.fields.size(); i++)
                            if (!v.fields[i].ispad && walk(ei->vftypes[vi][i], depth + 1)) return true;
                    }
                    return false;
                }
                default: return false;
            }
        };
        return walk(t, 0);
    }

    // The render procedures of types that reach themselves, one per type
    // (CodeGen::RenderFn): named by the type's mangled name, queued as they
    // are asked for and built once the function asking is done, as only one
    // procedure is built at a time (TinyGenBackend::BuildRenders).
    map<string, string> renderfns;
    vector<pair<string, TypeExpr *>> renderq;
    string RenderFn(TypeExpr *t) {
        auto k = cg.Mangle(t);
        auto it = renderfns.find(k);
        if (it != renderfns.end()) return it->second;
        auto name = cat("gs_tg_render_", k);
        renderfns[k] = name;
        renderq.emplace_back(name, t);
        return name;
    }
    // The verifier procedure of an image's element type (CodeGen::VerifyFn):
    // int64(payload, n, bitmap), queued like a render procedure.
    string VerifyFn(TypeExpr *elem) {
        auto k = cg.Mangle(elem);
        auto name = cat("gs_tg_verify_", k);
        if (renderfns.emplace(name, name).second) renderq.emplace_back(name, elem);
        return name;
    }
    // Whether a render procedure can take the type: a fixed struct or ADT
    // whose parts are all held (TypeOk), or an image of one (a variable-size
    // struct, a variable-mode ADT), handed the address of its value.
    bool RenderFnOk(TypeExpr *t) {
        if (t->kind == TY_ENUM && IsVm(t)) return VmOk(t);
        if (t->kind == TY_STRUCT && (VsOk(t) || FoOk(t) || RsOk(t))) return true;
        return (t->kind == TY_STRUCT || t->kind == TY_ENUM) && cg.IsFix(t) && !FoOk(t) && TypeOk(t);
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
        if (!d->isglobal || !cg.gnames.count(d)) return false;
        if (cg.PrefVar(d)) return cg.IsFatRef(d->type) && TypeOk(d->type);   // A gs_pref.
        if (d->reusable) return IsGrow(d->type) && RzOk(d->type) && cg.gstks.count(d) && cg.gpools.count(d);
        if (TypeOk(d->type)) return true;
        return ((IsGrow(d->type) || IsVarArr(d->type)) && RzOk(d->type) && cg.gstks.count(d)) ||
               (VarImgOk(d->type) && cg.gstks.count(d)) ||
               ((VsOk(d->type) || VmOk(d->type) || VvOk(d->type) || RcOk(d->type) || VrOk(d->type)) && cg.gstks.count(d)) ||
               (IsGrow(d->type) && VeOk(d->type) && cg.gstks.count(d)) || (FoOk(d->type) && cg.gstks.count(d)) ||
               (RsOk(d->type) && cg.gstks.count(d)) || (RbOk(d->type) && cg.gstks.count(d));
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
                if (!RetOk(r)) {
                    why = cat("return from target with result type ", NameWhy(r));
                    return false;
                }
        for (auto r : sp->rets)
            if (!RetOk(r)) { why = cat("result type ", NameWhy(r)); return false; }
        for (size_t i = 0; i < sp->argtypes.size(); i++)
            if (cg.IsPoolParam(sp, i) ? !(cg.IsFatRef(sp->argtypes[i]) && TypeOk(sp->argtypes[i]))
                                      : !(TypeOk(sp->argtypes[i]) || RzParam(sp->argtypes[i]))) {
                why = cat("parameter type ", NameWhy(sp->argtypes[i]));
                return false;
            }
        return true;
    }
    // A result this backend holds: a fixed value, or a resizable array or
    // a T[] image of fixed elements, built at the caller's destination
    // stack.
    bool RetOk(TypeExpr *t) {
        if (IsRel(t)) return false;
        return TypeOk(t) || ((IsGrow(t) || IsVarArr(t)) && RzOk(t)) || VsOk(t) || VmOk(t) || VvOk(t) || RcOk(t) ||
               FoOk(t) || RsOk(t) || RbOk(t) ||
               VarImgOk(t) || VeOk(t);
    }
    // A free variable of a nested function (§7.5), as SigParams passes it:
    // a fixed variable by its address, a T[] by its image's address, and
    // a resizable by its header's address and then its stack. A pool
    // reference by its gs_pref's address.
    bool FvOk(const VarDef *fv) {
        if (!fv->type) return false;
        if (cg.PrefVar(fv)) return cg.IsFatRef(fv->type) && TypeOk(fv->type);   // A gs_pref's address.
        if (fv->reusable) return IsGrow(fv->type) && RzOk(fv->type);   // A gs_pref.
        return TypeOk(fv->type) || ((IsGrow(fv->type) || IsVarArr(fv->type)) && RzOk(fv->type)) || VsOk(fv->type) ||
               VrOk(fv->type) || RcOk(fv->type) || FoOk(fv->type) || RbOk(fv->type) ||
               VmOk(fv->type) || VvOk(fv->type) || VarImgOk(fv->type) || VeOk(fv->type) || RsOk(fv->type);
    }

    tyg_sig *Sig(FnSpec *sp) {
        if (auto it = sigs.find(sp); it != sigs.end()) return it->second;
        auto &si = cg.sinfo[sp];
        vector<const tyg_type *> ps;
        for (size_t i = 0; i < sp->argtypes.size(); i++) {
            auto t = sp->argtypes[i];
            if (cg.IsPoolParam(sp, i)) {
                // A pool reference, gs_pref, as its four members (ParamFields).
                for (int k = 0; k < 4; k++) ps.push_back(tyg_type_ptr(m));
                continue;
            }
            // A by-value T[] is its image's address; a by-value T[>..] its
            // header, then its stack (CodeGen::SigParams).
            if (RzParam(t) && !Resz(t)) {
                ps.push_back(tyg_type_ptr(m));
                continue;
            }
            // A large fixed value is its copy's address (SigParams).
            ps.push_back(cg.IsLargeFixed(t) ? tyg_type_ptr(m) : AT(t));
            if (RzParam(t)) ps.push_back(tyg_type_ptr(m));
        }
        for (auto fv : si.freevars) {
            if (fv->reusable) {
                for (int k = 0; k < 4; k++) ps.push_back(tyg_type_ptr(m));   // gs_pref's members
                continue;
            }
            ps.push_back(tyg_type_ptr(m));
            if (Resz(fv->type)) ps.push_back(tyg_type_ptr(m));   // its stack
        }
        for (size_t i = 0; i < sp->rets.size(); i++) {
            if (Resz(sp->rets[i])) {
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
    // ---- IR shorthands ---------------------------------------------------------
    tyg_val I64(int64_t v) { return tyg_iconst(p, TYG_I64, v); }
    tyg_addr AtL(tyg_local l, int64_t off = 0) { return tyg_at_local(l, off); }
    tyg_addr AtV(tyg_val v, int64_t off = 0) { return tyg_at_val(v, off); }
    tyg_val Ld(tyg_addr a) { return tyg_load(p, TYG_I64, a, 0); }
    void St(tyg_addr a, tyg_val v) { tyg_store(p, TYG_I64, a, v, 0); }
    tyg_val LdL(tyg_local l, int64_t off = 0) { return Ld(AtL(l, off)); }
    void StL(tyg_local l, tyg_val v, int64_t off = 0) { St(AtL(l, off), v); }
    tyg_val Add(tyg_val a, tyg_val b) { return tyg_bin(p, TYG_ADD, a, b); }
    tyg_val Sub(tyg_val a, tyg_val b) { return tyg_bin(p, TYG_SUB, a, b); }
    tyg_val Mul(tyg_val a, tyg_val b) { return tyg_bin(p, TYG_MUL, a, b); }
    tyg_val Cmp(tyg_cond c, tyg_val a, tyg_val b) { return tyg_cmp(p, c, a, b); }
    void Branch(tyg_val c, tyg_block a, tyg_block b) { tyg_branch(p, c, a, b); }
    tyg_block NewBlock() { return tyg_block_new(p); }
    void SetBlock(tyg_block b) { tyg_block_set(p, b); }
    // body(i) for i from `from` up to n (n read once), each in a block of
    // its own; on after the last.
    template <class F> void Loop(tyg_val from, tyg_val n, F body) {
        auto il = Local(from), nl = Local(n);
        auto head = NewBlock(), b = NewBlock(), done = NewBlock();
        tyg_jump(p, head);
        SetBlock(head);
        Branch(Cmp(TYG_SLT, LdL(il), LdL(nl)), b, done);
        SetBlock(b);
        body(LdL(il));
        if (!Ended()) {
            StL(il, Add(LdL(il), I64(1)));
            tyg_jump(p, head);
        }
        SetBlock(done);
    }
    // then() where c holds; on after it either way.
    template <class F> void If(tyg_val c, F then) {
        auto t = NewBlock(), done = NewBlock();
        Branch(c, t, done);
        SetBlock(t);
        then();
        if (!Ended()) tyg_jump(p, done);
        SetBlock(done);
    }
    template <class F, class G> void IfElse(tyg_val c, F then, G other) {
        auto t = NewBlock(), e = NewBlock(), done = NewBlock();
        Branch(c, t, e);
        SetBlock(t);
        then();
        if (!Ended()) tyg_jump(p, done);
        SetBlock(e);
        other();
        if (!Ended()) tyg_jump(p, done);
        SetBlock(done);
    }
    // An i64 local holding v.
    tyg_local Local(tyg_val v) {
        auto l = tyg_local_new(p, 8, 8);
        StL(l, v);
        return l;
    }
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
        // A value built in place (an image or a resizable) rather than
        // stored in res: each break builds it at the same top of dstk.
        int inplace = 0;          // 1 an image, 2 a resizable.
        tyg_val dstk = 0;
        TypeExpr *dt = nullptr;
        tyg_addr hdr {};
        tyg_local top0 = 0;       // dstk's top where the value starts.
        bool pref = false;        // Its value is a pool reference (GenPref).
    };
    vector<LoopTargets> loops;
    // A value a match, a block or a loop delivers, built in place at the
    // top of a stack (GenImg for mode 1, GenRz for mode 2).
    struct InPlace {
        int mode = 0;
        tyg_val stk = 0;
        TypeExpr *t = nullptr;
        tyg_addr hdr {};
    };
    void BuildTo(Node *v, const InPlace &ip, Line l) {
        if (ip.mode == 1) GenImg(v, ip.stk, ip.t, l);
        else if (M.IsRb(ip.t)) GenRb(v, ip.stk, ip.hdr, ip.t, l);
        else GenRz(v, ip.stk, ip.hdr, ip.t, l);
    }
    // A match, block or loop expression built in place.
    void BuildExpr(Node *n, const InPlace &ip, Line l) {
        if (auto me = Is<MatchExpr>(n)) GenMatch(me, false, &ip);
        else if (auto e = Is<EarlyBlock>(n)) GenEarly(e, false, &ip);
        else GenLoopX(static_cast<LoopExpr *>(n), false, &ip);
    }
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
    bool hassp = false;   // spl holds gs_sp (a local's id may be 0).
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
    // Pool reference parameters (gs_pref), by the parameter.
    map<const VarDef *, tyg_local> prefs;

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
    static V MemV(TypeExpr *t, tyg_addr a, tyg_val stk = 0) { return V { 0, a, true, t, stk }; }
    static V ValV(TypeExpr *t, tyg_val v) { return V { v, {}, false, t, 0 }; }
    // A place: memory holding a value of type t, and for a resizable
    // array the stack its elements grow on, where known (else 0).
    struct P {
        tyg_addr a;
        TypeExpr *t;
        tyg_val stk = 0;
        bool enc = false;   // A varint in a bytes value: zigzag ULEB128 (ZigRead).
        // The tail of a resizable struct (RsFieldPlace): its elements start
        // here, not where the header's base says, and the header is the
        // owner's, whose count it is.
        tyg_val rbase = 0;
        // A frame object that is the tail of a resizable struct (C.2): bytes
        // as the other fields are, its fixed fields packed where `a` is, its
        // elements behind them, the count in the owner's header ohdr.
        bool bfo = false;
        tyg_addr ohdr {};
        bool tmphdr = false;   // A resizable struct's tail: a header made for it, its stack the struct's.
    };
    tyg_val BaseOf(const P &pl) { return pl.rbase ? pl.rbase : Ld(pl.a); }

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
        if (tyg_block_terminated(p)) SetBlock(NewBlock());
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
        if (!hassp) No(l, "data stack without gs_sp");
        return LdL(spl);
    }
    // GS(gs_sp + k): the gs_stack (one pointer, its top) at that index.
    tyg_val Stk(int k, Line l) {
        auto sp0 = SpVal(l);
        if (!stksl) {
            stksl = tyg_local_new(p, 8, 8);
            M.NeedShim("gs_tg_stks");
        }
        auto base = LdL(stksl);
        auto idx = Add(sp0, I64(k));
        return Add(base, Mul(idx, I64(8)));
    }
    tyg_val Top(tyg_val stk) { return Ld(AtV(stk)); }
    void SetTop(tyg_val stk, tyg_val top) { St(AtV(stk), top); }

    // The next free index, for a local (in the innermost scope that is not
    // a statement's, AllocStk(forlocal) in the C) or a temporary (in the
    // statement's), its top saved to restore.
    int AllocStk(bool forlocal, Line l) {
        auto k = live++;
        maxown = std::max(maxown, live);
        auto mark = Local(Top(Stk(k, l)));
        auto i = sscopes.size() - 1;
        while (forlocal && i > 0 && sscopes[i].stmt) i--;
        sscopes[i].marks.push_back({ k, mark });
        return k;
    }
    void Restore(const SScope &sc) {
        for (size_t i = sc.marks.size(); i-- > 0;)
            SetTop(Stk(sc.marks[i].k, Line {}), LdL(sc.marks[i].top));
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
        return Add(SpVal(l), I64(live));
    }

    // Appends x, of the element type et, at the top of stk to the
    // resizable whose count is at len.
    void PushRaw(tyg_val stk, tyg_addr len, TypeExpr *et, const V &x) {
        if (!cg.IsFix(et)) No(Line {}, "an element of variable size");
        auto top = Top(stk);
        Store(P { AtV(top), et }, x);
        SetTop(stk, Add(top, I64(cg.FixedSize(et))));
        St(len, Add(Ld(len), I64(1)));
    }
    // Copies n elements of et from elems to the top of stk, adding them to
    // the count at len. The source may lie on stk itself, below its top.
    void CopyRun(tyg_val stk, tyg_addr len, TypeExpr *et, tyg_val elems, tyg_val n) {
        if (!cg.IsFix(et)) No(Line {}, "a copy of elements of variable size");
        auto top = Top(stk);
        auto bytes = Mul(n, I64(cg.FixedSize(et)));
        tyg_move_dyn(p, top, elems, bytes);
        SetTop(stk, Add(top, bytes));
        St(len, Add(Ld(len), n));
    }

    // CopyRun of a view's elements, which may be images: their bytes as
    // one run (to RunEnd), their count added to the one at len.
    void CopyView(tyg_val stk, tyg_addr len, TypeExpr *et, const View &v) {
        if (cg.IsFix(et)) return CopyRun(stk, len, et, v.elems, v.len);
        auto bytes = Sub(RunEnd(v), v.elems);
        auto top = Top(stk);
        tyg_move_dyn(p, top, v.elems, bytes);
        SetTop(stk, Add(top, bytes));
        St(len, Add(Ld(len), v.len));
    }

    // Builds n's value, a resizable array of type t, at the top of stk,
    // its header at hdr (CodeGen::GenConstruct): a literal's elements one
    // by one, a call's at the destination it is handed, anything else (an
    // array, a slice, copy(x)) as a copy of its elements.
    void GenRz(Node *n, tyg_val stk, tyg_addr hdr, TypeExpr *t, Line l) {
        if (M.IsRs(t)) return GenRs(n, stk, hdr, t, l);
        auto et = t->arr->sub;
        if (auto c = Is<Call>(n); c && c->builtin == B_COPY && c->ArgNodes().size() == 1)
            n = c->ArgNodes()[0];
        auto len = hdr;
        len.offset += 8;
        auto begin = [&] {
            St(hdr, Top(stk));
            St(len, I64(0));
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
        if (Is<MatchExpr>(n) || Is<EarlyBlock>(n) || Is<LoopExpr>(n)) {
            BuildExpr(n, InPlace { 2, stk, t, hdr }, l);
            return;
        }
        if (auto c = Is<Call>(n); c && c->fvbody && c->fvbody->tail) {
            BuildFv(c, l, [&](Node *v) { GenRz(v, stk, hdr, t, l); });
            return;
        }
        if (auto al = Is<ArrayLit>(n); al && !cg.IsFix(et) && !al->fillval && !al->capexpr) {
            // Image elements, each built behind the last.
            begin();
            for (auto e : al->elems) {
                GenImg(e, stk, et, l);
                if (Ended()) return;
                St(len, Add(Ld(len), I64(1)));
            }
            return;
        }
        if (auto al = Is<ArrayLit>(n); al && al->fillval && !al->capexpr && cg.IsFix(et)) {
            // [v; k] (CodeGen::GenArrayLit): v built once, then copied k
            // times behind the top; k is a constant (§4.2).
            auto fc = Is<IntLit>(al->fillcount);
            if (!fc || fc->val < 0) No(l, "fill count");
            auto esz = cg.FixedSize(et);
            if (SelfRelLit(al->fillval) || (TgModule::IsRel(et) && !et->ref->pool && !Is<NullLit>(al->fillval))) {
                // Each slot built where it lies, its links measured from it.
                auto leaves = FillCapture(al);
                if (Ended()) return;
                begin();
                auto top = Top(stk);
                tyg_zero(p, AtV(top), std::max<int64_t>(fc->val * esz, 1));
                FillStore(leaves, top, fc->val, al);
                SetTop(stk, Add(top, I64(fc->val * esz)));
                St(len, I64(fc->val));
                return;
            }
            auto one = Temp(et);
            Into(P { AtL(one), et }, al->fillval);
            if (Ended()) return;
            begin();
            auto top = Top(stk);
            Loop(I64(0), I64(fc->val),
                 [&](tyg_val i) { tyg_copy(p, AtV(Add(top, Mul(i, I64(esz)))), AtL(one), esz); });
            SetTop(stk, Add(top, I64(fc->val * esz)));
            St(len, I64(fc->val));
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
        if (auto c = Is<Call>(n); c && c->builtin == B_TO_BYTES && c->ArgNodes().size() == 1) {
            if (!IsU8(et)) No(l, "to_bytes of this type");
            begin();
            ToBytesInto(stk, len, c->ArgNodes()[0], l);
            return;
        }
        if (auto c = Is<Call>(n); c && c->builtin == B_STR) {
            if (!IsU8(et)) No(l, "str of this type");
            begin();
            auto an = c->ArgNodes();
            TextInto(stk, len, c, an, 0);
            return;
        }
        if (auto c = Is<Call>(n); DestSpec(c) && IsGrow(DestSpec(c)->rets[0])) {
            if (cg.Mangle(DestSpec(c)->rets[0]->arr->sub) != cg.Mangle(et)) No(l, "result of another element type");
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
                    I64((int64_t)s->val.size()));
            return;
        }
        auto pl = ElemsSource(n, l);
        if (Ended()) return;
        auto v = ViewOf(pl, l);
        if (cg.Mangle(v.elem) != cg.Mangle(et)) No(l, "array of another element type");
        begin();
        CopyView(stk, len, et, v);
    }
    // ---- Resizable ADTs (TgModule::IsRb) -----------------------------------------

    // The bytes of the value of type t at base, whose tail has cnt elements:
    // the tag (an ADT's), the variant's fixed fields, the tail's elements.
    tyg_val RbBytes(TypeExpr *t, tyg_val base, tyg_val cnt) {
        if (t->kind == TY_STRUCT) {
            auto sh = M.RbStShape(t);
            auto tt = cg.SI(t)->ftypes[(size_t)sh.tailfi];
            auto at = Add(base, I64(sh.pre));
            return Add(I64(sh.pre), RbBytes(tt, at, cnt));
        }
        auto ei = M.EI(t);
        auto one = [&](int vi, bool tag) {
            auto sh = M.RbShapeOf(ei, vi);
            if (sh.img) {
                // A variable-size variant: its image's size, walked.
                auto vt = t->kind == TY_VARIANT ? t : cg.VariantType(t, vi);
                auto at = tag ? Add(base, I64(M.TagSize(t))) : base;
                return Add(I64(tag ? M.TagSize(t) : 0), ImgSize(vt, at));
            }
            auto n = I64((tag ? M.TagSize(t) : 0) + sh.pre);
            if (sh.elem)
                n = Add(n, Mul(cnt, I64(cg.FixedSize(sh.elem))));
            return n;
        };
        if (t->kind == TY_VARIANT) return one(M.VariantIdx(t), false);
        auto res = tyg_local_new(p, 8, 8);
        auto tag = LoadTag(AtV(base), t);
        auto done = NewBlock();
        for (size_t vi = 0; vi < ei->en->variants.size(); vi++) {
            auto hit = NewBlock(), next = NewBlock();
            Branch(Cmp(TYG_EQ, tag, I64((int64_t)vi)), hit, next);
            SetBlock(hit);
            StL(res, one((int)vi, true));
            tyg_jump(p, done);
            SetBlock(next);
        }
        StL(res, I64(0));
        tyg_jump(p, done);
        SetBlock(done);
        return LdL(res);
    }

    // Builds n's value, a resizable ADT or variant of type t, at the top of
    // stk, its header at hdr (CodeGen::GenConstruct, GenStructLit,
    // VariantBehindTag): the tag, the variant's fields in turn, its tail's
    // elements counted into the header. A literal, a variant constant, a
    // call returning one handed stk, or a fixed variant of the ADT (its
    // payload copied behind the tag); the C copies no other.
    void GenRb(Node *n, tyg_val stk, tyg_addr hdr, TypeExpr *t, Line l) {
        auto len = hdr;
        len.offset += 8;
        auto adt = t->kind == TY_ENUM;
        auto isst = t->kind == TY_STRUCT;
        auto ei = isst ? nullptr : M.EI(t);
        auto ts = adt ? M.TagSize(t) : 0;
        // The header at the top, the ADT's tag behind it.
        auto begin = [&](int vi) {
            auto top = Top(stk);
            St(hdr, top);
            St(len, I64(0));
            if (adt) {
                StoreTag(AtV(top), t, vi);
                SetTop(stk, Add(top, I64(ts)));
            }
        };
        auto zero = [&](int64_t k) {
            if (!k) return;
            auto top = Top(stk);
            tyg_zero(p, AtV(top), k);
            SetTop(stk, Add(top, I64(k)));
        };
        auto fields = [&](StructLit *sl, int vi) {
            auto &fs = ei->en->variants[(size_t)vi].fields;
            auto sh = M.RbShapeOf(ei, vi);
            if (sh.img) {
                RunLit(sl, stk, fs, ei->vftypes[(size_t)vi]);   // Its fields in turn, as a variable-size variant's.
                return;
            }
            for (size_t i = 0; i < fs.size() && !Ended(); i++) {
                if (fs[i].ispad) {
                    zero(fs[i].padsize);
                    continue;
                }
                auto ft = ei->vftypes[(size_t)vi][i];
                auto init = sl->InitFor((int)i);
                if ((int)i == sh.tailfi && sh.fo) {
                    if (!init || Is<SelfRef>(init)) No(l, "resizable ADT without its frame object");
                    GenFoBytes(init, stk, ft, len, l);
                    if (Ended()) return;
                    continue;
                }
                if ((int)i == sh.tailfi) {
                    if (!init) continue;
                    if (Is<SelfRef>(init)) No(l, "self reference in a resizable ADT");
                    auto th = AtL(tyg_local_new(p, 16, 8));
                    GenRz(init, stk, th, ft, l);
                    if (Ended()) return;
                    auto tl = th;
                    tl.offset += 8;
                    St(len, Ld(tl));
                    continue;
                }
                if (!init) {
                    zero(cg.FixedSize(ft));
                    continue;
                }
                if (Is<SelfRef>(init)) No(l, "self reference in a resizable ADT");
                auto x = GenAs(init, ft);
                if (Ended()) return;
                if (!M.IsScalar(ft) && !x.mem) No(init->line, "aggregate value");
                auto top = Top(stk);
                Store(P { AtV(top), ft }, x);
                SetTop(stk, Add(top, I64(cg.FixedSize(ft))));
            }
        };
        if (auto c = Is<Call>(n); c && c->builtin == B_COPY && c->ArgNodes().size() == 1) No(l, "copy of a resizable ADT");
        if (auto ie = Is<IfExpr>(n); ie && ie->elseb) {
            BuildIf(ie, l, [&](Node *v) { GenRb(v, stk, hdr, t, l); });
            return;
        }
        if (auto b = Is<Block>(n); b && b->tail) {
            BuildArm(b, l, [&](Node *v) { GenRb(v, stk, hdr, t, l); });
            return;
        }
        if (Is<MatchExpr>(n) || Is<EarlyBlock>(n) || Is<LoopExpr>(n)) {
            BuildExpr(n, InPlace { 2, stk, t, hdr }, l);
            return;
        }
        if (auto c = Is<Call>(n); c && c->fvbody && c->fvbody->tail) {
            BuildFv(c, l, [&](Node *v) { GenRb(v, stk, hdr, t, l); });
            return;
        }
        if (isst) {
            if (auto sl = Is<StructLit>(n); sl && sl->exprtype && cg.TEq(sl->exprtype, t) && sl->sinst && !sl->variant) {
                // Its fixed fields in turn, then its tail, a resizable ADT of its own.
                auto si = cg.SI(t);
                auto sh = M.RbStShape(t);
                St(hdr, Top(stk));
                St(len, I64(0));
                for (size_t i = 0; i < si->st->fields.size() && !Ended(); i++) {
                    auto &f = si->st->fields[i];
                    if (f.ispad) {
                        zero(f.padsize);
                        continue;
                    }
                    auto ft = si->ftypes[i];
                    auto init = sl->InitFor((int)i);
                    if ((int)i == sh.tailfi) {
                        if (!init || Is<SelfRef>(init)) No(l, "resizable struct without its tail");
                        auto th = AtL(tyg_local_new(p, 16, 8));
                        GenRb(init, stk, th, ft, l);
                        if (Ended()) return;
                        auto tl = th;
                        tl.offset += 8;
                        St(len, Ld(tl));
                        continue;
                    }
                    if (!init) {
                        zero(cg.FixedSize(ft));
                        continue;
                    }
                    if (Is<SelfRef>(init)) No(l, "self reference in a resizable struct");
                    auto x = GenAs(init, ft);
                    if (Ended()) return;
                    if (!M.IsScalar(ft) && !x.mem) No(init->line, "aggregate value");
                    auto top = Top(stk);
                    Store(P { AtV(top), ft }, x);
                    SetTop(stk, Add(top, I64(cg.FixedSize(ft))));
                }
                return;
            }
            if (auto c = Is<Call>(n); DestSpec(c) && cg.Mangle(DestSpec(c)->rets[0]) == cg.Mangle(t)) {
                RzDst d { stk, hdr };
                rzdst = &d;
                GenCall(c);
                rzdst = nullptr;
                return;
            }
            No(l, cat("resizable struct built from this value"));
        }
        if (auto sl = Is<StructLit>(n); sl && sl->exprtype && cg.TEq(sl->exprtype, t) && (adt ? sl->variant != nullptr : true)) {
            auto vi = adt ? ei->en->VariantIndex(sl->variant) : M.VariantIdx(t);
            begin(vi);
            fields(sl, vi);
            return;
        }
        if (auto d = Is<Dot>(n); d && d->variantconst && adt && d->exprtype && cg.TEq(d->exprtype, t)) {
            begin(ei->en->VariantIndex(d->variantconst));
            return;
        }
        if (auto c = Is<Call>(n); DestSpec(c)) {
            auto rt = DestSpec(c)->rets[0];
            if (cg.Mangle(rt) == cg.Mangle(t)) {
                RzDst d { stk, hdr };
                rzdst = &d;
                GenCall(c);
                rzdst = nullptr;
                return;
            }
            if (adt && rt->kind == TY_VARIANT && M.IsRb(rt) && M.EI(rt) == ei) {
                // The variant behind the ADT's tag, built where the call
                // puts its result; the header then starts at the tag.
                auto start = tyg_local_new(p, 8, 8);
                auto top = Top(stk);
                StL(start, top);
                StoreTag(AtV(top), t, M.VariantIdx(rt));
                SetTop(stk, Add(top, I64(ts)));
                RzDst d { stk, hdr };
                rzdst = &d;
                GenCall(c);
                rzdst = nullptr;
                if (!Ended()) St(hdr, LdL(start));
                return;
            }
            if (adt && rt->kind == TY_VARIANT && !M.IsRb(rt) && M.IsVv(rt) && M.EI(rt) == ei) {
                // A variable-size variant, built behind the tag by the call.
                begin(M.VariantIdx(rt));
                imgdst = stk;
                GenCall(c);
                imgdst = 0;
                return;
            }
        }
        // A value the ADT's variant is: its type is the variant's, not the
        // ADT's the checker gave the node.
        auto x = Gen1(n);
        if (Ended()) return;
        while (x.t && IsAddrRef(x.t)) x = Load(RefTarget(x), l);
        if (!x.mem || !x.t) No(l, cat("value of type ", M.NameWhy(t)));
        if (adt && x.t->kind == TY_VARIANT && M.EI(x.t) == ei && cg.IsFix(x.t)) {
            begin(M.VariantIdx(x.t));
            auto sz = cg.FixedSize(x.t);
            if (sz) {
                auto top = Top(stk);
                tyg_copy(p, AtV(top), x.a, sz);
                SetTop(stk, Add(top, I64(sz)));
            }
            return;
        }
        if (adt && x.t->kind == TY_VARIANT && M.EI(x.t) == ei && !M.IsRb(x.t) && M.IsVv(x.t)) {
            begin(M.VariantIdx(x.t));
            ImgCopy(stk, tyg_addr_of(p, x.a), x.t);
            return;
        }
        No(l, cat("resizable ADT built from ", M.NameWhy(x.t)));
    }
    // A value x of type xt, a result held elsewhere (a forwarded call's),
    // as the ADT or variant t at the top of stk, its header at hdr: its
    // bytes copied, an ADT's tag put in front of a variant's payload.
    void RbFrom(tyg_val stk, tyg_addr hdr, const V &x, TypeExpr *t, Line l) {
        auto len = hdr;
        len.offset += 8;
        if (!x.mem || !x.t) No(l, cat("value of type ", M.NameWhy(t)));
        auto adt = t->kind == TY_ENUM;
        auto ts = adt ? M.TagSize(t) : 0;
        auto top = Top(stk);
        St(hdr, top);
        if (cg.Mangle(x.t) == cg.Mangle(t) && M.IsRb(x.t)) {
            auto xl = x.a;
            xl.offset += 8;
            auto xb = Ld(x.a);
            auto cnt = Ld(xl);
            auto n = RbBytes(t, xb, cnt);
            tyg_move_dyn(p, top, xb, n);
            SetTop(stk, Add(top, n));
            St(len, cnt);
            return;
        }
        if (!adt || x.t->kind != TY_VARIANT || M.EI(x.t) != M.EI(t)) No(l, cat("value of type ", M.NameWhy(x.t), " as ", M.Name(t)));
        StoreTag(AtV(top), t, M.VariantIdx(x.t));
        if (M.IsRb(x.t)) {
            auto xl = x.a;
            xl.offset += 8;
            auto xb = Ld(x.a);
            auto cnt = Ld(xl);
            auto n = RbBytes(x.t, xb, cnt);
            tyg_move_dyn(p, Add(top, I64(ts)), xb, n);
            SetTop(stk, Add(Add(top, I64(ts)), n));
            St(len, cnt);
            return;
        }
        if (!cg.IsFix(x.t)) {
            // A variable-size variant's image, behind the tag.
            SetTop(stk, Add(top, I64(ts)));
            ImgCopy(stk, tyg_addr_of(p, x.a), x.t);
            St(len, I64(0));
            return;
        }
        auto sz = cg.FixedSize(x.t);
        if (sz) tyg_copy(p, AtV(Add(top, I64(ts))), x.a, sz);
        SetTop(stk, Add(top, I64(ts + sz)));
        St(len, I64(0));
    }
    // A field of a resizable struct: a fixed one where it lies; the tail, a
    // resizable ADT, through a header made for it (its base and the
    // struct's count), which reads see as the struct's own (tmphdr).
    P RbStFieldPlace(const P &base, int fi) {
        auto si = cg.SI(base.t);
        auto sh = M.RbStShape(base.t);
        auto at = Add(Ld(base.a), I64(sh.offs[(size_t)fi]));
        auto ft = si->ftypes[(size_t)fi];
        if (fi != sh.tailfi) return P { AtV(at), ft };
        auto hl = base.a;
        hl.offset += 8;
        auto th = tyg_local_new(p, 16, 8);
        StL(th, at);
        StL(th, Ld(hl), 8);
        P r { AtL(th), ft, base.stk };
        r.tmphdr = true;
        r.ohdr = base.tmphdr ? base.ohdr : base.a;   // The struct's own header, whose count it is.
        return r;
    }
    // A field of a resizable variant's payload, where it lies.
    P RbFieldPlace(const P &base, int fi, Line l) {
        auto ei = cg.EIVar(base.t);
        auto vi = M.VariantIdx(base.t);
        auto sh = M.RbShapeOf(ei, vi);
        if (fi == sh.tailfi) No(l, "tail of a resizable variant");
        return P { AtV(Ld(base.a), sh.offs[(size_t)fi]), ei->vftypes[(size_t)vi][(size_t)fi] };
    }

    // Where the elements an array is built from lie: the place of n.
    // The checker types a popped or default fixed array as the T[] it is
    // adapted to, but a fixed array is what lies there: its elements are
    // copied from it (CodeGen::EmitPop, B_DEFAULT).
    static bool IsStrCall(Node *n) {
        auto c = Is<Call>(n);
        return c && c->builtin == B_STR;
    }
    static bool FixedTyped(Node *n) {
        auto pc = Is<Call>(n);
        return pc && (pc->builtin == B_POP || pc->builtin == B_DEFAULT);
    }
    static bool BytesBuiltin(Node *n) {
        auto c = Is<Call>(n);
        return c && ((c->builtin == B_TO_BYTES && c->ArgNodes().size() == 1) || c->builtin == B_FROM_BYTES);
    }
    P ElemsSource(Node *n, Line l) {
        // to_bytes and from_bytes are typed as the array they are adapted
        // to, but what they build is their own array (CodeGen::EmitToBytes,
        // EmitFromBytes).
        if (FixedTyped(n) || BytesBuiltin(n)) {
            auto x = Gen1(n);
            if (Ended()) return P {};
            if (!x.mem) No(l, "popped value in no memory");
            return P { x.a, x.t, x.stk };
        }
        return LimCallResult(n) ? LimCallPlace(n) : Place(n);
    }
    // The function a call to it builds its first result at a destination
    // for (CodeGen's in-place construction): its own, or a dispatch's
    // first case, whose results all the cases share.
    FnSpec *DestSpec(Call *c) {
        if (!c || c->builtin >= 0) return nullptr;
        auto sp = c->spec ? c->spec : !c->dispatch.empty() ? c->dispatch[0] : nullptr;
        if (!sp || sp->sf->isextern || sp->rets.empty()) return nullptr;
        return sp;
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
    void PatchPrefix(tyg_val pre, TypeExpr *t, tyg_val n, tyg_val stk) {
        auto w = TgModule::LenWidth(t);
        if (w == 0) {
            // A varint: one byte reserved, the elements moved up behind a
            // count that needs more.
            auto nl = Local(n);
            auto cnt = [&] { return LdL(nl); };
            auto small = NewBlock(), big = NewBlock(), done = NewBlock();
            Branch(Cmp(TYG_ULT, cnt(), I64(128)), small, big);
            SetBlock(small);
            tyg_store(p, TYG_I8, AtV(pre), tyg_convert(p, TYG_TRUNC, TYG_I8, cnt()), 0);
            tyg_jump(p, done);
            SetBlock(big);
            auto tmp = tyg_local_new(p, 16, 8);
            auto ms = UlebWrite(tyg_addr_of(p, AtL(tmp)), cnt());
            auto msl = Local(ms);
            auto msv = [&] { return LdL(msl); };
            auto elems = Add(pre, I64(1));
            auto top = Top(stk);
            tyg_move_dyn(p, Add(elems, Sub(msv(), I64(1))), elems, Sub(top, elems));
            tyg_copy_dyn(p, pre, tyg_addr_of(p, AtL(tmp)), msv());
            SetTop(stk, Add(top, Sub(msv(), I64(1))));
            tyg_jump(p, done);
            SetBlock(done);
            return;
        }
        auto s = w == 1 ? TYG_I8 : w == 2 ? TYG_I16 : w == 4 ? TYG_I32 : TYG_I64;
        if (w < 8) {
            auto ok = NewBlock(), bad = NewBlock();
            auto max = (int64_t)((1ull << (8 * w)) - 1);
            Branch(Cmp(TYG_ULE, n, I64(max)), ok, bad);
            SetBlock(bad);
            string msg = "array length exceeds storage range";
            msg.push_back('\0');
            CallC("gs_panic", { tyg_type_ptr(m) }, { tyg_addr_of(p, tyg_at_sym(M.Bytes(msg), 0)) }, nullptr);
            tyg_unreachable(p);
            SetBlock(ok);
            n = tyg_convert(p, TYG_TRUNC, s, n);
        }
        tyg_store(p, s, AtV(pre), n, 0);
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
            SetTop(stk, Add(top, ZigWrite(top, x.v)));
            return top;
        }
        if (auto ie = n && !from ? Is<IfExpr>(n) : nullptr; ie && ie->elseb) {
            // Each arm builds its value at the same top (CodeGen's
            // GenConstruct of an if): the image starts where the top was.
            auto start = Local(Top(stk));
            BuildIf(ie, l, [&](Node *v) { GenImg(v, stk, t, l); });
            return LdL(start);
        }
        if (auto b = n && !from ? Is<Block>(n) : nullptr; b && b->tail) {
            auto start = Local(Top(stk));
            BuildArm(b, l, [&](Node *v) { GenImg(v, stk, t, l); });
            return LdL(start);
        }
        if (n && !from && (Is<MatchExpr>(n) || Is<EarlyBlock>(n) || Is<LoopExpr>(n))) {
            auto start = Local(Top(stk));
            BuildExpr(n, InPlace { 1, stk, t, {} }, l);
            return LdL(start);
        }
        if (auto c = n && !from ? Is<Call>(n) : nullptr; c && c->fvbody && c->fvbody->tail) {
            auto start = Local(Top(stk));
            BuildFv(c, l, [&](Node *v) { GenImg(v, stk, t, l); });
            return LdL(start);
        }
        if (TgModule::IsVr(t)) {
            // A varint-width relative reference: the offset of the plain
            // reference n is, whatever relative width it came from.
            if (from) No(l, "relative reference without a value");
            if (!n) {   // Declared without one: null, the zero byte.
                auto top = Top(stk);
                tyg_store(p, TYG_I8, AtV(top), tyg_iconst(p, TYG_I8, 0), 0);
                SetTop(stk, Add(top, I64(1)));
                return top;
            }
            auto x = GenAs(n, t);
            if (Ended()) return Top(stk);
            return GenVr(x, stk, t, l);
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
        auto start = Local(Top(stk));
        auto at = [&] { return LdL(start); };
        if (auto c = Is<Call>(n); DestSpec(c) && cg.Mangle(DestSpec(c)->rets[0]) == cg.Mangle(t)) {
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
        SetTop(stk, Add(at(), I64(TgModule::PrefixBytes(t))));
        auto hdr = AtL(tyg_local_new(p, 16, 8));
        auto len = hdr;
        len.offset += 8;
        if (n) {
            GenRz(n, stk, hdr, t, l);
        } else {
            St(hdr, Top(stk));
            St(len, I64(0));
            if (from) {
                if (cg.Mangle(from->elem) != cg.Mangle(t->arr->sub)) No(l, "array of another element type");
                CopyRun(stk, len, t->arr->sub, from->elems, from->len);
            }
        }
        if (Ended()) return at();
        PatchPrefix(at(), t, Ld(len), stk);
        return at();
    }

    // A T[..] built at the top of stk, at `at` (CodeGen::GenArrayLit):
    // [..cap] its capacity and no elements, the slots left as they are;
    // another T[..] copied whole, capacity and all; any other array or
    // literal its elements, the capacity their count.
    void GenRc(Node *n, tyg_val stk, TypeExpr *t, Line l, const View *from, tyg_val at) {
        auto et = t->arr->sub;
        auto esz = cg.FixedSize(et);
        P pl { AtV(at), t };
        auto al = n && !from ? Is<ArrayLit>(n) : nullptr;
        if (al && al->capexpr) {
            auto cap = Index64(al->capexpr);
            if (Ended()) return;
            AbortUnless(Cmp(TYG_ULE, cap, I64(0xffffffffll)), "CAPRANGE", l);
            tyg_store(p, TYG_I32, pl.a, tyg_convert(p, TYG_TRUNC, TYG_I32, cap), 0);
            LimSetLen(pl, I64(0));
            SetTop(stk, Add(at, Add(I64(8), Mul(cap, I64(esz)))));
            return;
        }
        if (n && !from && !al && !Is<StrLit>(n) && !LimCallResult(n) && !FixedTyped(n) && !BytesBuiltin(n) && !IsStrCall(n) &&
            n->exprtype &&
            cg.TEq(n->exprtype, t)) {
            auto src = Deref(Place(n));
            if (Ended()) return;
            ImgCopy(stk, tyg_addr_of(p, src.a), t);
            return;
        }
        // The elements behind an 8-byte header, built as a resizable's
        // are; then the header, capacity and length both their count.
        SetTop(stk, Add(at, I64(8)));
        auto hdr = AtL(tyg_local_new(p, 16, 8));
        auto len = hdr;
        len.offset += 8;
        St(hdr, Top(stk));
        St(len, I64(0));
        if (from) {
            if (cg.Mangle(from->elem) != cg.Mangle(et)) No(l, "array of another element type");
            CopyView(stk, len, et, *from);
        } else if (al || Is<StrLit>(n) || IsStrCall(n)) {
            GenRz(n, stk, hdr, t, l);
        } else if (n) {
            if (auto c = Is<Call>(n); c && c->builtin == B_COPY) n = c->ArgNodes()[0];
            auto pl = ElemsSource(n, l);
            if (Ended()) return;
            auto v = ViewOf(pl, l);
            if (cg.Mangle(v.elem) != cg.Mangle(et)) No(l, "array of another element type");
            CopyView(stk, len, et, v);
        }
        if (Ended()) return;
        auto cnt = Ld(len);
        AbortUnless(Cmp(TYG_ULE, cnt, I64(0xffffffffll)), "CAPRANGE", l);
        tyg_store(p, TYG_I32, pl.a, tyg_convert(p, TYG_TRUNC, TYG_I32, cnt), 0);
        LimSetLen(pl, cnt);
    }

    // A T[] of images built at the top of stk (its start in `start`): the
    // length prefix, then a literal's elements each built in place behind
    // the last, or another such array's elements copied as one run of
    // bytes, then the prefix patched with the count.
    tyg_val GenImgOfImgs(Node *n, tyg_val stk, TypeExpr *t, Line l, const View *from, tyg_local start) {
        auto at = [&] { return LdL(start); };
        auto et = t->arr->sub;
        SetTop(stk, Add(at(), I64(TgModule::PrefixBytes(t))));
        auto cnt = Local(I64(0));
        View v {};
        auto al = n ? Is<ArrayLit>(n) : nullptr;
        if (al && al->fillval && !al->capexpr) {
            // [v; k]: v built once, then copied behind itself.
            auto fc = Is<IntLit>(al->fillcount);
            if (!fc || fc->val < 0) No(l, "fill count");
            // The value is built once even for none (CodeGen::FreezeFill),
            // then copied behind itself in a loop.
            auto saved = Top(stk);
            auto first = GenImg(al->fillval, stk, et, l);
            if (Ended()) return at();
            if (fc->val == 0) {
                SetTop(stk, saved);
            } else if (fc->val > 1) {
                auto fl = Local(first);
                Loop(I64(1), I64(fc->val), [&](tyg_val) { ImgCopy(stk, LdL(fl), et); });
            }
            StL(cnt, I64(fc->val));
        } else if (al && !al->fillval && !al->capexpr) {
            for (auto e : al->elems) {
                GenImg(e, stk, et, l);
                if (Ended()) return at();
                StL(cnt, Add(LdL(cnt), I64(1)));
            }
        } else {
            if (auto c = n ? Is<Call>(n) : nullptr; c && c->builtin == B_COPY && c->ArgNodes().size() == 1)
                n = c->ArgNodes()[0];
            if (from) v = *from;
            else if (n) v = ViewOf(LimCallResult(n) ? LimCallPlace(n) : Place(n), l);
            else No(l, "array of images without a value");
            if (Ended()) return at();
            if (cg.Mangle(v.elem) != cg.Mangle(et)) No(l, "array of another element type");
            auto bytes = Sub(RunEnd(v), v.elems);
            auto top = Top(stk);
            tyg_move_dyn(p, top, v.elems, bytes);
            SetTop(stk, Add(top, bytes));
            StL(cnt, v.len);
        }
        PatchPrefix(at(), t, LdL(cnt), stk);
        return at();
    }

    // to_bytes (CodeGen::EmitToBytes): the payload's byte count ULEB128,
    // then its bytes, appended at the top of stk to the u8 count at len.
    void ToBytesInto(tyg_val stk, tyg_addr len, Node *a, Line l) {
        auto v = ViewOf(Place(a), l);
        if (Ended()) return;
        auto sl = tyg_local_new(p, 16, 8);
        StL(sl, v.elems);
        StL(sl, cg.IsFix(v.elem) ? Mul(v.len, I64(cg.FixedSize(v.elem))) : Sub(RunEnd(v), v.elems), 8);
        auto src = [&] { return LdL(sl); };
        auto sz = [&] { return LdL(sl, 8); };
        auto top = Top(stk);
        auto pn = UlebWrite(top, sz());
        auto at = Add(top, pn);
        tyg_move_dyn(p, at, src(), sz());
        auto tot = Add(pn, sz());
        SetTop(stk, Add(top, tot));
        St(len, Add(Ld(len), tot));
    }

    // to_bytes appended to a limited u8 array: the count's ULEB128 and then
    // the payload, each copied under the capacity check (CodeGen::AppendBytes).
    void ToBytesIntoLim(const P &out, Node *a, Line l) {
        auto v = ViewOf(Place(a), l);
        if (Ended()) return;
        auto sl = tyg_local_new(p, 16, 8);
        StL(sl, v.elems);
        StL(sl, cg.IsFix(v.elem) ? Mul(v.len, I64(cg.FixedSize(v.elem))) : Sub(RunEnd(v), v.elems), 8);
        auto src = [&] { return LdL(sl); };
        auto sz = [&] { return LdL(sl, 8); };
        auto pfx = tyg_local_new(p, 16, 8);
        auto pn = Local(UlebWrite(tyg_addr_of(p, AtL(pfx)), sz()));
        auto append = [&](tyg_val from, tyg_val n) {
            auto ol = Local(LimLen(out));
            auto o = [&] { return LdL(ol); };
            AbortUnless(Cmp(TYG_SLE, Add(o(), n), LimCapOf(out)), "CAPACITY", l);
            tyg_move_dyn(p, ElemAddr(LimView(out), o()), from, n);
            LimSetLen(out, Add(o(), n));
        };
        append(tyg_addr_of(p, AtL(pfx)), LdL(pn));
        append(src(), sz());
    }

    // A resizable temporary: a stack of the statement's, its header.
    V RzTemp(TypeExpr *t, Line l) {
        return MemV(t, AtL(tyg_local_new(p, 16, 8)), Stk(AllocStk(false, l), l));
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
        // A relative reference too, as its slot's decoded address (RelLoad).
        while ((IsAddrRef(x.t) || (TgModule::IsRel(x.t) && M.RelOk(x.t, 1) && !x.mem)) && want->kind != TY_REF)
            x = Load(RefTarget(x), l);
        if (M.IsScalar(x.t) && M.IsScalar(want)) {
            if (x.t->kind == TY_REF || want->kind == TY_REF) {
                if (x.t->kind != want->kind) {
                    string a, b;
                    x.t->Dump(a);
                    want->Dump(b);
                    No(l, cat("reference conversion from ", a, " to ", b));
                }
                x.t = want;
                return x;
            }
            x.v = Conv(x.v, x.t, want, l);
            x.t = want;
            return x;
        }
        if (LimAdapts(x, want)) return LimFrom(x, want, l);
        if (want->kind == TY_ENUM && M.IsVm(want) && x.mem && M.VmOk(want) && x.t->kind != TY_REF &&
            (x.t->kind == TY_VARIANT || (x.t->kind == TY_ENUM && !M.IsVm(x.t))) && M.EI(x.t) == M.EI(want)) {
            // A variant, or a fixed-mode ADT, wanted as a variable-mode one:
            // its image on a temporary's stack.
            auto stk = Stk(AllocStk(false, l), l);
            return MemV(want, AtV(VmFrom(stk, x.a, x.t, want, l)));
        }
        if (x.t->kind == TY_VARIANT && want->kind == TY_ENUM && x.mem) return AdaptToAdt(x, want, l);
        if (x.t->kind == TY_ENUM && want->kind == TY_ENUM && x.mem && M.IsVm(x.t) && !M.IsVm(want) &&
            M.EI(x.t) == M.EI(want) && M.TypeOk(want)) {
            // A variable-mode value read as fixed-mode (CodeGen::AdaptToFixed):
            // its tag, then the variant's payload copied.
            auto r = MemTemp(want, true);
            auto ei = cg.EIOf(want);
            auto tag = Local(LoadTag(x.a, x.t));
            tyg_store(p, M.TagScal(want), r.a, tyg_convert(p, TYG_TRUNC, M.TagScal(want), LdL(tag)), 0);
            auto done = NewBlock();
            for (size_t vi = 0; vi < ei->en->variants.size(); vi++) {
                auto psz = cg.VariantLayout(ei, (int)vi).size;
                if (!psz) continue;
                auto hit = NewBlock(), next = NewBlock();
                Branch(Cmp(TYG_EQ, LdL(tag), I64((int64_t)vi)), hit, next);
                SetBlock(hit);
                auto to = r.a, from = x.a;
                to.offset += M.TagSize(want);
                from.offset += M.TagSize(x.t);
                tyg_copy(p, to, from, psz);
                tyg_jump(p, done);
                SetBlock(next);
            }
            tyg_jump(p, done);
            SetBlock(done);
            return r;
        }
        if (want->kind == TY_SLICE && x.t->kind == TY_ARRAY && x.t->arr->akind == A_FIXED && x.mem &&
            cg.Mangle(x.t->arr->sub) == cg.Mangle(want->sub)) {
            // A fixed array where a slice is wanted: a view of where it lies.
            auto v = ViewOf(P { x.a, x.t }, l);
            return SliceValue(want, v.elems, v.len);
        }
        if (OptFit(x, want)) return x;
        if (x.mem && x.t->kind == TY_ARRAY && want->kind == TY_SLICE && M.TypeOk(want) &&
            (IsGrow(x.t) || IsVarArr(x.t)) && cg.Mangle(x.t->arr->sub) == cg.Mangle(want->sub)) {
            // An array temporary (a call's result) where a slice is wanted:
            // a view of it (§6.4).
            auto v = ViewOf(P { x.a, x.t, x.stk }, l);
            return SliceValue(want, v.elems, v.len);
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
        if (!M.TypeOk(pl.t) && (M.ImgOk(pl.t) || M.RsOk(pl.t) || M.FoOk(pl.t))) {
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
        tyg_copy(p, AtL(t), pl.a, M.Size(pl.t));
        x.mem = true;
        x.a = AtL(t);
        return x;
    }

    void Store(const P &pl, const V &x) {
        if (Ended()) return;
        if (pl.enc) No(Line {}, "a store into a varint in a bytes value");
        if (TgModule::IsVr(pl.t)) {
            // Only into a temporary carrying a value across blocks (Size):
            // the language never assigns one.
            RelOffsetInto(pl, x.v, Line {});
            return;
        }
        if (TgModule::IsRel(pl.t)) {
            RelStore(pl, x.v, Line {});
            return;
        }
        // A scalar without a value comes from code after a return or break,
        // which tinygen keeps in a block that is never placed.
        if (M.IsScalar(pl.t) && !x.v) return;
        if (M.FoOk(pl.t) && x.mem && pl.stk && x.stk) {
            FoCopy(pl, P { x.a, x.t, x.stk }, Line {});
            return;
        }
        if (!M.IsScalar(pl.t) && !M.TypeOk(pl.t)) No(Line {}, cat("store of a ", M.Name(pl.t)));
        if (M.IsScalar(pl.t)) tyg_store(p, M.Scal(pl.t), pl.a, x.v, 0);
        else tyg_move(p, pl.a, x.a, M.Size(pl.t));
    }

    // ---- Places --------------------------------------------------------------

    // What the reference at pl refers to: a fat reference's header, with
    // its stack.
    P Step(const P &pl) {
        if (TgModule::IsRel(pl.t)) return P { AtV(RelLoad(pl, Line {})), pl.t->ref->sub };
        auto ptr = Ld(pl.a);
        P r { AtV(ptr), pl.t->ref->sub };
        r.enc = TgModule::IsVarint(r.t);   // A varint in a bytes value, read-only.
        if (cg.IsFatRef(pl.t)) {
            auto a8 = pl.a;
            a8.offset += 8;
            r.stk = Ld(a8);
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
        auto base = Ld(pp.a);
        return Sub(base, I64(1));
    }
    // The address the slot at pl decodes to, null for a 0 offset.
    tyg_val RelLoad(const P &pl, Line l) {
        auto s = M.RelScal(pl.t);
        // Signed from the slot, unsigned from a pool (CodeGen::RelParts).
        tyg_val off;
        if (TgModule::IsVr(pl.t)) {
            auto at = tyg_addr_of(p, pl.a);
            off = pl.t->ref->pool ? UlebRead(at) : ZigRead(at);
        } else {
            off = tyg_load(p, s, pl.a, 0);
            if (s != TYG_I64) off = tyg_convert(p, pl.t->ref->pool ? TYG_ZEXT : TYG_SEXT, TYG_I64, off);
        }
        auto at = Add(RelOrigin(pl, l), off);
        if (!pl.t->ref->optional) return at;
        return tyg_select(p, Cmp(TYG_EQ, off, I64(0)), I64(0), at);
    }
    // The offset the slot at pl holds for the address v: its distance
    // from the origin, zero for a null (CodeGen::RelOffset).
    tyg_val RelOffset(const P &pl, tyg_val v, Line l) {
        auto pool = pl.t->ref->pool;
        auto off = Sub(v, RelOrigin(pl, l));
        if (pl.t->ref->optional) {
            auto isnull = Cmp(TYG_EQ, v, I64(0));
            off = tyg_select(p, isnull, I64(0), off);
            // A link to its own slot would read back as null (RelOffset).
            if (!pool)
                AbortUnless(tyg_bin(p, TYG_OR, isnull, Cmp(TYG_NE, off, I64(0))), "RELNULL", l);
        }
        return off;
    }
    // The address v as a varint-width offset written at the top of stk:
    // ULEB128 of an `in pool` one, zigzag of a self-relative one
    // (CodeGen::EmitRelStore); no range to check.
    tyg_val RelOffsetInto(const P &pl, tyg_val v, Line l) {
        auto off = RelOffset(pl, v, l);
        auto at = tyg_addr_of(p, pl.a);
        return pl.t->ref->pool ? UlebWrite(at, off) : ZigWrite(at, off);
    }
    tyg_val GenVr(const V &x, tyg_val stk, TypeExpr *t, Line l) {
        auto top = Top(stk);
        auto n = RelOffsetInto(P { AtV(top), t }, x.v, l);
        SetTop(stk, Add(top, n));
        return top;
    }
    // The address v as the slot's offset, range-checked (EmitRelStoreAt).
    void RelStore(const P &pl, tyg_val v, Line l) {
        auto s = M.RelScal(pl.t);
        auto pool = pl.t->ref->pool;
        auto off = RelOffset(pl, v, l);
        auto bits = TgModule::ScalSize(s) * 8;
        if (bits < 64 && pool)
            AbortUnless(Cmp(TYG_ULE, off, I64((int64_t)((1ull << bits) - 1))), "RELOFF", l);
        if (bits < 64 && !pool) {
            auto lo = Cmp(TYG_SGE, off, I64(-(int64_t)(1ull << (bits - 1))));
            auto hi = Cmp(TYG_SLT, off, I64((int64_t)(1ull << (bits - 1))));
            AbortUnless(tyg_bin(p, TYG_AND, lo, hi), "RELOFF", l);
        }
        tyg_store(p, s, pl.a, s == TYG_I64 ? off : tyg_convert(p, TYG_TRUNC, s, off), 0);
    }
    // What a reference value refers to.
    P RefTarget(const V &x) {
        if (x.mem) return Step(P { x.a, x.t });
        P r { AtV(x.v), x.t->ref->sub };
        r.enc = TgModule::IsVarint(r.t);
        return r;
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
            if (M.RsOk(base.t)) return RsFieldPlace(base, d->fieldidx, n->line);
            if (M.IsRb(base.t) && base.t->kind == TY_VARIANT) return RbFieldPlace(base, d->fieldidx, n->line);
            if (M.IsRb(base.t) && base.t->kind == TY_STRUCT && M.RbOk(base.t)) return RbStFieldPlace(base, d->fieldidx);
            if (M.VvOk(base.t)) {
                auto ei = cg.EIVar(base.t);
                auto vi = (size_t)ei->en->VariantIndex(base.t->var->variant);
                auto ft = ei->vftypes[vi][(size_t)d->fieldidx];
                P r { AtV(VvField(base.t, tyg_addr_of(p, base.a), d->fieldidx)), ft };
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
        P pl { AtL(t), x.t };
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
            P pl { AtV(LdL(it->second)), vd->type };
            // A captured resizable: its header's address, its stack.
            if (auto s = rzstk.find(vd); s != rzstk.end() && M.Resz(vd->type))
                pl.stk = LdL(s->second);
            return pl;
        }
        P pl { AtL(it->second), vd->type };
        if (auto s = rzstk.find(vd); s != rzstk.end())
            pl.stk = LdL(s->second);
        return pl;
    }

    P GlobalPlace(VarDef *d, Line l) {
        if (!M.GlobalOk(d)) No(l, cat("global of type ", M.NameWhy(d->type)));
        auto s = M.TypeOk(d->type) ? M.StaticGlobal(d) : TYG_NO_SYM;
        if (s != TYG_NO_SYM) return P { tyg_at_sym(s, 0), d->type };
        // The shims now, so that an abandoned body takes them back.
        auto at = LdL(AccessorLocal(M.Accessor(d)));
        if (IsImg(d->type)) {
            // A T[], any image: the instance holds its address.
            return P { AtV(Ld(AtV(at))), d->type };
        }
        P pl { AtV(at), d->type };
        if (IsGrow(d->type) || M.FoOk(d->type) || M.RsOk(d->type) || M.IsRb(d->type))
            pl.stk = LdL(AccessorLocal(M.StackAccessor(d)));
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
    // A literal of a fixed struct or array with a non-null self-relative
    // link stored in it at any depth: the C builds it where it lies
    // (CodeGen::FixedLitAtLv), whose slots the offsets measure from.
    bool SelfRelLit(Node *n) {
        if (!SelfRelLitOk(n)) return false;
        auto stores = [&](TypeExpr *ft, Node *init) {
            if (!init) return false;
            // An `in pool` link is the pool's offset, but its `self` is the slot's.
            if (TgModule::IsRel(ft)) return ft->ref->pool ? Is<SelfRef>(init) != nullptr : !Is<NullLit>(init);
            return SelfRelLit(init) && cg.TEq(init->exprtype, ft);
        };
        if (auto sl = Is<StructLit>(n)) {
            auto sh = LitShape(sl);
            for (size_t i = 0; i < sh.fields->size(); i++)
                if (!(*sh.fields)[i].ispad && stores((*sh.ftypes)[i], sl->InitFor((int)i))) return true;
            return false;
        }
        auto al = Is<ArrayLit>(n);
        auto et = al->exprtype->arr->sub;
        if (al->fillval) return stores(et, al->fillval);
        for (auto e : al->elems)
            if (stores(et, e)) return true;
        return false;
    }
    // A struct or array literal SelfRelInto can build.
    // The fields of a struct, variant or fixed-mode ADT literal, their
    // types and offsets from the value's start (a tag first, for the ADT).
    struct LitSh {
        const vector<Field> *fields;
        const vector<TypeExpr *> *ftypes;
        vector<int64_t> offs;
    };
    LitSh LitShape(StructLit *sl) {
        auto t = sl->exprtype;
        if (t->kind == TY_STRUCT) {
            auto si = cg.SI(t);
            return { &si->st->fields, &si->ftypes, cg.StructLayout(si).offs };
        }
        auto ei = M.EI(t);
        auto vi = t->kind == TY_VARIANT ? M.VariantIdx(t) : ei->en->VariantIndex(sl->variant);
        auto offs = cg.VariantLayout(ei, vi).offs;
        if (t->kind == TY_ENUM)
            for (auto &o : offs) o += M.TagSize(t);
        return { &ei->en->variants[(size_t)vi].fields, &ei->vftypes[(size_t)vi], offs };
    }
    bool SelfRelLitOk(Node *n) {
        if (auto sl = Is<StructLit>(n)) {
            auto t = sl->exprtype;
            if (!t || !M.TypeOk(t)) return false;
            if (t->kind == TY_STRUCT) return sl->sinst && !sl->variant;
            if (t->kind == TY_VARIANT) return true;
            return t->kind == TY_ENUM && !t->enu->varmode && sl->variant;
        }
        auto al = Is<ArrayLit>(n);
        return al && !al->capexpr && al->exprtype && al->exprtype->kind == TY_ARRAY &&
               al->exprtype->arr->akind == A_FIXED && M.TypeOk(al->exprtype);
    }
    // The value a literal being built in place starts at, which `self` in
    // its fields refers to (§3.9), or 0.
    tyg_val selfbase = 0;
    // Such a literal built at pl: zero bytes, then its fields where they lie.
    void SelfRelInto(const P &pl, Node *lit) {
        auto t = lit->exprtype;
        tyg_zero(p, pl.a, M.Size(t));
        if (auto al = Is<ArrayLit>(lit)) {
            auto et = t->arr->sub;
            auto esz = cg.FixedSize(et);
            if (al->fillval) {
                auto leaves = FillCapture(al);
                if (Ended()) return;
                FillStore(leaves, tyg_addr_of(p, pl.a), cg.ArrSize(t->arr), al);
                return;
            }
            for (size_t i = 0; i < al->elems.size() && !Ended(); i++) {
                auto a = pl.a;
                a.offset += (int64_t)i * esz;
                Into(P { a, et }, al->elems[i]);
            }
            return;
        }
        auto sl = Is<StructLit>(lit);
        auto outer = selfbase;
        selfbase = tyg_addr_of(p, pl.a);
        struct Back {
            tyg_val &at, was;
            ~Back() { at = was; }
        } back { selfbase, outer };
        auto sh = LitShape(sl);
        if (t->kind == TY_ENUM) StoreTag(pl.a, t, M.EI(t)->en->VariantIndex(sl->variant));
        for (size_t i = 0; i < sh.fields->size() && !Ended(); i++) {
            if ((*sh.fields)[i].ispad) continue;
            auto init = sl->InitFor((int)i);
            if (!init) continue;
            auto a = pl.a;
            a.offset += sh.offs[i];
            Into(P { a, (*sh.ftypes)[i] }, init);
        }
    }
    // A fill [v; k] of a value linking to itself (CodeGen::FreezeFill): the
    // leaves of v evaluated once, in order; then each slot built in place,
    // its links measured from it (FillStore).
    struct FillLeaf {
        TypeExpr *ft;
        int64_t off;
        bool self;
        tyg_local tmp;
        shared_ptr<vector<FillLeaf>> sub;   // An inner fill: its leaves,
        ArrayLit *inner = nullptr;          // repeated for the inner literal.
    };
    vector<FillLeaf> FillCapture(ArrayLit *al) {
        vector<FillLeaf> leaves;
        if (!al->fillval) return leaves;
        auto et = al->exprtype->arr->sub;
        auto capture = [&](Node *init, TypeExpr *ft, int64_t off) {
            if (Is<SelfRef>(init)) {
                leaves.push_back({ ft, off, true, 0 });
                return;
            }
            if (auto il = Is<ArrayLit>(init); il && il->fillval && SelfRelLit(init) && SelfRelLitOk(init) &&
                                               Is<IntLit>(il->fillcount)) {
                FillLeaf lf { ft, off, false, 0 };
                lf.sub = make_shared<vector<FillLeaf>>(FillCapture(il));
                lf.inner = il;
                leaves.push_back(lf);
                return;
            }
            if (SelfRelLit(init)) No(init->line, "fill of a value linking to itself");
            auto x = GenAs(init, ft);
            if (Ended()) return;
            FillLeaf lf { ft, off, false, tyg_local_new(p, std::max<int64_t>(M.Size(ft), 8), 8) };
            if (M.IsScalar(ft) || TgModule::IsRel(ft)) {
                tyg_store(p, TgModule::IsRel(ft) ? TYG_I64 : M.Scal(ft), AtL(lf.tmp), x.v, 0);
            } else {
                if (!x.mem) No(init->line, "aggregate value");
                tyg_copy(p, AtL(lf.tmp), x.a, M.Size(ft));
            }
            leaves.push_back(lf);
        };
        if (auto sl = Is<StructLit>(al->fillval); sl && SelfRelLitOk(sl) && sl->exprtype->kind == TY_STRUCT) {
            auto sh = LitShape(sl);
            for (size_t i = 0; i < sh.fields->size() && !Ended(); i++) {
                if ((*sh.fields)[i].ispad) continue;
                if (auto init = sl->InitFor((int)i)) capture(init, (*sh.ftypes)[i], sh.offs[i]);
            }
        } else {
            capture(al->fillval, et, 0);
        }
        return leaves;
    }
    // The elements a literal has: its elements', or its fill's count.
    int64_t LitCount(ArrayLit *al) { return al->fillval ? Is<IntLit>(al->fillcount)->val : (int64_t)al->elems.size(); }
    // A literal's run of k elements built where it stays, at the address in
    // base: zeroed, then each element in place (its fill's leaves, if any,
    // captured already).
    void LitRunAt(ArrayLit *al, TypeExpr *et, tyg_local base, int64_t k, const vector<FillLeaf> &leaves) {
        auto esz = cg.FixedSize(et);
        auto at = [&](int64_t off) { return AtV(LdL(base), off); };
        tyg_zero(p, at(0), std::max<int64_t>(k * esz, 1));
        if (al->fillval) {
            FillStore(leaves, LdL(base), k, al);
            return;
        }
        for (int64_t i = 0; i < k && !Ended(); i++) Into(P { at(i * esz), et }, al->elems[(size_t)i]);
    }
    // k slots at base (zeroed), each built from the captured leaves.
    void FillStore(const vector<FillLeaf> &leaves, tyg_val base, int64_t k, ArrayLit *al) {
        auto esz = cg.FixedSize(al->exprtype->arr->sub);
        Loop(I64(0), I64(k), [&](tyg_val i) {
            auto slot = Add(base, Mul(i, I64(esz)));
            for (auto &lf : leaves) {
                auto a = AtV(slot, lf.off);
                if (lf.inner) {
                    FillStore(*lf.sub, Add(slot, I64(lf.off)),
                              cg.ArrSize(lf.inner->exprtype->arr), lf.inner);
                    continue;
                }
                V y;
                y.t = lf.ft;
                if (lf.self) {
                    y.v = slot;
                } else if (M.IsScalar(lf.ft) || TgModule::IsRel(lf.ft)) {
                    y.v = tyg_load(p, TgModule::IsRel(lf.ft) ? TYG_I64 : M.Scal(lf.ft), AtL(lf.tmp), 0);
                } else {
                    y.mem = true;
                    y.a = AtL(lf.tmp);
                }
                Store(P { a, lf.ft }, y);
            }
        });
    }
    void Into(const P &pl, Node *n) {
        if (SelfRelLit(n) && cg.TEq(n->exprtype, pl.t)) {
            SelfRelInto(pl, n);
            return;
        }
        if (auto al = Is<ArrayLit>(n);
            al && !al->fillval && !al->capexpr && pl.t->kind == TY_ARRAY && cg.HasRelRef(pl.t) &&
            cg.TEq(n->exprtype, pl.t) && M.TypeOk(pl.t) && cg.ArrSize(pl.t->arr) > 0 &&
            (pl.t->arr->akind == A_FIXED || cg.IsStaticLimited(pl.t))) {
            // An array of links built where it lies, so each offset measures
            // from its own slot (CodeGen::GenArrayLit's in-place path).
            auto lim = pl.t->arr->akind != A_FIXED;
            auto et = pl.t->arr->sub;
            auto esz = cg.FixedSize(et);
            auto cnt = (int64_t)al->elems.size();
            if (cnt > cg.ArrSize(pl.t->arr)) No(n->line, "array literal length");
            tyg_zero(p, pl.a, M.Size(pl.t));
            for (int64_t i = 0; i < cnt && !Ended(); i++) {
                auto a = pl.a;
                a.offset += (lim ? M.LimWidth(pl.t) : 0) + i * esz;
                Into(P { a, et }, al->elems[(size_t)i]);
            }
            if (lim && !Ended()) LimSetLen(pl, I64(cnt));
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
        if (t && t->kind == TY_REF && cg.IsFatRef(t) && n->exprtype &&
            (IsGrow(n->exprtype) || M.FoOk(n->exprtype) || M.RsOk(n->exprtype) || M.IsRb(n->exprtype))) {
            // A resizable where a reference to one is wanted: gs_rref, its
            // header's address and its stack.
            auto pl = Deref(Place(n));
            if (Ended()) return Dummy(t);
            if (!pl.stk || pl.tmphdr) No(n->line, "reference to an array of this kind");
            auto tmp = tyg_local_new(p, 16, 8);
            StL(tmp, tyg_addr_of(p, pl.a));
            StL(tmp, pl.stk, 8);
            return MemV(t, AtL(tmp));
        }
        if (t && IsAddrRef(t) && n->exprtype && IsImg(n->exprtype) && M.ImgOk(t->ref->sub)) {
            // A T[] (or a variable-size struct) where a reference to one is
            // wanted, plain or optional: its image's address.
            auto pl = Deref(Place(n));
            return ValV(t, Ended() ? I64(0) : tyg_addr_of(p, pl.a));
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
                return ValV(n->exprtype, selfbase);
            }
            case NK_BoolLit: {
                return ValV(M.ast.booltype, tyg_iconst(p, TYG_I8, static_cast<BoolLit *>(n)->val ? 1 : 0));
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
                auto &val = static_cast<StrLit *>(n)->val;
                if (t && t->kind == TY_ARRAY && t->arr->akind == A_FIXED && IsU8(t->arr->sub) &&
                    M.TypeOk(t) && (int64_t)val.size() == cg.FixedSize(t)) {
                    // A u8[k] of exactly its bytes (§4.2): they, where they lie.
                    return MemV(t, tyg_at_sym(M.Bytes(val), 0));
                }
                if (!t || t->kind != TY_SLICE || !IsU8(t->sub)) No(n->line, "string of this type");
                auto tmp = tyg_local_new(p, 16, 8);
                StL(tmp, tyg_addr_of(p, tyg_at_sym(M.Bytes(val), 0)));
                StL(tmp, I64((int64_t)val.size()), 8);
                return MemV(t, AtL(tmp));
            }
            case NK_Unary: return GenUnary(static_cast<Unary *>(n));
            case NK_Binary: return GenBinary(static_cast<Binary *>(n));
            case NK_AsCast: return GenCast(static_cast<AsCast *>(n));
            case NK_Call: return GenCall(static_cast<Call *>(n));
            case NK_StructLit: return GenStructLit(static_cast<StructLit *>(n));
            case NK_Block: {
                // An image or a resizable tail: built on a temporary's stack.
                V w;
                auto b = static_cast<Block *>(n);
                if (b->tail && !armwant && WantInPlace(b, false, w)) return w;
                return GenBlock(b, false);
            }
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
                if ((!IsGrow(pl.t) && !M.FoOk(pl.t) && !M.RsOk(pl.t) && !M.IsRb(pl.t)) || !pl.stk || pl.rbase || pl.tmphdr)
                    No(u->line, "reference to an array of this kind");
                auto tmp = tyg_local_new(p, 16, 8);
                StL(tmp, tyg_addr_of(p, pl.a));
                StL(tmp, pl.stk, 8);
                return MemV(et, AtL(tmp));
            }
            if (et && et->kind == TY_REF) {
                if (!IsPlainRef(et) && !M.OptRef(et) && !(TgModule::IsRel(et) && M.RelOk(et, 1)))
                    No(u->line, "reference kind");
                auto pl = Deref(Place(u->child));
                return ValV(et, tyg_addr_of(p, pl.a));
            }
            return Load(Deref(Place(u->child)), u->line);
        }
        if (u->op == T_NOT && OptCond(u->child)) {
            return ValV(M.ast.booltype, Cmp(TYG_EQ, OptTest(u->child), tyg_iconst(p, TYG_I8, 0)));
        }
        auto ot = cg.OperandT(u->child->exprtype);
        if (u->op == T_MINUS && (ot->kind == TY_STRUCT || ot->kind == TY_ARRAY)) {
            // Elementwise (CodeGen::GenElemwiseNegInto).
            auto x = GenAs(u->child, ot);
            if (Ended()) return Dummy(ot);
            if (!x.mem || !M.TypeOk(ot)) No(u->line, "unary operand type");
            auto src = Temp(ot);
            tyg_copy(p, AtL(src), x.a, M.Size(ot));
            auto r = MemTemp(ot, false);
            ElemLeaves(ot, 0, u->line, [&](TypeExpr *tt, int64_t off, const ElemDyn &dy) {
                auto v = tyg_load(p, M.Scal(tt), ElemAt(AtL(src), off, dy), 0);
                tyg_store(p, M.Scal(tt), ElemAt(r.a, off, dy), ScalarNeg(tt, v, u->line), 0);
            });
            return r;
        }
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
                x.v = Cmp(TYG_EQ, x.v, tyg_iconst(p, M.Scal(ot), 0));
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
            auto lb = Cmp(TYG_NE, l, tyg_iconst(p, TYG_I8, 0));
            tyg_store(p, TYG_I8, AtL(t), lb, 0);
            auto rhs = NewBlock(), done = NewBlock();
            if (op == T_ANDAND) Branch(lb, rhs, done);
            else Branch(lb, done, rhs);
            SetBlock(rhs);
            OpenScope(true);   // Its temporaries exist only when it runs.
            auto r = Truth(b->right);
            CloseScope();
            if (!Ended()) {
                tyg_store(p, TYG_I8, AtL(t), Cmp(TYG_NE, r, tyg_iconst(p, TYG_I8, 0)), 0);
                tyg_jump(p, done);
            }
            SetBlock(done);
            x.t = M.ast.booltype;
            x.v = tyg_load(p, TYG_I8, AtL(t), 0);
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
        if ((op == T_PLUS || op == T_MINUS || op == T_MUL || op == T_DIV || op == T_MOD) && b->exprtype &&
            (b->exprtype->kind == TY_STRUCT || b->exprtype->kind == TY_ARRAY))
            return GenElemwise(b);
        auto lt = cg.OperandT(b->left->exprtype);
        if ((op == T_EQ || op == T_NEQ) && !M.IsScalar(lt)) return GenValEq(b, lt);
        if (!M.IsScalar(lt) || lt->kind == TY_REF) No(b->line, cat("operand type ", M.NameWhy(lt)));
        auto isflt = lt->kind == TY_FLT;
        auto sgn = TgModule::IsSigned(lt);
        if (op == T_SHL || op == T_SHR) {
            if (lt->kind != TY_INT) No(b->line, "shift type");
            auto l = GenAs(b->left, lt);
            auto r = GenAs(b->right, M.ast.inttypes[IS_I64]);
            auto s = M.Scal(lt);
            auto bits = IntBits(lt->intstorage);
            auto n = tyg_bin(p, TYG_AND, r.v, I64(bits - 1));
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
            x.v = Cmp(isflt ? fl : sgn ? si : un, l.v, r.v);
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

    // Structural equality (§4.5; CodeGen::EqX): on the first difference
    // a jump to fail, else on past. Fixed values field by field, element
    // by element, an ADT's tag then its variant's payload; floats as
    // floats; a bytes value of exact encodings by its bytes.
    void EqJump(const P &a, const P &b, tyg_block fail, Line l) {
        auto t = a.t;
        auto next = [&](tyg_val same) {
            auto ok = NewBlock();
            Branch(same, ok, fail);
            SetBlock(ok);
        };
        if (!cg.IsFix(t)) {
            if (!EqBitwise(t)) {
                // Floats or pads among its bytes: walked field by field,
                // over a cursor in each (CodeGen::EmitEqWalk).
                auto ca = tyg_local_new(p, 8, 8), cb = tyg_local_new(p, 8, 8);
                StL(ca, tyg_addr_of(p, a.a));
                StL(cb, tyg_addr_of(p, b.a));
                EqWalk(t, ca, cb, fail, l);
                return;
            }
            auto pa = tyg_addr_of(p, a.a), pb = tyg_addr_of(p, b.a);
            auto na = ImgSize(t, pa);
            next(Cmp(TYG_EQ, na, ImgSize(t, pb)));
            auto d = CallC("memcmp", { tyg_type_ptr(m), tyg_type_ptr(m), IT(TYG_I64, false) },
                           { tyg_addr_of(p, a.a), tyg_addr_of(p, b.a), ImgSize(t, tyg_addr_of(p, a.a)) },
                           IT(TYG_I32));
            next(Cmp(TYG_EQ, d, tyg_iconst(p, TYG_I32, 0)));
            return;
        }
        if (TgModule::IsRel(t)) {
            auto s2 = M.RelScal(t);
            next(Cmp(TYG_EQ, tyg_load(p, s2, a.a, 0), tyg_load(p, s2, b.a, 0)));
            return;
        }
        if (t->kind == TY_REF && cg.IsFatRef(t)) {
            next(Cmp(TYG_EQ, Ld(a.a), Ld(b.a)));
            return;
        }
        if (M.IsScalar(t)) {
            auto s2 = M.Scal(t);
            next(Cmp(t->kind == TY_FLT ? TYG_FEQ : TYG_EQ, tyg_load(p, s2, a.a, 0), tyg_load(p, s2, b.a, 0)));
            return;
        }
        if (t->kind == TY_SLICE) {
            auto a8 = a.a, b8 = b.a;
            a8.offset += 8;
            b8.offset += 8;
            next(Cmp(TYG_EQ, Ld(a.a), Ld(b.a)));
            next(Cmp(TYG_EQ, Ld(a8), Ld(b8)));
            return;
        }
        if (!M.TypeOk(t)) No(l, cat("comparison of type ", M.NameWhy(t)));
        if (t->kind == TY_STRUCT || t->kind == TY_VARIANT) {
            for (auto &f : M.Fields(t)) {
                auto fa = a.a, fb = b.a;
                fa.offset += f.off;
                fb.offset += f.off;
                EqJump(P { fa, f.t }, P { fb, f.t }, fail, l);
            }
            return;
        }
        if (t->kind == TY_ENUM) {
            auto ta = LoadTag(a.a, t);
            next(Cmp(TYG_EQ, ta, LoadTag(b.a, t)));
            auto ei = cg.EIOf(t);
            auto done = NewBlock();
            for (size_t vi = 0; vi < ei->en->variants.size(); vi++) {
                auto vt = cg.VariantType(t, (int)vi);
                if (!cg.FixedSize(vt)) continue;
                auto hit = NewBlock(), miss = NewBlock();
                Branch(Cmp(TYG_EQ, LoadTag(a.a, t), I64((int64_t)vi)), hit, miss);
                SetBlock(hit);
                auto pa = a.a, pb = b.a;
                pa.offset += M.TagSize(t);
                pb.offset += M.TagSize(t);
                EqJump(P { pa, vt }, P { pb, vt }, fail, l);
                tyg_jump(p, done);
                SetBlock(miss);
            }
            tyg_jump(p, done);
            SetBlock(done);
            return;
        }
        if (t->kind == TY_ARRAY) {
            View va, vb;
            if (TgModule::IsLim(t)) {
                va = LimView(a);
                vb = LimView(b);
                next(Cmp(TYG_EQ, va.len, vb.len));
            } else {
                va = ViewOf(a, l);
                vb = ViewOf(b, l);
            }
            EqElems(va, vb, fail, l);
            return;
        }
        No(l, cat("comparison of type ", M.NameWhy(t)));
    }
    // The bytes value at each cursor compared with the other's, both
    // cursors left behind it (CodeGen::EmitEqWalk): fixed parts by value,
    // a varint by its canonical bytes, an array by its length and then
    // its elements, a struct by its fields in order, an ADT by its tag and
    // then its variant's payload.
    void EqWalk(TypeExpr *t, tyg_local ca, tyg_local cb, tyg_block fail, Line l) {
        auto A = [&] { return LdL(ca); };
        auto B = [&] { return LdL(cb); };
        auto adv = [&](tyg_val n) {
            StL(ca, Add(A(), n));
            StL(cb, Add(B(), n));
        };
        auto next = [&](tyg_val same) {
            auto ok = NewBlock();
            Branch(same, ok, fail);
            SetBlock(ok);
        };
        if (cg.IsFix(t)) {
            if (!M.TypeOk(t)) No(l, cat("comparison of type ", M.NameWhy(t)));
            EqJump(P { AtV(A()), t }, P { AtV(B()), t }, fail, l);
            adv(I64(cg.FixedSize(t)));
            return;
        }
        switch (t->kind) {
            case TY_INT: case TY_REF: {
                if (!TgModule::IsVarint(t) && !TgModule::IsVr(t)) No(l, cat("comparison of type ", M.NameWhy(t)));
                auto na = Local(UlebSize(A()));
                auto n = [&] { return LdL(na); };
                next(Cmp(TYG_EQ, UlebSize(B()), n()));
                auto d = CallC("memcmp", { tyg_type_ptr(m), tyg_type_ptr(m), IT(TYG_I64, false) }, { A(), B(), n() }, IT(TYG_I32));
                next(Cmp(TYG_EQ, d, tyg_iconst(p, TYG_I32, 0)));
                adv(n());
                return;
            }
            case TY_ARRAY: {
                auto sub = t->arr->sub;
                auto lena = tyg_local_new(p, 8, 8), lenb = tyg_local_new(p, 8, 8);
                auto capa = tyg_local_new(p, 8, 8), capb = tyg_local_new(p, 8, 8);
                auto rc = TgModule::IsLim(t);
                // The length (and a limited array's capacity) at a cursor, which moves past them.
                auto head = [&](tyg_local c, tyg_local len, tyg_local cap) {
                    auto at = [&] { return LdL(c); };
                    auto put = [&](tyg_local l2, tyg_val v) { StL(l2, v); };
                    if (t->arr->akind == A_VAR) {
                        auto w = TgModule::LenWidth(t);
                        if (w == 0) {
                            put(len, UlebRead(at()));
                            StL(c, Add(at(), UlebSize(at())));
                        } else {
                            auto sc = w == 1 ? TYG_I8 : w == 2 ? TYG_I16 : w == 4 ? TYG_I32 : TYG_I64;
                            auto n = tyg_load(p, sc, AtV(at()), 0);
                            if (sc != TYG_I64) n = tyg_convert(p, TgModule::SignedLen(t) ? TYG_SEXT : TYG_ZEXT, TYG_I64, n);
                            put(len, n);
                            StL(c, Add(at(), I64(w)));
                        }
                    } else if (rc) {
                        put(cap, tyg_convert(p, TYG_ZEXT, TYG_I64, tyg_load(p, TYG_I32, AtV(at()), 0)));
                        put(len, tyg_convert(p, TYG_ZEXT, TYG_I64, tyg_load(p, TYG_I32, AtV(at(), 4), 0)));
                        StL(c, Add(at(), I64(8)));
                    } else {
                        No(l, cat("comparison of type ", M.NameWhy(t)));
                    }
                };
                head(ca, lena, capa);
                head(cb, lenb, capb);
                auto la = [&] { return LdL(lena); };
                auto lb = [&] { return LdL(lenb); };
                next(Cmp(TYG_EQ, la(), lb()));
                auto il = Local(I64(0));
                auto hd = NewBlock(), body = NewBlock(), done = NewBlock();
                tyg_jump(p, hd);
                SetBlock(hd);
                Branch(Cmp(TYG_SLT, LdL(il), la()), body, done);
                SetBlock(body);
                EqWalk(sub, ca, cb, fail, l);
                StL(il, Add(LdL(il), I64(1)));
                tyg_jump(p, hd);
                SetBlock(done);
                if (rc) {
                    // Each side's unused slots.
                    auto esz = cg.FixedSize(sub);
                    auto ld = [&](tyg_local c, tyg_local cap, tyg_local len) {
                        auto gap = Mul(Sub(LdL(cap), LdL(len)), I64(esz));
                        StL(c, Add(LdL(c), gap));
                    };
                    ld(ca, capa, lena);
                    ld(cb, capb, lenb);
                }
                return;
            }
            case TY_STRUCT: case TY_VARIANT:
                cg.EachField(t, [&](TypeExpr *ft) { EqWalk(ft, ca, cb, fail, l); });
                return;
            case TY_ENUM: {
                auto ei = cg.EIOf(t);
                auto ts = M.TagSize(t);
                auto ta = LoadTag(AtV(A()), t);
                next(Cmp(TYG_EQ, ta, LoadTag(AtV(B()), t)));
                auto tag = Local(LoadTag(AtV(A()), t));
                adv(I64(ts));
                auto done = NewBlock();
                for (size_t vi = 0; vi < ei->en->variants.size(); vi++) {
                    auto hit = NewBlock(), miss = NewBlock();
                    Branch(Cmp(TYG_EQ, LdL(tag), I64((int64_t)vi)), hit, miss);
                    SetBlock(hit);
                    EqWalk(cg.VariantType(t, (int)vi), ca, cb, fail, l);
                    tyg_jump(p, done);
                    SetBlock(miss);
                }
                tyg_jump(p, done);
                SetBlock(done);
                return;
            }
            default: No(l, cat("comparison of type ", M.NameWhy(t)));
        }
    }
    // The first n elements of two views, n their (equal) count.
    void EqElems(const View &va, const View &vb, tyg_block fail, Line l) {
        auto et = va.elem;
        auto ca = Local(va.elems), cb = Local(vb.elems);
        Loop(I64(0), va.len, [&](tyg_val) {
            EqJump(P { AtV(LdL(ca)), et }, P { AtV(LdL(cb)), et }, fail, l);
            StL(ca, StepElem(va, LdL(ca)));
            StL(cb, StepElem(vb, LdL(cb)));
        });
    }
    // Whether a bytes value's bytes are equal exactly when it is: no pad,
    // no float, no fixed-mode ADT (CodeGen::EmitEqBytes's bitwise).
    bool EqBitwise(TypeExpr *x) {
        if (cg.IsFix(x)) return cg.BitwiseEq(x);
        switch (x->kind) {
            case TY_INT: case TY_REF: return true;
            case TY_ARRAY: return EqBitwise(x->arr->sub);
            case TY_STRUCT: case TY_VARIANT: case TY_ENUM:
                for (auto &run : cg.FieldRuns(x))
                    for (size_t i = 0; i < run.fields->size(); i++)
                        if ((*run.fields)[i].ispad || !EqBitwise((*run.ftypes)[i])) return false;
                return true;
            default: return false;
        }
    }
    // a == b, a != b of values that are not scalars.
    V GenValEq(Binary *b, TypeExpr *lt) {
        auto pa = Deref(Place(b->left));
        auto pb = Deref(Place(b->right));
        if (Ended()) return Dummy(M.ast.booltype);
        auto res = tyg_local_new(p, 1, 1);
        auto fail = NewBlock(), done = NewBlock();
        EqJump(pa, P { pb.a, pa.t, pb.stk }, fail, b->line);
        tyg_store(p, TYG_I8, AtL(res), tyg_iconst(p, TYG_I8, 1), 0);
        tyg_jump(p, done);
        SetBlock(fail);
        tyg_store(p, TYG_I8, AtL(res), tyg_iconst(p, TYG_I8, 0), 0);
        tyg_jump(p, done);
        SetBlock(done);
        V x = ValV(M.ast.booltype, tyg_load(p, TYG_I8, AtL(res), 0));
        if (b->op == T_NEQ) x.v = Cmp(TYG_EQ, x.v, tyg_iconst(p, TYG_I8, 0));
        return x;
    }

    // Elementwise arithmetic on structs and fixed arrays (§6.1;
    // CodeGen::GenElemwiseInto): each numeric leaf of the result from the
    // same leaf of each operand, or from a scalar operand itself.
    // A leaf's place in the value its operand lies in: the constant offset
    // from the leaf's callback, and for an element of an array too big to
    // unroll, the bytes to it from the array's start, kept in a local.
    struct ElemDyn {
        bool on;
        tyg_local loc;
    };
    tyg_addr ElemAt(tyg_addr a, int64_t off, const ElemDyn &dy) {
        if (!dy.on) {
            a.offset += off;
            return a;
        }
        return AtV(Add(tyg_addr_of(p, a), LdL(dy.loc)), off);
    }
    // F(leaf type, offset, ElemDyn) for each scalar leaf, in order: the
    // elements of an array of up to 256 unrolled, those of a bigger one
    // (CodeGen's loop over it) in a counted loop.
    template <class F> void ElemLeaves(TypeExpr *t, int64_t off, Line l, F leaf) {
        ElemLeaves(t, off, l, leaf, ElemDyn { false, 0 });
    }
    template <class F> void ElemLeaves(TypeExpr *t, int64_t off, Line l, F leaf, ElemDyn dy) {
        switch (t->kind) {
            case TY_INT: case TY_FLT:
                if (t->kind == TY_INT && t->intstorage == IS_VARINT) No(l, "elementwise varint");
                leaf(t, off, dy);
                return;
            case TY_STRUCT:
                for (auto &f : M.Fields(t)) ElemLeaves(f.t, off + f.off, l, leaf, dy);
                return;
            case TY_ARRAY: {
                if (t->arr->akind != A_FIXED) No(l, "elementwise array");
                auto esz = cg.FixedSize(t->arr->sub);
                auto n = cg.ArrSize(t->arr);
                if (n <= 256) {
                    for (int64_t i = 0; i < n && !Ended(); i++) ElemLeaves(t->arr->sub, off + i * esz, l, leaf, dy);
                    return;
                }
                Loop(I64(0), I64(n), [&](tyg_val i) {
                    auto at = Mul(i, I64(esz));
                    ElemLeaves(t->arr->sub, off, l, leaf, ElemDyn { true, Local(dy.on ? Add(at, LdL(dy.loc)) : at) });
                });
                return;
            }
            default: No(l, cat("elementwise ", M.NameWhy(t)));
        }
    }
    tyg_val ScalarArith(TType op, TypeExpr *t, tyg_val a, tyg_val b, Line l) {
        switch (op) {
            case T_PLUSEQ: op = T_PLUS; break;
            case T_MINUSEQ: op = T_MINUS; break;
            case T_MULEQ: op = T_MUL; break;
            case T_DIVEQ: op = T_DIV; break;
            case T_MODEQ: op = T_MOD; break;
            default: break;
        }
        if (t->kind == TY_FLT) {
            switch (op) {
                case T_PLUS: return tyg_bin(p, TYG_FADD, a, b);
                case T_MINUS: return tyg_bin(p, TYG_FSUB, a, b);
                case T_MUL: return tyg_bin(p, TYG_FMUL, a, b);
                case T_DIV: return tyg_bin(p, TYG_FDIV, a, b);
                default: {
                    auto f32 = M.Scal(t) == TYG_F32;
                    auto ft = tyg_type_scalar(m, M.Scal(t));
                    vector<const tyg_type *> pts { ft, ft };
                    auto sig = tyg_sig_new(m, ft, pts.data(), 2, 0);
                    tyg_arg as[2] {};
                    as[0].val = a;
                    as[1].val = b;
                    return tyg_call(p, sig, tyg_direct(M.Sym(f32 ? "fmodf" : "fmod",
                                                             TYG_SYM_FUNC | TYG_SYM_GLOBAL | TYG_SYM_FOREIGN)),
                                    as, 2, nullptr, nullptr);
                }
            }
        }
        return IntOp(op, t, a, b, l);
    }
    tyg_val ScalarNeg(TypeExpr *t, tyg_val v, Line l) {
        if (t->kind == TY_FLT) return tyg_un(p, TYG_FNEG, v);
        if (!IsUnsigned(t->intstorage) && M.checks) return CheckedShim("neg", t, { v }, l);
        return tyg_un(p, TYG_NEG, v);
    }
    // An operand: a scalar's value, or a copy of the struct or array.
    struct ElemOp {
        bool scalar;
        TypeExpr *t;
        tyg_val v;
        tyg_local mem;
    };
    ElemOp ElemOperand(Node *n, TypeExpr *et) {
        auto t = cg.OperandT(n->exprtype);
        ElemOp o { t->kind != et->kind, t, 0, 0 };
        auto x = GenAs(n, t);
        if (Ended()) return o;
        if (o.scalar) {
            if (!M.IsScalar(t)) No(n->line, "elementwise operand");
            o.v = x.v;
            return o;
        }
        if (!x.mem || !M.TypeOk(t)) No(n->line, cat("elementwise operand ", M.NameWhy(t)));
        o.mem = Temp(t);
        tyg_copy(p, AtL(o.mem), x.a, M.Size(t));
        return o;
    }
    tyg_val ElemLeaf(const ElemOp &o, TypeExpr *tt, int64_t off, const ElemDyn &dy, Line l) {
        if (!o.scalar) return tyg_load(p, M.Scal(tt), ElemAt(AtL(o.mem), off, dy), 0);
        return cg.TEq(o.t, tt) ? o.v : Conv(o.v, o.t, tt, l);
    }
    V GenElemwise(Binary *b) {
        auto et = b->exprtype;
        if (!M.TypeOk(et)) No(b->line, cat("elementwise ", M.NameWhy(et)));
        auto lo = ElemOperand(b->left, et);
        auto ro = ElemOperand(b->right, et);
        if (Ended()) return Dummy(et);
        auto r = MemTemp(et, false);
        ElemLeaves(et, 0, b->line, [&](TypeExpr *tt, int64_t off, const ElemDyn &dy) {
            tyg_store(p, M.Scal(tt), ElemAt(r.a, off, dy),
                      ScalarArith(b->op, tt, ElemLeaf(lo, tt, off, dy, b->line), ElemLeaf(ro, tt, off, dy, b->line), b->line), 0);
        });
        return r;
    }

    // a == b of arrays or slices (GenRangeEq, §4.5): the lengths, then
    // the elements' bytes where that is their equality (integers, bools).
    V GenArrayEq(Binary *b) {
        auto l = ViewOf(Place(b->left), b->line);
        auto r = ViewOf(Place(b->right), b->line);
        auto et = l.elem;
        if (cg.Mangle(et) != cg.Mangle(r.elem)) No(b->line, "array comparison of another element type");
        if (!(et->kind == TY_INT || et->kind == TY_BOOL) || et->intstorage == IS_VARINT) {
            // Element by element.
            auto res = tyg_local_new(p, 1, 1);
            auto fail = NewBlock(), done = NewBlock(), ok = NewBlock();
            Branch(Cmp(TYG_EQ, l.len, r.len), ok, fail);
            SetBlock(ok);
            EqElems(l, r, fail, b->line);
            tyg_store(p, TYG_I8, AtL(res), tyg_iconst(p, TYG_I8, 1), 0);
            tyg_jump(p, done);
            SetBlock(fail);
            tyg_store(p, TYG_I8, AtL(res), tyg_iconst(p, TYG_I8, 0), 0);
            tyg_jump(p, done);
            SetBlock(done);
            V x = ValV(M.ast.booltype, tyg_load(p, TYG_I8, AtL(res), 0));
            if (b->op == T_NEQ) x.v = Cmp(TYG_EQ, x.v, tyg_iconst(p, TYG_I8, 0));
            return x;
        }
        auto res = tyg_local_new(p, 1, 1);
        auto same = Cmp(TYG_EQ, l.len, r.len);
        tyg_store(p, TYG_I8, AtL(res), same, 0);
        If(same, [&] {
            auto bytes = Mul(l.len, I64(cg.FixedSize(et)));
            auto d = CallC("memcmp", { tyg_type_ptr(m), tyg_type_ptr(m), IT(TYG_I64, false) },
                           { l.elems, r.elems, bytes }, IT(TYG_I32));
            tyg_store(p, TYG_I8, AtL(res), Cmp(TYG_EQ, d, tyg_iconst(p, TYG_I32, 0)), 0);
        });
        V x = ValV(M.ast.booltype, tyg_load(p, TYG_I8, AtL(res), 0));
        if (b->op == T_NEQ) x.v = Cmp(TYG_EQ, x.v, tyg_iconst(p, TYG_I8, 0));
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
                          { d, tyg_iconst(p, TYG_I32, M.Scal(st) == TYG_F32), I64(lo),
                            I64(hi), TypeNameArg(is), FileArg(c->line),
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
                      { x.v, I64(IntRange(is).second), TypeNameArg(is),
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
                  { w, I64(lo), I64(hi), TypeNameArg(is),
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
        if (M.RbOk(t)) {
            // A resizable ADT's literal: on a temporary's stack, its header beside it.
            auto x = RzTemp(t, sl->line);
            GenRb(sl, x.stk, x.a, t, sl->line);
            return x;
        }
        if (IsImg(t) && M.ImgOk(t)) {
            // An image: built at the top of a temporary's stack.
            auto stk = Stk(AllocStk(false, sl->line), sl->line);
            return MemV(t, AtV(GenImg(sl, stk, t, sl->line)));
        }
        if (t->kind == TY_VARIANT || t->kind == TY_ENUM) return GenVariantLit(sl);
        if (!sl->sinst || sl->variant || t->kind != TY_STRUCT || !M.TypeOk(t))
            No(sl->line, "literal of this type");
        auto tmp = Temp(t);
        tyg_zero(p, AtL(tmp), M.Size(t));
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
            Into(P { AtL(tmp, lay.offs[i]), ft }, init);
        }
        return MemV(t, AtL(tmp));
    }

    // ---- Arrays and slices -------------------------------------------------

    // The elements of an array or slice in memory at pl (references read
    // through) and their count: a fixed array's lie in place, a slice's
    // where its data member points.
    View ViewOf(P pl, Line l) {
        pl = Deref(pl);
        if (pl.t->kind == TY_ARRAY && pl.t->arr->akind == A_FIXED && M.TypeOk(pl.t)) {
            auto k = cg.ArrSize(pl.t->arr);
            return { tyg_addr_of(p, pl.a), I64(k), pl.t->arr->sub, k };
        }
        if ((pl.t->kind == TY_SLICE && M.TypeOk(pl.t)) || (IsGrow(pl.t) && M.RzOk(pl.t))) {
            // { data, len }, and gs_rhdr { base, len } alike.
            auto a8 = pl.a;
            a8.offset += 8;
            auto elem = pl.t->kind == TY_SLICE ? pl.t->sub : pl.t->arr->sub;
            return { BaseOf(pl), Ld(a8), elem, -1 };
        }
        if (TgModule::IsLim(pl.t) && M.LimHeld(pl.t)) return LimView(pl);
        if (IsGrow(pl.t) && M.VeOk(pl.t)) {
            // Elements of variable size: walked from the first (StepElem).
            auto a8 = pl.a;
            a8.offset += 8;
            return { BaseOf(pl), Ld(a8), pl.t->arr->sub, -1 };
        }
        if (IsArrayKind(pl.t, A_VAR) && (M.RzOk(pl.t) || M.VarImgOk(pl.t))) {
            // A T[] image: the length prefix, the elements behind it.
            auto w = TgModule::LenWidth(pl.t);
            auto base = tyg_addr_of(p, pl.a);
            if (w == 0) {
                auto n = UlebRead(base);
                return { Add(base, UlebSize(base)), n, pl.t->arr->sub, -1 };
            }
            auto s = w == 1 ? TYG_I8 : w == 2 ? TYG_I16 : w == 4 ? TYG_I32 : TYG_I64;
            auto n = tyg_load(p, s, pl.a, 0);
            if (s != TYG_I64) n = tyg_convert(p, TgModule::SignedLen(pl.t) ? TYG_SEXT : TYG_ZEXT, TYG_I64, n);
            return { Add(base, I64(w)), n, pl.t->arr->sub, -1 };
        }
        No(l, "array of this kind");
    }
    tyg_val ElemAddr(const View &v, tyg_val i) {
        if (!cg.IsFix(v.elem)) No(Line {}, "an element of variable size by its index");
        auto esz = cg.FixedSize(v.elem);
        auto off = Mul(i, I64(esz));
        return Add(v.elems, off);
    }

    // The element after the one at `at`: a fixed size on, or past its
    // image's bytes.
    // The element at `at`: a varint one zigzag-encoded there.
    P ElemPlace(tyg_val at, TypeExpr *et) {
        P r { AtV(at), et };
        r.enc = TgModule::IsVarint(et);
        return r;
    }
    tyg_val StepElem(const View &v, tyg_val at) {
        if (cg.IsFix(v.elem)) return Add(at, I64(cg.FixedSize(v.elem)));
        return Add(at, ImgSize(v.elem, at));
    }

    // The end of a view's elements: past the last, each walked by its size
    // where they vary (CodeGen's SizeFn).
    tyg_val RunEnd(const View &v) {
        if (cg.IsFix(v.elem))
            return Add(v.elems, Mul(v.len, I64(cg.FixedSize(v.elem))));
        auto cl = Local(v.elems);
        Loop(I64(0), v.len, [&](tyg_val) { StL(cl, Add(LdL(cl), ImgSize(v.elem, LdL(cl)))); });
        return LdL(cl);
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
        auto ok = NewBlock(), bad = NewBlock();
        Branch(Cmp(TYG_ULT, i, n), ok, bad);
        SetBlock(bad);
        CallC("gs_idxfail", { IT(TYG_I64), IT(TYG_I64), tyg_type_ptr(m), IT(TYG_I32) },
              { i, n, FileArg(l), LineArg(l) }, nullptr);
        tyg_unreachable(p);
        SetBlock(ok);
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
        return P { AtV(ElemAddr(v, i)), v.elem };
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
            len = I64(cg.ArrSize(ot->arr));
        } else {
            len = ViewOf(Place(obj), n->line).len;
        }
        return ValV(M.ast.inttypes[IS_I64], len);
    }

    // A slice { data, len } in a temporary.
    V SliceValue(TypeExpr *t, tyg_val data, tyg_val len) {
        auto tmp = tyg_local_new(p, 16, 8);
        StL(tmp, data);
        StL(tmp, len, 8);
        return MemV(t, AtL(tmp));
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
                Into(P { AtL(tmp, i * esz), et }, al->elems[(size_t)i]);
                if (Ended()) return Dummy(t);
            }
            return SliceValue(t, tyg_addr_of(p, AtL(tmp)), I64(k));
        }
        if (auto al = Is<ArrayLit>(n); al && al->fillval && !al->capexpr && cg.IsFix(t->sub)) {
            // [v; k] at a slice destination: v built once, copied k times
            // into a temporary of its own, viewed (CodeGen::GenArrayLit).
            auto fc = Is<IntLit>(al->fillcount);
            auto et = t->sub;
            if (!fc || fc->val < 0 || fc->val * cg.FixedSize(et) > (1 << 20)) No(n->line, "fill count");
            if (SelfRelLit(al->fillval) || (TgModule::IsRel(et) && !et->ref->pool && !Is<NullLit>(al->fillval)))
                No(n->line, "fill of a value linking to itself");
            auto esz = cg.FixedSize(et);
            auto k = fc->val;
            auto tmp = tyg_local_new(p, std::max<int64_t>(k * esz, 1), 8);
            auto one = Temp(et);
            Into(P { AtL(one), et }, al->fillval);
            if (Ended()) return Dummy(t);
            Loop(I64(0), I64(k), [&](tyg_val i) {
                tyg_copy(p, AtV(Add(tyg_addr_of(p, AtL(tmp)), Mul(i, I64(esz)))), AtL(one), esz);
            });
            return SliceValue(t, tyg_addr_of(p, AtL(tmp)), I64(k));
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
            return fromend ? Sub(v.len, x) : x;
        };
        auto lo = bound(se->lo, se->lo_from_end, I64(0));
        auto hi = bound(se->hi, se->hi_from_end, v.len);
        if (!se->nobc) {
            auto ok = NewBlock(), bad = NewBlock(), c2 = NewBlock(),
                 c3 = NewBlock();
            Branch(Cmp(TYG_SLT, lo, I64(0)), bad, c2);
            SetBlock(c2);
            Branch(Cmp(TYG_SLT, hi, lo), bad, c3);
            SetBlock(c3);
            Branch(Cmp(TYG_SGT, hi, v.len), bad, ok);
            SetBlock(bad);
            M.NeedShim("gs_tg_slicefail");
            CallC("gs_tg_slicefail", { tyg_type_ptr(m), IT(TYG_I32) },
                  { FileArg(se->line), LineArg(se->line) }, nullptr);
            tyg_unreachable(p);
            SetBlock(ok);
        }
        auto t = se->exprtype;
        // A slice the checker typed as the array it builds (a T[] result)
        // is a view all the same, which that array is built from.
        if (t && t->kind == TY_ARRAY && t->arr->akind != A_FIXED) t = M.ast.SliceOf(t->arr->sub, se->line);
        if (!t || t->kind != TY_SLICE || cg.Mangle(t->sub) != cg.Mangle(v.elem)) No(se->line, "slice to this type");
        // Variable elements: whole-array slices only (the checker's rule).
        if (!cg.IsFix(v.elem)) return SliceValue(t, v.elems, v.len);
        return SliceValue(t, ElemAddr(v, lo), Sub(hi, lo));
    }

    // [a, b, c] and [v; k] as a fixed array.
    V GenArrayLit(ArrayLit *al) {
        auto t = al->exprtype;
        if (TgModule::IsLim(t)) return LimLit(al);
        if (t && IsImg(t) && M.ImgOk(t)) {
            // An image: built at the top of a temporary's stack.
            auto stk = Stk(AllocStk(false, al->line), al->line);
            return MemV(t, AtV(GenImg(al, stk, t, al->line)));
        }
        if (t && IsGrow(t) && (M.RzOk(t) || M.VeOk(t))) {
            auto x = RzTemp(t, al->line);
            GenRz(al, x.stk, x.a, t, al->line);
            return x;
        }
        if (t && t->kind == TY_SLICE && M.TypeOk(t) && !al->capexpr && cg.IsFix(t->sub)) {
            // A literal in slice position: a temporary fixed array, sliced
            // whole (CodeGen::GenFixedArrayLit).
            auto count = al->fillval ? (Is<IntLit>(al->fillcount) ? Is<IntLit>(al->fillcount)->val : -1)
                                     : (int64_t)al->elems.size();
            if (count < 0) No(al->line, "fill count");
            auto at = M.ast.ArrayOf(t->sub, A_FIXED, al->line, count);
            // Put back even when the literal is left to C.
            struct Restore {
                ArrayLit *al;
                TypeExpr *t;
                ~Restore() { al->exprtype = t; }
            } restore { al, t };
            al->exprtype = at;
            auto av = GenArrayLit(al);
            if (Ended() || !av.mem) return Dummy(t);
            return SliceValue(t, tyg_addr_of(p, av.a), I64(count));
        }
        if (!t || t->kind != TY_ARRAY || t->arr->akind != A_FIXED || !M.TypeOk(t) || al->capexpr)
            No(al->line, cat("array literal of type ", t ? M.NameWhy(t) : "?"));
        auto k = cg.ArrSize(t->arr);
        auto et = t->arr->sub;
        auto esz = cg.FixedSize(et);
        // Its links measure from the slot it lies in, which a temporary is not.
        if (SelfRelLit(al)) No(al->line, al->fillval ? "fill of a value linking to itself" : "self-relative link in a literal");
        auto tmp = Temp(t);
        tyg_zero(p, AtL(tmp), M.Size(t));
        if (al->fillval) {
            auto il = Is<IntLit>(al->fillcount);
            if (!il || il->val != k) No(al->line, "fill count");
            // The value once, then copied into every slot.
            auto one = Temp(et);
            Into(P { AtL(one), et }, al->fillval);
            if (k <= 16) {
                for (int64_t i = 0; i < k; i++)
                    tyg_copy(p, AtL(tmp, i * esz), AtL(one), esz);
            } else {
                Loop(I64(0), I64(k), [&](tyg_val c) {
                    View v { tyg_addr_of(p, AtL(tmp)), 0, et, k };
                    tyg_copy(p, AtV(ElemAddr(v, c)), AtL(one), esz);
                });
            }
        } else {
            if ((int64_t)al->elems.size() > k) No(al->line, "array literal length");
            for (size_t i = 0; i < al->elems.size(); i++)
                Into(P { AtL(tmp, (int64_t)i * esz), et }, al->elems[i]);
        }
        return MemV(t, AtL(tmp));
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
        if (!M.IsRc(pl.t)) return I64(LimCap(pl.t));
        return tyg_convert(p, TYG_ZEXT, TYG_I64, tyg_load(p, TYG_I32, pl.a, 0));
    }
    View LimView(const P &pl) {
        auto elems = Add(tyg_addr_of(p, pl.a), I64(M.LimWidth(pl.t)));
        return { elems, LimLen(pl), pl.t->arr->sub, -1 };
    }
    // A new value of type t in a temporary: zero bytes, an empty array.
    P LimTemp(TypeExpr *t) {
        P pl { AtL(Temp(t)), t };
        tyg_zero(p, pl.a, M.Size(t));
        return pl;
    }
    V LimValue(const P &pl) {
        return MemV(pl.t, pl.a);
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
        x.v = I64(LimCap(ot));
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
        auto nl = Local(v.len);
        auto n = [&] { return LdL(nl); };
        AbortUnless(Cmp(TYG_SLE, n(), LimCapOf(dst)), "CAPACITY", l);
        auto to = LimView(dst).elems;
        tyg_move_dyn(p, to, v.elems, Mul(n(), I64(cg.FixedSize(et))));
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
            tyg_copy(p, AtV(LimView(dst).elems), tyg_at_sym(M.Bytes(s->val), 0),
                     (int64_t)s->val.size());
        LimSetLen(dst, I64((int64_t)s->val.size()));
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
            Into(P { AtL(one), et }, al->fillval);
            for (int64_t i = 0; i < count && !Ended(); i++) {
                auto a = dst.a;
                a.offset += M.LimWidth(t) + i * esz;
                tyg_copy(p, a, AtL(one), esz);
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
        if (!Ended()) LimSetLen(dst, I64(count));
        return LimValue(dst);
    }
    // A call returning a T[..k] that the checker typed as the array built
    // from it (a T[], a resizable): its result where it lies, to copy from.
    bool LimCallResult(Node *n) {
        auto c = Is<Call>(n);
        if (!c || c->builtin >= 0 || !n->exprtype) return false;
        auto sp = c->spec ? c->spec : !c->dispatch.empty() ? c->dispatch[0] : nullptr;
        if (!sp || sp->rets.empty()) return false;
        auto rt = sp->rets[0];
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
        auto dp = discardpush;
        discardpush = false;
        if (!M.LimHeld(pl.t)) No(c->line, cat("push to ", M.NameWhy(pl.t)));
        auto et = pl.t->arr->sub;
        // A literal linking to itself is built in its slot, once the
        // capacity is checked (CodeGen::EmitPush).
        auto atslot = cg.HasRelRef(et) && SelfRelLitOk(c->ArgNodes()[1]) && cg.TEq(c->ArgNodes()[1]->exprtype, et);
        V x;
        if (!atslot) {
            x = GenAs(c->ArgNodes()[1], et);
            if (Ended()) return Dummy(c->exprtype);
            if (!M.IsScalar(et) && !x.mem) No(c->line, "aggregate value");
        }
        auto v = LimView(pl);
        auto nl = Local(v.len);
        auto n = [&] { return LdL(nl); };
        AbortUnless(Cmp(TYG_SLT, n(), LimCapOf(pl)), "CAPACITY", c->line);
        auto at = ElemAddr(LimView(pl), n());
        if (atslot) SelfRelInto(P { AtV(at), et }, c->ArgNodes()[1]);
        else Store(P { AtV(at), et }, x);
        if (Ended()) return Dummy(c->exprtype);
        LimSetLen(pl, Add(n(), I64(1)));
        V r;
        r.t = c->exprtype && c->exprtype->kind == TY_REF ? c->exprtype : nullptr;
        if (r.t) r.v = at;
        // The element itself, as a value, where one is wanted.
        pushed = P { AtV(at), et };
        if (!r.t && !dp && c->exprtype && c->exprtype->kind != TY_VOID && M.TypeOk(et))
            return Load(*pushed, c->line);
        return r;
    }
    // A reusable pool's array, its freelist's header and the freelist's
    // stack: a local's, or a global's through its accessors.
    void PoolAt(VarDef *vd, Line l, P &pl, tyg_addr &fl, tyg_val &flstk) {
        if (vd && prefs.count(vd)) {
            auto loc = prefs[vd];
            pl = VarPlace(vd, l);
            if (IsAddrRef(pl.t)) pl = Deref(pl);
            fl = AtV(LdL(loc, 16));
            flstk = LdL(loc, 24);
            return;
        }
        if (vd && cg.PrefVar(vd)) {
            // A pool reference variable: gs_pref { hdr, stk, fl, flstk } where
            // it lies, a local's, a global's, or a free variable's.
            pl = VarPlace(vd, l);
            auto at = pl.a;
            if (IsAddrRef(pl.t)) pl = Deref(pl);
            at.offset += 16;
            fl = AtV(Ld(at));
            at.offset += 8;
            flstk = Ld(at);
            return;
        }
        if (!vd || !(poolfl.count(vd) || (vd->isglobal && vd->reusable))) No(l, "pool of this kind");
        pl = VarPlace(vd, l);
        if (vd->isglobal) {
            auto [h, st] = M.PoolAccessors(vd);
            fl = AtV(LdL(AccessorLocal(h)));
            flstk = LdL(AccessorLocal(st));
        } else {
            fl = AtL(poolfl[vd].first);
            flstk = LdL(poolfl[vd].second);
        }
    }
    // A slice pool (§5.4; CodeGen::EmitSlicePool): runs of elements of a
    // reusable array, its freelist of (index, count) spans kept by the
    // runtime's gs_spans_*.
    V GenSlicePool(Call *c) {
        auto an = c->ArgNodes();
        auto recv = an[0];
        if (auto u = Is<Unary>(recv); u && u->op == T_BITAND) recv = u->child;
        auto id = Is<Ident>(recv);
        P pl;
        tyg_addr fl;
        tyg_val flstk;
        PoolAt(id ? id->vdef : nullptr, c->line, pl, fl, flstk);
        if (!IsGrow(pl.t) || !M.RzOk(pl.t) || !pl.stk) No(c->line, "slice pool of this kind");
        auto elem = pl.t->arr->sub;
        auto esz = cg.FixedSize(elem);
        auto len = pl.a;
        len.offset += 8;
        auto fllen = fl;
        fllen.offset += 8;
        auto pt = tyg_type_ptr(m);
        auto spanargs = [&]() -> vector<tyg_val> {
            return { Ld(fl), tyg_addr_of(p, fllen), flstk };
        };
        auto elems = [&] { return Ld(pl.a); };
        // A slice's index in the pool: 0 for an empty one; else, where the
        // checker cannot root it here, checked to start a whole number of
        // elements in and to end inside the length.
        auto indexof = [&](tyg_val data, tyg_val n) {
            auto il = tyg_local_new(p, 8, 8);
            StL(il, I64(0));
            auto ne = NewBlock(), done = NewBlock();
            Branch(Cmp(TYG_NE, n, I64(0)), ne, done);
            SetBlock(ne);
            auto off = Sub(data, elems());
            if (c->poolcheck && esz) {
                auto bad = tyg_bin(p, TYG_OR, Cmp(TYG_NE, tyg_bin(p, TYG_UREM, off, I64(esz)), I64(0)),
                                   Cmp(TYG_UGE, tyg_bin(p, TYG_UDIV, off, I64(esz)), Ld(len)));
                auto k = tyg_bin(p, TYG_UDIV, off, I64(esz));
                bad = tyg_bin(p, TYG_OR, bad, Cmp(TYG_UGT, n, Sub(Ld(len), k)));
                AbortUnless(Cmp(TYG_EQ, bad, tyg_iconst(p, TYG_I8, 0)), "POOLSLICE", c->line);
            }
            StL(il, tyg_bin(p, TYG_SDIV, off, I64(std::max<int64_t>(esz, 1))));
            tyg_jump(p, done);
            SetBlock(done);
            return il;
        };
        auto slicelen = [&](Node *n) {
            auto nl = tyg_local_new(p, 8, 8);
            StL(nl, Index64(n));
            if (esz) AbortUnless(Cmp(TYG_ULE, LdL(nl), I64((int64_t)((2048ull << 20) / (uint64_t)esz))),
                                 "SLICELEN", c->line);
            else AbortUnless(Cmp(TYG_SGE, LdL(nl), I64(0)), "SLICELEN", c->line);
            return nl;
        };
        auto slice = [&](Node *n, tyg_local &data, tyg_local &cnt) {
            auto x = GenAs(n, M.ast.SliceOf(elem, n->line));
            data = tyg_local_new(p, 8, 8);
            cnt = tyg_local_new(p, 8, 8);
            StL(data, Ld(x.a));
            auto a8 = x.a;
            a8.offset += 8;
            StL(cnt, Ld(a8));
        };
        if (c->builtin == B_FREE_SLICE) {
            tyg_local d, n;
            slice(an[1], d, n);
            if (Ended()) return V {};
            auto il = indexof(LdL(d), LdL(n));
            auto as = spanargs();
            CallC("gs_spans_free", { pt, pt, pt, IT(TYG_I64), IT(TYG_I64) }, { as[0], as[1], as[2], LdL(il), LdL(n) }, nullptr);
            return V {};
        }
        // Where a run of n goes: past the end while the freelist is empty.
        auto place = [&](tyg_val n) {
            auto r = tyg_local_new(p, 8, 8);
            auto some = NewBlock(), none = NewBlock(), done = NewBlock();
            Branch(Cmp(TYG_NE, Ld(fllen), I64(0)), some, none);
            SetBlock(some);
            auto as = spanargs();
            StL(r, CallC("gs_spans_alloc", { pt, pt, pt, IT(TYG_I64), IT(TYG_I64) },
                        { as[0], as[1], as[2], Ld(len), n }, IT(TYG_I64)));
            tyg_jump(p, done);
            SetBlock(none);
            StL(r, Ld(len));
            tyg_jump(p, done);
            SetBlock(done);
            return r;
        };
        auto extend = [&](tyg_val end) {
            auto ext = Sub(end, Ld(len));
            auto grow = NewBlock(), done = NewBlock();
            auto el = tyg_local_new(p, 8, 8);
            StL(el, ext);
            Branch(Cmp(TYG_SGT, ext, I64(0)), grow, done);
            SetBlock(grow);
            St(len, Add(Ld(len), LdL(el)));
            SetTop(pl.stk, Add(Top(pl.stk), Mul(LdL(el), I64(esz))));
            tyg_jump(p, done);
            SetBlock(done);
        };
        // Default values for count elements from first (EmitDefaultElems).
        auto defaults = [&](tyg_val first, tyg_val count) {
            if (!esz) return;
            if (!cg.HasFieldDefaults(elem)) {
                auto at = Add(elems(), Mul(first, I64(esz)));
                CallC("memset", { pt, IT(TYG_I32), IT(TYG_I64, false) },
                      { at, tyg_iconst(p, TYG_I32, 0), Mul(count, I64(esz)) }, pt);
                return;
            }
            if (!c->defaultinit) No(c->line, "slice pool defaults");
            auto fl0 = Local(first);
            Loop(I64(0), count, [&](tyg_val k) {
                OpenScope(true);
                Into(P { AtV(Add(elems(), Mul(Add(LdL(fl0), k), I64(esz)))), elem }, c->defaultinit);
                CloseScope();
            });
        };
        tyg_local il, nl;
        if (c->builtin == B_ALLOC_SLICE) {
            nl = slicelen(an[1]);
            if (Ended()) return Dummy(c->exprtype);
            il = place(LdL(nl));
            extend(Add(LdL(il), LdL(nl)));
            defaults(LdL(il), LdL(nl));
        } else {
            tyg_local d, ol;
            slice(an[1], d, ol);
            nl = slicelen(an[2]);
            if (Ended()) return Dummy(c->exprtype);
            il = indexof(LdL(d), LdL(ol));
            auto shrink = NewBlock(), growb = NewBlock(), done = NewBlock();
            Branch(Cmp(TYG_SLE, LdL(nl), LdL(ol)), shrink, growb);
            SetBlock(shrink);
            {
                auto as = spanargs();
                CallC("gs_spans_free", { pt, pt, pt, IT(TYG_I64), IT(TYG_I64) },
                      { as[0], as[1], as[2], Add(LdL(il), LdL(nl)), Sub(LdL(ol), LdL(nl)) },
                      nullptr);
            }
            tyg_jump(p, done);
            SetBlock(growb);
            auto empty = NewBlock(), notempty = NewBlock(), moved = NewBlock(), placed = NewBlock();
            Branch(Cmp(TYG_EQ, LdL(ol), I64(0)), empty, notempty);
            SetBlock(empty);
            StL(il, LdL(place(LdL(nl))));
            tyg_jump(p, placed);
            SetBlock(notempty);
            Branch(Cmp(TYG_NE, Add(LdL(il), LdL(ol)), Ld(len)), moved, placed);
            SetBlock(moved);
            {
                auto as = spanargs();
                auto ni = tyg_local_new(p, 8, 8);
                StL(ni, CallC("gs_spans_regrow", { pt, pt, pt, IT(TYG_I64), IT(TYG_I64), IT(TYG_I64), IT(TYG_I64) },
                             { as[0], as[1], as[2], Ld(len), LdL(il), LdL(ol), LdL(nl) }, IT(TYG_I64)));
                auto mv = NewBlock(), same = NewBlock();
                Branch(Cmp(TYG_NE, LdL(ni), LdL(il)), mv, same);
                SetBlock(mv);
                tyg_move_dyn(p, Add(elems(), Mul(LdL(ni), I64(esz))),
                             Add(elems(), Mul(LdL(il), I64(esz))),
                             Mul(LdL(ol), I64(esz)));
                tyg_jump(p, same);
                SetBlock(same);
                StL(il, LdL(ni));
            }
            tyg_jump(p, placed);
            SetBlock(placed);
            extend(Add(LdL(il), LdL(nl)));
            defaults(Add(LdL(il), LdL(ol)), Sub(LdL(nl), LdL(ol)));
            tyg_jump(p, done);
            SetBlock(done);
        }
        return SliceValue(M.ast.SliceOf(elem, c->line), Add(elems(), Mul(LdL(il), I64(esz))), LdL(nl));
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
        P pl;
        tyg_addr fl;
        tyg_val flstk;
        PoolAt(vd, c->line, pl, fl, flstk);
        auto fllen = fl;
        fllen.offset += 8;
        auto len = pl.a;
        len.offset += 8;
        auto elem = pl.t->arr->sub;
        auto esz = cg.FixedSize(elem);
        if (c->builtin == B_FREE) {
            auto i = Index64(an[1]);
            if (Ended()) return V {};
            BoundsCheck(i, Ld(len), c->line);
            auto top = Top(flstk);
            St(AtV(top), i);
            SetTop(flstk, Add(top, I64(8)));
            St(fllen, Add(Ld(fllen), I64(1)));
            return V {};
        }
        // The value first, then the slot (the checker keeps the array from
        // growing into a fresh one meanwhile); a literal with a self-relative
        // link is built once the slot is known (CodeGen's atslot).
        auto atslot = cg.HasRelRef(elem) && SelfRelLitOk(an[1]) && cg.TEq(an[1]->exprtype, elem);
        V x;
        if (!atslot) {
            x = GenAs(an[1], elem);
            if (Ended()) return Dummy(c->exprtype);
            if (!M.IsScalar(elem) && !x.mem) No(c->line, "aggregate value");
        }
        auto il = tyg_local_new(p, 8, 8);
        auto pop = NewBlock(), fresh = NewBlock(), got = NewBlock();
        Branch(Cmp(TYG_SGT, Ld(fllen), I64(0)), pop, fresh);
        SetBlock(pop);
        St(fllen, Sub(Ld(fllen), I64(1)));
        auto ftop = Sub(Top(flstk), I64(8));
        SetTop(flstk, ftop);
        StL(il, Ld(AtV(ftop)));
        tyg_jump(p, got);
        SetBlock(fresh);
        StL(il, Ld(len));
        tyg_jump(p, got);
        SetBlock(got);
        auto idx = [&] { return LdL(il); };
        auto base = Ld(pl.a);
        auto e = Add(base, Mul(idx(), I64(esz)));
        if (atslot) SelfRelInto(P { AtV(e), elem }, an[1]);
        else Store(P { AtV(e), elem }, x);
        auto grow = NewBlock(), done = NewBlock();
        Branch(Cmp(TYG_EQ, idx(), Ld(len)), grow, done);
        SetBlock(grow);
        St(len, Add(Ld(len), I64(1)));
        SetTop(pl.stk, Add(Top(pl.stk), I64(esz)));
        tyg_jump(p, done);
        SetBlock(done);
        V r;
        r.t = c->exprtype;
        if (c->builtin == B_ALLOC_INDEX) {
            r.v = Conv(idx(), M.ast.inttypes[IS_I64], c->exprtype, c->line);
        } else {
            auto base2 = Ld(pl.a);
            r.v = Add(base2, Mul(idx(), I64(esz)));
            // A value the checker decayed the reference to: the element.
            if (c->exprtype && c->exprtype->kind != TY_REF && !M.IsScalar(c->exprtype) && M.TypeOk(elem))
                return Load(P { AtV(r.v), elem }, c->line);
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
        if (auto al = Is<ArrayLit>(src); al && !al->capexpr && cg.HasRelRef(et) && al->exprtype &&
                                         al->exprtype->kind == TY_ARRAY && cg.TEq(al->exprtype->arr->sub, et) &&
                                         (al->fillval ? Is<IntLit>(al->fillcount) != nullptr : true)) {
            // A literal of elements holding relative references builds its
            // run in the free slots, once the capacity is checked.
            auto k = LitCount(al);
            auto ol = Local(LimLen(pl));
            auto o = [&] { return LdL(ol); };
            AbortUnless(Cmp(TYG_SLE, Add(o(), I64(k)), LimCapOf(pl)), "CAPACITY", c->line);
            auto leaves = FillCapture(al);
            if (Ended()) return V {};
            auto base = Local(ElemAddr(LimView(pl), o()));
            LitRunAt(al, et, base, k, leaves);
            if (Ended()) return V {};
            LimSetLen(pl, Add(o(), I64(k)));
            return V {};
        }
        if (auto s = Is<StrLit>(src)) {
            if (!IsU8(et)) No(c->line, "string of this type");
            v = { tyg_addr_of(p, tyg_at_sym(M.Bytes(s->val), 0)),
                  I64((int64_t)s->val.size()), et, -1 };
        } else {
            if (Is<Call>(src) && src->exprtype && (IsGrow(src->exprtype) || IsVarArr(src->exprtype)))
                No(c->line, "append of this call");
            auto from = Place(src);
            if (Ended()) return V {};
            v = ViewOf(from, c->line);
        }
        if (cg.Mangle(v.elem) != cg.Mangle(et)) No(c->line, "append of another element type");
        auto nl = Local(v.len);
        auto n = [&] { return LdL(nl); };
        auto ol = Local(LimLen(pl));
        auto o = [&] { return LdL(ol); };
        auto total = [&] { return Add(o(), n()); };
        AbortUnless(Cmp(TYG_SLE, total(), LimCapOf(pl)), "CAPACITY", c->line);
        tyg_move_dyn(p, ElemAddr(LimView(pl), o()), v.elems, Mul(n(), I64(cg.FixedSize(et))));
        LimSetLen(pl, total());
        return V {};
    }
    // pop, pop_n, resize and clear (codegen_builtins.h): the receiver,
    // the arguments, the checks; only the length moves.
    V LimShrink(Call *c, const P &pl) {
        if (!M.LimHeld(pl.t)) No(c->line, cat(BuiltinByKind(c->builtin).name, " of ", M.NameWhy(pl.t)));
        auto an = c->ArgNodes();
        auto et = pl.t->arr->sub;
        switch (c->builtin) {
            case B_POP: {
                auto nl = Sub(LimLen(pl), I64(1));
                AbortUnless(Cmp(TYG_SGE, nl, I64(0)), "POP", c->line);
                LimSetLen(pl, nl);
                return Load(P { AtV(ElemAddr(LimView(pl), LimLen(pl))), et }, c->line);
            }
            case B_POP_N: {
                auto nn = Index64(an[1]);
                if (Ended()) return V {};
                auto ol = LimLen(pl);
                AbortUnless(Cmp(TYG_ULE, nn, ol), "POPN", c->line);
                LimSetLen(pl, Sub(ol, nn));
                return V {};
            }
            case B_CLEAR:
                LimSetLen(pl, I64(0));
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
        auto nl = Local(nn);
        auto n = [&] { return LdL(nl); };
        auto ol = Local(LimLen(pl));
        auto o = [&] { return LdL(ol); };
        AbortUnless(Cmp(TYG_SGE, n(), I64(0)), "RESIZENEG", c->line);
        auto shrink = NewBlock(), notshrink = NewBlock(), grow = NewBlock(),
             done = NewBlock();
        Branch(Cmp(TYG_SLT, n(), o()), shrink, notshrink);
        SetBlock(shrink);
        LimSetLen(pl, n());
        tyg_jump(p, done);
        SetBlock(notshrink);
        Branch(Cmp(TYG_SGT, n(), o()), grow, done);
        SetBlock(grow);
        if (an.size() <= 2) {
            Abort("RESIZEFILL", c->line);
        } else {
            AbortUnless(Cmp(TYG_SLE, n(), LimCapOf(pl)), "CAPACITY", c->line);
            Loop(o(), n(), [&](tyg_val i) { Store(P { AtV(ElemAddr(LimView(pl), i)), et }, fv); });
            LimSetLen(pl, n());
            tyg_jump(p, done);
        }
        SetBlock(done);
        return V {};
    }

    // ---- Calls -------------------------------------------------------------

    tyg_arg Arg(Node *n, TypeExpr *pt) {
        if (!M.TypeOk(pt)) No(n->line, "argument of this type");
        tyg_arg a {};
        auto x = GenAs(n, pt);
        if (TgModule::IsRel(pt)) {
            // A relative parameter takes the offset (CodeGen::EmitArg),
            // the same in the callee's copy for the `in pool` form.
            if (Ended()) return a;
            auto slot = Temp(pt);
            Store(P { AtL(slot), pt }, x);
            a.val = tyg_load(p, M.RelScal(pt), AtL(slot), 0);
            return a;
        }
        if (M.IsScalar(pt)) a.val = x.v;
        else a.mem = x.a;
        return a;
    }

    // A large fixed value by value: the address of a copy of its own
    // (CodeGen::SigParams), which the callee reads through.
    tyg_arg LargeArg(Node *n, TypeExpr *pt) {
        if (!M.TypeOk(pt)) No(n->line, "argument of this type");
        tyg_arg a {};
        auto x = GenAs(n, pt);
        if (Ended()) return a;
        auto cp = Temp(pt);
        tyg_copy(p, AtL(cp), x.a, M.Size(pt));
        a.val = tyg_addr_of(p, AtL(cp));
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
        auto ra = AtL(t);
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
            auto ok = NewBlock(), bad = NewBlock();
            Branch(Truth(c->ArgNodes()[0]), ok, bad);
            SetBlock(bad);
            Abort("ASSERT", c->line);
            SetBlock(ok);
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
            auto d = Sub(r.v, v.elems);
            auto ix = tyg_bin(p, TYG_SDIV, d, I64(std::max<int64_t>(cg.FixedSize(v.elem), 1)));
            return ValV(c->exprtype, Conv(ix, M.ast.inttypes[IS_I64], c->exprtype, c->line));
        }
        if ((c->builtin == B_ALLOC_INDEX || c->builtin == B_ALLOC_REF || c->builtin == B_FREE) &&
            c->ArgNodes().size() == 2)
            return GenPool(c);
        if (c->builtin == B_ALLOC_SLICE || c->builtin == B_REALLOC_SLICE || c->builtin == B_FREE_SLICE)
            return GenSlicePool(c);
        if (c->builtin == B_COPY && c->ArgNodes().size() == 1 && !cg.IsFix(c->rettypes[0])) {
            // A copy of a variable-size value on a temporary's stack: an
            // image, a resizable, a frame object.
            auto rt = c->rettypes[0];
            V x;
            if (IsImg(rt) && M.ImgOk(rt)) {
                auto stk = Stk(AllocStk(false, c->line), c->line);
                x.t = rt;
                x.mem = true;
                x.a = AtV(GenImg(c->ArgNodes()[0], stk, rt, c->line));
            } else if (IsGrow(rt) && (M.RzOk(rt) || M.VeOk(rt))) {
                x = RzTemp(rt, c->line);
                GenRz(c, x.stk, x.a, rt, c->line);
            } else if (M.FoOk(rt)) {
                x.t = rt;
                x.mem = true;
                x.a = AtL(tyg_local_new(p, M.Size(rt), 8));
                x.stk = Stk(AllocStk(false, c->line), c->line);
                GenFo(c->ArgNodes()[0], P { x.a, rt, x.stk }, c->line);
            } else {
                No(c->line, cat("copy of type ", M.NameWhy(rt)));
            }
            if (Ended()) return Dummy(c->exprtype);
            if (c->exprtype && c->exprtype->kind == TY_SLICE) {
                auto v = ViewOf(P { x.a, x.t, x.stk }, c->line);
                return SliceValue(c->exprtype, v.elems, v.len);
            }
            return x;
        }
        if (c->builtin == B_COPY && c->ArgNodes().size() == 1 && cg.IsFix(c->rettypes[0]) &&
            M.TypeOk(c->rettypes[0])) {
            // A fresh copy of a fixed value; at a slice destination, a view
            // of the copy, not of its source (CodeGen's B_COPY).
            auto rt = c->rettypes[0];
            auto x = GenAs(c->ArgNodes()[0], rt);
            if (Ended()) return Dummy(c->exprtype);
            if (M.IsScalar(rt)) return x;
            auto tmp = Temp(rt);
            tyg_copy(p, AtL(tmp), x.a, M.Size(rt));
            if (c->exprtype->kind == TY_SLICE) {
                auto v = ViewOf(P { AtL(tmp), rt }, c->line);
                return SliceValue(c->exprtype, v.elems, v.len);
            }
            return MemV(rt, AtL(tmp));
        }
        if (c->builtin == B_TO_BYTES && c->ArgNodes().size() == 2) {
            // Appended to a u8 resizable.
            auto out = Deref(Place(c->ArgNodes()[1]));
            if (TgModule::IsLim(out.t) && M.LimHeld(out.t) && IsU8(out.t->arr->sub)) {
                ToBytesIntoLim(out, c->ArgNodes()[0], c->line);
                return V {};
            }
            if (!IsGrow(out.t) || !IsU8(out.t->arr->sub) || !out.stk) No(c->line, "to_bytes into this kind");
            auto len = out.a;
            len.offset += 8;
            ToBytesInto(out.stk, len, c->ArgNodes()[0], c->line);
            return V {};
        }
        if (c->builtin == B_FROM_BYTES) {
            // Its array alone, where the flag is not wanted.
            lastrets = GenFromBytes(c);
            return lastrets[0];
        }
        if (c->builtin == B_TO_BYTES && c->ArgNodes().size() == 1) {
            auto x = RzTemp(cg.GrowU8(), c->line);
            GenRz(c, x.stk, x.a, x.t, c->line);
            return x;
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
            auto v = ViewOf(Place(a), c->line);
            if (Ended()) return Dummy(c->exprtype);
            if (!cg.IsFix(elem)) return SliceValue(c->exprtype, v.elems, Sub(RunEnd(v), v.elems));
            auto n = Mul(v.len, I64(cg.FixedSize(elem)));
            return SliceValue(c->exprtype, v.elems, n);
        }
        if (ZeroDefault(c) && M.TypeOk(c->rettypes[0])) {
            // Zero bytes (CodeGen::EmitDefaultInto).
            auto t = c->rettypes[0];
            if (M.IsScalar(t)) return Dummy(t);
            V x = MemV(t, AtL(Temp(t)));
            tyg_zero(p, x.a, M.Size(t));
            return x;
        }
        if (c->builtin == B_DEFAULT && c->rettypes.size() == 1 && cg.IsFix(c->rettypes[0]) &&
            M.TypeOk(c->rettypes[0])) {
            // A fixed value from its type's defaults (CodeGen's B_DEFAULT):
            // the default initializer, per element for a fixed array.
            auto t = c->rettypes[0];
            auto tmp = Temp(t);
            P pl { AtL(tmp), t };
            tyg_zero(p, pl.a, M.Size(t));
            if (c->defaultinit && t->kind == TY_ARRAY && t->arr->akind == A_FIXED) {
                auto et = t->arr->sub;
                Loop(I64(0), I64(cg.ArrSize(t->arr)), [&](tyg_val i) {
                    Into(P { AtV(Add(tyg_addr_of(p, pl.a), Mul(i, I64(cg.FixedSize(et))))), et }, c->defaultinit);
                });
            } else if (c->defaultinit) {
                Into(pl, c->defaultinit);
            }
            if (Ended()) return Dummy(t);
            if (M.IsScalar(t)) return Load(pl, c->line);
            return MemV(t, pl.a);
        }
        if (c->builtin == B_HARDWARE_THREADS) {
            // The runtime's count, capped by the thread limit.
            return ValV(c->exprtype, CallC("gs_hardware_threads", {}, {}, IT(TYG_I64)));
        }
        if ((c->builtin == B_EMBED_FILE || c->builtin == B_EMBED_SHADER) && c->shaderblob &&
            c->rettypes.size() == 1) {
            // A read-only view of static data (CodeGen's B_EMBED_FILE): the
            // blob's bytes in the object, the slice's data and length.
            auto &blob = *c->shaderblob;
            auto t = Temp(c->rettypes[0]);
            StL(t, tyg_addr_of(p, tyg_at_sym(M.Bytes(blob), 0)));
            StL(t, I64((int64_t)blob.size()), 8);
            return MemV(c->rettypes[0], AtL(t));
        }
        if (c->builtin == B_THREAD_WAIT) return GenThreadWait(c);
        if (c->builtin == B_THREAD_SPAWN) return GenThreadSpawn(c);
        if (c->builtin == B_QPUT) return GenQput(c);
        if (c->builtin == B_QGET || c->builtin == B_QPOLL) return GenQget(c, idst);
        if (c->builtin >= 0) No(c->line, cat("builtin ", BuiltinByKind(c->builtin).name));
        if (c->fvbody) return GenFvCall(c);
        if (!c->dispatch.empty()) return GenDispatch(c, idst, dst);
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
            if (cg.IsPoolParam(callee, i)) {
                PrefArg(an[i], args);
                continue;
            }
            if (M.RzParam(pt)) {
                ValArg(an[i], pt, args);
                continue;
            }
            args.push_back(cg.IsLargeFixed(pt) ? LargeArg(an[i], pt) : Arg(an[i], pt));
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
        auto rets = RetChans(callee, ci, idst, dst, args, c->line);
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

    // A channel per result after the declared arguments (C.3), appended
    // to args: a resizable's destination stack -- a temporary's, or dst --
    // and the count's address; a T[]'s stack -- a temporary's, or idst;
    // a temporary's address for a fixed result the C does not return.
    // Where each result will be.
    vector<V> RetChans(FnSpec *callee, CodeGen::SpecInfo &ci, tyg_val idst, const RzDst *dst,
                       vector<tyg_arg> &args, Line l) {
        vector<V> rets(callee->rets.size());
        for (size_t i = 0; i < callee->rets.size(); i++) {
            auto &r = rets[i];
            r.t = callee->rets[i];
            if (IsImg(r.t)) {
                // A T[]: built whole at the top of a temporary's stack, or
                // of the one this call builds at.
                auto stk = i == 0 && idst ? idst : Stk(AllocStk(false, l), l);
                r.mem = true;
                r.a = AtV(Top(stk));
                tyg_arg s {};
                s.val = stk;
                args.push_back(s);
                continue;
            }
            if (M.FoOk(r.t)) {
                // A frame object: its struct in a temporary, its tail on
                // a stack of the statement's.
                r.mem = true;
                r.a = AtL(tyg_local_new(p, M.Size(r.t), 8));
                r.stk = Stk(AllocStk(false, l), l);
                tyg_arg s {}, n {};
                s.val = r.stk;
                n.val = tyg_addr_of(p, r.a);
                args.push_back(s);
                args.push_back(n);
                continue;
            }
            if (IsGrow(r.t) || M.IsRs(r.t) || M.IsRb(r.t)) {
                if (i == 0 && dst) {
                    r.mem = true;
                    r.stk = dst->stk;
                    r.a = dst->hdr;
                } else {
                    r = RzTemp(r.t, l);
                }
                auto len = r.a;
                len.offset += 8;
                St(r.a, Top(r.stk));
                St(len, I64(0));
                tyg_arg s {}, n {};
                s.val = r.stk;
                n.val = tyg_addr_of(p, len);
                args.push_back(s);
                args.push_back(n);
                continue;
            }
            if ((int)i == ci.cret) continue;
            auto t = Temp(r.t);
            r.a = AtL(t);
            tyg_arg a {};
            a.val = tyg_addr_of(p, r.a);
            args.push_back(a);
            if (!M.IsScalar(r.t)) r.mem = true;
        }
        return rets;
    }

    // A pool reference argument (CodeGen::GenPrefVal): the pool reference
    // value as gs_pref's four members.
    void PrefArg(Node *n, vector<tyg_arg> &args) {
        auto at = GenPref(n);
        for (int k = 0; k < 4; k++) {
            tyg_arg a {};
            auto x = at;
            x.offset += 8 * k;
            a.val = Ended() ? I64(0) : Ld(x);
            args.push_back(a);
        }
    }

    // Whether a variable holds a gs_pref: a pool reference variable, or a
    // parameter handed a pool.
    bool IsPrefStore(const VarDef *vd) { return cg.PrefVar(vd) || (prefs.count(vd) && !vd->reusable); }
    // The pool reference a node gives (CodeGen::GenPrefVal), the address of
    // its 32 bytes: a pool's (&pool), another pool reference variable's, or
    // what a construct choosing among pools builds, each arm into one
    // temporary.
    bool prefctl = false;      // Set for the construct to be evaluated, which
    tyg_local prefloc = 0;     // builds its value into this temporary.
    bool TakePref(tyg_local &res) {
        auto was = prefctl;
        prefctl = false;
        res = prefloc;
        return was;
    }
    tyg_addr GenPref(Node *n) {
        if (cg.IsCtl(n)) {
            auto res = tyg_local_new(p, 32, 8);
            if (Is<Block>(n)) {
                BuildArm(n, n->line, [&](Node *v) { PrefInto(AtL(res), v); });
            } else if (Is<IfExpr>(n) || Is<MatchExpr>(n) || Is<EarlyBlock>(n) || Is<LoopExpr>(n)) {
                prefctl = true;
                prefloc = res;
                Gen(n);
                if (prefctl) {
                    prefctl = false;
                    No(n->line, "pool reference from this construct");
                }
            } else {
                No(n->line, "pool reference from this construct");
            }
            return AtL(res);
        }
        auto c = n;
        if (auto u = Is<Unary>(n); u && u->op == T_BITAND) c = u->child;
        auto id = Is<Ident>(c);
        auto vd = id ? id->vdef : nullptr;
        if (!vd) No(n->line, "pool reference argument of this kind");
        if (IsPrefStore(vd)) return VarPlace(vd, n->line).a;
        P pl;
        tyg_addr fl;
        tyg_val flstk;
        PoolAt(vd, n->line, pl, fl, flstk);
        if (!pl.stk) No(n->line, "pool without its stack");
        auto res = tyg_local_new(p, 32, 8);
        auto put = [&](int off, tyg_val v) { StL(res, v, off); };
        put(0, tyg_addr_of(p, pl.a));
        put(8, pl.stk);
        put(16, tyg_addr_of(p, fl));
        put(24, flstk);
        return AtL(res);
    }
    // A pool reference built into the 32 bytes at dst.
    void PrefInto(tyg_addr dst, Node *n) {
        auto src = GenPref(n);
        if (!Ended()) tyg_copy(p, dst, src, 32);
    }
    // An arm of a control construct, its value stored in the construct's
    // temporary: as the type, or as a pool reference where `pr`.
    void ArmVal(Node *n, bool valued, bool pr, TypeExpr *et, tyg_local res) {
        if (valued && pr) {
            BuildArm(n, n->line, [&](Node *v) { PrefInto(AtL(res), v); });
            return;
        }
        auto x = GenArm(n, !valued, valued ? et : nullptr);
        if (valued && !Ended() && x.t) Store(P { AtL(res), et }, Fit(x, et, n->line));
    }

    // A by-value array, image or frame object argument: a copy of its own,
    // on a stack of the statement's, as the C passes it (SigParams).
    void ValArg(Node *n, TypeExpr *pt, vector<tyg_arg> &args) {
        // A by-value resizable: a copy of its own, on a stack of the
        // statement's, as its header and its stack.
        if (M.FoOk(pt)) {
            // A frame object: a copy of its own, its tail on a
            // stack of the statement's.
            P x { AtL(tyg_local_new(p, M.Size(pt), 8)), pt,
                  Stk(AllocStk(false, n->line), n->line) };
            GenFo(n, x, n->line);
            tyg_arg h {}, k {};
            h.mem = x.a;
            k.val = x.stk;
            args.push_back(h);
            args.push_back(k);
            return;
        }
        if (!IsGrow(pt) && !M.IsRs(pt) && !M.IsRb(pt)) {
            // A T[]: an image of its own, on a stack of the
            // statement's, by its address.
            auto stk = Stk(AllocStk(false, n->line), n->line);
            tyg_arg a {};
            a.val = GenImg(n, stk, pt, n->line);
            args.push_back(a);
            return;
        }
        auto x = RzTemp(pt, n->line);
        if (M.IsRb(pt)) GenRb(n, x.stk, x.a, pt, n->line);
        else GenRz(n, x.stk, x.a, pt, n->line);
        tyg_arg h {}, k {};
        h.mem = x.a;
        k.val = x.stk;
        args.push_back(h);
        args.push_back(k);
    }

    void FvArgs(Call *c, CodeGen::SpecInfo &ci, vector<tyg_arg> &args) {
        for (auto fv : ci.freevars) {
            auto v = c->FreeVarArg(fv);
            if (fv->reusable) {
                // A pool, as gs_pref's members (CodeGen::SigParams).
                P pl;
                tyg_addr fl;
                tyg_val flstk;
                PoolAt(const_cast<VarDef *>(v), c->line, pl, fl, flstk);
                if (!pl.stk) No(c->line, "pool without its stack");
                for (auto x : { tyg_addr_of(p, pl.a), pl.stk, tyg_addr_of(p, fl), flstk }) {
                    tyg_arg a {};
                    a.val = x;
                    args.push_back(a);
                }
                continue;
            }
            auto pl = VarPlace(v, c->line);
            tyg_arg a {};
            a.val = tyg_addr_of(p, pl.a);
            args.push_back(a);
            if (M.Resz(v->type)) {
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
        if (wantsval && !M.TypeOk(et)) {
            // An image, a resizable: built at the top of a temporary's stack.
            V x;
            if (InPlaceValue(c, et, c->line, x)) return x;
            No(c->line, cat("function value of type ", M.Name(et)));
        }
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
            P pl { AtL(t), x.t };
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
    // The element the last push added, where it lies: what print renders
    // of a push's value (a reference, §3.3), read there.
    std::optional<P> pushed;
    bool discardpush = false;   // A push in statement position: its value unwanted.
    V GenPush(Call *c) {
        auto dp = discardpush;
        discardpush = false;
        auto an = c->ArgNodes();
        auto pl = Deref(Place(an[0]));
        if (TgModule::IsLim(pl.t)) {
            discardpush = dp;
            return LimPush(c, pl);
        }
        if (IsGrow(pl.t) && M.VeOk(pl.t) && pl.stk) {
            // An image element: built straight at the top of the array's own
            // stack, behind the elements before it, then counted.
            auto la = pl.a;
            la.offset += 8;
            auto at = GenImg(an[1], pl.stk, pl.t->arr->sub, c->line);
            if (Ended()) return Dummy(c->exprtype);
            St(la, Add(Ld(la), I64(1)));
            auto et = c->exprtype;
            if (et && et->kind != TY_REF && !cg.IsVoidT(et) && M.IsScalar(et))
                return Fit(Load(ElemPlace(at, pl.t->arr->sub), c->line), et, c->line);   // A varint, decoded.
            V r;
            r.t = et && et->kind == TY_REF ? et : nullptr;
            if (r.t) r.v = at;
            return r;
        }
        if (!IsGrow(pl.t) || !M.RzOk(pl.t) || !pl.stk) No(c->line, "push to an array of this kind");
        auto et = pl.t->arr->sub;
        if ((SelfRelLit(an[1]) || (cg.HasRelRef(et) && SelfRelLitOk(an[1]))) && cg.TEq(an[1]->exprtype, et)) {
            // Built in its slot at the top, then counted.
            auto slot = Top(pl.stk);
            auto sv = Local(slot);
            SelfRelInto(P { AtV(slot), et }, an[1]);
            if (Ended()) return Dummy(c->exprtype);
            auto at = LdL(sv);
            SetTop(pl.stk, Add(at, I64(cg.FixedSize(et))));
            auto la = pl.a;
            la.offset += 8;
            St(la, Add(Ld(la), I64(1)));
            auto et = c->exprtype;
            if (et && et->kind != TY_REF && !cg.IsVoidT(et) && M.IsScalar(et))
                return Fit(Load(ElemPlace(at, pl.t->arr->sub), c->line), et, c->line);   // A varint, decoded.
            V r;
            r.t = et && et->kind == TY_REF ? et : nullptr;
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
        // The element itself, as a value, where one is wanted.
        pushed = P { AtV(top), et };
        if (!r.t && !dp && c->exprtype && c->exprtype->kind != TY_VOID && M.TypeOk(et))
            return Load(*pushed, c->line);
        return r;
    }

    // v.append(x) (EmitAppend): a call's resizable result built straight
    // at v's top, its count added; anything else holding elements (an
    // array, a slice, a string literal, copy(x)) copied there.
    V GenAppend(Call *c) {
        auto an = c->ArgNodes();
        auto pl = Deref(Place(an[0]));
        if (TgModule::IsLim(pl.t)) return LimAppend(c, pl);
        if (IsGrow(pl.t) && M.VeOk(pl.t) && pl.stk) {
            // Elements of variable size: the source's run of bytes copied
            // whole behind the last, its count added.
            auto src = an[1];
            if (auto cc = Is<Call>(src); cc && cc->builtin == B_COPY && cc->ArgNodes().size() == 1)
                src = cc->ArgNodes()[0];
            auto v = ViewOf(LimCallResult(src) ? LimCallPlace(src) : Place(src), c->line);
            if (Ended()) return V {};
            if (cg.Mangle(v.elem) != cg.Mangle(pl.t->arr->sub)) No(c->line, "append of another element type");
            auto bytes = Sub(RunEnd(v), v.elems);
            auto n = v.len;
            auto top = Top(pl.stk);
            tyg_move_dyn(p, top, v.elems, bytes);
            SetTop(pl.stk, Add(top, bytes));
            auto len = pl.a;
            len.offset += 8;
            St(len, Add(Ld(len), n));
            return V {};
        }
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
        // A function value's call builds it in place as well (GenRz).
        auto fvcall = Is<Call>(src) && Is<Call>(src)->fvbody && Is<Call>(src)->fvbody->tail;
        if (Is<Call>(src) && st && IsGrow(st) && Is<Call>(src)->builtin < 0 && !DestSpec(Is<Call>(src)) && !fvcall)
            No(l, "append of this call");
        if (auto cc = Is<Call>(src); cc && cc->builtin < 0 && st && IsGrow(st)) {
            auto tmp = tyg_local_new(p, 16, 8);
            GenRz(src, pl.stk, AtL(tmp), st, l);
            if (Ended()) return;
            St(len, Add(Ld(len), LdL(tmp, 8)));
            return;
        }
        if (auto s = Is<StrLit>(src)) {
            if (!IsU8(et)) No(l, "string of this type");
            CopyRun(pl.stk, len, et, tyg_addr_of(p, tyg_at_sym(M.Bytes(s->val), 0)),
                    I64((int64_t)s->val.size()));
            return;
        }
        if (auto al = Is<ArrayLit>(src); al && !al->capexpr && cg.HasRelRef(et) && al->exprtype &&
                                         al->exprtype->kind == TY_ARRAY && cg.TEq(al->exprtype->arr->sub, et) &&
                                         (al->fillval ? Is<IntLit>(al->fillcount) != nullptr : true)) {
            // A literal of elements holding relative references builds its
            // run where the elements stay, at the receiver's top (§4.3).
            auto esz = cg.FixedSize(et);
            auto k = LitCount(al);
            auto base = tyg_local_new(p, 8, 8);
            auto leaves = FillCapture(al);
            if (Ended()) return;
            StL(base, Top(pl.stk));
            LitRunAt(al, et, base, k, leaves);
            if (Ended()) return;
            SetTop(pl.stk, Add(LdL(base), I64(k * esz)));
            St(len, Add(Ld(len), I64(k)));
            return;
        }
        // A literal is a fixed array, all of it evaluated before any of it
        // is copied, as in the C.
        auto from = Place(src);
        if (Ended()) return;
        auto v = ViewOf(from, l);
        if (cg.Mangle(v.elem) != cg.Mangle(et)) No(l, "append of another element type");
        CopyView(pl.stk, len, et, v);
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
    TypeExpr *inlinet = nullptr;   // The type a render procedure's body renders inline.

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
        auto hdr = len;
        hdr.offset -= 8;
        auto r = tyg_local_new(p, 16, 8);
        StL(r, tyg_addr_of(p, hdr));
        StL(r, stk, 8);
        vector<tyg_arg> args;
        tyg_arg out {};
        out.mem = AtL(r);
        args.push_back(out);
        tyg_arg v {};
        if (pl.enc || TgModule::IsVarint(pl.t)) {
            // An i64 read out of varint storage, which holds no i64: the
            // overload is handed a temporary holding its value (RefArg).
            auto i64t = M.ast.inttypes[IS_I64];
            auto tmp = Temp(i64t);
            StL(tmp, Load(pl, l).v);
            pl = P { AtL(tmp), i64t };
        }
        if (pt->kind == TY_REF && cg.IsFatRef(pt)) {
            // A resizable by reference: gs_rref, its header's address and
            // its stack.
            if (!pl.stk) No(l, "format overload taking a resizable without its stack");
            auto fr = tyg_local_new(p, 16, 8);
            StL(fr, tyg_addr_of(p, pl.a));
            StL(fr, pl.stk, 8);
            v.mem = AtL(fr);
        } else if (pt->kind == TY_REF) {
            v.val = tyg_addr_of(p, pl.a);
        } else if (M.IsScalar(pt)) {
            v.val = Load(P { pl.a, pt }, l).v;
        } else if (M.RzParam(pt)) {
            No(l, "format overload taking a variable-size value");
        } else if (cg.IsLargeFixed(pt)) {
            // The address of a copy of its own (SigParams).
            auto cp = Temp(pt);
            tyg_copy(p, AtL(cp), pl.a, M.Size(pt));
            v.val = tyg_addr_of(p, AtL(cp));
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

    // Where an argument of print, str or format is rendered from: a call's
    // reference result where it points, as a reference variable is, not as
    // the value the checker decayed it to; anything else its place.
    P RenderPlace(Node *a) {
        auto fc = Is<Call>(a);
        if (fc && fc->builtin == B_PUSH) {
            pushed.reset();
            auto x = Gen1(a);
            if (Ended()) return P {};
            if (pushed) return *pushed;
            if (x.mem) return P { x.a, x.t, x.stk };
            if (!x.t || x.t->kind == TY_VOID) No(a->line, "place of no value");
            auto tmp = Temp(x.t);
            P pl { AtL(tmp), x.t };
            Store(pl, x);
            return pl;
        }
        auto rs = !fc ? nullptr : fc->spec ? fc->spec : !fc->dispatch.empty() ? fc->dispatch[0] : nullptr;
        if (fc && fc->builtin < 0 && rs && !rs->rets.empty() && rs->rets[0]->kind == TY_REF &&
            IsPlainRef(rs->rets[0])) {
            auto x = Gen1(a);
            if (Ended()) return P {};
            return RefTarget(x);
        }
        return Place(a);
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
                    StL(tmp, x.v);
                    UserFormat(stk, len, P { AtL(tmp), i64t }, sp, a->line);
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
                auto pl = RenderPlace(a);
                if (Ended()) return;
                if (IsAddrRef(pl.t)) {
                    // A reference to a varint renders what it refers to.
                    auto d = Deref(pl);
                    if (d.enc) pl = d;
                }
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
                            I64((int64_t)s->val.size()));
                    continue;
                }
                auto v = ViewOf(Place(a), a->line);
                if (Ended()) return;
                CopyRun(stk, len, v.elem, v.elems, v.len);
                continue;
            }
            auto ot = cg.OperandT(a->exprtype);
            if (!ot || !M.IsScalar(ot) || ot->kind == TY_REF) {
                auto pl = RenderPlace(a);
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
        SetTop(stk, Add(Top(stk), n));
        St(len, Add(Ld(len), n));
    }
    // Literal text appended as FmtScalar appends.
    void TextLit(tyg_val stk, tyg_addr len, const string &text) {
        CopyRun(stk, len, M.ast.inttypes[IS_U8], tyg_addr_of(p, tyg_at_sym(M.Bytes(text), 0)),
                I64((int64_t)text.size()));
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
        // A reference renders what it refers to, a relative one decoded
        // (CodeGen::RenderInto).
        while (IsPlainRef(pl.t) || (TgModule::IsRel(pl.t) && !IsOptional(pl.t) && M.RelOk(pl.t, 1))) pl = Step(pl);
        auto t = pl.t;
        if (fmtc) {
            if (auto sp = cg.FmtSpecFor(fmtc, t)) {
                UserFormat(stk, len, pl, sp, l);
                return;
            }
        }
        if (M.OptRef(t) || (IsOptional(t) && M.RelOk(t))) {
            RenderOpt(stk, len, pl, nested, l, depth);
            return;
        }
        // A struct that reaches itself is rendered by a procedure of its
        // own, which calls itself for the next level (CodeGen::EmitRenderCall).
        // It runs no overloads: the checker leaves none in such a type
        // (CheckPrintable's OverloadedPart). The procedure's own body
        // renders the first level here (inlinet).
        if ((t->kind == TY_STRUCT || t->kind == TY_ENUM) && M.Recursive(t)) {
            if (inlinet && cg.TEq(inlinet, t)) {
                inlinet = nullptr;
            } else {
                if ((pl.stk && !M.FoOk(t) && !M.RsOk(t)) || pl.enc || !M.RenderFnOk(t))
                    No(l, cat("text of type ", M.Name(t), " [recursive]"));
                auto ptr = tyg_type_ptr(m);
                // A frame object that is bytes is read as the C one first.
                if (pl.bfo) pl = FoView(pl);
                CallC(M.RenderFn(t), { ptr, ptr, ptr }, { stk, tyg_addr_of(p, len), tyg_addr_of(p, pl.a) }, nullptr);
                return;
            }
        }
        if (M.IsRb(t)) {
            if (!M.RbOk(t) || pl.enc) No(l, cat("text of type ", M.NameWhy(t)));
            RenderRb(stk, len, pl, l, depth);
            return;
        }
        switch (t->kind) {
            case TY_VARIANT:
                RenderVariant(stk, len, pl, l, depth);
                return;
            case TY_ENUM: {
                // The variant the tag says, as its literal (CodeGen::RenderLoc).
                if (!M.TypeOk(t) && !M.VmOk(t)) break;
                auto ei = cg.EIOf(t);
                auto varmode = M.IsVm(t);
                auto tag = Local(LoadTag(pl.a, t));
                auto done = NewBlock();
                for (size_t vi = 0; vi < ei->en->variants.size(); vi++) {
                    auto hit = NewBlock(), next = NewBlock();
                    Branch(Cmp(TYG_EQ, LdL(tag), I64((int64_t)vi)), hit, next);
                    SetBlock(hit);
                    auto vt = cg.VariantType(t, (int)vi);
                    auto pa = pl.a;
                    pa.offset += M.TagSize(t);
                    P vp { pa, vt };
                    if (!varmode && fmtc && !fmtc->fmtspecs.empty() && cg.FixedSize(vt)) {
                        // An overload may overwrite the value: its parts
                        // rendered from a copy.
                        auto cp = Temp(vt);
                        tyg_copy(p, AtL(cp), pa, cg.FixedSize(vt));
                        vp.a = AtL(cp);
                    }
                    RenderVariant(stk, len, vp, l, depth);
                    tyg_jump(p, done);
                    SetBlock(next);
                }
                tyg_jump(p, done);
                SetBlock(done);
                return;
            }
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
                if (M.RsOk(t)) {
                    auto si = cg.SI(t);
                    TextLit(stk, len, cat(TextName(t), " { "));
                    auto first = true;
                    for (size_t i = 0; i < si->st->fields.size(); i++) {
                        if (si->st->fields[i].ispad) continue;
                        if (!first) TextLit(stk, len, ", ");
                        first = false;
                        RenderInto(stk, len, RsFieldPlace(pl, (int)i, l, true), true, l, depth + 1);
                    }
                    TextLit(stk, len, " }");
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
                        RenderInto(stk, len, FoFieldPlace(pl, (int)i, l, true), true, l, depth + 1);
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
                    SetTop(stk, Add(Top(stk), n));
                    St(len, Add(Ld(len), n));
                    return;
                }
                TextLit(stk, len, "[");
                auto cl = Local(v.elems);   // The element's address, walked on from the first.
                Loop(I64(0), v.len, [&](tyg_val i) {
                    If(Cmp(TYG_NE, i, I64(0)), [&] { TextLit(stk, len, ", "); });
                    RenderInto(stk, len, ElemPlace(LdL(cl), v.elem), true, l, depth + 1);
                    StL(cl, StepElem(v, LdL(cl)));
                });
                TextLit(stk, len, "]");
                return;
            }
            default: break;
        }
        No(l, cat("text of type ", M.NameWhy(t)));
    }

    // A resizable ADT, or one of its variants, at the header pl: the
    // variant the tag says, read once, as its literal; each field where the
    // walk finds it, the header's base and count read as its rendering
    // begins (CodeGen::RenderLoc).
    void RenderRb(tyg_val stk, tyg_addr len, const P &pl, Line l, int depth) {
        auto t = pl.t;
        if (t->kind == TY_STRUCT) {
            auto si = cg.SI(t);
            TextLit(stk, len, cat(TextName(t), " { "));
            auto first = true;
            for (size_t i = 0; i < si->st->fields.size(); i++) {
                if (si->st->fields[i].ispad) continue;
                if (!first) TextLit(stk, len, ", ");
                first = false;
                RenderInto(stk, len, RbStFieldPlace(pl, (int)i), true, l, depth + 1);
            }
            TextLit(stk, len, " }");
            return;
        }
        auto ei = M.EI(t);
        auto hl = pl.a;
        hl.offset += 8;
        auto variant = [&](int vi, int64_t pay) {
            auto &v = ei->en->variants[(size_t)vi];
            if (v.fields.empty()) {
                TextLit(stk, len, cat(ei->en->name, ".", v.name));
                return;
            }
            auto sh = M.RbShapeOf(ei, vi);
            if (sh.img) {
                // A variable-size variant: where it lies, its fields walked.
                RenderVariant(stk, len, P { AtV(Add(Ld(pl.a), I64(pay))),
                                            t->kind == TY_VARIANT ? t : cg.VariantType(t, vi) }, l, depth);
                return;
            }
            TextLit(stk, len, cat(v.name, " { "));
            auto first = true;
            for (size_t i = 0; i < v.fields.size(); i++) {
                if (v.fields[i].ispad) continue;
                if (!first) TextLit(stk, len, ", ");
                first = false;
                auto ft = ei->vftypes[(size_t)vi][i];
                auto at = Add(Ld(pl.a), I64(pay + sh.offs[i]));
                if ((int)i == sh.tailfi && sh.fo) {
                    P fp { AtV(at), ft };
                    fp.bfo = true;
                    fp.ohdr = pl.a;
                    RenderInto(stk, len, fp, true, l, depth + 1);
                } else if ((int)i == sh.tailfi) {
                    auto th = tyg_local_new(p, 16, 8);
                    StL(th, at);
                    StL(th, Ld(hl), 8);
                    RenderInto(stk, len, P { AtL(th), ft }, true, l, depth + 1);
                } else {
                    RenderInto(stk, len, P { AtV(at), ft }, true, l, depth + 1);
                }
            }
            TextLit(stk, len, " }");
        };
        if (t->kind == TY_VARIANT) {
            variant(M.VariantIdx(t), 0);
            return;
        }
        auto tag = Local(LoadTag(AtV(Ld(pl.a)), t));
        auto done = NewBlock();
        for (size_t vi = 0; vi < ei->en->variants.size(); vi++) {
            auto hit = NewBlock(), next = NewBlock();
            Branch(Cmp(TYG_EQ, LdL(tag), I64((int64_t)vi)), hit, next);
            SetBlock(hit);
            variant((int)vi, M.TagSize(t));
            tyg_jump(p, done);
            SetBlock(next);
        }
        tyg_jump(p, done);
        SetBlock(done);
    }

    // A variant's positional literal (CodeGen::RenderVariant): `E.V` for
    // one without fields, else `V { a, b }`.
    void RenderVariant(tyg_val stk, tyg_addr len, const P &pl, Line l, int depth) {
        auto t = pl.t;
        auto ei = cg.EIVar(t);
        auto vi = (size_t)ei->en->VariantIndex(t->var->variant);
        auto &v = ei->en->variants[vi];
        if (v.fields.empty()) {
            TextLit(stk, len, cat(ei->en->name, ".", v.name));
            return;
        }
        auto fixed = cg.IsFix(t);
        if (fixed && !M.TypeOk(t)) No(l, cat("text of type ", M.NameWhy(t)));
        if (!fixed && !M.VvOk(t)) No(l, cat("text of type ", M.NameWhy(t)));
        TextLit(stk, len, cat(v.name, " { "));
        auto first = true;
        for (size_t i = 0; i < v.fields.size(); i++) {
            if (v.fields[i].ispad) continue;
            if (!first) TextLit(stk, len, ", ");
            first = false;
            auto ft = ei->vftypes[vi][i];
            P fp;
            if (fixed) {
                fp = VariantField(pl, (int)i);
            } else {
                fp = P { AtV(VvField(t, tyg_addr_of(p, pl.a), (int)i)), ft };
                fp.enc = TgModule::IsVarint(ft);
            }
            RenderInto(stk, len, fp, true, l, depth + 1);
        }
        TextLit(stk, len, " }");
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
        auto ok = NewBlock(), bad = NewBlock();
        Branch(cond, ok, bad);
        SetBlock(bad);
        Abort(what, l);
        SetBlock(ok);
    }

    // pop, pop_n, resize and clear of a resizable (codegen_builtins.h): the
    // receiver, then the arguments, then the checks; the elements end at
    // the stack's top, which moves with the count.
    V GenShrink(Call *c) {
        auto an = c->ArgNodes();
        auto pl = Deref(Place(an[0]));
        if (TgModule::IsLim(pl.t)) return LimShrink(c, pl);
        if (IsGrow(pl.t) && M.VeOk(pl.t) && pl.stk && c->builtin == B_CLEAR) {
            // Elements of variable size: none, their bytes given back.
            auto len = pl.a;
            len.offset += 8;
            St(len, I64(0));
            SetTop(pl.stk, BaseOf(pl));
            return V {};
        }
        if (!IsGrow(pl.t) || !M.RzOk(pl.t) || !pl.stk)
            No(c->line, cat(BuiltinByKind(c->builtin).name, " of an array of this kind"));
        auto et = pl.t->arr->sub;
        auto esz = cg.FixedSize(et);
        auto len = pl.a;
        len.offset += 8;
        auto setlen = [&](tyg_val n) { St(len, n); };
        auto at = [&](tyg_val n) {   // The address of element n.
            return Add(BaseOf(pl), Mul(n, I64(esz)));
        };
        switch (c->builtin) {
            case B_POP: {
                auto nl = Sub(Ld(len), I64(1));
                AbortUnless(Cmp(TYG_SGE, nl, I64(0)), "POP", c->line);
                setlen(nl);
                auto top = Sub(Top(pl.stk), I64(esz));
                SetTop(pl.stk, top);
                return Load(P { AtV(top), et }, c->line);
            }
            case B_POP_N: {
                auto nn = Index64(an[1]);
                if (Ended()) return V {};
                auto ol = Ld(len);
                AbortUnless(Cmp(TYG_ULE, nn, ol), "POPN", c->line);
                setlen(Sub(ol, nn));
                SetTop(pl.stk, Sub(Top(pl.stk), Mul(nn, I64(esz))));
                return V {};
            }
            case B_CLEAR:
                SetTop(pl.stk, BaseOf(pl));
                setlen(I64(0));
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
        auto nl = Local(nn);
        auto n = [&] { return LdL(nl); };
        auto ol = Local(Ld(len));
        auto o = [&] { return LdL(ol); };
        AbortUnless(Cmp(TYG_SGE, n(), I64(0)), "RESIZENEG", c->line);
        auto shrink = NewBlock(), notshrink = NewBlock(), grow = NewBlock(),
             done = NewBlock();
        Branch(Cmp(TYG_SLT, n(), o()), shrink, notshrink);
        SetBlock(shrink);
        setlen(n());
        SetTop(pl.stk, at(n()));
        tyg_jump(p, done);
        SetBlock(notshrink);
        Branch(Cmp(TYG_SGT, n(), o()), grow, done);
        SetBlock(grow);
        if (an.size() <= 2) {
            Abort("RESIZEFILL", c->line);
        } else {
            // The new slots through a cursor, the top written once after.
            auto top = Top(pl.stk);
            if (esz) {
                auto cnt = tyg_bin(p, TYG_SDIV, Sub(at(n()), top), I64(esz));
                Loop(I64(0), cnt, [&](tyg_val k) { Store(P { AtV(Add(top, Mul(k, I64(esz)))), et }, fv); });
            }
            SetTop(pl.stk, at(n()));
            setlen(n());
            tyg_jump(p, done);
        }
        SetBlock(done);
        return V {};
    }

    // str(a, b, ...): a fresh u8[>..] on a stack of the statement's (or
    // where GenRz builds it); format(out, a, b, ...): appended to out.
    V GenStr(Call *c) {
        auto t = c->exprtype;
        auto slice = t && t->kind == TY_SLICE && IsU8(t->sub);
        if (t && M.RcOk(t) && IsU8(t->arr->sub)) {
            // Typed as a runtime-capacity u8[..]: its image, the text
            // its elements (GenRc).
            auto stk = Stk(AllocStk(false, c->line), c->line);
            auto at = GenImg(c, stk, t, c->line);
            return MemV(t, AtV(at));
        }
        if (t && TgModule::IsLim(t) && M.TypeOk(t) && IsU8(t->arr->sub)) {
            // Typed as a static-capacity u8[..k]: the text rendered as a
            // resizable's, then copied under the capacity check.
            auto r = RzTemp(cg.GrowU8(), c->line);
            GenRz(c, r.stk, r.a, r.t, c->line);
            if (Ended()) return Dummy(t);
            return LimFrom(r, t, c->line);
        }
        if (!slice && (!t || !IsGrow(t) || !IsU8(t->arr->sub))) No(c->line, cat("str of type ", t ? M.Name(t) : "?"));
        auto x = RzTemp(slice ? cg.GrowU8() : t, c->line);
        GenRz(c, x.stk, x.a, x.t, c->line);
        if (!slice || Ended()) return x;
        // Where the checker typed it as the slice it is passed as: a view.
        auto len = x.a;
        len.offset += 8;
        return SliceValue(t, Ld(x.a), Ld(len));
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
            St(b.a, Top(b.stk));
            St(blen, I64(0));
            TextInto(b.stk, blen, c, an, 1);
            if (Ended()) return V {};
            auto n = Ld(blen);
            auto ol = LimLen(pl);
            AbortUnless(Cmp(TYG_SLE, Add(ol, n), LimCapOf(pl)), "CAPACITY", c->line);
            auto v = LimView(pl);
            tyg_copy_dyn(p, Add(v.elems, LimLen(pl)), Ld(b.a), n);
            LimSetLen(pl, Add(LimLen(pl), n));
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
    // gs_uleb_check: the ULEB128 at `at`, bounded by `end`, shortest and
    // within 64 bits. Its byte count into k, 0 where it is none, and its
    // value into v.
    struct Uleb {
        tyg_local k, v;
    };
    Uleb UlebCheck(tyg_val at, tyg_val end) {
        Uleb r { tyg_local_new(p, 8, 8), tyg_local_new(p, 8, 8) };
        auto sh = tyg_local_new(p, 8, 8), q = tyg_local_new(p, 8, 8);
        StL(r.v, I64(0));
        StL(sh, I64(0));
        StL(q, at);
        auto head = NewBlock(), rd = NewBlock(), top = NewBlock(), chk = NewBlock(),
             body = NewBlock(), more = NewBlock(), last = NewBlock(), chk2 = NewBlock(),
             ok = NewBlock(), fail = NewBlock(), done = NewBlock();
        tyg_jump(p, head);
        SetBlock(head);
        Branch(Cmp(TYG_ULT, LdL(q), end), rd, fail);
        SetBlock(rd);
        auto qa = LdL(q);
        auto b = tyg_convert(p, TYG_ZEXT, TYG_I64, tyg_load(p, TYG_I8, AtV(qa), 0));
        auto shv = LdL(sh);
        StL(q, Add(qa, I64(1)));
        Branch(Cmp(TYG_SGT, shv, I64(63)), fail, top);
        SetBlock(top);
        Branch(Cmp(TYG_EQ, shv, I64(63)), chk, body);
        SetBlock(chk);
        Branch(Cmp(TYG_NE, tyg_bin(p, TYG_AND, b, I64(0x7e)), I64(0)), fail, body);
        SetBlock(body);
        StL(r.v, tyg_bin(p, TYG_OR, LdL(r.v), tyg_bin(p, TYG_SHL, tyg_bin(p, TYG_AND, b, I64(0x7f)), shv)));
        Branch(Cmp(TYG_NE, tyg_bin(p, TYG_AND, b, I64(0x80)), I64(0)), more, last);
        SetBlock(more);
        StL(sh, Add(shv, I64(7)));
        tyg_jump(p, head);
        SetBlock(last);
        // A redundant high zero group is no shortest form.
        Branch(Cmp(TYG_NE, shv, I64(0)), chk2, ok);
        SetBlock(chk2);
        Branch(Cmp(TYG_EQ, b, I64(0)), fail, ok);
        SetBlock(ok);
        StL(r.k, Sub(LdL(q), at));
        tyg_jump(p, done);
        SetBlock(fail);
        StL(r.k, I64(0));
        tyg_jump(p, done);
        SetBlock(done);
        return r;
    }
    // from_bytes<T[>..]>(bytes) (CodeGen::EmitFromBytes): the framing
    // prefix has to count exactly the rest of the slice; then the payload
    // is verified as an image of elements (plain fixed ones by their size
    // alone, the others by the element's verifier procedure,
    // TgModule::VerifyFn) and copied as the array's elements. The array
    // result, and whether the bytes were one.
    vector<V> GenFromBytes(Call *c) {
        auto t = c->rettypes.empty() ? nullptr : c->rettypes[0];
        // A T[] image result (a statement's own stack) as well as a resizable.
        auto isimg = t && IsVarArr(t) && M.RzOk(t);
        if (!t || !((IsGrow(t) && (M.RzOk(t) || M.VeOk(t))) || isimg)) No(c->line, "from_bytes of this type");
        auto et = t->arr->sub;
        auto plain = cg.IsFix(et) && !cg.NeedsVerifyWalk(et) && cg.FixedSize(et) > 0;
        auto esz = plain ? cg.FixedSize(et) : 0;
        auto an = c->ArgNodes();
        P src0;
        if (Is<Call>(an[0]) && an[0]->exprtype && an[0]->exprtype->kind == TY_SLICE) {
            // A call's array result handed as the slice: viewed where it lies.
            auto x = Gen1(an[0]);
            if (!Ended() && (!x.mem || !x.t || x.t->kind != TY_ARRAY)) No(c->line, "from_bytes of this source");
            src0 = P { x.a, x.t, x.stk };
        } else {
            src0 = Place(an[0]);
        }
        if (Ended()) return { Dummy(t), Dummy(M.ast.booltype) };
        auto v = ViewOf(src0, c->line);
        if (!IsU8(v.elem)) No(c->line, "from_bytes of this source");
        V r;
        if (isimg) {
            r.t = t;
            r.mem = true;
            r.stk = Stk(AllocStk(false, c->line), c->line);
        } else {
            r = RzTemp(t, c->line);
        }
        auto rlen = r.a;
        rlen.offset += 8;
        auto src = tyg_local_new(p, 8, 8), len = tyg_local_new(p, 8, 8), pay = tyg_local_new(p, 8, 8),
             pn = tyg_local_new(p, 8, 8), okl = tyg_local_new(p, 8, 8), cnt = tyg_local_new(p, 8, 8);
        StL(src, v.elems);
        StL(len, v.len);
        StL(pay, LdL(src));
        StL(pn, I64(0));
        StL(okl, I64(0));
        StL(cnt, I64(0));
        auto uc = UlebCheck(LdL(src), Add(LdL(src), LdL(len)));
        auto c1 = NewBlock(), c2 = NewBlock(), good = NewBlock(), whole = NewBlock(),
             framed = NewBlock();
        Branch(Cmp(TYG_NE, LdL(uc.k), I64(0)), c1, framed);
        SetBlock(c1);
        Branch(Cmp(TYG_SGE, LdL(uc.v), I64(0)), c2, framed);
        SetBlock(c2);
        Branch(Cmp(TYG_EQ, LdL(uc.v), Sub(LdL(len), LdL(uc.k))), good, framed);
        SetBlock(good);
        StL(pay, Add(LdL(src), LdL(uc.k)));
        StL(pn, LdL(uc.v));
        if (plain) {
            // Whole elements: their count, else not an image.
            Branch(Cmp(TYG_EQ, tyg_bin(p, TYG_SREM, LdL(pn), I64(esz)), I64(0)), whole, framed);
            SetBlock(whole);
            StL(okl, I64(1));
            StL(cnt, tyg_bin(p, TYG_SDIV, LdL(pn), I64(esz)));
            tyg_jump(p, framed);
        } else {
            StL(okl, I64(1));
            tyg_jump(p, whole);
            SetBlock(whole);
            tyg_jump(p, framed);
        }
        SetBlock(framed);
        if (!plain) {
            // The verifier. An image with relative links to variable
            // elements gets a bitmap of element starts, on a stack of its
            // own; one needing more of that stack than is reserved (GS_BM_MAX)
            // cannot be verified and is rejected.
            tyg_val bm = I64(0);
            if (!cg.IsFix(et) && cg.HasRelRefAny(et)) {
                auto bstk = Stk(AllocStk(false, c->line), c->line);
                bm = Top(bstk);
                auto bn = tyg_local_new(p, 8, 8);
                StL(bn, tyg_bin(p, TYG_SDIV, Add(LdL(pn), I64(7)), I64(8)));
                If(Cmp(TYG_SGT, LdL(bn), I64(2048ll << 20)), [&] {
                    StL(bn, I64(0));
                    StL(okl, I64(0));
                });
                tyg_set_dyn(p, bm, I64(0), LdL(bn));
                SetTop(bstk, Add(bm, LdL(bn)));
            }
            If(Cmp(TYG_NE, LdL(okl), I64(0)), [&] {
                auto i64t = IT(TYG_I64);
                auto n = CallC(M.VerifyFn(et), { i64t, i64t, i64t }, { LdL(pay), LdL(pn), bm }, i64t);
                StL(cnt, n);
                StL(okl, tyg_convert(p, TYG_ZEXT, TYG_I64, Cmp(TYG_SGE, n, I64(0))));
            });
        }
        if (isimg) {
            // The prefix reserved, the payload behind it, the count patched
            // in (CodeGen::OpenRzDest, EmitPrefixPatch).
            auto start = tyg_local_new(p, 8, 8);
            StL(start, Top(r.stk));
            SetTop(r.stk, Add(LdL(start), I64(TgModule::PrefixBytes(t))));
            auto copy = NewBlock(), out = NewBlock();
            Branch(Cmp(TYG_NE, LdL(okl), I64(0)), copy, out);
            SetBlock(copy);
            auto top = Top(r.stk);
            tyg_move_dyn(p, top, LdL(pay), LdL(pn));
            SetTop(r.stk, Add(top, LdL(pn)));
            tyg_jump(p, out);
            SetBlock(out);
            // The count: the verifier's, or none for a rejected image.
            auto zero = NewBlock(), keep = NewBlock();
            Branch(Cmp(TYG_NE, LdL(okl), I64(0)), keep, zero);
            SetBlock(zero);
            StL(cnt, I64(0));
            tyg_jump(p, keep);
            SetBlock(keep);
            PatchPrefix(LdL(start), t, LdL(cnt), r.stk);
            r.a = AtV(LdL(start));
            V ok = ValV(M.ast.booltype, tyg_convert(p, TYG_TRUNC, TYG_I8, LdL(okl)));
            return { r, ok };
        }
        St(r.a, Top(r.stk));
        St(rlen, I64(0));
        If(Cmp(TYG_NE, LdL(okl), I64(0)), [&] {
            auto top = Top(r.stk);
            tyg_move_dyn(p, top, LdL(pay), LdL(pn));
            SetTop(r.stk, Add(top, LdL(pn)));
            St(rlen, LdL(cnt));
        });
        V ok = ValV(M.ast.booltype, tyg_convert(p, TYG_TRUNC, TYG_I8, LdL(okl)));
        return { r, ok };
    }
    vector<V> GenCallAll(Node *n, size_t count) {
        auto c = Is<Call>(n);
        if (c && c->builtin == B_FROM_BYTES && count == 2) return GenFromBytes(c);
        if (c && c->builtin == B_QPOLL && count == 2) {
            GenCall(c);
            if (lastrets.size() != count) No(n->line, "result count");
            return lastrets;
        }
        if (!DestSpec(c)) No(n->line, "multiple results of this kind");
        GenCall(c);
        if (lastrets.size() != count) No(n->line, "result count");
        return lastrets;
    }

    // ---- Threads and queues (§11.2) ------------------------------------------

    // The address of the queue for elements of type t.
    tyg_val QueueAddr(TypeExpr *t) {
        cg.usesthreads = true;
        return CallC(M.QueueAccessor(t), {}, {}, tyg_type_ptr(m));
    }
    // Whether a value of type t crosses a queue as this backend holds it: a
    // fixed value or a T[] / T[..] image (CodeGen's IsBytesT), by its bytes.
    bool QueueOk(TypeExpr *t) {
        if (!t || t->kind == TY_REF) return false;
        if (cg.IsFix(t)) return M.TypeOk(t);
        // A resizable, or a frame object, crosses as a flat image (RzImage).
        if (M.Resz(t)) return (IsGrow(t) && (M.RzOk(t) || M.VeOk(t))) || M.FoOk(t);
        return (M.IsRc(t) && M.RcOk(t)) || (IsVarArr(t) && M.RzOk(t)) || (M.IsVs(t) && M.VsOk(t));
    }
    // The bytes of one freelist unit of pool d (CodeGen::FlEntrySize,
    // GS_SPAN_NODE of a slice pool's tree).
    static int64_t FlEntry(const VarDef *d) { return d->reusable == RU_SLICES ? 512 : 8; }
    // Whether a thread program's global is one a spawn image carries: a
    // fixed value, a resizable or frame object (a pool with its
    // freelist), or a T[] image.
    bool ThreadGlobalOk(VarDef *d) {
        if (!M.GlobalOk(d) || cg.PrefVar(d)) return false;
        auto t = d->type;
        if (cg.IsFix(t)) return M.TypeOk(t) && !d->reusable;
        if (M.Resz(t)) return (IsGrow(t) && (M.RzOk(t) || M.VeOk(t)) && (!d->reusable || M.RzOk(t))) || M.FoOk(t);
        return IsImg(t) && !d->reusable;
    }
    // node's value bytes follow its gs_qnode header (next, size).
    static constexpr int64_t QNODE = 16;

    // The resizable (or frame object) n of type t built in a temporary of
    // its own: its header, or the frame object, and its stack.
    P RzValue(Node *n, TypeExpr *t, Line l) {
        if (M.FoOk(t)) {
            P x { AtL(tyg_local_new(p, M.Size(t), 8)), t, Stk(AllocStk(false, l), l) };
            GenFo(n, x, l);
            return x;
        }
        auto x = RzTemp(t, l);
        GenRz(n, x.stk, x.a, t, l);
        return P { x.a, t, x.stk };
    }
    // The flat image of the resizable or frame object at src, at the top
    // of stk: [int64 count][a frame object's fixed fields][tail elements]
    // (CodeGen::EmitRzImage); its address.
    tyg_val RzImage(tyg_val stk, const P &src, TypeExpr *t) {
        auto start = Top(stk);
        auto cnt = AtV(start);
        St(cnt, I64(0));
        SetTop(stk, Add(start, I64(8)));
        auto h = src.a;
        TypeExpr *et;
        if (M.FoOk(t)) {
            auto pre = M.FoHdrOff(t);
            auto top = Top(stk);
            if (pre) tyg_copy(p, AtV(top), src.a, pre);
            SetTop(stk, Add(top, I64(pre)));
            h.offset += pre;
            et = cg.FoTailArr(t)->arr->sub;
        } else {
            et = t->arr->sub;
        }
        auto hl = h;
        hl.offset += 8;
        View v { Ld(h), Ld(hl), et, -1 };
        CopyView(stk, cnt, et, v);
        return start;
    }

    V GenThreadWait(Call *c) {
        auto an = c->ArgNodes();
        cg.usesthreads = true;
        auto id = GenAs(an[0], M.ast.inttypes[IS_I64]).v;
        if (Ended()) return V {};
        CallC("gs_thread_wait", { IT(TYG_I64), tyg_type_ptr(m), IT(TYG_I32) },
              { id, FileArg(c->line), LineArg(c->line) }, nullptr);
        return V {};
    }

    // thread_spawn(worker, args...) (CodeGen::EmitThreadSpawn): the
    // arguments contiguous on a stack of the statement's, handed to the
    // worker's thunk, which unpacks them. A worker that names globals needs
    // the spawn image of them, which this backend does not build.
    V GenThreadSpawn(Call *c) {
        auto w = c->spec;
        auto an = c->ArgNodes();
        if (!w || an.size() != 1 + w->argtypes.size()) No(c->line, "thread_spawn arguments");
        string why;
        if (!M.SigOk(w, false, why)) No(c->line, cat("thread program: ", why));
        if (!cg.sinfo[w].freevars.empty()) No(c->line, "thread program with free variables");
        // The globals the worker names, copied as the C packs them: each
        // as [int64 size][bytes]. Fixed values only; a resizable's or a
        // T[]'s image is the C half's to build.
        for (auto d : cg.ThreadGlobals(w))
            if (!ThreadGlobalOk(d))
                No(c->line, cat("thread program with a global of type ", M.NameWhy(d->type)));
        for (size_t i = 0; i < w->argtypes.size(); i++) {
            auto pt = w->argtypes[i];
            if (cg.IsPoolParam(w, i) || (M.RzParam(pt) ? !(QueueOk(pt) || (M.IsRs(pt) && M.RsOk(pt))) : !M.TypeOk(pt)))
                No(c->line, cat("thread argument of type ", M.NameWhy(pt)));
        }
        cg.usesthreads = true;
        auto thunk = CallC(M.ThunkAccessor(w), {}, {}, tyg_type_ptr(m));
        auto stk = Stk(AllocStk(false, c->line), c->line);
        auto base = Top(stk);
        for (size_t i = 0; i < w->argtypes.size(); i++) {
            auto pt = w->argtypes[i];
            if (M.Resz(pt)) {
                // [int64 size][flat image]: the worker's thunk rebuilds the
                // header on a stack of its own (CodeGen::EmitThreadSpawn).
                if (M.IsRs(pt)) {
                    // A variable-prefix struct: built straight into the
                    // packet, prefix and tail elements as they lie, the
                    // count from its header (CodeGen::EmitThreadSpawn).
                    auto szp = Top(stk);
                    auto cntp = AtV(Add(szp, I64(8)));
                    SetTop(stk, Add(szp, I64(16)));
                    auto hdr = AtL(tyg_local_new(p, 16, 8));
                    auto hlen = hdr;
                    hlen.offset += 8;
                    GenRs(an[1 + i], stk, hdr, pt, c->line);
                    if (Ended()) return Dummy(M.ast.inttypes[IS_I64]);
                    St(cntp, Ld(hlen));
                    St(AtV(szp), Sub(Top(stk), Add(szp, I64(8))));
                    continue;
                }
                auto src = RzValue(an[1 + i], pt, c->line);
                if (Ended()) return Dummy(M.ast.inttypes[IS_I64]);
                auto szp = Top(stk);
                SetTop(stk, Add(szp, I64(8)));
                RzImage(stk, src, pt);
                St(AtV(szp), Sub(Top(stk), Add(szp, I64(8))));
                continue;
            }
            if (M.RzParam(pt)) {
                // An image, built at the top: the packet holds it whole.
                GenImg(an[1 + i], stk, pt, c->line);
                if (Ended()) return Dummy(M.ast.inttypes[IS_I64]);
                continue;
            }
            auto top = Top(stk);
            Into(P { AtV(top), pt }, an[1 + i]);
            if (Ended()) return Dummy(M.ast.inttypes[IS_I64]);
            SetTop(stk, Add(top, I64(cg.FixedSize(pt))));
        }
        for (auto d : cg.ThreadGlobals(w)) {
            if (!cg.IsFix(d->type)) {
                // [int64 size][image]: a resizable's flat image (with a
                // pool's freelist after it), or a T[]'s bytes.
                auto szp = Top(stk);
                SetTop(stk, Add(szp, I64(8)));
                auto pl = VarPlace(d, c->line);
                if (M.Resz(d->type)) {
                    RzImage(stk, pl, d->type);
                    if (d->reusable) {
                        // The freelist: its count, then its entries.
                        P hp;
                        tyg_addr fl;
                        tyg_val flstk;
                        PoolAt(d, c->line, hp, fl, flstk);
                        auto top = Top(stk);
                        auto flen = Ld(AtV(tyg_addr_of(p, fl), 8));
                        St(AtV(top), flen);
                        auto bytes = Mul(flen, I64(FlEntry(d)));
                        auto at = Add(top, I64(8));
                        tyg_move_dyn(p, at, Ld(fl), bytes);
                        SetTop(stk, Add(at, bytes));
                    }
                } else {
                    ImgCopy(stk, tyg_addr_of(p, pl.a), d->type);
                }
                St(AtV(szp), Sub(Top(stk), Add(szp, I64(8))));
                continue;
            }
            auto top = Top(stk);
            auto size = cg.FixedSize(d->type);
            St(AtV(top), I64(size));
            auto at = Add(top, I64(8));
            if (size) tyg_copy(p, AtV(at), VarPlace(d, c->line).a, size);
            SetTop(stk, Add(at, I64(size)));
        }
        M.NeedShim("gs_tg_thread_spawn");
        V x;
        x.t = M.ast.inttypes[IS_I64];
        x.v = CallC("gs_tg_thread_spawn", { tyg_type_ptr(m), tyg_type_ptr(m), IT(TYG_I64) },
                    { thunk, base, Sub(Top(stk), base) }, IT(TYG_I64));
        return x;
    }

    // qput(v): the value's bytes copied into a node of the queue.
    V GenQput(Call *c) {
        auto an = c->ArgNodes();
        auto t = an[0]->exprtype;
        if (!QueueOk(t)) No(c->line, cat("queue element of type ", M.NameWhy(t)));
        // A fixed aggregate is read where it lies, not copied: it may be
        // large (a native temporary of it could overflow a worker's stack).
        if (cg.IsFix(t) && !M.IsScalar(t) && cg.TEq(an[0]->exprtype, t)) {
            auto pl = Place(an[0]);
            if (Ended()) return V {};
            CallC("gs_qput", { tyg_type_ptr(m), tyg_type_ptr(m), IT(TYG_I64) },
                  { QueueAddr(t), tyg_addr_of(p, pl.a), I64(cg.FixedSize(t)) }, nullptr);
            return V {};
        }
        tyg_val ptr, size;
        if (M.Resz(t)) {
            // Flat: the count, the fixed fields, the elements.
            auto src = RzValue(an[0], t, c->line);
            if (Ended()) return V {};
            auto stk = Stk(AllocStk(false, c->line), c->line);
            ptr = RzImage(stk, src, t);
            size = Sub(Top(stk), ptr);
        } else if (!cg.IsFix(t)) {
            // An image, built on a stack of the statement's.
            auto stk = Stk(AllocStk(false, c->line), c->line);
            ptr = GenImg(an[0], stk, t, c->line);
            if (Ended()) return V {};
            size = Sub(Top(stk), ptr);
        } else {
            auto x = GenAs(an[0], t);
            if (Ended()) return V {};
            auto tmp = Temp(t);
            Store(P { AtL(tmp), t }, x);
            ptr = tyg_addr_of(p, AtL(tmp));
            size = I64(cg.FixedSize(t));
        }
        CallC("gs_qput", { tyg_type_ptr(m), tyg_type_ptr(m), IT(TYG_I64) }, { QueueAddr(t), ptr, size },
              nullptr);
        return V {};
    }

    // qget<T>() and qpoll<T>() (CodeGen's B_QGET): the oldest node's bytes
    // as the value, the node freed; a missed poll gives the zero value and
    // false. An image is built at idst's top (or a stack of the
    // statement's), so a poll of one is left to C.
    V GenQget(Call *c, tyg_val idst) {
        auto t = c->rettypes[0];
        auto poll = c->builtin == B_QPOLL;
        if (!QueueOk(t) || (poll && !cg.IsFix(t) && !M.Resz(t))) No(c->line, cat("queue element of type ", M.NameWhy(t)));
        auto node = CallC(poll ? "gs_qpoll" : "gs_qget", { tyg_type_ptr(m) }, { QueueAddr(t) }, IT(TYG_I64));
        auto freenode = [&] { CallC("free", { tyg_type_ptr(m) }, { node }, nullptr); };
        auto src = [&] { return AtV(Add(node, I64(QNODE))); };
        V x;
        x.t = t;
        if (M.Resz(t)) {
            // Rebuilt from the image on a stack of the statement's: the
            // zero value until a node is read (a missed poll leaves it).
            auto stk = Stk(AllocStk(false, c->line), c->line);
            auto fo = M.FoOk(t);
            auto pre = fo ? M.FoHdrOff(t) : 0;
            auto obj = AtL(tyg_local_new(p, fo ? M.Size(t) : 16, 8));
            tyg_zero(p, obj, fo ? M.Size(t) : 16);
            auto hdr = obj;
            hdr.offset += pre;
            auto hlen = hdr;
            hlen.offset += 8;
            St(hdr, Top(stk));
            tyg_block have = 0, done = 0;
            if (poll) {
                have = NewBlock();
                done = NewBlock();
                Branch(Cmp(TYG_NE, node, I64(0)), have, done);
                SetBlock(have);
            }
            auto image = Add(node, I64(QNODE + 8));
            auto size = Ld(AtV(node, 8));
            St(hlen, Ld(AtV(node, QNODE)));
            if (pre) tyg_copy(p, obj, AtV(image), pre);
            auto n = Sub(size, I64(8 + pre));
            auto top = Top(stk);
            tyg_move_dyn(p, top, Add(image, I64(pre)), n);
            SetTop(stk, Add(top, n));
            freenode();
            if (poll) {
                tyg_jump(p, done);
                SetBlock(done);
            }
            x.mem = true;
            x.a = obj;
            x.stk = stk;
            lastrets.clear();
            lastrets.push_back(x);
            if (poll) {
                V got = ValV(M.ast.booltype, Cmp(TYG_NE, node, I64(0)));
                lastrets.push_back(got);
            }
            return x;
        }
        if (!cg.IsFix(t)) {
            auto stk = idst ? idst : Stk(AllocStk(false, c->line), c->line);
            auto top = Top(stk);
            auto size = Ld(AtV(node, 8));
            tyg_move_dyn(p, top, Add(node, I64(QNODE)), size);
            SetTop(stk, Add(top, size));
            freenode();
            x.mem = true;
            x.a = AtV(top);
            lastrets.clear();
            lastrets.push_back(x);
            return x;
        }
        auto size = cg.FixedSize(t);
        auto tmp = Temp(t);
        auto at = AtL(tmp);
        V got;
        got.t = M.ast.booltype;
        if (poll) {
            tyg_zero(p, at, std::max<int64_t>(size, 1));
            auto have = NewBlock(), done = NewBlock();
            Branch(Cmp(TYG_NE, node, I64(0)), have, done);
            // The result of the poll as a bool, kept past the join.
            SetBlock(have);
            if (size) tyg_copy(p, at, src(), size);
            freenode();
            tyg_jump(p, done);
            SetBlock(done);
        } else {
            if (size) tyg_copy(p, at, src(), size);
            freenode();
        }
        if (M.IsScalar(t)) {
            x = Load(P { at, t }, c->line);
        } else {
            x.mem = true;
            x.a = at;
        }
        lastrets.clear();
        lastrets.push_back(x);
        if (poll) {
            got.v = Cmp(TYG_NE, node, I64(0));
            lastrets.push_back(got);
        }
        return x;
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
                StL(t, tyg_addr_of(p, tyg_at_sym(M.Bytes(s->val), 0)));
                StL(t, I64((int64_t)s->val.size()), 8);
                tyg_arg a {};
                a.mem = AtL(t);
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
        // Nor does an export's own wrapper, which the object defines.
        auto direct = M.RuntimeApi(callee->sf->cname);
        for (auto sf : M.ast.functions) direct = direct || (sf->isexport && sf->cname == callee->sf->cname);
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
            St(b.a, Top(b.stk));
            St(len, I64(0));
            TextInto(b.stk, len, c, an, 0);
            if (Ended()) return V {};
            CallC("gs_out_bytes", { tyg_type_ptr(m), IT(TYG_I64) }, { Ld(b.a), Ld(len) }, nullptr);
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
                                   I64((int64_t)s->val.size()) },
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
            tyg_zero(p, AtL(tmp), 16);
            x.mem = true;
            x.a = AtL(tmp);
            return x;
        }
        if (!t || t->kind != TY_REF || (t->ref->lenstorage >= 0 && !M.RelOk(t, 1))) No(l, "null of this type");
        x.v = I64(0);
        return x;
    }
    // A reference value's address: a fat reference's hdr.
    tyg_val Addr(const V &x, Line l) {
        if (!x.t || x.t->kind != TY_REF || (x.t->ref->lenstorage >= 0 && !M.RelOk(x.t, 1))) No(l, "address of this value");
        if (x.mem) return Ld(x.a);
        return x.v;
    }
    // A reference as a test or identity sees it: a variable's own value,
    // even where it is narrowed; null; anything else's value.
    tyg_val RefAddr(Node *n) {
        if (Is<NullLit>(n)) return I64(0);
        if (auto id = Is<Ident>(n); id && id->vdef && id->vdef->type && id->vdef->type->kind == TY_REF) {
            auto x = Load(VarPlace(id->vdef, n->line), n->line);
            return Ended() ? I64(0) : Addr(x, n->line);
        }
        auto x = Gen(n);
        return Ended() ? I64(0) : Addr(x, n->line);
    }
    // Whether a condition is an optional's null test: an optional
    // variable (narrowed already or not), or a value of an optional type.
    bool OptCond(Node *n) {
        if (auto id = Is<Ident>(n); id && id->vdef && id->vdef->type && IsOptional(id->vdef->type)) return true;
        return n->exprtype && IsOptional(n->exprtype);
    }
    tyg_val OptTest(Node *n) { return Cmp(TYG_NE, RefAddr(n), I64(0)); }
    // `a == null`, `a != null`, and identity `a .== b`, `a .!= b`: the
    // addresses, left first.
    V GenRefEq(Binary *b) {
        auto l = RefAddr(b->left);
        auto r = RefAddr(b->right);
        return ValV(M.ast.booltype, Cmp(b->op == T_EQ || b->op == T_DOTEQ ? TYG_EQ : TYG_NE, l, r));
    }
    // An optional's text (CodeGen::RenderLoc): `null`, or its pointee's.
    // A pointee that reaches its own type (a list) is rendered by a render
    // procedure (RenderInto).
    void RenderOpt(tyg_val stk, tyg_addr len, const P &pl, bool nested, Line l, int depth) {
        auto none = NewBlock(), some = NewBlock(), done = NewBlock();
        // A relative link's offset decodes to its address, null for 0.
        auto at = TgModule::IsRel(pl.t) ? RelLoad(pl, l) : Ld(pl.a);
        Branch(Cmp(TYG_EQ, at, I64(0)), none, some);
        SetBlock(none);
        TextLit(stk, len, "null");
        tyg_jump(p, done);
        SetBlock(some);
        RenderInto(stk, len, Step(pl), nested, l, depth + 1);
        Jump(done);
        SetBlock(done);
    }

    // ---- Control -----------------------------------------------------------

    // A block's value is its tail's (none in statement position).
    // A block's value is its tail's (none in statement position); a
    // function body's tail with a result is returned (tailret).
    TypeExpr *armwant = nullptr;   // What GenArm wants of a block arm's tail.
    V GenBlock(Block *b, bool discard, bool tailret = false) {
        auto want = armwant;
        armwant = nullptr;
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
                x = want ? GenSliceAs(b->tail, want) : Gen(b->tail);
                if (x.t && (IsGrow(x.t) || (IsImg(x.t) && !TgModule::IsVr(x.t)))) No(b->tail->line, "array value of a block");
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
        StL(v, I64(0));
        StL(sh, I64(0));
        StL(q, at);
        auto head = NewBlock(), more = NewBlock(), done = NewBlock();
        tyg_jump(p, head);
        SetBlock(head);
        auto qa = LdL(q);
        auto b = tyg_convert(p, TYG_ZEXT, TYG_I64, tyg_load(p, TYG_I8, AtV(qa), 0));
        auto bits = tyg_bin(p, TYG_SHL, tyg_bin(p, TYG_AND, b, I64(0x7f)), LdL(sh));
        StL(v, tyg_bin(p, TYG_OR, LdL(v), bits));
        StL(q, Add(qa, I64(1)));
        Branch(Cmp(TYG_NE, tyg_bin(p, TYG_AND, b, I64(0x80)), I64(0)), more, done);
        SetBlock(more);
        StL(sh, Add(LdL(sh), I64(7)));
        tyg_jump(p, head);
        SetBlock(done);
        return LdL(v);
    }
    // A varint value: (u >> 1) ^ -(u & 1) (gs_zig_read).
    tyg_val ZigRead(tyg_val at) {
        auto u = UlebRead(at);
        auto half = tyg_bin(p, TYG_LSHR, u, I64(1));
        auto sign = tyg_un(p, TYG_NEG, tyg_bin(p, TYG_AND, u, I64(1)));
        return tyg_bin(p, TYG_XOR, half, sign);
    }
    // The bytes of the ULEB128 at `at` (gs_uleb_size).
    tyg_val UlebSize(tyg_val at) {
        auto q = Local(at);
        auto head = NewBlock(), more = NewBlock(), done = NewBlock();
        tyg_jump(p, head);
        SetBlock(head);
        auto qa = LdL(q);
        auto b = tyg_convert(p, TYG_ZEXT, TYG_I64, tyg_load(p, TYG_I8, AtV(qa), 0));
        StL(q, Add(qa, I64(1)));
        Branch(Cmp(TYG_NE, tyg_bin(p, TYG_AND, b, I64(0x80)), I64(0)), more, done);
        SetBlock(more);
        tyg_jump(p, head);
        SetBlock(done);
        return Sub(LdL(q), at);
    }
    // Writes the varint value v at `at`, zigzag ULEB128 (gs_zig_write); its
    // byte count.
    tyg_val ZigWrite(tyg_val at, tyg_val v) {
        return UlebWrite(at, tyg_bin(p, TYG_XOR, tyg_bin(p, TYG_SHL, v, I64(1)),
                                     tyg_bin(p, TYG_ASHR, v, I64(63))));
    }
    // Writes the unsigned uv at `at`, ULEB128 (gs_uleb_write); its byte count.
    tyg_val UlebWrite(tyg_val at, tyg_val uv0) {
        auto u = tyg_local_new(p, 8, 8), q = tyg_local_new(p, 8, 8);
        StL(u, uv0);
        StL(q, at);
        auto head = NewBlock(), more = NewBlock(), last = NewBlock(), done = NewBlock();
        tyg_jump(p, head);
        SetBlock(head);
        auto uv = LdL(u);
        auto b = tyg_bin(p, TYG_AND, uv, I64(0x7f));
        auto rest = tyg_bin(p, TYG_LSHR, uv, I64(7));
        StL(u, rest);
        auto qa = LdL(q);
        StL(q, Add(qa, I64(1)));
        Branch(Cmp(TYG_NE, rest, I64(0)), more, last);
        SetBlock(more);
        tyg_store(p, TYG_I8, AtV(qa),
                  tyg_convert(p, TYG_TRUNC, TYG_I8, tyg_bin(p, TYG_OR, b, I64(0x80))), 0);
        tyg_jump(p, head);
        SetBlock(last);
        tyg_store(p, TYG_I8, AtV(qa), tyg_convert(p, TYG_TRUNC, TYG_I8, b), 0);
        tyg_jump(p, done);
        SetBlock(done);
        return Sub(LdL(q), at);
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
        auto tb = NewBlock(), eb = NewBlock(), done = NewBlock();
        Branch(c, tb, eb);
        SetBlock(tb);
        BuildArm(ie->thenb, l, build);
        Jump(done);
        SetBlock(eb);
        BuildArm(ie->elseb, l, build);
        Jump(done);
        SetBlock(done);
    }

    // An arm runs or not: what it takes on a stack is its own.
    V GenArm(Node *n, bool discard, TypeExpr *want = nullptr) {
        // A slice wanted: an array arm is a view of the array where it lies
        // (§6.4), not of a copy.
        if (want && want->kind != TY_SLICE) want = nullptr;
        if (auto b = Is<Block>(n)) {
            auto was = armwant;
            armwant = discard ? nullptr : want;
            auto x = GenBlock(b, discard);
            armwant = was;
            return x;
        }
        if (discard) {
            Stmt(n);
            return V {};
        }
        OpenScope(true);
        auto x = want ? GenSliceAs(n, want) : Gen(n);
        if (x.t && (IsGrow(x.t) || (IsImg(x.t) && !TgModule::IsVr(x.t)))) No(n->line, "array value of an arm");
        CloseScope();
        return x;
    }

    // The type a value is carried in between blocks: a relative reference
    // is an offset from its own slot, so a temporary holds the plain one.
    TypeExpr *Carry(TypeExpr *t) {
        if (t && TgModule::IsRel(t) && !TgModule::IsVr(t)) return M.ast.RefTo(t->ref->sub, Line {}, t->ref->optional);
        return t;
    }
    // An image or a resizable value of n built on a temporary's stack,
    // each arm or exit building it there (GenImg, GenRz); false if et is
    // neither.
    bool InPlaceValue(Node *n, TypeExpr *et, Line l, V &x) {
        if (IsImg(et) && M.ImgOk(et)) {
            auto stk = Stk(AllocStk(false, l), l);
            x = V {};
            x.t = et;
            x.mem = true;
            x.a = AtV(GenImg(n, stk, et, l));
            return true;
        }
        if (IsGrow(et) && (M.RzOk(et) || M.VeOk(et))) {
            x = RzTemp(et, l);
            GenRz(n, x.stk, x.a, et, l);
            return true;
        }
        if (M.RbOk(et)) {
            x = RzTemp(et, l);
            GenRb(n, x.stk, x.a, et, l);
            return true;
        }
        return false;
    }
    // A value wanted of a type neither scalar nor held in a register: in
    // place if an image or a resizable.
    bool WantInPlace(Node *n, bool discard, V &x) {
        auto et = n->exprtype;
        if (discard || !et || et->kind == TY_VOID || M.TypeOk(et) || M.VrOk(et)) return false;
        return InPlaceValue(n, et, n->line, x);
    }

    V GenIf(IfExpr *i, bool discard) {
        tyg_local res = 0;
        auto pr = TakePref(res);
        auto et = Carry(i->exprtype);
        auto valued = !discard && et && et->kind != TY_VOID;
        // A varint-width relative reference is carried through a temporary
        // as any value of one is: encoded in it, decoded out.
        if (valued && i->elseb && !M.TypeOk(et) && !M.VrOk(et)) {
            V x;
            if (InPlaceValue(i, et, i->line, x)) return x;
        }
        if (valued && !M.TypeOk(et) && !M.VrOk(et)) No(i->line, "if value type");
        if (valued && !pr) res = Temp(et);
        auto c = Truth(i->cond);
        auto tb = NewBlock(), eb = NewBlock(), done = NewBlock();
        Branch(c, tb, i->elseb ? eb : done);
        SetBlock(tb);
        auto arm = [&](Node *n) {
            ArmVal(n, valued, pr, et, res);
            Jump(done);
        };
        arm(i->thenb);
        if (i->elseb) {
            SetBlock(eb);
            arm(i->elseb);
        }
        SetBlock(done);
        if (!valued) return V {};
        return Load(P { AtL(res), et }, i->line);
    }

    // A value a loop, block or match delivers, if wanted: of a type held.
    bool Valued(Node *n, bool discard) {
        auto et = n->exprtype;
        if (discard || !et || et->kind == TY_VOID) return false;
        if (!M.TypeOk(et) && !M.VrOk(et)) No(n->line, cat("value of type ", M.NameWhy(et)));
        return true;
    }

    // `block { ... }` (EarlyBlock::CgAny): its tail's value, or a break's.
    V GenEarly(EarlyBlock *e, bool discard, const InPlace *ip = nullptr) {
        tyg_local res = 0;
        auto pr = TakePref(res);
        V w;
        if (!ip && !pr && WantInPlace(e, discard, w)) return w;
        auto valued = !ip && Valued(e, discard);
        auto et = Carry(e->exprtype);
        if (valued && !pr) res = Temp(et);
        auto done = NewBlock();
        LoopTargets lt { done, done, sscopes.size(), res, valued ? et : nullptr, true };
        lt.pref = valued && pr;
        if (ip) {
            lt.inplace = ip->mode, lt.dstk = ip->stk, lt.dt = ip->t, lt.hdr = ip->hdr;
            lt.top0 = tyg_local_new(p, 8, 8);
            StL(lt.top0, Top(ip->stk));
        }
        loops.push_back(lt);
        if (ip) {
            BuildArm(e->body, e->line, [&](Node *v) { BuildTo(v, *ip, e->line); });
            loops.pop_back();
            Jump(done);
            SetBlock(done);
            return V {};
        }
        armwant = valued && et->kind == TY_SLICE ? et : nullptr;
        V x;
        if (valued && pr) BuildArm(e->body, e->line, [&](Node *v) { PrefInto(AtL(res), v); });
        else x = GenBlock(e->body, !valued);
        loops.pop_back();
        if (valued && !pr && !Ended() && x.t) Store(P { AtL(res), et }, Fit(x, et, e->line));
        Jump(done);
        SetBlock(done);
        if (!valued) return V {};
        return Load(P { AtL(res), et }, e->line);
    }

    // `loop { ... }`, its value a break's.
    V GenLoopX(LoopExpr *lp, bool discard, const InPlace *ip = nullptr) {
        tyg_local res = 0;
        auto pr = TakePref(res);
        V w;
        if (!ip && !pr && std::any_of(lp->breaks.begin(), lp->breaks.end(), [](Break *b) { return b->val; }) &&
            WantInPlace(lp, discard, w))
            return w;
        auto valued = !ip && std::any_of(lp->breaks.begin(), lp->breaks.end(), [](Break *b) { return b->val; }) &&
                      Valued(lp, discard);
        auto et = Carry(lp->exprtype);
        if (valued && !pr) res = Temp(et);
        auto body = NewBlock(), done = NewBlock();
        tyg_jump(p, body);
        SetBlock(body);
        LoopTargets lt { done, body, sscopes.size(), res, valued ? et : nullptr };
        lt.pref = valued && pr;
        if (ip) {
            lt.inplace = ip->mode, lt.dstk = ip->stk, lt.dt = ip->t, lt.hdr = ip->hdr;
            lt.top0 = tyg_local_new(p, 8, 8);
            StL(lt.top0, Top(ip->stk));
        }
        loops.push_back(lt);
        GenBlock(lp->body, true);
        loops.pop_back();
        Jump(body);
        SetBlock(done);
        if (!valued) return V {};
        return Load(P { AtL(res), et }, lp->line);
    }

    // An integer match (MatchExpr::CgAny): the scrutinee evaluated once,
    // then the arms tested in order, each listed value or range at the
    // scrutinee's type, unsigned for an unsigned one. ADTs are not held.
    V GenMatch(MatchExpr *me, bool discard, const InPlace *ip = nullptr) {
        tyg_local res = 0;
        auto pr = TakePref(res);
        V w;
        if (!ip && !pr && WantInPlace(me, discard, w)) return w;
        if (auto adt = AdtOf(me->scrutinee->exprtype)) {
            if (pr) {
                prefctl = true;
                prefloc = res;
            }
            return GenAdtMatch(me, adt, discard, ip);
        }
        auto ot = cg.OperandT(me->scrutinee->exprtype);
        if (!ot || ot->kind != TY_INT || ot->intstorage == IS_VARINT)
            No(me->line, cat("match over type ", ot ? M.NameWhy(ot) : string("?")));
        auto valued = !ip && Valued(me, discard);
        auto et = Carry(me->exprtype);
        if (valued && !pr) res = Temp(et);
        auto uns = IsUnsigned(ot->intstorage);
        auto x0 = Conv(GenAs(me->scrutinee, ot).v, ot, M.ast.inttypes[IS_I64], me->line);
        if (Ended()) return valued ? Dummy(et) : V {};
        auto xl = Local(x0);
        auto x = [&] { return LdL(xl); };
        auto k = [&](int64_t v) { return I64(v); };
        auto done = NewBlock();
        auto arm = [&](Node *body) {
            if (ip) {
                BuildArm(body, me->line, [&](Node *v) { BuildTo(v, *ip, me->line); });
                Jump(done);
                return;
            }
            ArmVal(body, valued, pr, et, res);
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
            auto hit = NewBlock(), next = NewBlock();
            for (size_t i = 0; i < a.ranges.size(); i++) {
                auto &r = a.ranges[i];
                auto miss = i + 1 < a.ranges.size() ? NewBlock() : next;
                if (r.lo == r.hi) {
                    Branch(Cmp(TYG_EQ, x(), k(r.lo)), hit, miss);
                } else {
                    auto upper = NewBlock();
                    Branch(Cmp(uns ? TYG_UGE : TYG_SGE, x(), k(r.lo)), upper, miss);
                    SetBlock(upper);
                    Branch(Cmp(uns ? TYG_ULE : TYG_SLE, x(), k(r.hi)), hit, miss);
                }
                if (miss != next) SetBlock(miss);
            }
            if (a.ranges.empty()) tyg_jump(p, next);
            SetBlock(hit);
            arm(a.body);
            SetBlock(next);
        }
        if (!wild) Jump(done);
        SetBlock(done);
        if (!valued) return V {};
        return Load(P { AtL(res), et }, me->line);
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
        V x = MemV(t, AtL(Temp(t)));
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
        if (M.RbOk(t)) {
            auto x = RzTemp(t, d->line);
            GenRb(d, x.stk, x.a, t, d->line);
            return x;
        }
        if (M.VmOk(t)) {
            // A variable-mode value: its image on a temporary's stack.
            auto stk = Stk(AllocStk(false, d->line), d->line);
            return MemV(t, AtV(GenImg(d, stk, t, d->line)));
        }
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
        if (M.RbOk(adt)) {
            // A resizable value is read where it lies: its header holds
            // where its bytes start, the tag first.
            auto pl = Deref(Place(n));
            Scrut s { V {}, adt, tyg_local_new(p, 8, 8) };
            s.v.t = adt;
            s.v.mem = true;
            s.v.a = Ended() ? AtL(Temp(M.ast.inttypes[IS_I64]))
                            : AtV(Ld(pl.a));
            StL(s.tag, LoadTag(s.v.a, adt));
            return s;
        }
        if (M.VmOk(adt)) {
            // A variable-mode value is read where it lies: its tag first,
            // its payload behind it, as a fixed-mode one's.
            auto pl = Deref(Place(n));
            Scrut s { V {}, adt, tyg_local_new(p, 8, 8) };
            s.v.t = adt;
            s.v.mem = true;
            s.v.a = pl.a;
            if (Ended()) s.v.a = AtL(Temp(M.ast.inttypes[IS_I64]));
            StL(s.tag, LoadTag(s.v.a, adt));
            return s;
        }
        if (!M.TypeOk(adt)) No(n->line, cat("match over type ", M.NameWhy(adt)));
        Scrut s { GenAs(n, adt), adt, tyg_local_new(p, 8, 8) };
        if (Ended()) s.v = MemTemp(adt, false);
        StL(s.tag, LoadTag(s.v.a, adt));
        return s;
    }
    // Branches to `hit` if the scrutinee's tag is variant vi, else on.
    void TagIs(const Scrut &s, int vi, tyg_block hit) {
        auto next = NewBlock();
        auto tag = LdL(s.tag);
        Branch(Cmp(TYG_EQ, tag, I64(vi)), hit, next);
        SetBlock(next);
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

    V GenAdtMatch(MatchExpr *me, TypeExpr *adt, bool discard, const InPlace *ip = nullptr) {
        tyg_local res = 0;
        auto pr = TakePref(res);
        auto valued = !ip && Valued(me, discard);
        auto et = Carry(me->exprtype);
        if (valued && !pr) res = Temp(et);
        auto s = AdtScrutinee(me->scrutinee, adt);
        auto done = NewBlock();
        auto wild = false;
        for (auto &a : me->arms) {
            auto hit = NewBlock();
            if (a.pat.kind == P_WILDCARD) {
                wild = true;
                tyg_jump(p, hit);
            } else {
                for (auto v : a.variants) TagIs(s, adt->enu->en->VariantIndex(v), hit);
            }
            auto next = NewBlock();
            if (!wild) tyg_jump(p, next);
            SetBlock(hit);
            tyg_scope_begin(p);
            // The arm's own stack scope: a binder's stack is taken and given
            // back on this arm's path only.
            OpenScope(false);
            if (a.binder) {
                auto vi = adt->enu->en->VariantIndex(a.variants[0]);
                auto vt = cg.VariantType(adt, vi);
                auto pay = s.v.a;
                pay.offset += M.TagSize(adt);
                // The C binds no resizable payload, and by value only a
                // resizable ADT's others (§8.1).
                if (M.IsRb(adt) && (a.pat.byref || cg.IsResz(vt))) No(me->line, "match arm binding of a resizable ADT");
                if (a.pat.byref) {
                    // Variable-mode payloads only (§8.1): its address.
                    if (!M.IsVm(adt)) No(me->line, "match arm binding by reference");
                    auto slot = Local(tyg_addr_of(p, pay));
                    locals[a.binder] = slot;
                    if (!IsAddrRef(a.binder->type)) indirect.insert(a.binder);
                } else if (!cg.IsFix(vt)) {
                    // A variable payload, copied onto the binder's own stack.
                    Declare(a.binder, me->line);
                    auto stk = LdL(rzstk[a.binder]);
                    StL(locals[a.binder], ImgCopy(stk, tyg_addr_of(p, pay), vt));
                } else {
                    Declare(a.binder, me->line);
                    CopyPayload(s, vi, AtL(locals[a.binder]));
                }
            }
            Loc(a.body->line);   // A breakpoint on the arm's line stops here, bound.
            if (ip) {
                BuildArm(a.body, me->line, [&](Node *v) { BuildTo(v, *ip, me->line); });
            } else {
                ArmVal(a.body, valued, pr, et, res);
            }
            CloseScope();
            tyg_scope_end(p);
            Jump(done);
            SetBlock(next);
            if (wild) break;
        }
        if (!wild) BadTag(me->line);
        else tyg_unreachable(p);
        SetBlock(done);
        if (!valued) return V {};
        return Load(P { AtL(res), et }, me->line);
    }

    // A case-function call dispatched on its ADT argument's tag (§8.2):
    // every argument evaluated once, in order, then the variant's case
    // called with its payload by value and the others shared.
    V GenDispatch(Call *c, tyg_val idst, const RzDst *dst) {
        auto sp0 = c->dispatch[0];
        auto pos = c->dispatcharg;
        auto an = c->ArgNodes();
        if (an.size() < sp0->argtypes.size()) No(c->line, "argument count");
        an.resize(sp0->argtypes.size());
        auto adt = AdtOf(an[pos]->exprtype);
        if (!adt) No(c->line, "dispatch over this type");
        if (M.IsRb(adt)) No(c->line, "dispatch over a resizable ADT");
        for (auto sp : c->dispatch) {
            string why;
            if (!M.SigOk(sp, false, why)) No(c->line, cat("call to ", sp->sf->name, ": ", why));
            if (sp->rets.size() != sp0->rets.size() || cg.sinfo[sp].cret != cg.sinfo[sp0].cret)
                No(c->line, "dispatch with results of different kinds");
            for (size_t i = 0; i < sp->rets.size(); i++)
                if (cg.Mangle(sp->rets[i]) != cg.Mangle(sp0->rets[i])) No(c->line, "dispatch with results of different types");
            for (size_t i = 0; i < sp->argtypes.size(); i++)
                if ((int)i != pos && cg.Mangle(sp->argtypes[i]) != cg.Mangle(sp0->argtypes[i]))
                    No(c->line, "dispatch with arguments of different types");
            // A variable-size case takes its payload by value as an image:
            // a copy of the variable-mode value's own (below).
            auto cpt = sp->argtypes[(size_t)pos];
            if (M.RzParam(cpt) && !(M.IsVv(cpt) && !M.Resz(cpt) && M.IsVm(adt)))
                No(c->line, "dispatch to a case of this kind");
            for (size_t i = 0; i < sp->argtypes.size(); i++)
                if (cg.IsPoolParam(sp, i) != cg.IsPoolParam(sp0, i)) No(c->line, "dispatch with a pool reference");
        }
        // The shared arguments, each kept in a local of its own: the arms
        // are blocks of their own.
        Scrut s {};
        tyg_local copied = 0;
        auto havecopy = false;
        vector<tyg_local> shared(an.size());
        auto copyval = [&] {
            if (!Ended() && M.IsVm(adt) && std::any_of(c->dispatch.begin(), c->dispatch.end(),
                                        [&](FnSpec *k) { return k->argtypes[(size_t)pos]->kind != TY_REF; })) {
                // The whole value copied before the later arguments
                // run (CodeGen::EmitDispatch's `copied`).
                auto stk = Stk(AllocStk(false, c->line), c->line);
                copied = tyg_local_new(p, 8, 8);
                StL(copied, ImgCopy(stk, tyg_addr_of(p, s.v.a), adt));
                havecopy = true;
            }
        };

        vector<vector<tyg_arg>> valargs(an.size());   // By-value arrays', built once.
        for (size_t i = 0; i < an.size(); i++) {
            if ((int)i == pos && sp0->argtypes[i]->kind == TY_REF) {
                // Cases taking the variant by reference: the value where it
                // lies, its payload handed by address.
                auto pl = Deref(Place(an[i]));
                s = Scrut { V {}, adt, tyg_local_new(p, 8, 8) };
                s.v.t = adt;
                s.v.mem = true;
                s.v.a = pl.a;
                if (Ended()) s.v.a = AtL(Temp(M.ast.inttypes[IS_I64]));
                StL(s.tag, LoadTag(s.v.a, adt));
                copyval();
                continue;
            }
            if ((int)i == pos) {
                s = AdtScrutinee(an[i], adt);
                copyval();
                continue;
            }
            auto pt = sp0->argtypes[i];
            if (cg.IsPoolParam(sp0, i)) {
                // A pool reference, kept as its 32 bytes (GenPrefVal).
                shared[i] = tyg_local_new(p, 32, 8);
                PrefInto(AtL(shared[i]), an[i]);
                continue;
            }
            if (M.RzParam(pt)) {
                ValArg(an[i], pt, valargs[i]);
                continue;
            }
            if (!M.TypeOk(pt)) No(an[i]->line, "argument of this type");
            auto x = GenAs(an[i], pt);
            shared[i] = Temp(pt);
            if (Ended()) continue;
            if (M.IsScalar(pt)) tyg_store(p, M.Scal(pt), AtL(shared[i]), x.v, 0);
            else tyg_copy(p, AtL(shared[i]), x.a, M.Size(pt));
        }
        auto cret = cg.sinfo[sp0].cret;
        auto rt = cret >= 0 ? sp0->rets[(size_t)cret] : nullptr;
        tyg_local res = rt ? Temp(rt) : 0;
        lastrets.clear();
        if (Ended()) {
            for (auto r : sp0->rets) lastrets.push_back(Dummy(r));
            return lastrets.empty() ? V {} : lastrets[0];
        }
        // The results' channels, the same for every case.
        vector<tyg_arg> chanargs;
        auto rets = RetChans(sp0, cg.sinfo[sp0], idst, dst, chanargs, c->line);
        auto done = NewBlock();
        auto &variants = adt->enu->en->variants;
        vector<tyg_block> arms;
        for (size_t vi = 0; vi < variants.size(); vi++) {
            arms.push_back(NewBlock());
            TagIs(s, (int)vi, arms.back());
        }
        BadTag(c->line);
        for (size_t vi = 0; vi < variants.size(); vi++) {
            SetBlock(arms[vi]);
            auto sp = c->dispatch[vi];
            auto &ci = cg.sinfo[sp];
            vector<tyg_arg> args;
            for (size_t i = 0; i < an.size(); i++) {
                auto pt = sp->argtypes[i];
                tyg_arg a {};
                if ((int)i == pos && pt->kind == TY_REF && pt->ref->sub->kind == TY_VARIANT) {
                    a.val = Add(tyg_addr_of(p, s.v.a), I64(M.TagSize(adt)));
                } else if ((int)i == pos) {
                    if (M.IsVv(pt)) {
                        a.val = Add(LdL(copied), I64(M.TagSize(adt)));
                        args.push_back(a);
                        continue;
                    }
                    if (pt->kind != TY_VARIANT) No(c->line, "dispatch to a case of this kind");
                    auto t = Temp(pt);
                    auto cs = s;
                    if (havecopy) cs.v.a = AtV(LdL(copied));
                    CopyPayload(cs, (int)vi, AtL(t));
                    if (cg.IsLargeFixed(pt)) a.val = tyg_addr_of(p, AtL(t));
                    else a.mem = AtL(t);
                } else if (cg.IsPoolParam(sp, i)) {
                    for (int k = 0; k < 4; k++) {
                        tyg_arg q {};
                        q.val = LdL(shared[i], 8 * k);
                        args.push_back(q);
                    }
                    continue;
                } else if (M.RzParam(pt)) {
                    args.insert(args.end(), valargs[i].begin(), valargs[i].end());
                    continue;
                } else if (M.IsScalar(pt)) {
                    a.val = tyg_load(p, M.Scal(pt), AtL(shared[i]), 0);
                } else if (cg.IsLargeFixed(pt)) {
                    a.val = tyg_addr_of(p, AtL(shared[i]));
                } else {
                    a.mem = AtL(shared[i]);
                }
                args.push_back(a);
            }
            FvArgs(c, ci, args);
            args.insert(args.end(), chanargs.begin(), chanargs.end());
            if (ci.needssp) {
                tyg_arg sa {};
                sa.val = CalleeSp(sp, c->line);
                args.push_back(sa);
            }
            auto sym = M.Sym(ci.cname, TYG_SYM_FUNC | TYG_SYM_GLOBAL);
            auto x = Result(sp, ci.cret, M.Sig(sp), tyg_direct(sym), args);
            if (rt) Store(P { AtL(res), rt }, x);
            RfCheck(sp);
            Jump(done);
        }
        SetBlock(done);
        for (size_t i = 0; i < rets.size(); i++) {
            if ((int)i == cret) lastrets.push_back(Load(P { AtL(res), rt }, c->line));
            else if (M.IsScalar(rets[i].t)) lastrets.push_back(Load(P { rets[i].a, rets[i].t }, c->line));
            else lastrets.push_back(rets[i]);
        }
        return lastrets.empty() ? V {} : lastrets[0];
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
                auto head = NewBlock(), body = NewBlock(), done = NewBlock();
                tyg_jump(p, head);
                SetBlock(head);
                Loc(w->cond->line);
                OpenScope(true);
                auto c = Truth(w->cond);
                CloseScope();
                Branch(c, body, done);
                SetBlock(body);
                loops.push_back({ done, head, sscopes.size() });
                GenBlock(w->body, true);
                loops.pop_back();
                Jump(head);
                SetBlock(done);
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
                    if (t.pref) {
                        PrefInto(AtL(t.res), b->val);
                    } else if (t.inplace) {
                        // Built at the top, which something under
                        // construction may have moved up: the value then
                        // moves down to where the exit's value starts
                        // (CodeGen::GenExitValue, LandValue).
                        InPlace ip { t.inplace, t.dstk, t.dt, t.hdr };
                        auto st = Local(Top(t.dstk));
                        BuildTo(b->val, ip, b->line);
                        if (!Ended()) {
                            auto start = LdL(st);
                            auto top0 = LdL(t.top0);
                            If(Cmp(TYG_NE, start, top0), [&] {
                                auto n = Sub(Top(t.dstk), start);
                                tyg_move_dyn(p, top0, start, n);
                                SetTop(t.dstk, Add(top0, n));
                                if (t.inplace == 2) St(t.hdr, top0);
                            });
                        }
                    } else if (t.rt) {
                        auto x = GenAs(b->val, t.rt);
                        if (!M.IsScalar(t.rt) && !x.mem && !Ended()) No(n->line, "aggregate value");
                        Store(P { AtL(t.res), t.rt }, x);
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
            default: {
                auto pc = Is<Call>(n);
                discardpush = pc && pc->builtin == B_PUSH;
                Gen(n);
                discardpush = false;
                return;
            }
        }
    }

    // A nonfixed result built at the top of dst, from val or from x (a
    // forwarded call's value): a T[]'s image, or a resizable's elements,
    // whose count is stored at len.
    void BuildAt(Node *val, V *x, tyg_val dst, TypeExpr *t, tyg_addr len, Line l) {
        if (M.FoOk(t)) {
            // A frame object: its struct at len, its tail on dst.
            if (x) FoCopy(P { len, t, dst }, P { x->a, t, x->stk }, l);
            else GenFo(val, P { len, t, dst }, l);
            return;
        }
        if (IsImg(t)) {
            if (!x) {
                GenImg(val, dst, t, l);
            } else if (M.IsVm(t) && !M.IsVm(x->t)) {
                VmFrom(dst, x->a, x->t, t, l);   // A variant or a fixed ADT, adapted.
            } else if (M.IsVs(t) || M.IsVm(t)) {
                VsCopy(dst, *x, t, l);
            } else {
                auto v = ViewOf(P { x->a, x->t, x->stk }, l);
                GenImg(nullptr, dst, t, l, &v);
            }
            return;
        }
        auto hdr = AtL(tyg_local_new(p, 16, 8));
        auto hlen = hdr;
        hlen.offset += 8;
        if (M.IsRb(t)) {
            if (!x) GenRb(val, dst, hdr, t, l);
            else RbFrom(dst, hdr, *x, t, l);
        } else if (!x) {
            GenRz(val, dst, hdr, t, l);
        } else if (M.IsRs(t)) {
            RsCopy(dst, hdr, P { x->a, x->t, x->stk }, l);
        } else {
            auto v = ViewOf(P { x->a, x->t, x->stk }, l);
            St(hdr, Top(dst));
            St(hlen, I64(0));
            CopyView(dst, hlen, t->arr->sub, v);
        }
        if (Ended()) return;
        St(len, Ld(hlen));
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
        if (rets.size() == 1 && cret == 0 && vals.size() == 1 && SelfRelLit(vals[0]) && cg.TEq(vals[0]->exprtype, rets[0])) {
            // A literal linking to itself is built where the result lies.
            auto tmp = AtL(Temp(rets[0]));
            SelfRelInto(P { tmp, rets[0] }, vals[0]);
            if (Ended()) return;
            Leave();
            tyg_ret_mem(p, tmp);
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
        if (cret >= 0) first = P { AtL(Temp(rets[cret])), rets[cret] };
        for (size_t i = 0; i < rets.size(); i++) {
            if (IsImg(rets[i]) || M.Resz(rets[i])) {
                auto dst = LdL(chans[i].a);
                SetTop(dst, LdL(chans[i].top0));
                auto rl = M.Resz(rets[i]) ? AtV(LdL(chans[i].b))
                                          : tyg_addr {};
                BuildAt(xs.empty() ? vals[i] : nullptr, xs.empty() ? nullptr : &xs[i], dst, rets[i], rl, l);
                if (Ended()) return;
                continue;
            }
            if (xs.empty() && SelfRelLit(vals[i]) && cg.TEq(vals[i]->exprtype, rets[i])) {
                if ((int)i == cret) {
                    SelfRelInto(first, vals[i]);
                } else {
                    auto ptr = LdL(chans[i].a);
                    SelfRelInto(P { AtV(ptr), rets[i] }, vals[i]);
                }
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
            auto ptr = LdL(chans[i].a);
            Store(P { AtV(ptr), rets[i] }, x);
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
        return LdL(AccessorLocal("gs_tg_rf"));
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
        M.channels[name] = string(kind) == "lret" && !IsGrow(rt) && !M.IsRs(rt) && !M.IsRb(rt) ? M.Size(rt) : 8;
        return AtV(LdL(AccessorLocal(name)));
    }
    tyg_addr LretAt(FnSpec *t, size_t i) { return ChanAt(t, i, "lret"); }
    // A target's saves of its gs_fdst_ channels, restored as it returns.
    vector<pair<tyg_addr, tyg_local>> fdstsaves;
    void Leave() {
        RestoreFrom(0);
        for (auto &[at, sav] : fdstsaves) St(at, LdL(sav));
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
            tyg_zero(p, AtL(t), M.Size(sp->rets[cret]));
            tyg_ret_mem(p, AtL(t));
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
            if (IsImg(rets[i]) || M.Resz(rets[i]))
                St(ChanAt(t, i, "fval"), Top(Ld(ChanAt(t, i, "fdst"))));
        vector<V> xs;
        if (vals.size() == 1 && rets.size() > 1) xs = GenCallAll(vals[0], rets.size());
        else if (vals.size() != rets.size()) No(l, "result count");
        for (size_t i = 0; i < rets.size(); i++) {
            if (IsImg(rets[i]) || M.Resz(rets[i])) {
                auto dst = Ld(ChanAt(t, i, "fdst"));
                BuildAt(xs.empty() ? vals[i] : nullptr, xs.empty() ? nullptr : &xs[i], dst, rets[i],
                        M.Resz(rets[i]) ? LretAt(t, i) : tyg_addr {}, l);
                if (Ended()) return;
                continue;
            }
            auto x = xs.empty() ? GenAs(vals[i], rets[i]) : Fit(xs[i], rets[i], l);
            if (Ended()) return;
            if (!M.IsScalar(rets[i]) && !x.mem) No(l, "aggregate value");
            Store(P { LretAt(t, i), rets[i] }, x);
        }
        tyg_store(p, TYG_I32, AtV(RfAddr()), tyg_iconst(p, TYG_I32, cg.fromids[t]), 0);
        Propagate();
    }
    // After a call that can propagate a return (CodeGen::EmitRfCheck).
    void RfCheck(FnSpec *callee) {
        if (!cg.sinfo[callee].hasrf || cg.norfcheck || Ended()) return;
        auto rfa = RfAddr();
        auto rf = tyg_load(p, TYG_I32, AtV(rfa), 0);
        auto on = NewBlock(), cont = NewBlock();
        Branch(Cmp(TYG_NE, rf, tyg_iconst(p, TYG_I32, 0)), on, cont);
        SetBlock(on);
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
            auto here = NewBlock(), other = NewBlock();
            Branch(Cmp(TYG_EQ, rf, tyg_iconst(p, TYG_I32, tid)), here, other);
            SetBlock(here);
            // The propagation ends here: "nothing in flight" again, and
            // the values in the channels returned as this frame's own.
            tyg_store(p, TYG_I32, AtV(rfa), tyg_iconst(p, TYG_I32, 0), 0);
            auto &rets = sp->rets;
            auto cret = cg.sinfo[sp].cret;
            for (size_t i = 0; i < rets.size(); i++) {
                if ((int)i == cret) continue;
                if (IsImg(rets[i]) || M.Resz(rets[i])) {
                    // The value is on our destination, behind anything the
                    // calls it unwound had built there (CodeGen::LandValue).
                    auto dst = LdL(chans[i].a);
                    auto top0 = LdL(chans[i].top0);
                    auto start = Ld(ChanAt(sp, i, "fval"));
                    auto n = Sub(Top(dst), start);
                    tyg_move_dyn(p, top0, start, n);
                    SetTop(dst, Add(top0, n));
                    if (IsGrow(rets[i]) || M.IsRs(rets[i]) || M.IsRb(rets[i])) {
                        auto rl = LdL(chans[i].b);
                        St(AtV(rl), Ld(LretAt(sp, i)));
                    } else if (M.FoOk(rets[i])) {
                        // The frame object, its tail's base moved with it.
                        auto rl = LdL(chans[i].b);
                        tyg_copy(p, AtV(rl), LretAt(sp, i), M.Size(rets[i]));
                        auto hb = AtV(rl, M.FoHdrOff(rets[i]));
                        auto start2 = Ld(ChanAt(sp, i, "fval"));
                        auto top02 = LdL(chans[i].top0);
                        St(hb, Add(top02, Sub(Ld(hb), start2)));
                    }
                    continue;
                }
                auto ptr = LdL(chans[i].a);
                Store(P { AtV(ptr), rets[i] }, Load(P { LretAt(sp, i), rets[i] }, Line {}));
            }
            Leave();
            if (cret < 0) tyg_ret(p, 0);
            else if (M.IsScalar(rets[cret])) tyg_ret(p, Load(P { LretAt(sp, cret), rets[cret] }, Line {}).v);
            else tyg_ret_mem(p, LretAt(sp, cret));
            SetBlock(other);
        }
        elsewhere();
        SetBlock(cont);
    }

    void Declare(VarDef *d, Line l) {
        if (((IsGrow(d->type) && (M.RzOk(d->type) || M.VeOk(d->type))) || M.RsOk(d->type) || M.RbOk(d->type)) && !cg.PrefVar(d)) {
            // A resizable local: its header, and a stack of its own, empty.
            auto stk = Stk(AllocStk(true, l), l);
            auto hdr = tyg_local_new(p, 16, 8);
            auto sl = Local(stk);
            StL(hdr, Top(stk));
            StL(hdr, I64(0), 8);
            locals[d] = hdr;
            rzstk[d] = sl;
            tyg_debug_var(p, string(d->name).c_str(), M.DT(d->type), AtL(hdr),
                          M.File(d->line.fileidx), d->line.line);
            if (d->reusable) {
                // Its freelist: free slot indices on a stack of their own.
                auto flstk = Stk(AllocStk(true, l), l);
                auto fl = tyg_local_new(p, 16, 8);
                auto fsl = Local(flstk);
                StL(fl, Top(flstk));
                StL(fl, I64(0), 8);
                poolfl[d] = { fl, fsl };
            }
            return;
        }
        if (M.FoOk(d->type) && !cg.PrefVar(d)) {
            // A frame object local: its C struct here, its tail's elements
            // on a stack of its own, the tail empty.
            auto stk = Stk(AllocStk(true, l), l);
            auto fo = tyg_local_new(p, M.Size(d->type), 8);
            auto sl = Local(stk);
            tyg_zero(p, AtL(fo), M.Size(d->type));
            StL(fo, Top(stk), M.FoHdrOff(d->type));
            locals[d] = fo;
            rzstk[d] = sl;
            tyg_debug_var(p, string(d->name).c_str(), M.DT(d->type), AtL(fo),
                          M.File(d->line.fileidx), d->line.line);
            return;
        }
        if (((IsVarArr(d->type) && (M.RzOk(d->type) || M.VarImgOk(d->type))) || M.VsOk(d->type) || M.VmOk(d->type) ||
             M.VvOk(d->type) || M.RcOk(d->type) || M.VrOk(d->type)) &&
            !cg.PrefVar(d)) {
            // A T[] local: its image's address, the image on a stack of its
            // own (built by StmtVarDecl).
            auto stk = Stk(AllocStk(true, l), l);
            auto sl = tyg_local_new(p, 8, 8);
            auto loc = tyg_local_new(p, 8, 8);
            StL(sl, stk);
            StL(loc, Top(stk));
            locals[d] = loc;
            indirect.insert(d);
            rzstk[d] = sl;
            tyg_debug_var(p, string(d->name).c_str(), tyg_dt_pointer(m, M.DT(d->type)), AtL(loc),
                          M.File(d->line.fileidx), d->line.line);
            return;
        }
        if (!M.TypeOk(d->type)) No(l, cat("variable type of ", d->name, ": ", M.NameWhy(d->type)));
        // A pool reference: a gs_pref, the fat reference and the pool's
        // freelist (CodeGen::VarCT).
        auto loc = cg.PrefVar(d) ? tyg_local_new(p, 32, 8) : Temp(d->type);
        locals[d] = loc;
        tyg_debug_var(p, string(d->name).c_str(), M.DT(d->type), AtL(loc),
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
                if ((M.IsVs(d->type) || (M.IsVm(d->type) && !M.IsRb(d->type))) && !Ended()) {
                    auto stk = LdL(rzstk[d]);
                    StL(locals[d], VsCopy(stk, xs[i], d->type, vd->line));
                    continue;
                }
                if (IsVarArr(d->type) && !Ended()) {
                    auto v = ViewOf(P { xs[i].a, xs[i].t, xs[i].stk }, vd->line);
                    auto stk = LdL(rzstk[d]);
                    auto img = GenImg(nullptr, stk, d->type, vd->line, &v);
                    StL(locals[d], img);
                    continue;
                }
                if (M.FoOk(d->type) && !Ended()) {
                    auto stk = LdL(rzstk[d]);
                    FoCopy(P { AtL(locals[d]), d->type, stk }, P { xs[i].a, xs[i].t, xs[i].stk }, vd->line);
                    continue;
                }
                if (M.RsOk(d->type) && !Ended()) {
                    auto stk = LdL(rzstk[d]);
                    RsCopy(stk, AtL(locals[d]), P { xs[i].a, xs[i].t, xs[i].stk }, vd->line);
                    continue;
                }
                if (M.IsRb(d->type) && !Ended()) {
                    auto stk = LdL(rzstk[d]);
                    RbFrom(stk, AtL(locals[d]), xs[i], d->type, vd->line);
                    continue;
                }
                if (IsGrow(d->type) && !Ended()) {
                    // A resizable result, copied from its temporary onto
                    // the local's own stack.
                    if (!IsGrow(xs[i].t) || cg.Mangle(xs[i].t->arr->sub) != cg.Mangle(d->type->arr->sub))
                        No(vd->line, "array result of another type");
                    auto v = ViewOf(P { xs[i].a, xs[i].t, xs[i].stk }, vd->line);
                    auto stk = LdL(rzstk[d]);
                    CopyView(stk, AtL(locals[d], 8), d->type->arr->sub, v);
                    continue;
                }
                auto x = Fit(xs[i], d->type, vd->line);
                if (!M.IsScalar(d->type) && !x.mem && !Ended()) No(vd->line, "aggregate value");
                Store(P { AtL(locals[d]), d->type }, x);
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
        P pl { AtL(locals[d]), d->type };
        if (IsImg(d->type)) {
            // Built on its own stack.
            auto stk = LdL(rzstk[d]);
            auto img = GenImg(init, stk, d->type, l);
            if (!Ended()) St(pl.a, img);
            return;
        }
        if (IsGrow(d->type) || M.RsOk(d->type)) {
            // Built in place, on its own stack.
            if (init) GenRz(init, LdL(rzstk[d]), pl.a, d->type, l);
            return;
        }
        if (M.IsRb(d->type)) {
            if (!init) No(l, "resizable ADT without a value");
            GenRb(init, LdL(rzstk[d]), pl.a, d->type, l);
            return;
        }
        if (M.FoOk(d->type)) {
            if (init) GenFo(init, P { pl.a, d->type, LdL(rzstk[d]) }, l);
            return;
        }
        if (cg.PrefVar(d)) {
            if (!init) tyg_zero(p, pl.a, 32);
            else PrefInto(pl.a, init);
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
            if (auto id = Is<Ident>(a->lval); id && id->vdef && !a->pointee && IsPrefStore(id->vdef)) {
                // A pool reference rebound takes the pool's freelist too.
                PrefInto(pl.a, a->rhs);
                return;
            }
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
            if (!(M.RzOk(pl.t) || M.VeOk(pl.t)) || !pl.stk) No(a->line, "assignment to an array of this kind");
            SetTop(pl.stk, BaseOf(pl));
            if (pl.rbase) {
                // A resizable struct's tail: built behind a header of its
                // own, only the count the owner's.
                auto th = AtL(tyg_local_new(p, 16, 8));
                GenRz(a->rhs, pl.stk, th, pl.t, a->line);
                if (Ended()) return;
                auto ol = pl.a;
                ol.offset += 8;
                th.offset += 8;
                St(ol, Ld(th));
                return;
            }
            GenRz(a->rhs, pl.stk, pl.a, pl.t, a->line);
            return;
        }
        if (a->op == T_ASSIGN && pl.bfo && pl.stk) {
            // A frame object tail of a resizable struct, rebuilt from where
            // it starts as bytes (Assign::CgStmt, GenFoAsBytes).
            SetTop(pl.stk, tyg_addr_of(p, pl.a));
            auto ol = pl.ohdr;
            ol.offset += 8;
            GenFoBytes(a->rhs, pl.stk, pl.t, ol, a->line);
            return;
        }
        if (a->op == T_ASSIGN && M.FoOk(pl.t) && pl.stk) {
            // A whole frame object (Assign::CgStmt): its innermost tail's
            // elements given back, then the new value built in place.
            auto hb = pl.a;
            hb.offset += M.FoHdrOff(pl.t);
            SetTop(pl.stk, Ld(hb));
            GenFo(a->rhs, pl, a->line);
            return;
        }
        if (a->op == T_ASSIGN && pl.tmphdr && pl.stk && M.RbOk(pl.t)) {
            // The tail of a resizable struct, a resizable ADT (Assign::CgStmt):
            // the stack's top back to where the tail starts, the new one built
            // there, its count the struct's.
            SetTop(pl.stk, Ld(pl.a));
            GenRb(a->rhs, pl.stk, pl.a, pl.t, a->line);
            if (Ended()) return;
            auto tl = pl.a;
            tl.offset += 8;
            auto ol = pl.ohdr;
            ol.offset += 8;
            St(ol, Ld(tl));
            return;
        }
        if (a->op == T_ASSIGN && pl.tmphdr) No(a->line, "assignment to the tail of a resizable struct");
        if (a->op == T_ASSIGN && M.RsOk(pl.t)) {
            // A whole resizable struct: rebuilt from the start of its image.
            if (!pl.stk) No(a->line, "assignment to a resizable struct without its stack");
            SetTop(pl.stk, Ld(pl.a));
            GenRs(a->rhs, pl.stk, pl.a, pl.t, a->line);
            return;
        }
        if (a->op == T_ASSIGN && M.IsRb(pl.t)) {
            // A whole resizable ADT (Assign::CgStmt): its stack's top back to
            // where its bytes start, then the new value built there.
            if (!M.RbOk(pl.t) || !pl.stk) No(a->line, "assignment to a resizable ADT of this kind");
            SetTop(pl.stk, Ld(pl.a));
            GenRb(a->rhs, pl.stk, pl.a, pl.t, a->line);
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
                v = LimView(P { AtV(GenImg(a->rhs, stk, pl.t, a->line)), pl.t });
            }
            if (Ended()) return;
            if (cg.Mangle(v.elem) != cg.Mangle(pl.t->arr->sub)) No(a->line, "array of another element type");
            auto nl = Local(v.len);
            auto n = [&] { return LdL(nl); };
            AbortUnless(Cmp(TYG_SLE, n(), LimCapOf(pl)), "CAPACITY", a->line);
            auto esz = I64(cg.FixedSize(pl.t->arr->sub));
            tyg_move_dyn(p, LimView(pl).elems, v.elems, Mul(n(), esz));
            LimSetLen(pl, n());
            return;
        }
        if (a->op == T_ASSIGN) {
            // The destination's address is taken before the value runs (it
            // is a local slot or an address in a register), as the C does.
            if (SelfRelLit(a->rhs) && cg.TEq(a->rhs->exprtype, pl.t)) {
                // A literal linking to itself is built where it lies.
                SelfRelInto(pl, a->rhs);
                return;
            }
            auto x = GenAs(a->rhs, pl.t);
            if (!M.IsScalar(pl.t) && !x.mem && !Ended()) No(a->line, "aggregate value");
            Store(pl, x);
            return;
        }
        pl = Deref(pl);
        auto t = pl.t;
        if ((t->kind == TY_STRUCT || t->kind == TY_ARRAY) && M.TypeOk(t) &&
            (a->op == T_PLUSEQ || a->op == T_MINUSEQ || a->op == T_MULEQ || a->op == T_DIVEQ || a->op == T_MODEQ)) {
            // Elementwise, each leaf from its old value, read before the
            // operand runs, and the operand's.
            auto old = Temp(t);
            tyg_copy(p, AtL(old), pl.a, M.Size(t));
            auto ro = ElemOperand(a->rhs, t);
            if (Ended()) return;
            ElemLeaves(t, 0, a->line, [&](TypeExpr *tt, int64_t off, const ElemDyn &dy) {
                auto was = tyg_load(p, M.Scal(tt), ElemAt(AtL(old), off, dy), 0);
                tyg_store(p, M.Scal(tt), ElemAt(pl.a, off, dy),
                          ScalarArith(a->op, tt, was, ElemLeaf(ro, tt, off, dy, a->line), a->line), 0);
            });
            return;
        }
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
            n = tyg_bin(p, TYG_AND, n, I64(IntBits(t->intstorage) - 1));
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
        tyg_store(p, s, AtL(ctr), lo, 0);
        tyg_store(p, s, AtL(hiv), hi, 0);
        tyg_local ix = 0;
        tyg_scope_begin(p);
        if (f->idxdef) {
            ix = tyg_local_new(p, 8, 8);
            StL(ix, I64(0));
        }
        if (f->vdef) Declare(f->vdef, f->line);
        if (f->idxdef) Declare(f->idxdef, f->line);
        auto head = NewBlock(), body = NewBlock(), step = NewBlock(),
             done = NewBlock();
        tyg_jump(p, head);
        SetBlock(head);
        Loc(f->line);
        auto cv = tyg_load(p, s, AtL(ctr), 0);
        auto hv = tyg_load(p, s, AtL(hiv), 0);
        Branch(Cmp(TgModule::IsSigned(ct) ? TYG_SLT : TYG_ULT, cv, hv), body, done);
        SetBlock(body);
        if (f->vdef)
            tyg_store(p, M.Scal(f->vdef->type), AtL(locals[f->vdef]),
                      Conv(cv, ct, f->vdef->type, f->line), 0);
        if (f->idxdef)
            tyg_store(p, M.Scal(f->idxdef->type), AtL(locals[f->idxdef]),
                      Conv(LdL(ix), M.ast.inttypes[IS_I64],
                           f->idxdef->type, f->line), 0);
        loops.push_back({ done, step, sscopes.size() });
        GenBlock(f->body, true);
        loops.pop_back();
        Jump(step);
        SetBlock(step);
        Loc(f->line);
        auto c2 = tyg_load(p, s, AtL(ctr), 0);
        tyg_store(p, s, AtL(ctr), Add(c2, tyg_iconst(p, s, 1)), 0);
        if (f->idxdef) {
            auto i2 = LdL(ix);
            StL(ix, Add(i2, I64(1)));
        }
        tyg_jump(p, head);
        SetBlock(done);
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
        auto gi = Local(I64(0));
        // Elements of variable size are walked with a cursor, each one's
        // bytes on from the last (StepElem); the loop variable is the
        // element's address, as a T[] local is its image's.
        auto itt = f->iter->exprtype;
        while (itt && IsAddrRef(itt)) itt = itt->ref->sub;
        TypeExpr *ielem = !itt ? nullptr : itt->kind == TY_ARRAY ? itt->arr->sub
                        : itt->kind == TY_SLICE ? itt->sub : nullptr;
        auto walk = ielem && !cg.IsFix(ielem);
        // A varint element, or a relative reference one, is bound by its value, decoded.
        auto bindptr = walk && !TgModule::IsVarint(ielem) && !TgModule::IsVr(ielem);
        tyg_local cl = 0;
        tyg_scope_begin(p);
        if (f->vdef && bindptr) {
            auto loc = tyg_local_new(p, 8, 8);
            locals[f->vdef] = loc;
            // A reference variable holds the address itself; a value one is
            // read through it, as a T[] local is.
            if (!IsAddrRef(f->vdef->type)) indirect.insert(f->vdef);
            tyg_debug_var(p, string(f->vdef->name).c_str(), tyg_dt_pointer(m, M.DT(ielem)), AtL(loc),
                          M.File(f->vdef->line.fileidx), f->vdef->line.line);
        } else if (f->vdef) {
            Declare(f->vdef, f->line);
        }
        if (f->idxdef) Declare(f->idxdef, f->line);
        auto head = NewBlock(), body = NewBlock(), step = NewBlock(),
             done = NewBlock();
        tyg_local wb = 0;
        if (walk) {
            cl = tyg_local_new(p, 8, 8);
            wb = tyg_local_new(p, 8, 8);
            auto e0 = ViewOf(at(), f->line).elems;
            StL(cl, e0);
            StL(wb, e0);
        }
        tyg_jump(p, head);
        SetBlock(head);
        Loc(f->line);
        auto v = ViewOf(at(), f->line);
        if (walk && path) {
            // The path re-pointed: the cursor walks to the same index from
            // the start of the elements it leads to now.
            auto moved = NewBlock(), rewalk = NewBlock(), more = NewBlock(), same = NewBlock();
            Branch(Cmp(TYG_NE, v.elems, LdL(wb)), moved, same);
            SetBlock(moved);
            StL(wb, v.elems);
            StL(cl, v.elems);
            auto k = Local(I64(0));
            tyg_jump(p, rewalk);
            SetBlock(rewalk);
            Branch(Cmp(TYG_SLT, LdL(k), LdL(gi)), more, same);
            SetBlock(more);
            auto c1 = LdL(cl);
            StL(cl, Add(c1, ImgSize(ielem, c1)));
            StL(k, Add(LdL(k), I64(1)));
            tyg_jump(p, rewalk);
            SetBlock(same);
            v = ViewOf(at(), f->line);
        }
        auto i = LdL(gi);
        Branch(Cmp(TYG_SLT, i, v.len), body, done);
        SetBlock(body);
        auto ea = walk ? LdL(cl) : ElemAddr(v, i);
        if (f->vdef && bindptr) {
            StL(locals[f->vdef], ea);
        } else if (f->vdef) {
            auto vt = f->vdef->type;
            P dst { AtL(locals[f->vdef]), vt };
            if (vt->kind == TY_REF && v.elem->kind != TY_REF) {
                if (!IsPlainRef(vt)) No(f->line, "binding kind");
                St(dst.a, ea);
            } else {
                Store(dst, Fit(Load(ElemPlace(ea, v.elem), f->line), vt, f->line));
            }
        }
        if (f->idxdef)
            tyg_store(p, M.Scal(f->idxdef->type), AtL(locals[f->idxdef]),
                      Conv(i, M.ast.inttypes[IS_I64], f->idxdef->type, f->line), 0);
        loops.push_back({ done, step, sscopes.size() });
        GenBlock(f->body, true);
        loops.pop_back();
        Jump(step);
        SetBlock(step);
        Loc(f->line);
        auto i2 = LdL(gi);
        StL(gi, Add(i2, I64(1)));
        if (walk) {
            auto c0 = LdL(cl);
            StL(cl, Add(c0, ImgSize(ielem, c0)));
        }
        tyg_jump(p, head);
        SetBlock(done);
        tyg_scope_end(p);
    }

    // ---- Frame objects ---------------------------------------------------------

    // A fixed field at its offset in the C struct; the tail, a resizable
    // whose header is the struct's last member, on the frame object's stack.
    P FoFieldPlace(const P &base, int fi, Line l, bool read = false) {
        if (base.bfo) return BfoFieldPlace(base, fi, l, read);
        auto fl = M.FoLayout(base.t);
        auto si = cg.SI(base.t);
        auto a = base.a;
        if (fi == fl.tailfi) {
            if (!base.stk && !read) No(l, "frame object without its stack");
            a.offset += fl.tail;
            return P { a, si->ftypes[(size_t)fi], base.stk };
        }
        a.offset += fl.offs[(size_t)fi];
        return P { a, si->ftypes[(size_t)fi] };
    }
    // A frame object built at pl, its tail on pl.stk: a literal's fields
    // where they lie, the tail built in place (CodeGen's GenConstruct).
    void GenFo(Node *n, P pl, Line l) {
        if (auto ie = Is<IfExpr>(n); ie && ie->elseb) {
            BuildIf(ie, l, [&](Node *v) { GenFo(v, pl, l); });
            return;
        }
        if (auto b = Is<Block>(n); b && b->tail) {
            BuildArm(b, l, [&](Node *v) { GenFo(v, pl, l); });
            return;
        }
        if (auto c = Is<Call>(n); c && c->fvbody && c->fvbody->tail) {
            BuildFv(c, l, [&](Node *v) { GenFo(v, pl, l); });
            return;
        }
        auto sl = Is<StructLit>(n);
        if (!sl && n->exprtype && cg.TEq(n->exprtype, pl.t)) {
            // Any other one copied: a call's result, a variable.
            P src;
            if (Is<Call>(n)) {
                auto x = Gen1(n);
                src = P { x.a, x.t, x.stk };
            } else {
                src = Deref(Place(n));
            }
            if (Ended()) return;
            FoCopy(pl, src, l);
            return;
        }
        if (!sl || !sl->sinst || sl->variant) No(l, cat("value of type ", M.NameWhy(pl.t)));
        auto si = cg.SI(pl.t);
        auto fl = M.FoLayout(pl.t);
        for (size_t i = 0; i < si->st->fields.size(); i++) {
            if (si->st->fields[i].ispad) continue;
            auto init = sl->InitFor((int)i);
            auto fp = FoFieldPlace(pl, (int)i, l);
            if ((int)i == fl.tailfi && M.FoOk(fp.t)) {
                // A nested frame object, its tail on the same stack.
                if (init) {
                    GenFo(init, fp, l);
                } else {
                    tyg_zero(p, fp.a, M.Size(fp.t));
                    auto h = fp.a;
                    h.offset += M.FoHdrOff(fp.t);
                    St(h, Top(pl.stk));
                }
                if (Ended()) return;
                continue;
            }
            if ((int)i == fl.tailfi) {
                if (init) GenRz(init, pl.stk, fp.a, fp.t, l);
                else {
                    St(fp.a, Top(pl.stk));
                    auto len = fp.a;
                    len.offset += 8;
                    St(len, I64(0));
                }
                continue;
            }
            if (init) Into(fp, init);
            else tyg_zero(p, fp.a, M.Size(fp.t));
            if (Ended()) return;
        }
    }

    // The frame object at src copied to dst: its fixed fields, then its
    // tail's elements onto dst's stack.
    void FoCopy(const P &dst, const P &src0, Line l) {
        if (src0.bfo) {
            FoCopy(dst, FoView(src0), l);
            return;
        }
        const P &src = src0;
        auto fl = M.FoLayout(dst.t);
        if (!dst.stk || !src.stk) No(l, "frame object without its stack");
        auto tl = FoFieldPlace(src, fl.tailfi, l);
        if (M.FoOk(tl.t)) {
            if (fl.tail) tyg_move(p, dst.a, src.a, fl.tail);
            FoCopy(FoFieldPlace(dst, fl.tailfi, l), tl, l);
            return;
        }
        auto v = ViewOf(tl, l);
        // The tail's start and count first: the source may be anywhere.
        auto el = tyg_local_new(p, 24, 8);
        StL(el, v.elems);
        StL(el, v.len, 8);
        // Elements of variable size (varints) by their bytes.
        if (!cg.IsFix(v.elem)) StL(el, Sub(RunEnd(v), v.elems), 16);
        if (fl.tail) tyg_move(p, dst.a, src.a, fl.tail);
        auto dt = FoFieldPlace(dst, fl.tailfi, l);
        auto len = dt.a;
        len.offset += 8;
        St(dt.a, Top(dst.stk));
        St(len, I64(0));
        if (!cg.IsFix(v.elem)) {
            auto top = Top(dst.stk);
            auto bytes = LdL(el, 16);
            tyg_move_dyn(p, top, LdL(el), bytes);
            SetTop(dst.stk, Add(top, bytes));
            St(len, LdL(el, 8));
            return;
        }
        CopyRun(dst.stk, len, dt.t->arr->sub, LdL(el), LdL(el, 8));
    }

    // ---- Frame objects as bytes ---------------------------------------------------

    // Field fi of the frame object at base.a laid out as bytes
    // (CodeGen::FoBytes): its fixed fields packed in order, an explicit pad
    // taking its bytes and a bare one none, the tail where they end.
    P BfoFieldPlace(const P &base, int fi, Line l, bool read = false) {
        auto si = cg.SI(base.t);
        auto fl = M.FoLayout(base.t);
        auto &fields = si->st->fields;
        int64_t off = 0;
        for (int i = 0; i < fi; i++) {
            if (fields[(size_t)i].ispad) {
                off += fields[(size_t)i].padsize > 0 ? fields[(size_t)i].padsize : 0;
                continue;
            }
            off += cg.FixedSize(si->ftypes[(size_t)i]);
        }
        auto a = base.a;
        a.offset += off;
        auto ft = si->ftypes[(size_t)fi];
        if (fi != fl.tailfi) return P { a, ft };
        if (M.FoOk(ft)) {
            P r { a, ft, base.stk };
            r.bfo = true;
            r.ohdr = base.ohdr;
            return r;
        }
        if (!base.stk && !read) No(l, "frame object without its stack");
        P r { base.ohdr, ft, base.stk };
        r.rbase = tyg_addr_of(p, a);
        return r;
    }
    // Frame object `obj`'s fixed fields against the bytes at `at`: stored
    // there (explicit pads zeroed), or loaded back (CodeGen::FoBytes).
    void FoBytesCopy(TypeExpr *t, tyg_addr obj, tyg_addr at, bool tobytes) {
        auto si = cg.SI(t);
        auto fl = M.FoLayout(t);
        auto &fields = si->st->fields;
        for (size_t i = 0; i < fields.size(); i++) {
            if (fields[i].ispad) {
                if (fields[i].padsize <= 0) continue;
                if (tobytes) tyg_zero(p, at, fields[i].padsize);
                at.offset += fields[i].padsize;
                continue;
            }
            auto ft = si->ftypes[i];
            if (cg.IsResz(ft)) {
                if (cg.IsFrameObj(ft)) {
                    auto o = obj;
                    o.offset += fl.tail;
                    FoBytesCopy(ft, o, at, tobytes);
                }
                return;
            }
            auto o = obj;
            o.offset += fl.offs[i];
            auto n = cg.FixedSize(ft);
            if (tobytes) tyg_copy(p, at, o, n);
            else tyg_copy(p, o, at, n);
            at.offset += n;
        }
    }
    // The bytes frame object at src read as the C frame object a whole read
    // takes (CodeGen::FoView): its fixed fields loaded, its innermost tail's
    // header pointing at the elements where they lie.
    P FoView(const P &src) {
        auto t = src.t;
        auto tmp = AtL(tyg_local_new(p, M.Size(t), 8));
        tyg_zero(p, tmp, M.Size(t));
        FoBytesCopy(t, tmp, src.a, false);
        auto hb = tmp;
        hb.offset += M.FoHdrOff(t);
        auto ol = src.ohdr;
        ol.offset += 8;
        auto at = src.a;
        St(hb, Add(tyg_addr_of(p, at), I64(cg.FoBytesPrefix(t))));
        hb.offset += 8;
        St(hb, Ld(ol));
        return P { tmp, t, src.stk };
    }
    // Builds n's value, of frame object type t, at the top of stk as the
    // tail of a value that is not one: its fields as bytes, its innermost
    // tail's count into olen (CodeGen::GenFoAsBytes). A literal builds that
    // way directly; anything else is built as the frame object it is
    // everywhere else, its elements behind room left for the fields, which
    // are stored there after it.
    void GenFoBytes(Node *n, tyg_val stk, TypeExpr *t, tyg_addr olen, Line l) {
        if (auto ie = Is<IfExpr>(n); ie && ie->elseb) {
            BuildIf(ie, l, [&](Node *v) { GenFoBytes(v, stk, t, olen, l); });
            return;
        }
        if (auto b = Is<Block>(n); b && b->tail) {
            BuildArm(b, l, [&](Node *v) { GenFoBytes(v, stk, t, olen, l); });
            return;
        }
        auto si = cg.SI(t);
        auto fl = M.FoLayout(t);
        if (auto sl = Is<StructLit>(n); sl && sl->sinst && !sl->variant && cg.TEq(sl->exprtype, t)) {
            RunLit(sl, stk, si->st->fields, si->ftypes, (size_t)fl.tailfi);
            if (Ended()) return;
            auto tt = si->ftypes[(size_t)fl.tailfi];
            auto init = sl->InitFor(fl.tailfi);
            if (M.FoOk(tt)) {
                if (!init) No(l, "omitted frame object tail");
                GenFoBytes(init, stk, tt, olen, l);
                return;
            }
            auto th = AtL(tyg_local_new(p, 16, 8));
            auto tlen = th;
            tlen.offset += 8;
            if (init) {
                GenRz(init, stk, th, tt, l);
                if (Ended()) return;
            } else {
                St(th, Top(stk));
                St(tlen, I64(0));
            }
            St(olen, Ld(tlen));
            return;
        }
        auto pre = cg.FoBytesPrefix(t);
        auto at = tyg_local_new(p, 8, 8);
        auto top = Top(stk);
        StL(at, top);
        SetTop(stk, Add(top, I64(pre)));
        auto h = AtL(tyg_local_new(p, M.Size(t), 8));
        GenFo(n, P { h, t, stk }, l);
        if (Ended()) return;
        FoBytesCopy(t, h, AtV(LdL(at)), true);
        auto hl = h;
        hl.offset += M.FoHdrOff(t) + 8;
        St(olen, Ld(hl));
    }

    // ---- Resizable structs --------------------------------------------------------
    //
    // A struct with a variable-size prefix and a resizable tail (TgModule::
    // RsOk) is one image on a data stack, its header a gs_rhdr as a T[>..]'s
    // is: the fields before the tail packed as a variable-size struct's, the
    // tail's elements behind them, the count in the header.

    // Field fi of the struct at pl (its header). A prefix field is where
    // the walk over those before it lands (CodeGen::FieldPtr); the tail,
    // where the walk ends, its count the header's (CodeGen::BytesLoc).
    P RsFieldPlace(const P &base, int fi, Line l, bool read = false) {
        auto si = cg.SI(base.t);
        auto ft = si->ftypes[(size_t)fi];
        auto at = VsField(base.t, Ld(base.a), fi);
        if (fi == LastRealField(si->st->fields)) {
            if (!base.stk && !read) No(l, "resizable struct without its stack");
            if (M.FoOk(ft)) {
                P r { AtV(at), ft, base.stk };
                r.bfo = true;
                r.ohdr = base.a;
                return r;
            }
            P r { base.a, ft, base.stk };
            r.rbase = at;
            return r;
        }
        P r { AtV(at), ft };
        r.enc = TgModule::IsVarint(ft);
        return r;
    }
    // A resizable struct built at the top of stk, its header at hdr
    // (CodeGen::GenConstruct): a literal's fields in layout order then the
    // tail's elements, a call's result built there, anything else a copy.
    void GenRs(Node *n, tyg_val stk, tyg_addr hdr, TypeExpr *t, Line l) {
        if (auto c = Is<Call>(n); c && c->builtin == B_COPY && c->ArgNodes().size() == 1) n = c->ArgNodes()[0];
        if (auto ie = Is<IfExpr>(n); ie && ie->elseb) {
            BuildIf(ie, l, [&](Node *v) { GenRs(v, stk, hdr, t, l); });
            return;
        }
        if (auto b = Is<Block>(n); b && b->tail) {
            BuildArm(b, l, [&](Node *v) { GenRs(v, stk, hdr, t, l); });
            return;
        }
        if (auto c = Is<Call>(n); c && c->fvbody && c->fvbody->tail) {
            BuildFv(c, l, [&](Node *v) { GenRs(v, stk, hdr, t, l); });
            return;
        }
        auto len = hdr;
        len.offset += 8;
        if (auto sl = Is<StructLit>(n); sl && sl->sinst && !sl->variant && cg.TEq(sl->exprtype, t)) {
            auto si = cg.SI(t);
            auto last = LastRealField(si->st->fields);
            auto start = Local(Top(stk));
            RunLit(sl, stk, si->st->fields, si->ftypes, (size_t)last);
            if (Ended()) return;
            auto th = AtL(tyg_local_new(p, 16, 8));
            auto tlen = th;
            tlen.offset += 8;
            auto init = sl->InitFor(last);
            if (M.FoOk(si->ftypes[(size_t)last])) {
                // A frame object's fields as bytes, its count the header's.
                if (!init) No(l, "omitted frame object tail");
                GenFoBytes(init, stk, si->ftypes[(size_t)last], len, l);
                if (Ended()) return;
                St(hdr, LdL(start));
                return;
            }
            if (init) {
                GenRz(init, stk, th, si->ftypes[(size_t)last], l);
                if (Ended()) return;
            } else {
                St(th, Top(stk));
                St(tlen, I64(0));
            }
            St(hdr, LdL(start));
            St(len, Ld(tlen));
            return;
        }
        if (auto c = Is<Call>(n); DestSpec(c) && cg.Mangle(DestSpec(c)->rets[0]) == cg.Mangle(t)) {
            RzDst d { stk, hdr };
            rzdst = &d;
            GenCall(c);
            rzdst = nullptr;
            return;
        }
        auto pl = Deref(Place(n));
        if (Ended()) return;
        if (!cg.TEq(pl.t, t)) No(l, cat("value of type ", M.NameWhy(pl.t), " as ", M.Name(t)));
        RsCopy(stk, hdr, pl, l);
    }
    // The image at src copied to the top of stk, header at hdr: its prefix
    // and its elements, bytes as they lie.
    void RsCopy(tyg_val stk, tyg_addr hdr, const P &src, Line l) {
        auto si = cg.SI(src.t);
        auto last = LastRealField(si->st->fields);
        auto tail = si->ftypes[(size_t)last];
        auto sa = src.a;
        sa.offset += 8;
        auto from = Ld(src.a);
        auto n = Ld(sa);
        // To where the elements start: past a frame object tail's fields.
        auto estart = VsField(src.t, from, last);
        if (M.FoOk(tail)) estart = Add(estart, I64(cg.FoBytesPrefix(tail)));
        auto bytes = Add(Sub(estart, from), Mul(n, I64(cg.FixedSize(M.RsElem(src.t)))));
        auto top = Top(stk);
        tyg_move_dyn(p, top, from, bytes);
        SetTop(stk, Add(top, bytes));
        auto len = hdr;
        len.offset += 8;
        St(hdr, top);
        St(len, n);
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
        if (cg.IsFix(t)) return I64(cg.FixedSize(t));
        if (TgModule::IsVarint(t) || TgModule::IsVr(t)) return UlebSize(at);   // Zigzag has ULEB128's shape.
        if (M.IsVv(t)) return Sub(VvField(t, at, -1), at);
        if (M.IsVm(t)) {
            // The tag, and its variant's payload: a variable one walked,
            // only on its own tag's path.
            auto ei = cg.EIOf(t);
            auto ts = M.TagSize(t);
            auto res = tyg_local_new(p, 8, 8);
            auto tag = Local(LoadTag(AtV(at), t));
            auto done = NewBlock();
            for (size_t vi = 0; vi < ei->en->variants.size(); vi++) {
                auto vt = cg.VariantType(t, (int)vi);
                auto hit = NewBlock(), next = NewBlock();
                Branch(Cmp(TYG_EQ, LdL(tag), I64((int64_t)vi)), hit, next);
                SetBlock(hit);
                auto pay = Add(at, I64(ts));
                auto ps = cg.IsFix(vt) ? I64(cg.FixedSize(vt)) : ImgSize(vt, pay);
                StL(res, Add(ps, I64(ts)));
                tyg_jump(p, done);
                SetBlock(next);
            }
            StL(res, I64(ts));
            tyg_jump(p, done);
            SetBlock(done);
            return LdL(res);
        }
        if (M.IsRc(t)) {
            auto cap = LimCapOf(P { AtV(at), t });
            return Add(Mul(cap, I64(cg.FixedSize(t->arr->sub))), I64(8));
        }
        if (IsVarArr(t)) {
            auto v = ViewOf(P { AtV(at), t }, Line {});
            if (!cg.IsFix(t->arr->sub)) return Sub(RunEnd(v), at);
            auto bytes = Mul(v.len, I64(cg.FixedSize(t->arr->sub)));
            return Add(Sub(v.elems, at), bytes);
        }
        return Sub(VsField(t, at, -1), at);
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
            if (off) cur = Add(cur, I64(off));
            off = 0;
            cur = Add(cur, ImgSize(ft, cur));
        }
        return off ? Add(cur, I64(off)) : cur;
    }
    P VsFieldPlace(const P &base, int fi) {
        auto si = cg.SI(base.t);
        auto ft = si->ftypes[(size_t)fi];
        P r { AtV(VsField(base.t, tyg_addr_of(p, base.a), fi)), ft };
        r.enc = TgModule::IsVarint(ft);
        return r;
    }

    // Copies the image of type t at src to the top of stk; its new address.
    // The source may lie on stk itself, below its top.
    tyg_val ImgCopy(tyg_val stk, tyg_val src, TypeExpr *t) {
        auto top = Top(stk);
        auto size = ImgSize(t, src);
        tyg_move_dyn(p, top, src, size);
        SetTop(stk, Add(top, size));
        return top;
    }
    // A result of a call delivered elsewhere (a multi-value one), copied.
    tyg_val VsCopy(tyg_val stk, const V &x, TypeExpr *t, Line l) {
        if (!x.mem || !x.t || !cg.TEq(x.t, t)) No(l, cat("value of type ", M.NameWhy(t)));
        return ImgCopy(stk, tyg_addr_of(p, x.a), t);
    }

    // A variant or a fixed-mode ADT of the variable-mode ADT t's variants,
    // lying at `src`, adapted to t at the top of stk (CodeGen::
    // GenVarEnumFromLoc): the tag, then the payload; its address.
    tyg_val VmFrom(tyg_val stk, tyg_addr src, TypeExpr *srct, TypeExpr *t, Line l) {
        auto start = Local(Top(stk));
        auto at = [&] { return LdL(start); };
        {
            if (M.EI(srct) != M.EI(t) || (srct->kind != TY_VARIANT && !(srct->kind == TY_ENUM && !srct->enu->varmode)))
                No(l, cat("value of type ", M.NameWhy(t)));
            auto base = at();
            auto ts = M.TagSize(t);
            SetTop(stk, Add(base, I64(ts)));
            auto ei = M.EI(t);
            auto copyfix = [&](int64_t sz, tyg_addr from) {
                if (!sz) return;
                auto top = Top(stk);
                tyg_copy(p, AtV(top), from, sz);
                SetTop(stk, Add(top, I64(sz)));
            };
            if (srct->kind == TY_VARIANT) {
                StoreTag(AtV(base), t, M.VariantIdx(srct));
                if (cg.IsFix(srct)) copyfix(cg.FixedSize(srct), src);
                else ImgCopy(stk, tyg_addr_of(p, src), srct);
                return at();
            }
            auto tag = Local(LoadTag(src, srct));
            tyg_store(p, M.TagScal(t), AtV(base), tyg_convert(p, TYG_TRUNC, M.TagScal(t), LdL(tag)), 0);
            auto done = NewBlock();
            for (size_t vi = 0; vi < ei->en->variants.size(); vi++) {
                auto psz = cg.VariantLayout(ei, (int)vi).size;
                auto hit = NewBlock(), next = NewBlock();
                Branch(Cmp(TYG_EQ, LdL(tag), I64((int64_t)vi)), hit, next);
                SetBlock(hit);
                auto from = src;
                from.offset += ts;
                copyfix(psz, from);
                tyg_jump(p, done);
                SetBlock(next);
            }
            tyg_jump(p, done);
            SetBlock(done);
            return at();
        }
    }

    // Builds n's value, of the variable-size struct t, at the top of stk
    // (CodeGen::GenConstruct); its address. A literal field by field, a
    // call returning one handed stk as its destination, anything else (a
    // variable, a field, a pointee) copied whole.
    // Builds n's value, of the variable-mode ADT t, at the top of stk; its
    // address. A variant literal as its tag then its payload fields, a call
    // returning one handed stk, anything else copied whole.
    tyg_val GenVm(Node *n, tyg_val stk, TypeExpr *t, Line l) {
        auto start = Local(Top(stk));
        auto at = [&] { return LdL(start); };
        if (auto c = Is<Call>(n); DestSpec(c) && cg.Mangle(DestSpec(c)->rets[0]) == cg.Mangle(t)) {
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
            SetTop(stk, Add(base, I64(M.TagSize(t))));
            StoreTag(AtV(base), t, (int)vi);
            auto outer = selfbase;   // `self` is the value's start (§3.9).
            selfbase = base;
            RunLit(sl, stk, ei->en->variants[vi].fields, ei->vftypes[vi]);
            selfbase = outer;
            return at();
        }
        if (auto sl = Is<StructLit>(n); sl && sl->variant) {
            auto ei = cg.EIOf(t);
            auto vi = ei->en->VariantIndex(sl->variant);
            auto vt = cg.VariantType(t, vi);
            auto ts = M.TagSize(t);
            auto size = ts + cg.FixedSize(vt);
            auto base = at();
            SetTop(stk, Add(base, I64(size)));
            tyg_zero(p, AtV(base), size);
            StoreTag(AtV(base), t, vi);
            auto outer = selfbase;
            selfbase = base;
            struct Back {
                tyg_val &at, was;
                ~Back() { at = was; }
            } back { selfbase, outer };
            auto &lay = cg.VariantLayout(ei, vi);
            for (size_t i = 0; i < ei->en->variants[(size_t)vi].fields.size(); i++) {
                if (ei->en->variants[(size_t)vi].fields[i].ispad) continue;
                auto init = sl->InitFor((int)i);
                if (!init) continue;
                Into(P { AtV(at(), ts + lay.offs[i]), ei->vftypes[(size_t)vi][i] }, init);
                if (Ended()) return at();
            }
            return at();
        }
        if (auto d = Is<Dot>(n); d && d->variantconst) {
            // A variant without fields: its tag, its payload's zero bytes.
            auto ei = cg.EIOf(t);
            auto vi = ei->en->VariantIndex(d->variantconst);
            auto vt = cg.VariantType(t, vi);
            if (!cg.IsFix(vt)) No(l, cat("variant constant of type ", M.NameWhy(t)));
            auto size = M.TagSize(t) + cg.FixedSize(vt);
            auto base = at();
            SetTop(stk, Add(base, I64(size)));
            tyg_zero(p, AtV(base), size);
            StoreTag(AtV(base), t, vi);
            return at();
        }
        auto pl = Deref(Place(n));
        if (Ended()) return at();
        if (!cg.TEq(pl.t, t)) return VmFrom(stk, pl.a, pl.t, t, l);
        return ImgCopy(stk, tyg_addr_of(p, pl.a), t);
    }

    // A variable-size variant's value at the top of stk: a literal's fields
    // in turn, a call's built there, anything else copied whole.
    tyg_val GenVv(Node *n, tyg_val stk, TypeExpr *t, Line l) {
        auto start = Local(Top(stk));
        auto at = [&] { return LdL(start); };
        if (auto sl = Is<StructLit>(n); sl && cg.TEq(sl->exprtype, t)) {
            auto ei = cg.EIVar(t);
            auto vi = (size_t)ei->en->VariantIndex(t->var->variant);
            RunLit(sl, stk, ei->en->variants[vi].fields, ei->vftypes[vi]);
            return at();
        }
        if (auto c = Is<Call>(n); DestSpec(c) && cg.TEq(DestSpec(c)->rets[0], t)) {
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
        auto start = Local(Top(stk));
        auto at = [&] { return LdL(start); };
        if (auto c = Is<Call>(n); c && c->builtin == B_COPY && c->ArgNodes().size() == 1) n = c->ArgNodes()[0];
        if (auto sl = Is<StructLit>(n); sl && sl->sinst && !sl->variant && cg.TEq(sl->exprtype, t)) {
            VsLit(sl, stk, t);
            return at();
        }
        if (auto c = Is<Call>(n); DestSpec(c) && cg.TEq(DestSpec(c)->rets[0], t)) {
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
    void RunLit(StructLit *sl, tyg_val stk, const vector<Field> &fields, const vector<TypeExpr *> &ftypes,
                size_t count = (size_t)-1) {
        auto zero = [&](int64_t n) {
            auto top = Top(stk);
            tyg_zero(p, AtV(top), n);
            SetTop(stk, Add(top, I64(n)));
        };
        for (size_t i = 0; i < fields.size() && i < count && !Ended(); i++) {
            auto &f = fields[i];
            if (f.ispad) {
                if (f.padsize > 0) zero(f.padsize);
                continue;
            }
            auto ft = ftypes[i];
            auto init = sl->InitFor((int)i);
            if (!cg.IsFix(ft)) {
                if (!init && TgModule::IsVr(ft)) {
                    zero(1);   // An omitted optional: the one zero byte.
                    continue;
                }
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
            Store(P { AtV(top), ft }, x);
            SetTop(stk, Add(top, I64(cg.FixedSize(ft))));
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
        for (size_t i = 0; i < sp->argtypes.size(); i++) {
            auto t = sp->argtypes[i];
            if (cg.IsPoolParam(sp, i)) {
                auto vp = tyg_dt_pointer(m, M.DT(M.ast.inttypes[IS_U8]));
                for (int k = 0; k < 4; k++) pdt.push_back(vp);
                continue;
            }
            if (M.RzParam(t) && !M.Resz(t)) pdt.push_back(tyg_dt_pointer(m, M.DT(t)));
            else if (cg.IsLargeFixed(t)) pdt.push_back(tyg_dt_pointer(m, M.DT(t)));
            else pdt.push_back(M.DT(t));
            if (M.RzParam(t) && M.Resz(t)) pdt.push_back(tyg_dt_pointer(m, M.StackDT()));
        }
        for (auto fv : cg.sinfo[sp].freevars) {
            pdt.push_back(tyg_dt_pointer(m, M.DT(fv->type)));
            if (M.Resz(fv->type)) pdt.push_back(tyg_dt_pointer(m, M.StackDT()));
            if (fv->reusable) {
                pdt.push_back(tyg_dt_pointer(m, M.DT(fv->type)));
                pdt.push_back(tyg_dt_pointer(m, M.StackDT()));
            }
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
        for (size_t i = 0; i < sp->argtypes.size(); i++) {
            auto t = sp->argtypes[i];
            pi += cg.IsPoolParam(sp, i) ? 4 : M.RzParam(t) && M.Resz(t) ? 2 : 1;
        }
        auto param = [&] {
            auto loc = Local(tyg_param(p, pi++));
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
            tyg_debug_var_ref(p, string(fv->name).c_str(), M.DT(fv->type), AtL(loc),
                              M.File(fv->line.fileidx), fv->line.line);
            if (M.Resz(fv->type)) rzstk[fv] = param();
            if (fv->reusable) {
                // A pool, as gs_pref: its freelist's header and stack too.
                auto pr = tyg_local_new(p, 32, 8);
                StL(pr, LdL(loc));
                StL(pr, LdL(rzstk[fv]), 8);
                StL(pr, tyg_param(p, pi++), 16);
                StL(pr, tyg_param(p, pi++), 24);
                prefs[fv] = pr;
            }
        }
        int afterfv = pi;
        pi = 0;
        for (size_t i = 0; i < sp->params.size(); i++) {
            auto vd = sp->params[i];
            auto t = sp->argtypes[i];
            if (cg.IsPoolParam(sp, i)) {
                // gs_pref { hdr, stk, fl, flstk }: a fat reference, then
                // the freelist's header and stack.
                auto loc = tyg_local_new(p, 32, 8);
                for (int k = 0; k < 4; k++) StL(loc, tyg_param(p, pi++), 8 * k);
                locals[vd] = loc;
                prefs[vd] = loc;
                tyg_debug_param(p, string(vd->name).c_str(), M.DT(t), AtL(loc),
                                M.File(vd->line.fileidx), vd->line.line);
                continue;
            }
            auto pv = tyg_param(p, pi++);
            if (M.RzParam(t) && !M.Resz(t)) {
                // A by-value T[]: its image's address, the image a copy the
                // caller made.
                auto loc = tyg_local_new(p, 8, 8);
                locals[vd] = loc;
                indirect.insert(vd);
                StL(loc, pv);
                tyg_debug_param(p, string(vd->name).c_str(), tyg_dt_pointer(m, M.DT(t)),
                                AtL(loc), M.File(vd->line.fileidx), vd->line.line);
                continue;
            }
            if (cg.IsLargeFixed(t)) {
                // The caller's copy, read through its address.
                auto loc = tyg_local_new(p, 8, 8);
                locals[vd] = loc;
                indirect.insert(vd);
                StL(loc, pv);
                tyg_debug_param(p, string(vd->name).c_str(), tyg_dt_pointer(m, M.DT(t)),
                                AtL(loc), M.File(vd->line.fileidx), vd->line.line);
                continue;
            }
            auto psize = M.RzParam(t) && (IsGrow(t) || M.IsRs(t)) ? 16 : M.Size(t);
            auto loc = tyg_local_new(p, psize, 8);
            locals[vd] = loc;
            if (M.IsScalar(t)) tyg_store(p, M.Scal(t), AtL(loc), pv, 0);
            else tyg_copy(p, AtL(loc), AtV(pv), psize);
            tyg_debug_param(p, string(vd->name).c_str(), M.DT(t), AtL(loc),
                            M.File(vd->line.fileidx), vd->line.line);
            if (M.RzParam(t)) {
                // A by-value T[>..]: its header, then its stack.
                auto sl = Local(tyg_param(p, pi++));
                rzstk[vd] = sl;
            }
        }
        pi = afterfv;
        chans.resize(sp->rets.size());
        for (size_t i = 0; i < sp->rets.size(); i++) {
            if (M.Resz(sp->rets[i])) {
                chans[i].a = param();
                chans[i].b = param();
            } else if (M.IsImg(sp->rets[i]) || (int)i != cret) {
                chans[i].a = param();
            }
        }
        if (cg.sinfo[sp].needssp) {
            spl = param();
            hassp = true;
        }
        // Each destination's top on entry: a return builds its result
        // there, also one nested in another return's value that has
        // started building (`return str("x", if c { return "y"; } ...)`).
        for (size_t i = 0; i < sp->rets.size(); i++) {
            if (!M.Resz(sp->rets[i]) && !IsImg(sp->rets[i])) continue;
            chans[i].top0 = tyg_local_new(p, 8, 8);
            StL(chans[i].top0, Top(LdL(chans[i].a)));
        }
        gblk = NewBlock();
        gstart = NewBlock();
        tyg_jump(p, gblk);
        SetBlock(gstart);
        if (cg.fromids.count(sp)) {
            // A `return from` target: its destinations are where values
            // in flight to it are built (CodeGen::EmitSpec).
            for (size_t i = 0; i < sp->rets.size(); i++) {
                if (!IsImg(sp->rets[i]) && !M.Resz(sp->rets[i])) continue;
                auto at = ChanAt(sp, i, "fdst");
                auto sav = Local(Ld(at));
                St(at, LdL(chans[i].a));
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

    // The render procedure of t (CodeGen::EmitRenderFn): void(stk, &len,
    // &value), the value rendered as a nested part of a larger one.
    void EmitRender(const string &name, TypeExpr *t) {
        auto ptr = tyg_type_ptr(m);
        const tyg_type *pts[3] = { ptr, ptr, ptr };
        p = tyg_proc_begin(m, M.Sym(name, TYG_SYM_FUNC | TYG_SYM_GLOBAL), tyg_sig_new(m, nullptr, pts, 3, 0));
        if (!p) No(Line {}, cat("tyg_proc_begin: ", tyg_error(m) ? tyg_error(m) : "?"));
        // The entry block as a function's, for the globals a link reads
        // (a pool's base).
        gblk = NewBlock();
        gstart = NewBlock();
        tyg_jump(p, gblk);
        SetBlock(gstart);
        inlinet = t;
        RenderInto(tyg_param(p, 0), AtV(tyg_param(p, 1)), P { AtV(tyg_param(p, 2)), t }, true, Line {});
        if (!Ended()) tyg_ret(p, 0);
        Finish(Line {});
    }

    // ---- The verifier of an image's elements ---------------------------------
    //
    // gs_tg_verify_<T>(p, n, bm) (CodeGen::VerifyFn): the element count of the image at p, or -1 if its n
    // bytes are not one. Each type's walk mirrors CodeGen::EmitVerifyWalk over a cursor `vq`; any failure
    // jumps to `vfail`, which returns -1.
    tyg_val vP = 0, vN = 0, vBM = 0, vE = 0;
    tyg_local vq = 0;
    tyg_block vfail = 0;
    TypeExpr *velem = nullptr;

    tyg_val VQ() { return LdL(vq); }
    void VSetQ(tyg_val x) { StL(vq, x); }
    void VAddQ(tyg_val x) { VSetQ(Add(VQ(), x)); }
    tyg_local VNew() { return tyg_local_new(p, 8, 8); }
    tyg_val VBin(tyg_binop op, tyg_val a, tyg_val b) { return tyg_bin(p, op, a, b); }
    tyg_val VAt() { return Add(vP, VQ()); }
    // The integer of `size` bytes at address `at`, widened to 64 bits.
    tyg_val VLoad(tyg_val at, int64_t size, bool sgn) {
        auto s = size == 1 ? TYG_I8 : size == 2 ? TYG_I16 : size == 4 ? TYG_I32 : TYG_I64;
        auto x = tyg_load(p, s, AtV(at), 0);
        return size == 8 ? x : tyg_convert(p, sgn ? TYG_SEXT : TYG_ZEXT, TYG_I64, x);
    }
    // Fail where `bad`, else go on.
    void VFailIf(tyg_val bad) {
        auto ok = NewBlock();
        Branch(bad, vfail, ok);
        SetBlock(ok);
    }
    // Fail where fewer than `bytes` bytes are left.
    void VNeed(tyg_val bytes) { VFailIf(Cmp(TYG_SLT, Sub(vN, VQ()), bytes)); }
    // The ULEB128 at q (gs_uleb_check), failing where it is none.
    Uleb VUleb() {
        auto uc = UlebCheck(VAt(), vE);
        VFailIf(Cmp(TYG_EQ, LdL(uc.k), I64(0)));
        return uc;
    }
    // body() while q < n.
    template <class F> void VWhileQ(F body) {
        auto head = NewBlock(), b = NewBlock(), done = NewBlock();
        tyg_jump(p, head);
        SetBlock(head);
        Branch(Cmp(TYG_SLT, VQ(), vN), b, done);
        SetBlock(b);
        body();
        tyg_jump(p, head);
        SetBlock(done);
    }
    // The procedure's entry and body blocks; Finish fills in the entry.
    void NewProcBody() {
        gblk = NewBlock();
        gstart = NewBlock();
        tyg_jump(p, gblk);
        SetBlock(gstart);
    }
    void NewProcBegin(tyg_sym sym, tyg_sig *sig, Line line) {
        p = tyg_proc_begin(m, sym, sig);
        if (!p) No(line, cat("tyg_proc_begin: ", tyg_error(m) ? tyg_error(m) : "?"));
    }

    // One self-relative reference at field offset q holding `off` (CodeGen::EmitVerifyLink): the target must
    // be the start of an element, and where typed at a variant, one carrying that tag.
    void VLink(TypeExpr *rt, tyg_val off) {
        auto offl = Local(off);
        auto pt = rt->ref->sub;
        auto variant = pt->kind == TY_VARIANT && velem->kind == TY_ENUM;
        int64_t skip = 0, vi = 0;
        if (variant) {
            auto ei = cg.EIVar(pt);
            vi = ei->en->VariantIndex(pt->var->variant);
            skip = CodeGen::IntSize(cg.TagStore(ei->en));
        }
        auto check = [&] {
            auto v = Local(Sub(VQ(), I64(skip)));
            // The untrusted offset is bounded against the image before it is added.
            VFailIf(Cmp(TYG_SLT, LdL(offl), tyg_un(p, TYG_NEG, LdL(v))));
            VFailIf(Cmp(TYG_SGE, LdL(offl), Sub(vN, LdL(v))));
            StL(v, Add(LdL(v), LdL(offl)));
            if (cg.IsFix(velem)) {
                auto sz = I64(std::max<int64_t>(cg.FixedSize(velem), 1));
                VFailIf(Cmp(TYG_NE, VBin(TYG_SREM, LdL(v), sz), I64(0)));
            } else {   // GS_BM_GET(bm, v)
                auto byte = VLoad(Add(vBM, VBin(TYG_LSHR, LdL(v), I64(3))), 1, false);
                auto bit = VBin(TYG_AND, VBin(TYG_LSHR, byte, VBin(TYG_AND, LdL(v), I64(7))), I64(1));
                VFailIf(Cmp(TYG_EQ, bit, I64(0)));
            }
            if (variant) {
                VFailIf(Cmp(TYG_SLT, Sub(vN, LdL(v)), I64(skip)));
                VFailIf(Cmp(TYG_NE, VLoad(Add(vP, LdL(v)), skip, false), I64(vi)));
            }
        };
        if (rt->ref->optional) If(Cmp(TYG_NE, LdL(offl), I64(0)), check);
        else check();
    }

    // A run of fields (CodeGen::EmitVerifyFields). `lo` is the packed layout of a fixed struct or variant,
    // whose offsets may include alignment padding a sequential walk would miss; bytes layouts pass null.
    void VFields(const vector<Field> &fields, const vector<TypeExpr *> &ftypes, const CodeGen::Layout *lo,
                 int64_t total, bool links) {
        tyg_local start = 0;
        if (lo) {
            start = Local(VQ());
            VNeed(I64(total));
        }
        for (size_t i = 0; i < fields.size(); i++) {
            auto &f = fields[i];
            if (f.ispad) {
                if (!lo && f.padsize > 0) VAddQ(I64(f.padsize));
                continue;
            }
            if (lo && !cg.NeedsVerifyWalk(ftypes[i])) continue;
            if (lo) VSetQ(Add(LdL(start), I64(lo->offs[i])));
            VWalk(ftypes[i], links);
        }
        if (lo) VSetQ(Add(LdL(start), I64(total)));
    }

    // The checks and cursor advance for one value of type t at q (CodeGen::EmitVerifyWalk).
    void VWalk(TypeExpr *t, bool links) {
        if (!cg.NeedsVerifyWalk(t)) {
            auto sz = cg.FixedSize(t);
            if (sz > 0) {
                VNeed(I64(sz));
                VAddQ(I64(sz));
            }
            return;
        }
        auto walk = [this, links](TypeExpr *e) { return [this, e, links](tyg_val) { VWalk(e, links); }; };
        switch (t->kind) {
            case TY_BOOL:
                VNeed(I64(1));
                VFailIf(Cmp(TYG_UGT, VLoad(VAt(), 1, false), I64(1)));
                VAddQ(I64(1));
                return;
            case TY_INT:   // a varint field: only its length matters to the walk
                VAddQ(LdL(VUleb().k));
                return;
            case TY_REF: {
                auto w = (IntStorage)t->ref->lenstorage;
                auto off = VNew(), k = VNew();
                if (w == IS_VARINT) {
                    auto uc = VUleb();
                    StL(k, LdL(uc.k));
                    if (links) {   // gs_zig_check
                        auto u = LdL(uc.v);
                        auto sign = tyg_un(p, TYG_NEG, VBin(TYG_AND, u, I64(1)));
                        StL(off, VBin(TYG_XOR, VBin(TYG_LSHR, u, I64(1)), sign));
                    }
                } else {
                    VNeed(I64(CodeGen::IntSize(w)));
                    StL(k, I64(CodeGen::IntSize(w)));
                    if (links) StL(off, VLoad(VAt(), CodeGen::IntSize(w), !t->ref->pool));
                }
                if (links) VLink(t, LdL(off));
                VAddQ(LdL(k));
                return;
            }
            case TY_ARRAY: {
                auto &a = *t->arr;
                switch (a.akind) {
                    case A_FIXED: Loop(I64(0), I64(cg.ArrSize(t->arr)), walk(a.sub)); return;
                    case A_VAR: {
                        auto ls = cg.LenStore(t->arr);
                        auto nl = VNew();
                        if (ls == IS_VARINT) {
                            auto uc = VUleb();
                            VFailIf(Cmp(TYG_SLT, LdL(uc.v), I64(0)));   // above INT64_MAX
                            StL(nl, LdL(uc.v));
                            VAddQ(LdL(uc.k));
                        } else {
                            VNeed(I64(CodeGen::IntSize(ls)));
                            StL(nl, VLoad(VAt(), CodeGen::IntSize(ls), false));
                            VAddQ(I64(CodeGen::IntSize(ls)));
                        }
                        VFailIf(Cmp(TYG_SLT, LdL(nl), I64(0)));
                        auto per = I64(std::max<int64_t>(cg.MinBytes(a.sub), 1));
                        VFailIf(Cmp(TYG_SGT, LdL(nl), VBin(TYG_SDIV, Sub(vN, VQ()), per)));
                        if (!cg.NeedsVerifyWalk(a.sub)) VAddQ(Mul(LdL(nl), I64(cg.FixedSize(a.sub))));
                        else Loop(I64(0), LdL(nl), walk(a.sub));
                        return;
                    }
                    case A_LIMITED: {
                        // Slots past the length were never written, so only the live ones are walked;
                        // the cursor skips the capacity.
                        auto s = Local(VQ()), nl = VNew(), cap = VNew();
                        auto stat = cg.IsFix(t);
                        if (stat) {
                            auto ls = CodeGen::IntSize(cg.LenStore(t->arr));
                            VNeed(I64(cg.FixedSize(t)));
                            StL(nl, VLoad(VAt(), ls, false));
                            VFailIf(Cmp(TYG_SLT, LdL(nl), I64(0)));
                            VFailIf(Cmp(TYG_SGT, LdL(nl), I64(cg.ArrSize(t->arr))));
                            VAddQ(I64(ls));
                        } else {
                            VNeed(I64(8));
                            StL(cap, VLoad(VAt(), 4, false));
                            StL(nl, VLoad(Add(VAt(), I64(4)), 4, false));
                            VFailIf(Cmp(TYG_SGT, LdL(nl), LdL(cap)));
                            auto esz = I64(std::max<int64_t>(cg.FixedSize(a.sub), 1));
                            VFailIf(Cmp(TYG_SGT, LdL(cap), VBin(TYG_SDIV, Sub(Sub(vN, VQ()), I64(8)), esz)));
                            VAddQ(I64(8));
                        }
                        if (cg.NeedsVerifyWalk(a.sub)) Loop(I64(0), LdL(nl), walk(a.sub));
                        auto skip = stat ? I64(cg.FixedSize(t)) : Add(I64(8), Mul(LdL(cap), I64(cg.FixedSize(a.sub))));
                        VSetQ(Add(LdL(s), skip));
                        return;
                    }
                    default: No(Line {}, "verifier of this array");
                }
                return;
            }
            case TY_STRUCT: {
                auto si = cg.SI(t);
                auto fix = cg.IsFix(t);
                VFields(si->st->fields, si->ftypes, fix ? &cg.StructLayout(si) : nullptr, fix ? cg.FixedSize(t) : 0,
                        links);
                return;
            }
            case TY_VARIANT: {
                auto ei = cg.EIVar(t);
                auto vi = ei->en->VariantIndex(t->var->variant);
                auto fix = cg.IsFix(t);
                VFields(ei->en->variants[vi].fields, ei->vftypes[vi], fix ? &cg.VariantLayout(ei, vi) : nullptr,
                        fix ? cg.FixedSize(t) : 0, links);
                return;
            }
            case TY_ENUM: {
                // The tag is range-checked in both modes (a match has no default).
                auto ei = cg.EIOf(t);
                auto tsz = CodeGen::IntSize(cg.TagStore(ei->en));
                auto nv = (int64_t)ei->en->variants.size();
                auto fix = !t->enu->varmode;
                auto s = Local(VQ()), tag = VNew();
                VNeed(I64(fix ? cg.FixedSize(t) : tsz));
                StL(tag, VLoad(VAt(), tsz, false));
                VFailIf(Cmp(TYG_UGE, LdL(tag), I64(nv)));
                VAddQ(I64(tsz));
                auto after = NewBlock();
                for (int64_t i = 0; i < nv; i++) {
                    auto vt = cg.VariantType(t, (int)i);
                    auto arm = NewBlock(), next = NewBlock();
                    Branch(Cmp(TYG_EQ, LdL(tag), I64(i)), arm, next);
                    SetBlock(arm);
                    if (!fix || cg.NeedsVerifyWalk(vt)) VWalk(vt, links);
                    tyg_jump(p, after);
                    SetBlock(next);
                }
                tyg_jump(p, vfail);
                SetBlock(after);
                if (fix) VSetQ(Add(LdL(s), I64(cg.FixedSize(t))));
                return;
            }
            default: No(Line {}, "verifier of this type");
        }
    }

    void EmitVerify(const string &name, TypeExpr *elem) {
        auto i64t = IT(TYG_I64);
        const tyg_type *pts[3] = { i64t, i64t, i64t };
        NewProcBegin(M.Sym(name, TYG_SYM_FUNC | TYG_SYM_GLOBAL), tyg_sig_new(m, i64t, pts, 3, 0), Line {});
        NewProcBody();
        velem = elem;
        vP = tyg_param(p, 0);
        vN = tyg_param(p, 1);
        vBM = tyg_param(p, 2);
        vE = Add(vP, vN);
        vfail = NewBlock();
        // A type whose values can occupy no bytes has no recoverable element count: only the empty image is
        // one of its arrays.
        if (cg.MinBytes(elem) <= 0) {
            tyg_ret(p, tyg_select(p, Cmp(TYG_EQ, vN, I64(0)), I64(0), I64(-1)));
        } else {
            auto fixed = cg.IsFix(elem);
            auto rel = cg.HasRelRefAny(elem);
            vq = VNew();
            auto cnt = Local(I64(0));
            VSetQ(I64(0));
            VWhileQ([&] {
                if (!fixed && rel) {   // GS_BM_SET(bm, q)
                    auto at = Add(vBM, VBin(TYG_LSHR, VQ(), I64(3)));
                    auto bit = VBin(TYG_SHL, I64(1), VBin(TYG_AND, VQ(), I64(7)));
                    auto bits = tyg_convert(p, TYG_TRUNC, TYG_I8, VBin(TYG_OR, VLoad(at, 1, false), bit));
                    tyg_store(p, TYG_I8, AtV(at), bits, 0);
                }
                VWalk(elem, fixed && rel);
                StL(cnt, Add(LdL(cnt), I64(1)));
            });
            VFailIf(Cmp(TYG_NE, VQ(), vN));
            if (!fixed && rel) {   // The link pass, now that every element start is known.
                VSetQ(I64(0));
                VWhileQ([&] { VWalk(elem, true); });
            }
            tyg_ret(p, LdL(cnt));
        }
        SetBlock(vfail);
        tyg_ret(p, I64(-1));
        Finish(Line {});
    }

    // The entry's block (gblk): the addresses of the globals the body names, and the stack block, then on
    // to the body (gstart).
    void Finish(Line line) {
        SetBlock(gblk);
        Loc(line);
        if (stksl) StL(stksl, CallC("gs_tg_stks", {}, {}, tyg_type_ptr(m)));
        for (auto &[acc, loc] : gaddrs) StL(loc, CallC(acc, {}, {}, tyg_type_ptr(m)));
        tyg_jump(p, gstart);
        auto bad = tyg_proc_end(p) != 0;
        p = nullptr;
        if (bad) No(line, cat("tinygen: ", tyg_error(m) ? tyg_error(m) : "?"));
    }

    // CallC("gs_tg_stack_init") on the stack at `stk`.
    void StackInitCall(tyg_val stk) {
        M.NeedShim("gs_tg_stack_init");
        CallC("gs_tg_stack_init", { tyg_type_ptr(m) }, { stk }, nullptr);
    }
    // An empty array header at `at` over the stack `stk`.
    void EmptyArrayHdr(tyg_val at, tyg_val stk) {
        St(AtV(at), Top(stk));
        St(AtV(at, 8), I64(0));
    }

    // gs_init_globals as a procedure of the object, for a whole program build (CodeGen::EmitGlobalInit):
    // each initializer in declaration order with gs_sp fixed at 0, a resizable or T[] global built on its
    // dedicated stack, reserved first (gs_stack_init). No sp: what refers to the function's own results
    // (return, return from) is unsupported.
    void EmitInit(tyg_sym sym, Line line) {
        NewProcBegin(sym, tyg_sig_new(m, nullptr, nullptr, 0, 0), line);
        auto fty = tyg_dt_func(m, tyg_dt_void(m), nullptr, 0, 0);
        tyg_debug_proc(p, "gs_init_globals", M.File(line.fileidx), line.line, fty);
        Loc(line);
        spl = tyg_local_new(p, 8, 8);
        hassp = true;
        StL(spl, I64(0));
        NewProcBody();
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
                if (!M.GlobalOk(d)) No(d->line, "global of this kind");
                auto fo = M.FoOk(d->type);
                auto grow = IsGrow(d->type) || M.IsRs(d->type) || M.IsRb(d->type);
                if (fo || grow || IsImg(d->type)) {
                    auto at = LdL(AccessorLocal(M.Accessor(d)));
                    auto stk = LdL(AccessorLocal(M.StackAccessor(d)));
                    StackInitCall(stk);
                    if (fo) {   // A frame object: its tail on the global's own stack.
                        GenFo(init, P { AtV(at), d->type, stk }, init->line);
                    } else if (grow) {   // CodeGen::InitGlobalStack: an empty array, then its elements.
                        EmptyArrayHdr(at, stk);
                        if (M.IsRb(d->type)) GenRb(init, stk, AtV(at), d->type, init->line);
                        else GenRz(init, stk, AtV(at), d->type, init->line);
                        if (d->reusable && !Ended()) {   // Its freelist, empty, on a stack of its own.
                            auto [h, st] = M.PoolAccessors(d);
                            auto flstk = LdL(AccessorLocal(st));
                            StackInitCall(flstk);
                            EmptyArrayHdr(LdL(AccessorLocal(h)), flstk);
                        }
                    } else {
                        auto img = GenImg(init, stk, d->type, init->line);
                        if (!Ended()) St(AtV(at), img);
                    }
                } else if (cg.PrefVar(d)) {
                    PrefInto(GlobalPlace(d, init->line).a, init);
                } else {
                    Into(GlobalPlace(d, init->line), init);
                }
            }
            CloseScope();
        }
        CloseScope();
        if (!Ended()) tyg_ret(p, 0);
        Finish(line);
    }
};

// The backend: chooses the specializations it takes as CodeGen asks, then compiles those into one object.
struct TinyGenBackend : CodeGen::SpecBackend {
    Ast &ast;
    bool checks;
    string source;
    vector<FnSpec *> taken;
    vector<pair<FnSpec *, string>> left;   // Specializations left to C, and why.
    unique_ptr<TgModule> mod;
    string_view runtime_ext_text;
    string runtime_decls;   // runtime.h and runtime_ext.h (TgModule::RuntimeApi).
    // Functions left to C whatever they hold (--tinygen-skip): what tests calls across the two halves with.
    vector<string> skip;

    TinyGenBackend(Ast &_ast, bool _checks, const string &_source, string_view _ext, string _decls)
        : ast(_ast), checks(_checks), source(_source), runtime_ext_text(_ext),
          runtime_decls(std::move(_decls)) {}

    // ---- Helpers for the procedures this object writes by hand ----------------------------------
    // What a procedure asked of the module for the C half (shims, trampolines), to take back.
    struct Mark {
        size_t shims, externs, direct;
    };
    Mark Take() { return { mod->shims.size(), mod->externs.size(), mod->direct.size() }; }
    void Undo(Mark k) {
        mod->shims.resize(k.shims);
        mod->externs.resize(k.externs);
        mod->direct.resize(k.direct);
    }
    static string Why(const TgUnsupported &u) {
        return u.line.line > 0 ? cat(u.why, " (line ", u.line.line, ")") : u.why;
    }
    tyg_sym FSym(const string &name) { return mod->Sym(name, TYG_SYM_FUNC | TYG_SYM_GLOBAL); }
    tyg_sig *SigOf(const tyg_type *ret, const vector<const tyg_type *> &ps) {
        return tyg_sig_new(mod->m, ret, ps.data(), (int)ps.size(), 0);
    }
    void EndProc(tyg_proc *p, const string &name) {
        if (tyg_proc_end(p) != 0) throw CompileError { cat("tinygen: ", name, ": ", tyg_error(mod->m) ? tyg_error(mod->m) : "?") };
    }
    tyg_val CallSig(tyg_proc *p, tyg_sig *sig, tyg_sym fn, const vector<tyg_val> &vals) {
        vector<tyg_arg> as;
        for (auto v : vals) {
            tyg_arg a {};
            a.val = v;
            as.push_back(a);
        }
        return tyg_call(p, sig, tyg_direct(fn), as.data(), (int)as.size(), nullptr, nullptr);
    }
    // A call of the object's procedure `name` with value arguments.
    tyg_val CallProc(tyg_proc *p, const string &name, const tyg_type *ret, const vector<const tyg_type *> &ps,
                     const vector<tyg_val> &vals) {
        return CallSig(p, SigOf(ret, ps), FSym(name), vals);
    }
    // The address an accessor procedure returns.
    tyg_val Acc(tyg_proc *p, const string &name) { return CallProc(p, name, tyg_type_ptr(mod->m), {}, {}); }
    // A procedure `name` returning the address of `target`.
    void DefAccessor(const string &name, tyg_sym target) {
        auto m = mod->m;
        auto p = tyg_proc_begin(m, FSym(name), SigOf(tyg_type_ptr(m), {}));
        tyg_ret(p, tyg_addr_of(p, tyg_at_sym(target, 0)));
        EndProc(p, name);
    }
    // The body of a wrapper p of sp: its parameters passed on to `callee` (an address for each that is not
    // a scalar; gs_sp 0 after them if zerosp), its result returned. A slice result is as the C wrapper
    // gives it: the base returned, the length stored through the extra out-pointer parameter.
    void ForwardCall(tyg_proc *p, FnSpec *sp, tyg_sig *sig, tyg_sym callee, bool zerosp, bool slice) {
        vector<tyg_arg> args;
        for (size_t i = 0; i < sp->argtypes.size(); i++) {
            tyg_arg a {};
            if (mod->IsScalar(sp->argtypes[i])) a.val = tyg_param(p, (int)i);
            else a.mem = tyg_at_val(tyg_param(p, (int)i), 0);
            args.push_back(a);
        }
        if (zerosp) {
            tyg_arg a {};
            a.val = tyg_iconst(p, TYG_I64, 0);
            args.push_back(a);
        }
        auto call = [&](tyg_addr *ret) {
            return tyg_call(p, sig, tyg_direct(callee), args.data(), (int)args.size(), nullptr, ret);
        };
        auto rt = sp->rets.empty() ? nullptr : sp->rets[0];
        if (!rt) {
            call(nullptr);
            tyg_ret(p, 0);
        } else if (mod->IsScalar(rt)) {
            tyg_ret(p, call(nullptr));
        } else {
            auto t = tyg_local_new(p, mod->Size(rt), 8);
            auto ra = tyg_at_local(t, 0);
            call(&ra);
            if (slice) {
                tyg_store(p, TYG_I64, tyg_at_val(tyg_param(p, (int)sp->argtypes.size()), 0),
                          tyg_load(p, TYG_I64, tyg_at_local(t, 8), 0), 0);
                tyg_ret(p, tyg_load(p, TYG_I64, tyg_at_local(t, 0), 0));
            } else {
                tyg_ret_mem(p, ra);
            }
        }
    }

    // Builds sp into the module. A construct this backend does not support anywhere in the body abandons
    // the procedure (tyg_proc_abort), and what it asked of the module for the C half is taken back, so that
    // the whole body goes to C. "" when it compiled, else why not. A body that compiled tells the C half's
    // stack accounting (codegen_stacks.h) the stacks it owns and what it hands its callees, as one the C
    // backend emits does.
    string Build(CodeGen &cg, FnSpec *sp) {
        string why;
        // A simd function is its baseline version here, which §7.12 allows.
        if (!mod->SigOk(sp, true, why)) return cat("interface: ", why);
        auto mark = Take();
        TgFn fn(*mod, sp);
        try {
            fn.Emit();
        } catch (TgUnsupported &u) {
            if (fn.p) tyg_proc_abort(fn.p);
            Undo(mark);
            return Why(u);
        }
        cg.stackown[sp] = fn.maxown;
        cg.stackcalls[sp] = fn.stackcalls;
        return "";
    }

    // The specializations this backend takes: every one whose body compiles, each built straight into the
    // module; one that does not is left to C. The two halves call each other with gs_sp as two C functions
    // do, so what one takes does not depend on the other.
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
            bool skipped = std::find(skip.begin(), skip.end(), sp->sf->qname) != skip.end();
            auto w = skipped ? string("--tinygen-skip") : Build(cg, sp);
            if (w.empty()) {
                ours.insert(sp);
                taken.push_back(sp);
            } else {
                left.emplace_back(sp, w);
            }
        }
        BuildRenders();
        return ours;
    }

    // The render procedures the functions asked for (TgModule::RenderFn), each possibly asking for others.
    void BuildRenders() {
        for (size_t i = 0; mod && i < mod->renderq.size(); i++) {
            auto [name, t] = mod->renderq[i];
            TgFn fn(*mod, nullptr);
            try {
                if (name.rfind("gs_tg_verify_", 0) == 0) fn.EmitVerify(name, t);
                else fn.EmitRender(name, t);
            } catch (TgUnsupported &u) {
                if (fn.p) tyg_proc_abort(fn.p);
                throw CompileError { cat("tinygen: ", name, ": ", u.why) };
            }
        }
        if (mod) mod->renderq.clear();
    }

    // After CodeGen has made the C half: the extern fns the object calls are used, so the C half declares
    // them and their packages link.
    set<FnSpec *> declared;   // Extern fns the C half declares already.

    void Compile(CodeGen &cg) {
        if (!mod) return;
        declared.insert(cg.usedexterns.begin(), cg.usedexterns.end());
        for (auto list : { &mod->externs, &mod->direct })
            for (auto sp : *list)
                if (std::find(cg.usedexterns.begin(), cg.usedexterns.end(), sp) == cg.usedexterns.end())
                    cg.usedexterns.push_back(sp);
    }

    // The C the object calls into: the shims, and a trampoline per extern fn called. Appended to the C
    // half, after everything it names.
    string Shims(CodeGen &cg) {
        if (!mod) return "";
        string s = "\n/* ---- for the tinygen object (docs/design/tinygen_backend.md) ---- */\n";
        for (auto &sh : mod->shims) s += sh.second;
        for (auto sp : mod->externs) {
            auto name = sp->sf->cname;
            if (!declared.count(sp) && runtime_ext_text.find(cat(" ", name, "(")) == string_view::npos)
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

    // Whether the object can be the whole program, with no C half: every live specialization compiled,
    // nothing asked of the C half that the support object (TgSupportBody) or the object itself does not
    // hold -- globals it can lay out and initialize (gs_init_globals built here), `return from` channels
    // of fixed types, extern fns the runtime exports or the support object has trampolines for -- and a
    // plain main without threads. Else why not.
    bool WholeProgram(CodeGen &cg, string &why) {
        auto mainsf = ast.MainFunction();
        auto mainsp = mainsf && !mainsf->specs.empty() ? mainsf->specs[0] : nullptr;
        if (!mod) why = "nothing compiled";
        else if (!left.empty()) why = cat(left.size(), " function(s) left to C");
        else if (cg.library) why = "a library";
        else if (!mainsp || !cg.sinfo.count(mainsp) || !mainsp->params.empty() ||
                 !cg.sinfo[mainsp].freevars.empty())
            why = "no plain main";
        if (!why.empty()) return false;
        for (auto sf : ast.functions)
            if (sf->isexport && !ExportOk(cg, sf, why)) return false;
        for (auto sp : mod->externs)
            if (!ExternHeld(sp)) return why = cat("extern ", sp->sf->cname), false;
        if (!GlobalsOk(cg, why)) return false;
        // A gs_init_globals built but not used stays in the object, unreferenced, with what it asked of
        // the C half.
        if (!cg.noglobalinit && !BuildInit(cg, why)) return false;
        BuildRenders();
        for (auto &sh : mod->shims)
            if (TgFixedShim(sh.first).empty() && !OwnAccessor(sh.first)) {
                initsym = TYG_NO_SYM;
                return why = cat("C half's ", sh.first), false;
            }
        for (auto &[acc, sp] : mod->thunkacc)
            for (auto d : cg.ThreadGlobals(sp)) {
                mod->Accessor(d);
                if (cg.IsFix(d->type)) continue;
                mod->StackAccessor(d);
                if (d->reusable) mod->PoolAccessors(d);
            }
        EmitGlobalStorage(cg);
        EmitExports(cg);
        ForwardExterns();
        EmitThreads(cg);
        return true;
    }

    // Whether a whole program can call an extern fn through its trampoline (gs_tg_x_): the support object
    // holds one (TgSupportExterns), or the object does (ForwardExterns) for a symbol the linker resolves,
    // a package's or the C library's, which no header of the runtime defines.
    bool ExternHeld(FnSpec *sp) {
        auto &name = sp->sf->cname;
        for (auto &x : TgSupportExterns())
            if (name == x.name) return true;
        return mod->rtdecls.find(cat(" ", name, "(")) == string_view::npos &&
               mod->rtdecls.find(cat("*", name, "(")) == string_view::npos;
    }

    // The trampolines the object holds itself: a procedure of the name the calls use that calls the C
    // symbol with the same C parameters.
    void ForwardExterns() {
        for (auto sp : mod->externs) {
            auto &name = sp->sf->cname;
            auto held = false;
            for (auto &x : TgSupportExterns()) held = held || name == x.name;
            if (held) continue;
            auto sig = mod->ExternSig(sp);
            auto p = tyg_proc_begin(mod->m, FSym(cat("gs_tg_x_", name)), sig);
            ForwardCall(p, sp, sig, FSym(name), false, false);
            EndProc(p, name);
        }
    }

    // The queues and the worker thunks, which the C half holds in a mixed build: a queue is eight bytes of
    // data with an accessor, started by main (EmitMain); a thunk is a procedure taking the spawn image,
    // which opens the thread program's block of data stacks, unpacks the arguments and calls the worker
    // (CodeGen::EmitThreadThunk), with an accessor returning its address.
    void EmitThreads(CodeGen &cg) {
        auto m = mod->m;
        auto ptr = tyg_type_ptr(m);
        for (auto &[acc, q] : mod->queueacc) {
            auto sym = tyg_symbol(m, cat("gs_", q).c_str(), 0);
            tyg_data(m, sym, TYG_SECTION_BSS, nullptr, 8, 8);
            DefAccessor(acc, sym);
        }
        for (auto &[acc, sp] : mod->thunkacc) {
            auto &ki = cg.sinfo[sp];
            auto tsym = FSym(cg.thunks[sp]);
            auto p = tyg_proc_begin(m, tsym, SigOf(nullptr, { ptr }));
            TgFn fn(*mod, nullptr);   // Its IR shorthands, and the sizes of image arguments.
            fn.p = p;
            // The packet is read through `cur`, advanced past each argument and global in turn.
            auto cur = fn.Local(tyg_param(p, 0));
            auto ld = [&](tyg_val a, int64_t off = 0) { return fn.Ld(fn.AtV(a, off)); };
            auto st = [&](tyg_val a, int64_t off, tyg_val v) { fn.St(fn.AtV(a, off), v); };
            auto add = [&](tyg_val a, int64_t k) { return fn.Add(a, fn.I64(k)); };
            auto mv = [&](tyg_val dst, tyg_val src, tyg_val n) { tyg_move_dyn(p, dst, src, n); };
            CallProc(p, "gs_tg_stack_block", nullptr, { tyg_type_int(m, TYG_I64, 1) },
                     { fn.I64(cg.ProgramStacks({ sp })) });
            vector<tyg_arg> args;
            for (auto t : sp->argtypes) {
                auto at = fn.LdL(cur);
                tyg_arg a {};
                tyg_val size;
                if (mod->Resz(t)) {
                    // [int64 size][count][fixed fields][elements]: a header (or the frame object) and a
                    // stack of the thunk's own, as CodeGen::EmitThreadThunk makes them.
                    auto fo = mod->FoOk(t);
                    auto pre = fo ? mod->FoHdrOff(t) : 0;
                    auto osz = fo ? mod->Size(t) : 16;
                    auto obj = tyg_at_local(tyg_local_new(p, osz, 8), 0);
                    auto stkl = tyg_local_new(p, 8, 8);
                    auto stkaddr = tyg_addr_of(p, fn.AtL(stkl));
                    mod->NeedShim("gs_tg_stack_init");
                    CallProc(p, "gs_tg_stack_init", nullptr, { ptr }, { stkaddr });
                    tyg_zero(p, obj, osz);
                    auto hdr = obj, hlen = obj;
                    hdr.offset += pre;
                    hlen.offset += pre + 8;
                    auto sz = ld(at), top = fn.LdL(stkl);
                    fn.St(hdr, top);
                    fn.St(hlen, ld(at, 8));
                    if (pre) tyg_copy(p, obj, fn.AtV(at, 16), pre);
                    auto n = fn.Sub(sz, fn.I64(8 + pre));
                    mv(top, add(at, 16 + pre), n);
                    fn.StL(stkl, fn.Add(top, n));
                    a.mem = obj;
                    args.push_back(a);
                    tyg_arg k {};
                    k.val = stkaddr;
                    args.push_back(k);
                    fn.StL(cur, add(fn.Add(at, sz), 8));
                    continue;
                }
                if (mod->RzParam(t)) {
                    a.val = at;   // An image, by its address; the packet is the worker's own copy.
                    size = fn.ImgSize(t, at);
                } else {
                    size = fn.I64(cg.FixedSize(t));
                    if (cg.IsLargeFixed(t)) a.val = at;
                    else if (mod->IsScalar(t)) a.val = tyg_load(p, mod->Scal(t), fn.AtV(at), 0);
                    else a.mem = fn.AtV(at);
                }
                args.push_back(a);
                fn.StL(cur, fn.Add(at, size));
            }
            // The worker's globals, each [int64 size][bytes], into this thread's own instance of them.
            for (auto d : cg.ThreadGlobals(sp)) {
                auto at = fn.LdL(cur);
                if (cg.IsFix(d->type)) {
                    auto size = cg.FixedSize(d->type);
                    if (size) tyg_copy(p, fn.AtV(Acc(p, mod->accessors.at(d))), fn.AtV(add(at, 8)), size);
                    fn.StL(cur, add(at, 8 + size));
                    continue;
                }
                // [int64 size][image] onto the global's own stack in this thread's instance
                // (CodeGen::EmitThreadThunk).
                mod->NeedShim("gs_tg_stack_init");
                auto sz = ld(at), img = add(at, 8);
                auto gaddr = Acc(p, mod->accessors.at(d)), stk = Acc(p, mod->stkaccessors.at(d));
                CallProc(p, "gs_tg_stack_init", nullptr, { ptr }, { stk });
                auto top = ld(stk);
                auto adv = [&](tyg_val n) { st(stk, 0, fn.Add(top, n)); };
                auto fo = mod->FoOk(d->type), pool = !fo && mod->Resz(d->type) && d->reusable;
                if (fo || mod->Resz(d->type)) {
                    // A frame object keeps its header before the array's; a pool's elements are counted.
                    auto pre = fo ? mod->FoHdrOff(d->type) : 0;
                    auto n = pool ? fn.Mul(ld(img), fn.I64(cg.FixedSize(d->type->arr->sub)))
                                  : fn.Sub(sz, fn.I64(8 + pre));
                    if (pre) tyg_copy(p, fn.AtV(gaddr, 0), fn.AtV(img, 8), pre);
                    st(gaddr, pre, top);
                    st(gaddr, pre + 8, ld(img));
                    mv(top, add(img, 8 + pre), n);
                    adv(n);
                    if (pool) {   // [count][elements][freelist count][entries].
                        auto [fh, fs] = mod->PoolAccessors(d);
                        auto fl = Acc(p, fh), flstk = Acc(p, fs);
                        CallProc(p, "gs_tg_stack_init", nullptr, { ptr }, { flstk });
                        auto ftop = ld(flstk);
                        auto fat = fn.Add(img, add(n, 8));
                        auto fcnt = ld(fat);
                        auto fbytes = fn.Mul(fcnt, fn.I64(TgFn::FlEntry(d)));
                        st(fl, 0, ftop);
                        st(fl, 8, fcnt);
                        mv(ftop, add(fat, 8), fbytes);
                        st(flstk, 0, fn.Add(ftop, fbytes));
                    }
                } else {   // A T[]: the instance holds its address.
                    st(gaddr, 0, top);
                    mv(top, img, sz);
                    adv(sz);
                }
                fn.StL(cur, fn.Add(img, sz));
            }
            if (ki.needssp) {
                tyg_arg a {};
                a.val = fn.I64(0);
                args.push_back(a);
            }
            tyg_call(p, mod->Sig(sp), tyg_direct(FSym(ki.cname)), args.data(), (int)args.size(), nullptr, nullptr);
            tyg_ret(p, 0);
            EndProc(p, cg.thunks[sp]);
            DefAccessor(acc, tsym);
        }
    }

    // Whether an `export fn`'s wrapper is one this object writes: a plain specialization whose parameters
    // and result cross by value as the C wrapper's do (CodeGen::EmitExports). Else why not.
    bool ExportOk(CodeGen &cg, SFunction *sf, string &why) {
        auto fail = [&] { return why = "export fns", false; };
        if (sf->specs.size() != 1 || !cg.sinfo.count(sf->specs[0])) return fail();
        auto sp = sf->specs[0];
        auto &si = cg.sinfo[sp];
        if (!si.freevars.empty() || sp->rets.size() > 1 || !mod->SigOk(sp, false, why)) return fail();
        for (size_t i = 0; i < sp->argtypes.size(); i++) {
            auto t = sp->argtypes[i];
            if (cg.IsPoolParam(sp, i) || mod->RzParam(t) || cg.IsLargeFixed(t) || !mod->TypeOk(t)) return fail();
        }
        if (!sp->rets.empty() && (si.cret != 0 || !mod->TypeOk(sp->rets[0]))) return fail();
        return true;
    }

    // The C symbol of each `export fn`: a procedure taking the C parameters, and for a slice result a
    // length out-pointer after them, which calls the function, passing gs_sp 0 (CodeGen::EmitExports).
    void EmitExports(CodeGen &cg) {
        auto m = mod->m;
        for (auto sf : ast.functions) {
            if (!sf->isexport) continue;
            auto sp = sf->specs[0];
            auto &si = cg.sinfo[sp];
            auto rt = sp->rets.empty() ? nullptr : sp->rets[0];
            auto slice = rt && rt->kind == TY_SLICE;
            vector<const tyg_type *> ps;
            for (auto t : sp->argtypes) ps.push_back(mod->AT(t));
            if (slice) ps.push_back(tyg_type_ptr(m));
            auto p = tyg_proc_begin(m, FSym(sf->cname),
                                    SigOf(!rt ? nullptr : slice ? tyg_type_ptr(m) : mod->AT(rt), ps));
            ForwardCall(p, sp, mod->Sig(sp), FSym(si.cname), si.needssp, slice);
            EndProc(p, sf->cname);
        }
    }

    // ---- Globals of a whole program ------------------------------------------
    //
    // With no C half the object holds the globals: each member of the C's instance block (gs_globals_t) a
    // zero-filled data symbol of its own, under its Goose name for the debugger, a resizable's or T[]'s
    // dedicated stack another beside it; the static data the C makes of a `let` is the object's already
    // (TgModule::StaticGlobal). A whole program has no threads, so main's instance is the only one, as
    // GS_GL is &gs_globals_main in the C. The functions compiled reach a global through its accessor
    // (gs_tg_g<i>, gs_tg_gs<i>) as in a mixed build, which the object now defines, and gs_init_globals is
    // a procedure of the object (TgFn::EmitInit) that main calls.

    // Whether every global is one the object can hold: else why not.
    bool GlobalsOk(CodeGen &cg, string &why) {
        for (auto g : ast.globals)
            for (auto d : g->defs) {
                if (!cg.gstatic.count(d)) {
                    if (!mod->GlobalOk(d)) return why = cat("global ", d->name, " of this kind"), false;
                } else if (mod->accessors.count(d)) {   // Static data reached through an accessor: its bytes were not laid out.
                    return why = cat("global ", d->name, " as static data"), false;
                }
            }
        return true;
    }

    // Whether a program-specific shim is one the object defines itself: a global's accessor, or a
    // `return from` channel's of a fixed type.
    bool OwnAccessor(const string &name) {
        if (mod->channels.count(name) || mod->queueacc.count(name) || mod->thunkacc.count(name)) return true;
        for (auto &[d, n] : mod->accessors)
            if (n == name) return true;
        for (auto &[d, n] : mod->stkaccessors)
            if (n == name) return true;
        for (auto &[d, n] : mod->flaccessors)
            if (n.first == name || n.second == name) return true;
        return false;
    }

    // gs_init_globals into the object, if its initializers compile and need no more data stacks than the
    // C's (stackneed[nullptr], which gs_rt_init's counts were made with). Else why not.
    tyg_sym initsym = TYG_NO_SYM;
    int64_t initstacks = 0;   // The data stacks gs_tg_init_globals uses at once.
    bool BuildInit(CodeGen &cg, string &why) {
        auto mark = Take();
        auto sym = mod->Sym("gs_tg_init_globals", TYG_SYM_FUNC);
        TgFn fn(*mod, nullptr);
        Line line;
        for (auto g : ast.globals)
            if (!g->inits.empty()) {
                line = g->line;
                break;
            }
        try {
            fn.EmitInit(sym, line);
        } catch (TgUnsupported &u) {
            if (fn.p) tyg_proc_abort(fn.p);
            Undo(mark);
            return why = cat("global initializer: ", Why(u)), false;
        }
        int64_t need = fn.maxown;
        for (auto &c : fn.stackcalls) need = std::max(need, c.offset + cg.stackneed[c.callee]);
        // The C's count for it is what its own code needs; this one's temporaries (a dispatch's result
        // before it is re-prefixed) may need more, which the program's main asks the runtime for.
        if (need > cg.rtinit[0] && cg.maxstacks > 0 && need > cg.maxstacks)
            return why = "global initializers' data stacks", false;
        initstacks = need;
        for (auto sp : mod->externs)   // Extern fns: as for any function of a whole program.
            if (!ExternHeld(sp)) return why = cat("extern ", sp->sf->cname), false;
        initsym = sym;
        return true;
    }

    // The globals' storage, their debug entries, and the accessors the functions call, each returning its
    // global's address.
    void EmitGlobalStorage(CodeGen &cg) {
        auto m = mod->m;
        map<const VarDef *, tyg_sym> gsyms, ssyms;
        map<const VarDef *, pair<tyg_sym, tyg_sym>> flsyms;
        // Each thread program has an instance of the globals it names (§11.2): thread-local storage, which
        // a thread's thunk fills.
        auto tls = false;
        for (auto &[acc, sp] : mod->thunkacc) tls = tls || !cg.ThreadGlobals(sp).empty();
        auto def = [&](const string &name, int64_t size, int64_t align, VarDef *d, const tyg_dtype *dt) {
            auto sym = tyg_symbol(m, cat("gs_g_", name).c_str(), tls ? TYG_SYM_TLS : 0);
            tyg_data(m, sym, tls ? TYG_SECTION_TBSS : TYG_SECTION_BSS, nullptr, std::max<int64_t>(size, 1), align);
            if (dt) tyg_debug_global(m, string(d->name).c_str(), dt, sym, mod->File(d->line.fileidx), d->line.line);
            return sym;
        };
        for (auto g : ast.globals)
            for (auto d : g->defs) {
                if (cg.gstatic.count(d)) continue;
                auto name = cg.gnames[d].substr(7);   // "GS_GL->name"
                if (TgModule::IsGrow(d->type) || mod->IsRs(d->type)) {
                    gsyms[d] = def(name, 16, 8, d, mod->DT(d->type));
                } else if (mod->IsImg(d->type)) {
                    gsyms[d] = def(name, 8, 8, d, tyg_dt_pointer(m, mod->DT(d->type)));
                } else {
                    auto sz = cg.PrefVar(d) ? 32 : mod->Size(d->type);
                    int64_t align = 1;
                    while (align < 8 && align * 2 <= sz) align *= 2;
                    gsyms[d] = def(name, sz, align, d, mod->DT(d->type));
                }
                if (cg.gstks.count(d)) ssyms[d] = def(cat("gs_gstk_", name), 8, 8, d, nullptr);
                if (cg.gpools.count(d))
                    flsyms[d] = { def(cat(name, "_fl"), 16, 8, d, nullptr), def(cat("gs_gstk_", name, "_fl"), 8, 8, d, nullptr) };
            }
        for (auto &[d, n] : mod->accessors) DefAccessor(n, gsyms.at(d));
        for (auto &[d, n] : mod->stkaccessors) DefAccessor(n, ssyms.at(d));
        for (auto &[d, n] : mod->flaccessors) {
            DefAccessor(n.first, flsyms.at(d).first);
            DefAccessor(n.second, flsyms.at(d).second);
        }
        // The `return from` channels, which the C half keeps thread-local (gs_rf, the support object's, is
        // one too): thread-local here in a program with threads, plain data in one with a single thread.
        auto threaded = cg.usesthreads || !cg.queues.empty();
        for (auto &sh : mod->shims) {
            auto it = mod->channels.find(sh.first);
            if (it == mod->channels.end()) continue;
            auto sym = tyg_symbol(m, cat("gs_", sh.first.substr(6)).c_str(), threaded ? TYG_SYM_TLS : 0);   // gs_lret_<tid>_<i>
            tyg_data(m, sym, threaded ? TYG_SECTION_TBSS : TYG_SECTION_BSS, nullptr, std::max<int64_t>(it->second, 1), 8);
            DefAccessor(sh.first, sym);
        }
    }

    // The C main of a whole program build, which the C half otherwise writes (CodeGen::EmitProgramInit,
    // EmitMain): the runtime started with the program's stack counts, the globals initialized, fn main()
    // run, the stack report. In the debugger it is `main` at fn main()'s line.
    void EmitMain(CodeGen &cg) {
        auto m = mod->m;
        auto mainsp = ast.MainFunction()->specs[0];
        auto i32 = tyg_type_int(m, TYG_I32, 1), i64 = tyg_type_int(m, TYG_I64, 1);
        auto ptr = tyg_type_ptr(m);
        auto p = tyg_proc_begin(m, FSym("main"), SigOf(i32, { i32, ptr }));
        auto line = mainsp->sf->line;
        auto file = mod->File(line.fileidx);
        auto dint = tyg_dt_base(m, "int", 4, TYG_DT_SIGNED);
        const tyg_dtype *pdt[] = { dint, tyg_dt_pointer(m, tyg_dt_void(m)) };
        tyg_debug_proc(p, "main", file, line.line, tyg_dt_func(m, dint, pdt, 2, 0));
        tyg_loc(p, file, line.line, 0);
        auto I = [&](int64_t v) { return tyg_iconst(p, TYG_I64, v); };
        // The main program's stacks, or what the initializers use if more (the global regions follow them).
        auto mainstacks = std::max(cg.rtinit[0], initstacks);
        CallProc(p, "gs_tg_start", nullptr, { i32, ptr, i64, i64, i64 },
                 { tyg_param(p, 0), tyg_param(p, 1), I(mainstacks), I(cg.rtinit[1] + mainstacks - cg.rtinit[0]),
                   I(cg.rtinit[2]) });
        // The queues, before anything that could use one runs.
        for (auto &[acc, q] : mod->queueacc) CallProc(p, "gs_qinit", nullptr, { ptr }, { Acc(p, acc) });
        if (initsym != TYG_NO_SYM) CallSig(p, SigOf(nullptr, {}), initsym, {});
        CallSig(p, mod->Sig(mainsp), FSym(cg.sinfo[mainsp].cname),
                cg.sinfo[mainsp].needssp ? vector<tyg_val> { I(0) } : vector<tyg_val> {});
        CallProc(p, "gs_tg_end", nullptr, {}, {});
        tyg_ret(p, tyg_iconst(p, TYG_I32, 0));
        EndProc(p, "main");
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
