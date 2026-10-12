// Goose compiler — the tinygen backend (docs/design/tinygen_backend.md). tinygen (../tinygen, a C99
// library) lowers a small IR to machine code and writes an object file with debug info. This backend
// compiles the specializations it supports from the same optimized, annotated bodies the C backend
// reads, into one object linked beside the C the C backend still writes: that C keeps everything else
// -- main, the globals, the runtime's inline half, and every specialization this backend does not take,
// which it declares with external linkage (CodeGen::SpecBackend). The two halves call each other
// through the C calling convention of C.3, so a specialization falls back to C on its own.
//
// The runtime's helpers that runtime.h defines as static functions (checked arithmetic, `as` checks,
// the extern support) are reached through small exported shims appended to that C (Shims), compiled
// with its defines, so GS_DEBUG means the same on both sides.
#pragma once

#if GOOSE_HAVE_TINYGEN
#include "tinygen.h"

namespace goose {

// Thrown wherever the backend meets what it does not compile yet; the specialization then goes to the C
// backend.
struct TgUnsupported {
    Line line;
    string why;
};

// The shims that depend on the runtime alone, not on the program (checked arithmetic, `as` checks,
// aborts, the per-thread gs_stks and gs_rf). The C half defines the ones a program uses; a whole
// program build links them all from the support object (TgSupportBody). "" for any other name.
inline string TgFixedShim(const string &name) {
    static const char *ops[] = { "add", "sub", "mul", "neg", "div", "mod" };
    static const IntStorage ints[] = { IS_I8, IS_I16, IS_I32, IS_I64, IS_U8, IS_U16, IS_U32, IS_U64 };
    for (auto op : ops)
        for (auto is : ints) {
            auto sfx = string(CodeGen::IntSfx(is));
            if (name != cat("gs_tg_", op, "_", sfx)) continue;
            auto ct = string(CodeGen::IntCT(is));
            auto neg = string(op) == "neg";
            return cat(ct, " ", name, "(", ct, " a, ", neg ? "" : cat(ct, " b, "),
                       "const char *f, int l) { return gs_",
                       op, "_", sfx, neg ? "(a, f, l); }\n" : "(a, b, f, l); }\n");
        }
    // gs_thread_spawn is the runtime.h helper of a program with threads (GS_NEED_THREADS); the support
    // object has its own (TgSupportBody).
    static const map<string, const char *> fixed = {
        { "gs_tg_stks", "void *gs_tg_stks(void) { return gs_stks; }\n" },
        { "gs_tg_thread_spawn",
          "int64_t gs_tg_thread_spawn(void (*e)(uint8_t *), const void *a, int64_t n) "
          "{ return gs_thread_spawn(e, a, n); }\n" },
        { "gs_tg_stack_init", "void gs_tg_stack_init(void *s) { gs_stack_init((gs_stack *)s); }\n" },
        { "gs_tg_rf", "void *gs_tg_rf(void) { return &gs_rf; }\n" },
        { "gs_tg_rfpanic",
          "void gs_tg_rfpanic(void) { gs_panic(\"unexpected long-distance return\"); }\n" },
        { "gs_tg_f2iwrap", "int64_t gs_tg_f2iwrap(double d) { return gs_f2iwrap(d); }\n" },
        { "gs_tg_f2u", "uint64_t gs_tg_f2u(double d, int s, const char *f, int l) "
                       "{ (void)s; (void)f; (void)l; return GS_F2U(d, s, f, l); }\n" },
        { "gs_tg_f2i", "int64_t gs_tg_f2i(double d, int s, int64_t lo, int64_t hi, const char *t, "
                       "const char *f, int l) { (void)s; (void)lo; (void)hi; (void)t; (void)f; "
                       "(void)l; return GS_F2I(d, s, lo, hi, t, f, l); }\n" },
        { "gs_tg_range_u",
          "uint64_t gs_tg_range_u(uint64_t v, uint64_t hi, const char *t, const char *f, int l) "
          "{ (void)hi; (void)t; (void)f; (void)l; return GS_RANGE_U(v, hi, t, f, l); }\n" },
        { "gs_tg_range", "int64_t gs_tg_range(int64_t v, int64_t lo, int64_t hi, const char *t, "
                         "const char *f, int l) { (void)lo; (void)hi; (void)t; (void)f; (void)l; "
                         "return GS_RANGE(v, lo, hi, t, f, l); }\n" },
        { "gs_tg_slicefail",
          "void gs_tg_slicefail(const char *f, int l) { gs_abort(GS_E_SLICE, f, l); }\n" },
    };
    if (auto it = fixed.find(name); it != fixed.end()) return it->second;
    if (name.rfind("gs_tg_abort_", 0) == 0)
        return cat("void ", name, "(const char *f, int l) { gs_abort(GS_E_", name.substr(12),
                   ", f, l); }\n");
    return "";
}

// The extern fns that runtime_ext.h defines as static functions in the program's own C (or under asm
// labels): a whole program build has no C half to hold their trampolines, so the support object holds
// one for each, as the C half would write it (TinyGenBackend::Shims).
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
            auto ty = f ? "float" : "double";
            for (auto u : unary) {
                names.push_back(cat("gs_", u, f ? "f" : ""));
                r.push_back({ names.back().c_str(), ty, f ? "float a0" : "double a0", "a0" });
            }
            for (auto b : binary) {
                names.push_back(cat("gs_", b, f ? "f" : ""));
                r.push_back({ names.back().c_str(), ty, f ? "float a0, float a1" :
                              "double a0, double a1",
                              "a0, a1" });
            }
        }
        return r;
    }();
    return v;
}

// What the support object defines beyond runtime.h and runtime_ext.h, which precede it: every
// program-independent shim (TgFixedShim; an abort shim per GS_E_ code of `runtimeh`'s), the trampolines
// of TgSupportExterns, the thread-local gs_rf of a program with `return from`, and the start and end of
// a program whose main the tinygen object holds (TinyGenBackend::EmitMain), which set up and report the
// thread-locals kept here (gs_stks, gs_gl).
inline string TgSupportBody(string_view runtimeh) {
    string s = "\n/* ---- tinygen support (docs/design/tinygen_backend.md) ---- */\n"
               "#ifdef _MSC_VER\n#define GS_TG_TLS __declspec(thread)\n#else\n"
               "#define GS_TG_TLS __thread\n#endif\n"
               "static GS_TG_TLS int32_t gs_rf;\n";
    static const char *ops[] = { "add", "sub", "mul", "neg", "div", "mod" };
    static const IntStorage ints[] = { IS_I8, IS_I16, IS_I32, IS_I64, IS_U8, IS_U16, IS_U32, IS_U64 };
    for (auto op : ops)
        for (auto is : ints) {
            // runtime.h's unsigned helpers take no file and line; only unsigned div and mod need a
            // shim.
            if (IsUnsigned(is) && string(op) != "div" && string(op) != "mod") continue;
            s += TgFixedShim(cat("gs_tg_", op, "_", CodeGen::IntSfx(is)));
        }
    for (auto n : { "gs_tg_stack_init", "gs_tg_rf", "gs_tg_rfpanic", "gs_tg_f2iwrap", "gs_tg_f2u",
                    "gs_tg_f2i",
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
    // gs_stks is thread-local only in a program built with GS_NEED_THREADS, which this object is not,
    // so a thread-local of its own holds the block the procedures reach; a worker's thunk opens one and
    // its end frees it (the runtime's gs_stack_block and gs_thread_run).
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
         "}\n"
         "static uint8_t gs_tg_noglobals;\n"
         "void gs_tg_start(int argc, char **argv, int64_t mainstacks, int64_t mainregions,\n"
         "                 int64_t workerregions) {\n"
         "    gs_rt_init(argc, argv, mainstacks, mainregions, workerregions);\n"
         "    gs_tg_tstks = gs_stks;\n"
         "    gs_gl = &gs_tg_noglobals;\n"
         "}\n"
         "void gs_tg_end(void) { gs_rt_stats(); }\n";
    return s;
}

// One tinygen module and what its procedures share: types, debug types, symbols, file names, and the
// shims and extern trampolines their calls need from the C half.
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
    vector<pair<string, string>> shims;   // The C the procedures call into, by name, in order.
    vector<FnSpec *> externs;             // Extern fns called, for trampolines.
    vector<FnSpec *> direct;              // Extern fns called directly (RuntimeApi).
    // The queues and worker thunks the procedures name, each through an accessor returning its address:
    // the C half's static (a shim), or the object's own in a whole program. Queue accessor -> C queue.
    map<string, string> queueacc;
    map<string, FnSpec *> thunkacc;
    string_view rtdecls;   // The runtime's declarations (runtime.h, runtime_ext.h), for RuntimeApi.

    // Whether the C function `name` is one the runtime object exports (GS_API): a plain symbol, called
    // directly without a trampoline.
    map<string, bool> rtapi;
    bool RuntimeApi(const string &name) {
        if (auto it = rtapi.find(name); it != rtapi.end()) return it->second;
        // The C library's math functions are plain symbols (never macros, unlike isnan), so no
        // trampoline.
        static const set<string> libm = [] {
            set<string> r;
            string names = "acos asin atan atan2 cbrt ceil copysign cos cosh erf exp exp2 expm1 fabs "
                           "fdim floor fma fmax fmin fmod hypot lgamma log log10 log1p log2 nearbyint "
                           "pow remainder rint round sin sinh sqrt tan tanh tgamma trunc ";
            for (size_t b = 0, e; (e = names.find(' ', b)) != string::npos; b = e + 1) {
                r.insert(names.substr(b, e - b));
                r.insert(names.substr(b, e - b) + "f");
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
        // C11, the C half's: lldb formats the same under every code tinygen offers
        // (docs/design/tinygen_backend.md), and the two halves' units then read alike.
        if (m) tyg_module_language(m, TYG_LANG_C11);
    }
    ~TgModule() { if (m) tyg_module_free(m); }

    // The source file of index fileidx as the debugger and the abort messages name it.
    string SourceName(int fileidx) {
        return fileidx >= 0 && fileidx < (int)ast.sources.size() ? ast.sources[fileidx].first :
            string("?");
    }
    tyg_file File(int fileidx) {
        if (auto it = files.find(fileidx); it != files.end()) return it->second;
        auto path = SourceName(fileidx);
        std::error_code ec;
        auto abs = filesystem::absolute(filesystem::path(path), ec);
        if (!ec) path = abs.lexically_normal().string();
        return files[fileidx] = tyg_debug_file(m, path.c_str());
    }

    tyg_sym Sym(const string &name, unsigned flags) {
        if (auto it = syms.find(name); it != syms.end()) return it->second;
        return syms[name] = tyg_symbol(m, name.c_str(), flags);
    }

    // Read-only bytes: a string literal's, or a file name's (with its NUL) for the runtime's abort
    // messages.
    tyg_sym Bytes(const string &bytes) {
        if (auto it = strings.find(bytes); it != strings.end()) return it->second;
        auto s = tyg_symbol(m, cat("gs_tg_str", strings.size()).c_str(), 0);
        tyg_data(m, s, TYG_SECTION_RODATA, bytes.data(), (int64_t)max<size_t>(bytes.size(), 1), 1);
        return strings[bytes] = s;
    }
    tyg_sym FileName(int fileidx) {
        if (auto it = filenames.find(fileidx); it != filenames.end()) return it->second;
        auto f = SourceName(fileidx);
        for (auto &c : f)
            if (c == '\\') c = '/';
        f.push_back('\0');
        return filenames[fileidx] = Bytes(f);
    }

    void NeedShim(const string &name, const string &text) {
        for (auto &s : shims)
            if (s.first == name) return;
        shims.emplace_back(name, text);
    }
    // One of the shims that do not depend on the program (TgFixedShim).
    void NeedShim(const string &name) {
        auto text = TgFixedShim(name);
        assert(!text.empty());
        NeedShim(name, text);
    }
    // The accessor of the queue for elements of type t (CodeGen::QueueFor), returning the gs_queue's
    // address.
    string QueueAccessor(TypeExpr *t) {
        auto q = cg.QueueFor(t);
        auto name = cat("gs_tg_q_", q);
        // The shim again for each use: a function abandoned after it took it back.
        queueacc[name] = q;
        NeedShim(name, cat("void *", name, "(void) { return &", q, "; }\n"));
        return name;
    }
    // The accessor of a worker's entry thunk (CodeGen::EnsureThreadThunk), returning the thunk's
    // address.
    string ThunkAccessor(FnSpec *sp) {
        auto thunk = cg.EnsureThreadThunk(sp);
        auto name = cat("gs_tg_t_", thunk);
        thunkacc[name] = sp;
        NeedShim(name, cat("void *", name, "(void) { return (void *)", thunk, "; }\n"));
        return name;
    }

    // ---- Types ----------------------------------------------------------------------------------

    static string Name(TypeExpr *t) {
        string s;
        t->Dump(s);
        return s;
    }
    static bool IsSigned(TypeExpr *t) { return t->kind == TY_INT && !IsUnsigned(t->intstorage); }

    // The ordered fields of a struct or of one ADT variant's payload, built once per type: their types,
    // each one's offset (the C layout if all are fixed, else the packed bytes one, where a bare pad
    // adds nothing) up to the first variable-size field `var`, `pre` the bytes before it (the size if
    // none), `last` the last real field (a resizable's tail). A frame object's C struct has `coff`: its
    // fixed fields aligned, the tail's header at coff[var]. Pads are skipped by the walkers.
    struct FieldSet {
        const vector<Field> &f;
        const vector<TypeExpr *> &t;
        vector<int64_t> off, coff;
        int64_t pre = 0;
        int var = -1, last = -1;
        bool bare = false;   // A bare pad (aligns in a C layout, inserts nothing in bytes) among `off`.
        FieldSet(const vector<Field> &_f, const vector<TypeExpr *> &_t) : f(_f), t(_t) {}
        // Whether ok(type) holds for each real field among the first n.
        template <class P> bool All(P ok, size_t n = SIZE_MAX) const {
            for (size_t i = 0; i < f.size() && i < n; i++)
                if (!f[i].ispad && !ok(t[i])) return false;
            return true;
        }
        // The first non-empty blk(index, type), "" if none.
        template <class B> string First(B blk) const {
            for (size_t i = 0; i < f.size(); i++)
                if (!f[i].ispad)
                    if (auto b = blk(i, t[i]); !b.empty()) return b;
            return "";
        }
    };
    map<pair<const void *, int>, FieldSet> fieldsets;
    const FieldSet &RunOf(const void *key, int vi, const vector<Field> &f, const vector<TypeExpr *> &t,
                          bool fo = false) {
        auto [it, fresh] = fieldsets.try_emplace({ key, vi }, f, t);
        auto &fs = it->second;
        if (!fresh) return fs;
        fs.last = LastRealField(f);
        auto fix = true;
        for (size_t i = 0; i < f.size(); i++) fix = fix && (f[i].ispad || cg.IsFix(t[i]));
        if (fix) {
            auto lo = cg.LayoutFields(f, t);
            fs.off = lo.offs;
            fs.pre = lo.size;
        } else {
            for (size_t i = 0; fs.var < 0; i++) {
                fs.off.push_back(fs.pre);
                if (f[i].ispad) fs.pre += PadSize(f[i]);
                else if (!cg.IsFix(t[i])) fs.var = (int)i;
                else fs.pre += cg.FixedSize(t[i]);
            }
        }
        for (size_t i = 0; i < fs.off.size(); i++)
            fs.bare = fs.bare || (f[i].ispad && f[i].padsize < 0);
        if (fo && fs.var >= 0) {
            auto n = (long)fs.var;
            auto lo = cg.LayoutFields({ f.begin(), f.begin() + n }, { t.begin(), t.begin() + n });
            fs.coff = lo.offs;
            fs.coff.push_back(lo.size);
        }
        return fs;
    }
    const FieldSet &StFields(TypeExpr *t) {
        auto si = cg.SI(t);
        return RunOf(si, -1, si->st->fields, si->ftypes, cg.IsFrameObj(t));
    }
    const FieldSet &VarFields(EnumInst *ei, int vi) {
        return RunOf(ei, vi, ei->en->variants[(size_t)vi].fields, ei->vftypes[(size_t)vi]);
    }
    // The run of a struct type or of a variant type.
    const FieldSet &RunOfType(TypeExpr *t) {
        return t->kind == TY_VARIANT ? VarFields(cg.EIVar(t), VariantIdx(t)) : StFields(t);
    }
    static int64_t PadSize(const Field &f) { return f.padsize > 0 ? f.padsize : 0; }

    // The structs the predicates are inside of: one reached again (through an optional link, `struct
    // Node { next: Node? }`) holds if the rest of it does.
    vector<TypeExpr *> tyopen;
    struct Open {
        TgModule *mod;
        Open(TgModule *_mod, TypeExpr *t) : mod(_mod) { mod->tyopen.push_back(t); }
        ~Open() { mod->tyopen.pop_back(); }
    };
    // f() with t open; `reentered` instead where t is already being checked.
    template <class R, class F> R Within(TypeExpr *t, R reentered, F f) {
        for (auto o : tyopen)
            if (cg.TEq(o, t)) return reentered;
        Open open { this, t };
        return f();
    }

    // A type this backend holds: a machine integer, a float, a bool, a plain reference to a fixed value
    // it holds, or a fixed struct of those.
    bool TypeOk(TypeExpr *t, int depth = 0) {
        if (!t || depth > 32) return false;
        auto d = depth + 1;
        switch (t->kind) {
            // A varint on its own is an i64, as the C holds it; in a bytes value it is zigzag ULEB128.
            case TY_INT: case TY_FLT: case TY_BOOL: return true;
            case TY_REF: {
                // A relative reference outside a slot holds the offset (§3.9); a varint one is an
                // image.
                if (IsRel(t)) return !IsVr(t) && RelOk(t, depth);
                // The address of a fixed value, or of an array or image read in place (a fat gs_rref
                // for a resizable one); an optional one is null where empty.
                if (!IsPlainRef(t) && !OptRef(t)) return false;
                auto s = t->ref->sub;
                return (cg.IsFix(s) && TypeOk(s, d)) || IsVarint(s) || ImgOk(s, d) || ZeroOk(s, d) ||
                       FoOk(s, d) || RsOk(s, d) || (IsPlainRef(t) && VeOk(s, d));
            }
            case TY_ARRAY:   // T[k], packed; a static-capacity T[..k] has a length beside the elements.
                if (ZeroArr(t)) return depth == 0 && ZeroOk(t, depth);
                if (t->arr->akind == A_LIMITED) return LimOk(t, depth);
                return t->arr->akind == A_FIXED && cg.IsFix(t) && cg.ArrSize(t->arr) > 0 &&
                    TypeOk(t->arr->sub, d);
            // { data, len } over fixed elements (C.3), or over images walked by their sizes.
            case TY_SLICE:
                return cg.IsFix(t->sub) ? TypeOk(t->sub, d) : ImgOk(t->sub, d) || IsVarint(t->sub);
            case TY_STRUCT:
                if (!cg.IsFix(t) || !cg.SI(t)) return false;
                return Within(t, true, [&] {
                    return StFields(t).All([&](TypeExpr *ft) { return TypeOk(ft, d); });
                });
            case TY_ENUM: case TY_VARIANT:   // Fixed-mode, its payload fields held.
                if ((t->kind == TY_ENUM && t->enu->varmode) || !cg.IsFix(t)) return false;
                return Within(t, true, [&] {
                    return AllVariants(t, [&](int, const FieldSet &fs) {
                        return fs.All([&](TypeExpr *ft) { return TypeOk(ft, d); });
                    });
                });
            default: return false;
        }
    }
    // Why TypeOk refuses a type, as a class: what -v says beside the type, so a sweep ranks reasons.
    string Blocker(TypeExpr *t, int depth = 0) {
        if (!t || depth > 32) return "unknown type";
        auto d = depth + 1;
        // Field ft's blocker under `pre`; skipvs: bar the variable size such a struct has by nature.
        auto why = [&](TypeExpr *ft, const char *pre, bool skipvs) {
            auto b = Blocker(ft, d);
            return b.empty() || (skipvs && b == "variable-size value") ? string() : cat(pre, b);
        };
        switch (t->kind) {
            case TY_INT: case TY_FLT: case TY_BOOL: return "";
            case TY_REF:
                if (IsRel(t) && depth > 0 && RelOk(t, depth)) return "";
                if (!IsPlainRef(t) && !OptRef(t))
                    return t->ref->optional ? "optional relative reference" : "relative reference";
                return ImgOk(t->ref->sub, d) ? "" : Blocker(t->ref->sub, d);
            case TY_SLICE:
                if (cg.IsFix(t->sub)) return Blocker(t->sub, d);
                return ImgOk(t->sub, d) || IsVarint(t->sub) ? "" : "variable-size elements";
            case TY_ARRAY: {
                if (t->arr->akind == A_LIMITED && cg.ArrSize(t->arr) < 0)
                    return RcOk(t, depth) ? "" : "runtime-capacity limited array";
                if (!cg.IsFix(t->arr->sub))
                    return VarImgOk(t, depth) || VeOk(t, depth) ? "" : "variable-size elements";
                auto b = Blocker(t->arr->sub, d);
                if (!b.empty()) return b;
                return t->arr->akind == A_FIXED || t->arr->akind == A_LIMITED ? "" :
                    "variable-size value";
            }
            case TY_STRUCT: {
                auto nm = Name(t);
                if (nm.rfind("dictionary", 0) == 0) return "dictionary";
                if (nm.rfind("vec", 0) == 0) return "SIMD vector";
                if (cg.IsFrameObj(t)) return FoOk(t, depth) ? "" : "frame object";
                if (IsRb(t)) return RbOk(t, depth) ? "" : "resizable struct";
                if (IsRs(t)) {
                    if (RsOk(t, depth)) return "";
                    auto last = StFields(t).last;
                    auto b = StFields(t).First([&](size_t i, TypeExpr *ft) {
                        if (!cg.IsResz(ft) || (int)i != last)
                            return why(ft, "resizable struct with ", true);
                        return !(IsGrow(ft) && RzOk(ft)) && !FoOk(ft) ?
                            cat("resizable struct with tail ", Name(ft))
                                                                       : string();
                    });
                    return b.empty() ? "resizable struct" : b;
                }
                if (!cg.IsFix(t)) {
                    if (VsOk(t, depth)) return "";
                    auto b = StFields(t).First(
                        [&](size_t, TypeExpr *ft) {
                            return why(ft, "variable-size struct with ", true);
                        });
                    return b.empty() ? "variable-size struct" : b;
                }
                if (!cg.SI(t)) return "empty struct";
                return Within(t, string(), [&] {
                    return StFields(t).First([&](size_t, TypeExpr *ft) {
                        return why(ft, "struct with ", false);
                    });
                });
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

    // A variable-size or resizable array of fixed elements, read and grown where it lies but never a
    // slot's value: T[] / T[uN], a length prefix then the elements (§6.1); T[>..] / T[>..<], a gs_rhdr
    // { base, len } with the elements on a data stack. Held behind a reference or as a by-value
    // parameter.
    bool RzOk(TypeExpr *t, int depth = 0) {
        if (!t || t->kind != TY_ARRAY || depth > 32) return false;
        return (IsVarArr(t) || IsGrow(t)) && cg.IsFix(t->arr->sub) && TypeOk(t->arr->sub, depth + 1);
    }
    // A variable-size struct (VsOk) is an image too, read where it lies.
    bool ImgOk(TypeExpr *t, int depth = 0) {
        return RzOk(t, depth) || VsOk(t, depth) || VrOk(t, depth) || VarImgOk(t, depth) ||
            VmOk(t, depth) ||
               VvOk(t, depth) || RcOk(t, depth) || RbOk(t, depth);
    }
    // Elements that are images themselves (strings, variable-size structs), or varints.
    bool ImgElemOk(TypeExpr *e, int depth) {
        return e && !cg.IsFix(e) && (ImgOk(e, depth + 1) || IsVarint(e));
    }
    // A T[] of images (u8[][]), and a resizable one: the elements back to back, each walked by its size
    // (CodeGen::SizeFn) and never indexed.
    bool VarImgOk(TypeExpr *t, int depth = 0) {
        return IsVarArr(t) && depth <= 32 && ImgElemOk(t->arr->sub, depth);
    }
    bool VeOk(TypeExpr *t, int depth = 0) {
        return IsGrow(t) && depth <= 32 && ImgElemOk(t->arr->sub, depth);
    }
    static bool IsGrow(TypeExpr *t) {
        return t && t->kind == TY_ARRAY && (t->arr->akind == A_GROW || t->arr->akind == A_GROWSHRINK);
    }
    static bool IsVarArr(TypeExpr *t) { return t && t->kind == TY_ARRAY && t->arr->akind == A_VAR; }
    // A T[] length prefix's bytes: u32 by default, else its storage's; 0 for a varint
    // (CodeGen::EmitLenStore).
    static int LenWidth(TypeExpr *t) {
        auto ls = t->arr->lenstorage;
        return ls < 0 ? 4 : ls == IS_VARINT ? 0 : IntBits((IntStorage)ls) / 8;
    }
    // The bytes reserved while the elements are built: a varint's one, moved up behind a longer count.
    static int PrefixBytes(TypeExpr *t) { return LenWidth(t) ? LenWidth(t) : 1; }
    static bool SignedLen(TypeExpr *t) {
        auto ls = t->arr->lenstorage;
        return ls == IS_I8 || ls == IS_I16 || ls == IS_I32 || ls == IS_I64;
    }
    // A by-value parameter of an image or resizable type; also what a global's stack or a result may
    // hold.
    bool RzParam(TypeExpr *t) {
        return RzOk(t) || VarImgOk(t) || RcOk(t) || VeOk(t) || VsOk(t) || VrOk(t) || VmOk(t) ||
            VvOk(t) ||
               FoOk(t) || RsOk(t) || RbOk(t);
    }
    // Every type a parameter, a result or a free variable may have.
    bool Held(TypeExpr *t) { return TypeOk(t) || RzParam(t); }

    static tyg_scalar ScalOfBytes(int w) {
        return w == 1 ? TYG_I8 : w == 2 ? TYG_I16 : w == 4 ? TYG_I32 : TYG_I64;
    }
    // log2, 0..3
    static int SizeClass(int size) { return size == 1 ? 0 : size == 2 ? 1 : size == 4 ? 2 : 3; }
    TypeExpr *UIntOf(int w) {
        return ast.inttypes[w == 1 ? IS_U8 : w == 2 ? IS_U16 : w == 8 ? IS_U64 : IS_U32];
    }
    tyg_scalar Scal(TypeExpr *t) {
        switch (t->kind) {
            case TY_INT: return ScalOfBytes(IntBits(t->intstorage) / 8);
            case TY_FLT: return t->fltstorage == FS_F32 ? TYG_F32 : TYG_F64;
            case TY_BOOL: return TYG_I8;
            default: return IsRel(t) ? RelScal(t) : TYG_I64;   // A reference: an address.
        }
    }
    static int ScalSize(tyg_scalar s) {
        return s == TYG_I8 ? 1 : s == TYG_I16 ? 2 : s == TYG_I32 || s == TYG_F32 ? 4 : 8;
    }
    int64_t Size(TypeExpr *t) {
        if (IsVr(t)) return 16;   // A temporary for the value: up to ten bytes of varint.
        if (IsRb(t)) return 16;   // Its header.
        if (IsScalar(t)) return ScalSize(Scal(t));
        // A payload-less variant: the gs_empty byte.
        if (t->kind == TY_VARIANT && !cg.FixedSize(t)) return 1;
        if (ZeroArr(t)) return cg.FixedSize(t) + cg.FixedSize(t->arr->sub);
        if (cg.IsFrameObj(t)) return FoSize(t);
        return cg.FixedSize(t);
    }

    struct FieldAt { TypeExpr *t; int64_t off; string_view name; };
    // The real fields of a struct or variant type with their offsets.
    vector<FieldAt> Fields(TypeExpr *t) {
        auto &fs = RunOfType(t);
        auto &off = fs.coff.empty() ? fs.off : fs.coff;
        vector<FieldAt> r;
        for (size_t i = 0; i < off.size(); i++)
            if (!fs.f[i].ispad) r.push_back({ fs.t[i], off[i], fs.f[i].name });
        return r;
    }

    // The type the C calling convention sees: packed structs are records of their fields at their
    // packed offsets, aligned to 1, as #pragma pack(1) makes them in the C half.
    const tyg_type *AT(TypeExpr *t) {
        if (t->kind == TY_REF && cg.IsFatRef(t)) return PairAT();
        if (IsRel(t) && !IsVr(t)) return tyg_type_int(m, RelScal(t), 0);
        // A varint-width relative reference: its image.
        if (t->kind == TY_REF) return tyg_type_ptr(m);
        // gs_rhdr, as a slice.
        if (IsGrow(t) || IsRs(t) || IsRb(t) || t->kind == TY_SLICE) return SliceAT();
        if (t->kind == TY_INT) return tyg_type_int(m, Scal(t), IsSigned(t));
        if (t->kind == TY_BOOL) return tyg_type_int(m, TYG_I8, 0);
        if (t->kind == TY_FLT) return tyg_type_scalar(m, Scal(t));
        auto key = cg.Mangle(t);
        if (auto it = atypes.find(key); it != atypes.end()) return it->second;
        if (IsLim(t)) return LimAT(t);
        if (t->kind == TY_ENUM) {   // The tag, the payload records behind it.
            auto ei = cg.EIOf(t);
            vector<tyg_field> fs { { tyg_type_int(m, TagScal(t), 0), 0 } };
            for (size_t vi = 0; vi < ei->en->variants.size(); vi++)
                if (cg.VariantLayout(ei, (int)vi).size)
                    fs.push_back({ AT(cg.VariantType(t, (int)vi)), TagSize(t) });
            return atypes[key] = tyg_type_record(m, cg.FixedSize(t), 1, fs.data(), (int)fs.size());
        }
        if (t->kind == TY_VARIANT && !cg.FixedSize(t)) {   // struct { uint8_t gs_empty; }
            tyg_field f { tyg_type_int(m, TYG_I8, 0), 0 };
            return atypes[key] = tyg_type_record(m, 1, 1, &f, 1);
        }
        if (t->kind == TY_ARRAY) {   // The C half's struct { T e[k]; }, packed.
            tyg_field f { tyg_type_array(m, AT(t->arr->sub), std::max<int64_t>(cg.ArrSize(t->arr), 1)),
                          0 };
            return atypes[key] = tyg_type_record(m, Size(t), 1, &f, 1);
        }
        vector<tyg_field> fs;
        for (auto &f : Fields(t)) fs.push_back({ AT(f.t), f.off });
        return atypes[key] = tyg_type_record(m, Size(t), 1, fs.data(), (int)fs.size());
    }
    // A resizable value: a T[>..]'s header, or a frame object, with a stack.
    bool Resz(TypeExpr *t) {
        return IsGrow(t) || IsRs(t) || IsRb(t) || (t && t->kind == TY_STRUCT && cg.IsFrameObj(t));
    }
    // { ptr, <second> at 8 }: the extern boundary's u8 slice sl_u8 (data, length), and a fat reference
    // gs_rref (the header's address, the stack's).
    const tyg_type *PtrPairAT(const tyg_type *&cache, bool lenSecond) {
        if (cache) return cache;
        tyg_field fs[2] = { { tyg_type_ptr(m), 0 },
                            { lenSecond ? tyg_type_int(m, TYG_I64, 1) : tyg_type_ptr(m), 8 } };
        return cache = tyg_type_record(m, 16, 1, fs, 2);
    }
    const tyg_type *sliceat = nullptr, *pairat = nullptr;
    const tyg_type *SliceAT() { return PtrPairAT(sliceat, true); }
    const tyg_type *PairAT() { return PtrPairAT(pairat, false); }

    // ---- Debug types ----------------------------------------------------------------------------

    // A struct debug type under `name`, registered before its members so one may refer back to it.
    tyg_dtype *DRec(const string &name, int fileidx = 0, int line = 0) {
        auto rec = tyg_dt_record(m, TYG_DT_STRUCT, name.c_str(), File(fileidx), line);
        dtypes[name] = rec;
        return rec;
    }
    tyg_dtype *DRec(const string &name, Line ln) { return DRec(name, ln.fileidx, ln.line); }
    void DMem(tyg_dtype *rec, string_view name, const tyg_dtype *t, int64_t off) {
        tyg_dt_member(m, rec, string(name).c_str(), t, off);
    }
    const tyg_dtype *DEnd(tyg_dtype *rec, int64_t size) {
        tyg_dt_record_complete(m, rec, size, 1);
        return rec;
    }
    // { first: elem *, len: i64 }: a slice, a gs_rhdr, or a resizable value's header (elem null: u8).
    const tyg_dtype *DHdr(const string &name, const char *first, TypeExpr *elem, Line ln = {}) {
        auto rec = DRec(name, ln);
        DMem(rec, first, tyg_dt_pointer(m, DT(elem ? elem : ast.inttypes[IS_U8])), 0);
        DMem(rec, "len", DT(ast.inttypes[IS_I64]), 8);
        return DEnd(rec, 16);
    }

    // The type the debugger sees, under its Goose name.
    const tyg_dtype *DT(TypeExpr *t) {
        auto name = Name(t);
        if (auto it = dtypes.find(name); it != dtypes.end()) return it->second;
        // A scalar is a typedef of the C type of its size: lldb names a base type its own way (an i64
        // shows as `long`) but keeps a typedef's name.
        auto scalar = [&](const char *cname, tyg_dt_encoding enc) {
            auto base = tyg_dt_base(m, cname, ScalSize(Scal(t)), enc);
            return dtypes[name] = tyg_dt_typedef(m, name.c_str(), base, File(0), 0);
        };
        switch (t->kind) {
            case TY_INT: {
                static const char *cn[] = { "signed char", "short", "int", "long long", "unsigned char",
                                            "unsigned short", "unsigned int", "unsigned long long" };
                auto u = IsUnsigned(t->intstorage);
                return scalar(cn[(u ? 4 : 0) + SizeClass(ScalSize(Scal(t)))], u ? TYG_DT_UNSIGNED :
                              TYG_DT_SIGNED);
            }
            case TY_FLT: return scalar(Scal(t) == TYG_F32 ? "float" : "double", TYG_DT_FLOAT);
            case TY_BOOL: return scalar("_Bool", TYG_DT_BOOL);
            case TY_REF: {
                if (IsRel(t)) {   // The offset the slot holds, under the reference's name.
                    static const char *un[] = { "unsigned char", "unsigned short", "unsigned int",
                                                "unsigned long long" };
                    // A varint: its first byte
                    return scalar(un[SizeClass(IsVr(t) ? 1 : ScalSize(Scal(t)))], TYG_DT_UNSIGNED);
                }
                // An optional: a typedef under its Goose name over the C half's pointer.
                if (!cg.IsFatRef(t)) {
                    auto p = tyg_dt_pointer(m, DT(t->ref->sub));
                    return dtypes[name] = OptRef(t) ? tyg_dt_typedef(m, name.c_str(), p, File(0), 0) :
                        p;
                }
                auto rec = DRec(name);   // gs_rref: the header and the stack its elements grow on.
                DMem(rec, "hdr", tyg_dt_pointer(m, DT(t->ref->sub)), 0);
                DMem(rec, "stk", tyg_dt_pointer(m, StackDT()), 8);
                return DEnd(rec, 16);
            }
            case TY_ARRAY: {
                if (t->arr->akind == A_FIXED)
                    return dtypes[name] = tyg_dt_array(m, DT(t->arr->sub),
                                                       std::max<int64_t>(cg.ArrSize(t->arr), 1));
                if (IsLim(t)) return LimDT(t, name);
                if (t->arr->akind != A_VAR) return DHdr(name, "base", t->arr->sub);   // gs_rhdr
                // The image: its length prefix, then as many elements as it says.
                auto rec = DRec(name);
                auto w = PrefixBytes(t);
                DMem(rec, "len", DT(UIntOf(w)), 0);
                DMem(rec, "data", tyg_dt_array(m, DT(t->arr->sub), -1), w);
                return DEnd(rec, w);
            }
            // tinygen has no array whose count is a member
            case TY_SLICE: return DHdr(name, "data", t->sub);
            case TY_STRUCT: {
                if (IsRb(t)) return DHdr(name, "base", nullptr);
                if (IsVs(t)) return ImgDT(t, name);
                if (IsRs(t)) return DHdr(name, "base", nullptr, cg.SI(t)->st->line);
                return FieldsDT(t, name);
            }
            case TY_ENUM: case TY_VARIANT:
                if (IsRb(t)) return DHdr(name, "base", nullptr);
                if (IsVv(t)) return ImgDT(t, name);
                return t->kind == TY_VARIANT ? FieldsDT(t, name) : AdtDT(t, name);
            default: return dtypes[name] = tyg_dt_void(m);
        }
    }

    // ---- Limited arrays T[..k] ------------------------------------------------------------------

    // A static-capacity limited array (CodeGen::IsStaticLimited) is a fixed value: the C half's packed
    // struct { lenT len; T e[k]; }, lenT the smallest unsigned type that fits k (CodeGen::LenStore).
    static bool IsLim(TypeExpr *t) { return t && t->kind == TY_ARRAY && t->arr->akind == A_LIMITED; }
    // A runtime-capacity T[..]: [cap u32][len u32][cap slots].
    bool IsRc(TypeExpr *t) { return IsLim(t) && cg.ArrSize(t->arr) < 0; }
    bool RcOk(TypeExpr *t, int depth = 0) {
        return IsRc(t) && depth <= 32 && cg.IsFix(t->arr->sub) && TypeOk(t->arr->sub, depth + 1);
    }
    // A limited array this backend holds, of either kind.
    bool LimHeld(TypeExpr *t) { return TypeOk(t) || RcOk(t); }
    // A T[0] or T[..0] (§3.4): C gives it one element's slot, so as a value of its own it is that much
    // bigger than its layout; held only so (not as a field or element).
    bool ZeroArr(TypeExpr *t) {
        return t && t->kind == TY_ARRAY && (t->arr->akind == A_FIXED || cg.IsStaticLimited(t)) &&
               cg.ArrSize(t->arr) == 0;
    }
    bool ZeroOk(TypeExpr *t, int depth) {
        return ZeroArr(t) && cg.IsFix(t) && depth <= 32 && TypeOk(t->arr->sub, depth + 1);
    }
    bool LimOk(TypeExpr *t, int depth) {
        return cg.IsStaticLimited(t) && cg.ArrSize(t->arr) > 0 && TypeOk(t->arr->sub, depth + 1);
    }
    // The length field's bytes: where the slots start.
    int LimWidth(TypeExpr *t) { return IsRc(t) ? 8 : (int)CodeGen::IntSize(cg.LenStore(t->arr)); }
    tyg_scalar LimLenScal(TypeExpr *t) { return IsRc(t) ? TYG_I32 : ScalOfBytes(LimWidth(t)); }
    const tyg_type *LimAT(TypeExpr *t) {   // A record of the length and an array of the k slots.
        auto key = cg.Mangle(t);
        if (auto it = atypes.find(key); it != atypes.end()) return it->second;
        auto slots = tyg_type_array(m, AT(t->arr->sub), std::max<int64_t>(cg.ArrSize(t->arr), 1));
        tyg_field fs[2] = { { tyg_type_int(m, LimLenScal(t), 0), 0 }, { slots, LimWidth(t) } };
        return atypes[key] = tyg_type_record(m, Size(t), 1, fs, 2);
    }
    // `len` and all k slots `e` (tinygen has no array whose count is a member, gap 3); a
    // runtime-capacity one is its header.
    const tyg_dtype *LimDT(TypeExpr *t, const string &name) {
        auto rec = DRec(name);
        if (IsRc(t)) {
            DMem(rec, "cap", DT(ast.inttypes[IS_U32]), 0);
            DMem(rec, "len", DT(ast.inttypes[IS_U32]), 4);
            return DEnd(rec, 8);
        }
        auto w = LimWidth(t);
        DMem(rec, "len", DT(UIntOf(w)), 0);
        DMem(rec, "e", tyg_dt_array(m, DT(t->arr->sub), std::max<int64_t>(cg.ArrSize(t->arr), 1)), w);
        return DEnd(rec, Size(t));
    }

    // gs_stack { top }: where the next element goes.
    const tyg_dtype *stackdt = nullptr;
    const tyg_dtype *StackDT() {
        if (stackdt) return stackdt;
        auto rec = tyg_dt_record(m, TYG_DT_STRUCT, "gs_stack", File(0), 0);
        stackdt = rec;
        DMem(rec, "top", tyg_dt_pointer(m, DT(ast.inttypes[IS_U8])), 0);
        return DEnd(rec, 8);
    }

    // ---- Frame objects --------------------------------------------------------------------------

    // A frame object (C.2, CodeGen::IsFrameObj): a struct whose last field is a resizable: its fixed
    // fields, packed, then the tail's header { base, len } (EmitCFields), the elements on a data stack.
    bool FoOk(TypeExpr *t, int depth = 0) {
        if (!t || t->kind != TY_STRUCT || depth > 32 || !cg.IsFrameObj(t)) return false;
        return Within(t, true, [&] {
            auto &fs = StFields(t);
            if ((size_t)fs.var + 1 != fs.f.size()) return false;
            auto tail = fs.t[(size_t)fs.var];
            auto fixed = [&](TypeExpr *ft) { return cg.IsFix(ft) && TypeOk(ft, depth + 1); };
            return ((IsGrow(tail) && (RzOk(tail, depth + 1) || VeOk(tail, depth + 1))) ||
                    FoOk(tail, depth + 1)) &&
                   fs.All(fixed, (size_t)fs.var);
        });
    }
    // The C struct's bytes: the fixed fields, then the tail: a resizable's header, or a nested frame
    // object's struct, which ends in its own tail.
    int64_t FoSize(TypeExpr *t) {
        auto &fs = StFields(t);
        auto tail = fs.t[(size_t)fs.var];
        return fs.coff[(size_t)fs.var] + (IsGrow(tail) ? 16 : Size(tail));
    }
    // Where the innermost tail's header lies in the struct.
    int64_t FoHdrOff(TypeExpr *t) {
        auto &fs = StFields(t);
        auto tail = fs.t[(size_t)fs.var];
        return IsGrow(tail) ? fs.coff[(size_t)fs.var] : fs.coff[(size_t)fs.var] + FoHdrOff(tail);
    }

    // ---- Variable-size structs ------------------------------------------------------------------

    // A struct with a variable-size field (bytes class, §6.1): one image of its fields in declaration
    // order, packed, each variable-size one inline at its own size; a uint8_t * points at it, as at a
    // T[].
    bool IsVs(TypeExpr *t) { return t && t->kind == TY_STRUCT && cg.Cls(t) == SC_VARIABLE; }
    // A resizable struct that is no frame object: its fields before the tail are not all fixed, so they
    // are bytes as a variable-size struct's are, the tail's elements follow on the data stack. The
    // value is a gs_rhdr { base, len }: the image's start, the tail's count (CodeGen::FieldLocAt,
    // BytesLoc).
    bool IsRs(TypeExpr *t) {
        return t && t->kind == TY_STRUCT && cg.IsResz(t) && !cg.IsFrameObj(t) && !IsRb(t);
    }
    // Held: the fields before the tail as a variable-size struct's (FieldsOk), the tail a resizable of
    // fixed elements.
    bool RsOk(TypeExpr *t, int depth = 0) {
        if (!IsRs(t) || depth > 32) return false;
        return Within(t, true, [&] {
            auto &fs = StFields(t);
            if (fs.last < 0) return false;
            auto tail = fs.t[(size_t)fs.last];
            return ((IsGrow(tail) && RzOk(tail, depth + 1)) || FoOk(tail, depth + 1)) &&
                   FieldsOk(fs, depth + 1, (size_t)fs.last);
        });
    }
    // An image: a T[] or such a struct. (A resizable ADT is a header and a stack, IsRb, not an image.)
    bool IsImg(TypeExpr *t) {
        return IsVarArr(t) || IsVs(t) || ((IsVm(t) || IsVv(t)) && !IsRb(t)) || IsRc(t) || IsVr(t);
    }
    // Held: its fields are fixed values held, T[]s of fixed elements (RzOk), or such structs.
    bool VsOk(TypeExpr *t, int depth = 0) {
        if (!IsVs(t) || depth > 32 || cg.SI(t)->sclass != SC_VARIABLE) return false;
        return Within(t, true, [&] { return FieldsOk(StFields(t), depth); });
    }
    // The fewest bytes an image of t takes: its fields up to the first variable-size one, and that
    // one's least (a T[]'s prefix).
    int64_t VsMinSize(TypeExpr *t) {
        if (cg.IsFix(t)) return cg.FixedSize(t);
        if (IsVarint(t) || IsVr(t)) return 1;
        if (IsVarArr(t)) return PrefixBytes(t);
        if (IsRc(t)) return 8;   // cap and len.
        if (IsVm(t)) return TagSize(t);
        auto &fs = RunOfType(t);
        return fs.pre + VsMinSize(fs.t[(size_t)fs.var]);
    }
    // An image's record: its fields at their offsets up to and including the first variable-size one (a
    // T[]'s `len` and `data`), where later offsets stop being constants. A variable of it is a pointer
    // to the record, as for a T[].
    const tyg_dtype *ImgDT(TypeExpr *t, const string &name) {
        auto &fs = RunOfType(t);
        auto rec = t->kind == TY_STRUCT ? DRec(name, cg.SI(t)->st->line) : DRec(name);
        for (size_t i = 0; i < fs.off.size(); i++)
            if (!fs.f[i].ispad) DMem(rec, fs.f[i].name, DT(fs.t[i]), fs.off[i]);
        return DEnd(rec, VsMinSize(t));
    }

    // ---- ADTs -----------------------------------------------------------------------------------

    // As the C backend lays them out (CodeGen::CT): a fixed-mode ADT is the packed struct { tag; union
    // { v_<Variant> } }, the tag a u8 (u16 past 256 variants); a variant type is a packed struct of its
    // payload's fields, a payload-less one C's gs_empty byte.
    EnumInst *EI(TypeExpr *t) { return t->kind == TY_VARIANT ? cg.EIVar(t) : cg.EIOf(t); }
    int VariantIdx(TypeExpr *vt) { return cg.EIVar(vt)->en->VariantIndex(vt->var->variant); }
    int64_t TagSize(TypeExpr *t) { return cg.TagSize(EI(t)->en); }
    tyg_scalar TagScal(TypeExpr *t) { return TagSize(t) == 1 ? TYG_I8 : TYG_I16; }
    // The variants [lo, hi) a value of an ADT type may be: all of an enum's, a variant type's own.
    pair<int, int> VarSpan(TypeExpr *t) {
        if (t->kind != TY_VARIANT) return { 0, (int)EI(t)->en->variants.size() };
        auto vi = VariantIdx(t);
        return { vi, vi + 1 };
    }

    // Whether ok(vi, run) holds for each variant a value of the ADT or variant type t may be.
    template <class F> bool AllVariants(TypeExpr *t, F ok) {
        auto [lo, hi] = VarSpan(t);
        for (auto vi = lo; vi < hi; vi++)
            if (!ok(vi, VarFields(EI(t), vi))) return false;
        return true;
    }
    string AdtBlocker(TypeExpr *t, int depth) {
        if (IsRb(t)) return RbOk(t, depth) ? "" : "resizable ADT";
        if (t->kind == TY_ENUM && t->enu->varmode) return VmOk(t, depth) ? "" : "variable-mode ADT";
        if (!cg.IsFix(t)) return VvOk(t, depth) ? "" : "variable-size ADT";
        return Within(t, string(), [&] {
            auto ei = EI(t);
            auto [lo, hi] = VarSpan(t);
            for (auto vi = lo; vi < hi; vi++) {
                auto b = VarFields(ei, vi).First([&](size_t, TypeExpr *ft) {
                    return Blocker(ft, depth + 1);
                });
                if (!b.empty()) return b.rfind("ADT", 0) == 0 ? b : cat("ADT with ", b);   // One class.
            }
            return string();
        });
    }
    // A fixed struct or variant's record (a variant without fields: C's gs_empty byte). A field may
    // refer back to it through a reference.
    const tyg_dtype *FieldsDT(TypeExpr *t, const string &name) {
        auto fs = Fields(t);
        auto rec = t->kind == TY_STRUCT ? DRec(name, cg.SI(t)->st->line) : DRec(name);
        if (fs.empty() && t->kind == TY_VARIANT) DMem(rec, "gs_empty", DT(ast.inttypes[IS_U8]), 0);
        for (auto &f : fs) DMem(rec, f.name, DT(f.t), f.off);
        return DEnd(rec, Size(t));
    }
    // The C's debugger view under Goose names: `Shape` a struct of `tag`, an enum of the variants'
    // names, and `u`, a union of the payloads by variant name; `Shape.Circle` a struct of its fields.
    const tyg_dtype *AdtDT(TypeExpr *t, const string &name) {
        auto ei = cg.EIOf(t);
        auto en = ei->en;
        auto rec = DRec(name, en->line);
        auto tagbase = tyg_dt_base(m, TagSize(t) == 1 ? "unsigned char" : "unsigned short", TagSize(t),
                                   TYG_DT_UNSIGNED);
        auto tag = tyg_dt_enum(m, cat(name, ".tag").c_str(), tagbase, File(en->line.fileidx),
                               en->line.line);
        for (size_t vi = 0; vi < en->variants.size(); vi++)
            tyg_dt_enumerator(m, tag, string(en->variants[vi].name).c_str(), (int64_t)vi);
        DMem(rec, "tag", tag, 0);
        auto full = IsVm(t) ? TagSize(t) : cg.FixedSize(t);   // Variable-mode: the largest payload.
        for (size_t vi = 0; IsVm(t) && vi < en->variants.size(); vi++) {
            auto vt = cg.VariantType(t, (int)vi);
            if (cg.IsFix(vt)) full = std::max(full, TagSize(t) + cg.FixedSize(vt));
        }
        if (full > TagSize(t)) {
            auto u = tyg_dt_record(m, TYG_DT_UNION, nullptr, File(0), 0);
            for (size_t vi = 0; vi < en->variants.size(); vi++) {
                auto vt = cg.VariantType(t, (int)vi);
                if (!cg.IsFix(vt) || cg.VariantLayout(ei, (int)vi).size)
                    DMem(u, en->variants[vi].name, DT(vt), 0);
            }
            DEnd(u, full - TagSize(t));
            DMem(rec, "u", u, TagSize(t));
        }
        return DEnd(rec, full);
    }

    // ---- Variable-mode ADTs ---------------------------------------------------------------------

    // A variable-mode ADT (`Shape..`, §8.1) is a bytes value: its tag, then the variant's payload
    // fields, packed, each value its own variant's size (CodeGen's GenStructLit).
    static bool IsVm(TypeExpr *t) { return t && t->kind == TY_ENUM && t->enu->varmode; }
    static bool IsVarint(TypeExpr *t) { return t && t->kind == TY_INT && t->intstorage == IS_VARINT; }
    // A relative reference (§3.9): an offset of a fixed width in the slot, from its pool's base less
    // one where `in pool` (0 is null), else from the slot itself. A value of one is the address it
    // decodes to.
    static bool IsRel(const TypeExpr *t) { return t && t->kind == TY_REF && t->ref->lenstorage >= 0; }
    bool RelOk(TypeExpr *t, int depth = 0) {
        if (!IsRel(t) || depth > 32) return false;
        auto w = t->ref->lenstorage;
        return (w == IS_U8 || w == IS_U16 || w == IS_U32 || w == IS_U64 || w == IS_VARINT) &&
            RelSubOk(t, depth);
    }
    // The varint width (`T&<varint in pool>`, `T&<varint>`): an ULEB128 offset (zigzag when
    // self-relative), so the value is variable-size, a bytes value a uint8_t * points at, as a T[] is
    // (SC_VARIABLE).
    static bool IsVr(const TypeExpr *t) { return IsRel(t) && t->ref->lenstorage == IS_VARINT; }
    bool VrOk(TypeExpr *t, int depth = 0) { return IsVr(t) && depth <= 32 && RelSubOk(t, depth); }
    bool RelSubOk(TypeExpr *t, int depth) {
        auto sub = t->ref->sub;
        if (cg.IsFix(sub)) return TypeOk(sub, depth + 1);
        if (relimg.count(sub)) return true;   // An image it points at, which may link to its own type.
        relimg.insert(sub);
        auto ok = ImgOk(sub, depth + 1);
        relimg.erase(sub);
        return ok;
    }
    set<TypeExpr *> relimg;
    tyg_scalar RelScal(TypeExpr *t) {
        auto w = t->ref->lenstorage;
        return ScalOfBytes(w == IS_U8 ? 1 : w == IS_U16 ? 2 : w == IS_U32 ? 4 : 8);
    }
    // Whether the payload fields of each variant a value of t may be are ones this backend reads and
    // builds as bytes.
    bool PayloadsOk(TypeExpr *t, int depth) {
        return Within(t, true, [&] {
            return AllVariants(t, [&](int, const FieldSet &fs) { return FieldsOk(fs, depth + 1); });
        });
    }
    bool VmOk(TypeExpr *t, int depth = 0) { return IsVm(t) && depth <= 32 && PayloadsOk(t, depth); }
    // Whether the first n fields (all by default) are ones this backend reads and builds: fixed ones it
    // holds, T[]s and variable-size structs it holds as images.
    bool FieldsOk(const FieldSet &fs, int depth, size_t n = SIZE_MAX) {
        auto d = depth + 1;
        return fs.All([&](TypeExpr *ft) {
            if (IsVarint(ft)) return true;
            if (IsVr(ft)) return VrOk(ft, d);
            if (cg.IsFix(ft)) return TypeOk(ft, d);
            return (IsVarArr(ft) && (RzOk(ft, d) || VarImgOk(ft, d))) || RcOk(ft, d) || VsOk(ft, d) ||
                VmOk(ft, d);
        }, n);
    }
    // A variant type whose payload has a variable-size field (`Msg.Text` with a u8[]): an image of its
    // fields, as a variable-size struct is.
    bool IsVv(TypeExpr *t) { return t && t->kind == TY_VARIANT && !cg.IsFix(t); }
    bool VvOk(TypeExpr *t, int depth = 0) { return IsVv(t) && depth <= 32 && PayloadsOk(t, depth); }

    // ---- Resizable ADTs -------------------------------------------------------------------------

    // A resizable-class ADT (`Log..` with a variant ending in a T[>..]) or one of its variants: a
    // gs_rhdr { base, len } and a data stack, the bytes at base being the value's own layout: its tag,
    // the variant's fixed fields, packed, then the tail's `len` elements (CodeGen::GenStructLit,
    // VariantBehindTag). A fixed variant has no tail (count zero).
    bool IsRb(TypeExpr *t) {
        if (!t) return false;
        if (t->kind == TY_ENUM) return t->enu->varmode && cg.IsResz(t);
        // Resizable, no frame object: a fixed prefix, and such a value as the tail.
        if (t->kind == TY_STRUCT) {
            if (!cg.IsResz(t) || cg.IsFrameObj(t) || !cg.SI(t)) return false;
            auto &fs = StFields(t);
            return fs.last >= 0 && fs.t[(size_t)fs.last]->kind != TY_ARRAY &&
                IsRb(fs.t[(size_t)fs.last]);
        }
        return t->kind == TY_VARIANT && cg.IsResz(t);
    }
    // What a variant's payload is in a resizable ADT's bytes, past its tag: the bytes before the tail's
    // elements, and what the tail is.
    struct RbShape {
        int64_t pre = 0;
        TypeExpr *elem = nullptr;    // The tail's element type, null without one.
        int tailfi = -1;
        bool img = false;            // A variable-size field: an image, walked by its size (VvOk).
        // The tail is a frame object, its fields bytes ahead of its elements.
        bool fo = false;
        bool ok = true;
    };
    RbShape RbShapeOf(EnumInst *ei, int vi) {
        auto &fs = VarFields(ei, vi);
        RbShape sh;
        sh.pre = fs.pre;
        sh.ok = !fs.bare;
        if (fs.var < 0) return sh;
        auto ft = fs.t[(size_t)fs.var];
        if (cg.IsResz(ft)) {
            sh.tailfi = fs.var;
            if (fs.var != fs.last) sh.ok = false;
            else if (IsGrow(ft)) sh.elem = ft->arr->sub;
            // As bytes (CodeGen::FoBytes): its fixed fields, then its tail.
            else if (cg.IsFrameObj(ft)) {
                sh.elem = cg.FoTailArr(ft)->arr->sub;
                sh.fo = true;
                sh.pre += cg.FoBytesPrefix(ft);
            } else sh.ok = false;
        } else {   // A variable-size field: a size to walk.
            sh.img = true;
            for (auto j = (size_t)fs.var + 1; j < fs.f.size(); j++)
                if (!fs.f[j].ispad && cg.IsResz(fs.t[j])) sh.ok = false;
        }
        return sh;
    }
    // Variant vi of the ADT or variant type t, which a resizable ADT holds: a fixed one, a
    // variable-size one (an image, no tail), or one ending in a T[>..].
    bool RbVarOk(TypeExpr *t, int vi, int depth) {
        auto &fs = VarFields(EI(t), vi);
        auto sh = RbShapeOf(EI(t), vi);
        if (!sh.ok) return false;
        if (sh.img) return VvOk(t->kind == TY_VARIANT ? t : cg.VariantType(t, vi), depth + 1);
        auto d = depth + 1;
        auto tail = sh.tailfi < 0 ? nullptr : fs.t[(size_t)sh.tailfi];
        auto fixed = [&](TypeExpr *ft) { return cg.IsFix(ft) && TypeOk(ft, d); };
        return fs.All(fixed, tail ? (size_t)sh.tailfi : SIZE_MAX) &&
               (!tail || (sh.fo ? FoOk(tail, d) && RzOk(cg.FoTailArr(tail), d) : RzOk(tail, d)));
    }
    bool RbOk(TypeExpr *t, int depth = 0) {
        if (!IsRb(t) || depth > 32) return false;
        return Within(t, true, [&] {
            if (t->kind == TY_STRUCT) {   // Its fixed fields held, its tail a resizable ADT.
                auto &fs = StFields(t);
                if (fs.bare || fs.var < 0 || fs.var != fs.last) return false;
                auto tail = fs.t[(size_t)fs.var];
                return fs.All([&](TypeExpr *ft) { return TypeOk(ft, depth + 1); }, (size_t)fs.var) &&
                       tail->kind != TY_ARRAY && IsRb(tail) && RbOk(tail, depth + 1);
            }
            return AllVariants(t, [&](int vi, const FieldSet &) { return RbVarOk(t, vi, depth); });
        });
    }

    // ---- Optionals ------------------------------------------------------------------------------

    // An optional reference T? (§3.8) as the C backend has it: the address a plain T& would be, 0 for
    // null; for a resizable, the fat reference gs_rref { hdr, stk } with a null hdr. Relative optionals
    // stay in C.
    static bool OptRef(const TypeExpr *t) { return IsOptional(t) && t->ref->lenstorage < 0; }
    // Whether a struct or ADT reaches a value of its own type through its fields (an optional link, a
    // slice): its text would recurse, which the C half's render functions do at run time.
    bool Recursive(TypeExpr *t) {
        set<string> seen;
        function<bool(TypeExpr *, int)> walk = [&](TypeExpr *x, int depth) -> bool {
            if (!x || depth > 32) return true;
            auto sub = [&](TypeExpr *y) { return walk(y, depth + 1); };
            switch (x->kind) {
                case TY_REF: return sub(x->ref->sub);
                case TY_SLICE: return sub(x->sub);
                case TY_ARRAY: return sub(x->arr->sub);
                case TY_STRUCT: case TY_ENUM: case TY_VARIANT: {
                    if (depth > 0 && cg.TEq(x, t)) return true;
                    if (!seen.insert(cg.Mangle(x)).second) return false;
                    auto none = [&](TypeExpr *ft) { return !sub(ft); };
                    if (x->kind == TY_STRUCT) return cg.SI(x) && !StFields(x).All(none);
                    return !AllVariants(x, [&](int, const FieldSet &fs) { return fs.All(none); });
                }
                default: return false;
            }
        };
        return walk(t, 0);
    }

    // The render procedures of types that reach themselves, one per type (CodeGen::RenderFn): named by
    // the type's mangled name, queued as asked for and built once the asking function is done, as only
    // one procedure is built at a time (TinyGenBackend::BuildRenders).
    map<string, string> renderfns;
    vector<pair<string, TypeExpr *>> renderq;
    string RenderFn(TypeExpr *t) {
        auto k = cg.Mangle(t);
        if (auto it = renderfns.find(k); it != renderfns.end()) return it->second;
        auto name = cat("gs_tg_render_", k);
        renderfns[k] = name;
        renderq.emplace_back(name, t);
        return name;
    }
    // The verifier procedure of an image's element type (CodeGen::VerifyFn): int64(payload, n, bitmap).
    string VerifyFn(TypeExpr *elem) {
        auto name = cat("gs_tg_verify_", cg.Mangle(elem));
        if (renderfns.emplace(name, name).second) renderq.emplace_back(name, elem);
        return name;
    }
    // Whether a render procedure can take the type: a fixed struct or ADT whose parts are all held, or
    // an image of one (a variable-size struct, a variable-mode ADT), handed the address of its value.
    bool RenderFnOk(TypeExpr *t) {
        if (t->kind == TY_ENUM && IsVm(t)) return VmOk(t);
        if (t->kind == TY_STRUCT && (VsOk(t) || FoOk(t) || RsOk(t))) return true;
        return (t->kind == TY_STRUCT || t->kind == TY_ENUM) && cg.IsFix(t) && !FoOk(t) && TypeOk(t);
    }

    // ---- Globals --------------------------------------------------------------------------------

    // A global this backend reaches: a fixed value it holds, not a pool's. In a mixed build it stays
    // the C half's and is reached through an exported accessor returning its address in the running
    // thread's instance (GS_GL; Global); a whole program's object defines both
    // (TinyGenBackend::EmitGlobalStorage). A const `let` with a compile-time initializer is static data
    // here too (StaticGlobal, as in CodeGen::EmitGlobalDecls). A resizable or T[] global lives on its
    // own stack (CodeGen::gstks), which a second accessor gives (StackAccessor).
    bool GlobalOk(VarDef *d) {
        if (!d->isglobal || !cg.gnames.count(d)) return false;
        if (cg.PrefVar(d)) return cg.IsFatRef(d->type) && TypeOk(d->type);   // A gs_pref.
        if (d->reusable)
            return IsGrow(d->type) && RzOk(d->type) && cg.gstks.count(d) && cg.gpools.count(d);
        return TypeOk(d->type) || (RzParam(d->type) && cg.gstks.count(d));
    }

    // The accessor name of d in names, numbered in the order of first use.
    string AccessorName(map<const VarDef *, string> &names, VarDef *d, const char *prefix) {
        auto it = names.find(d);
        return it != names.end() ? it->second : (names[d] = cat(prefix, names.size()));
    }
    map<const VarDef *, string> accessors, stkaccessors;
    string Accessor(VarDef *d) {
        auto name = AccessorName(accessors, d, "gs_tg_g");
        NeedShim(name, cat("void *", name, "(void) { return (void *)&(", cg.gnames[d], "); }\n"));
        return name;
    }
    string StackAccessor(VarDef *d) {
        auto name = AccessorName(stkaccessors, d, "gs_tg_gs");
        NeedShim(name, cat("void *", name, "(void) { return (void *)", cg.gstks[d], "; }\n"));
        return name;
    }
    // A global pool's freelist accessors: its header's, its stack's.
    map<const VarDef *, pair<string, string>> flaccessors;
    pair<string, string> PoolAccessors(VarDef *d) {
        auto n = flaccessors.size();
        auto r = flaccessors.count(d) ? flaccessors[d] :
            (flaccessors[d] = { cat("gs_tg_gf", n), cat("gs_tg_gfs", n) });
        NeedShim(r.first, cat("void *", r.first, "(void) { return (void *)&(", cg.gpools[d].first,
                              "); }\n"));
        NeedShim(r.second, cat("void *", r.second, "(void) { return (void *)", cg.gpools[d].second,
                               "; }\n"));
        return r;
    }
    // The `return from` channels' accessors (gs_tg_lret_<tid>_<i>, gs_tg_fdst_, gs_tg_fval_), with
    // sizes.
    map<string, int64_t> channels;

    // The static copy of a global the C makes static data, with its debug entry under its Goose name;
    // TYG_NO_SYM for any other global, or one whose initializer this backend does not lay out.
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

    // n's value of type t as the bytes at `off`: what CodeGen::StaticInitX spells as a C initializer,
    // laid out as the packed C type holds it.
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
                auto v = i ? (uint64_t)i->val : 0;   // Little-endian: the low bytes.
                return i && t->intstorage != IS_VARINT && put(&v, ScalSize(Scal(t)));
            }
            case TY_FLT: {
                auto f = Is<FltLit>(n);
                auto s = f ? (float)f->val : 0.0f;
                auto d = f ? (double)f->val : 0.0;
                return f && (Scal(t) == TYG_F32 ? put(&s, 4) : put(&d, 8));
            }
            case TY_BOOL: {
                auto b = Is<BoolLit>(n);
                uint8_t v = b && b->val ? 1 : 0;
                return b && put(&v, 1);
            }
            case TY_STRUCT: {
                auto sl = Is<StructLit>(n);
                auto si = cg.SI(t);
                if (!sl || sl->sinst != si) return false;
                auto &fs = StFields(t);
                for (size_t fi = 0; fi < fs.f.size(); fi++)
                    if (!fs.f[fi].ispad &&
                        !StaticBytes(sl->InitFor((int)fi), fs.t[fi], out, off + fs.off[fi]))
                        return false;
                return true;
            }
            case TY_ARRAY: {
                if (t->arr->akind != A_FIXED) return false;
                auto k = cg.ArrSize(t->arr);
                auto et = t->arr->sub;
                auto esz = cg.FixedSize(et);
                if (auto str = Is<StrLit>(n))
                    return IsU8(et) && (int64_t)str->val.size() == k && put(str->val.data(), k);
                auto al = Is<ArrayLit>(n);
                if (!al || (!al->fillval && (int64_t)al->elems.size() != k)) return false;
                for (int64_t e = 0; e < k; e++)
                    if (!StaticBytes(al->fillval ? al->fillval : al->elems[(size_t)e], et, out,
                                     off + e * esz))
                        return false;
                return true;
            }
            default: return false;
        }
    }

    // ---- Signatures -----------------------------------------------------------------------------

    // Whether a call to sp, or sp's own definition (`define`), has a C interface this backend can match
    // (CodeGen::SigParams): the declared parameters, free variables (FvOk), then a channel per result:
    // the return value for the first fixed one (cret), `T *gs_r<i>` for other fixed ones, `gs_stack
    // *gs_dst<i>, int64_t *gs_rl<i>` for a resizable one, a destination stack alone for a T[] image;
    // then gs_sp. A `return from` travels in the C half's thread-locals; only a target whose results
    // are all fixed is defined here.
    bool SigOk(FnSpec *sp, bool define, string &why) {
        auto &si = cg.sinfo[sp];
        if (sp->sf->isextern) {
            why = "extern";
            return false;
        }
        for (auto fv : si.freevars)
            if (!FvOk(fv)) {
                why = cat("free variable of type ", fv->type ? NameWhy(fv->type) : "?");
                return false;
            }
        for (auto r : sp->rets)
            if (!RetOk(r)) {
                why = cat(define && cg.fromids.count(sp) ? "return from target with result type " :
                          "result type ",
                          NameWhy(r));
                return false;
            }
        for (size_t i = 0; i < sp->argtypes.size(); i++) {
            auto t = sp->argtypes[i];
            if (cg.IsPoolParam(sp, i) ? !(cg.IsFatRef(t) && TypeOk(t)) : !Held(t)) {
                why = cat("parameter type ", NameWhy(t));
                return false;
            }
        }
        return true;
    }
    // A result this backend holds: a fixed value, or a resizable array or a T[] image of fixed
    // elements, built at the caller's destination stack.
    bool RetOk(TypeExpr *t) { return !IsRel(t) && Held(t); }
    // A free variable of a nested function (§7.5), as SigParams passes it: a fixed variable by its
    // address, a T[] by its image's address, a resizable by its header's address and then its stack, a
    // pool reference by its gs_pref's address.
    bool FvOk(const VarDef *fv) {
        if (!fv->type) return false;
        if (cg.PrefVar(fv)) return cg.IsFatRef(fv->type) && TypeOk(fv->type);
        if (fv->reusable) return IsGrow(fv->type) && RzOk(fv->type);
        return Held(fv->type);
    }

    tyg_sig *Sig(FnSpec *sp) {
        if (auto it = sigs.find(sp); it != sigs.end()) return it->second;
        auto &si = cg.sinfo[sp];
        vector<const tyg_type *> ps;
        auto ptrs = [&](int n) { while (n-- > 0) ps.push_back(tyg_type_ptr(m)); };
        for (size_t i = 0; i < sp->argtypes.size(); i++) {
            auto t = sp->argtypes[i];
            // A pool reference, gs_pref, as its four members (ParamFields).
            if (cg.IsPoolParam(sp, i)) ptrs(4);
            else if (RzParam(t) && !Resz(t)) ptrs(1);   // A by-value T[]: its image's address.
            // A by-value T[>..]: its header, then its stack; a large fixed value: its copy's address.
            else {
                ps.push_back(cg.IsLargeFixed(t) ? tyg_type_ptr(m) : AT(t));
                if (RzParam(t)) ptrs(1);
            }
        }
        // gs_pref; header, stack.
        for (auto fv : si.freevars) ptrs(fv->reusable ? 4 : Resz(fv->type) ? 2 : 1);
        for (size_t i = 0; i < sp->rets.size(); i++) {
            if (Resz(sp->rets[i])) ptrs(2);   // gs_dst<i>, gs_rl<i>
            // gs_dst<i>: the image at its top; gs_r<i>
            else if (IsImg(sp->rets[i]) || (int)i != si.cret) ptrs(1);
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
    // ---- IR shorthands --------------------------------------------------------------------------
    tyg_val I64(int64_t v) { return tyg_iconst(p, TYG_I64, v); }
    tyg_addr AtL(tyg_local l, int64_t off = 0) { return tyg_at_local(l, off); }
    tyg_addr AtV(tyg_val v, int64_t off = 0) { return tyg_at_val(v, off); }
    tyg_val Ld(tyg_addr a) { return tyg_load(p, TYG_I64, a, 0); }
    void St(tyg_addr a, tyg_val v) { tyg_store(p, TYG_I64, a, v, 0); }
    tyg_val LdL(tyg_local l, int64_t off = 0) { return Ld(AtL(l, off)); }
    void StL(tyg_local l, tyg_val v, int64_t off = 0) { St(AtL(l, off), v); }
    tyg_val Bin(tyg_binop op, tyg_val a, tyg_val b) { return tyg_bin(p, op, a, b); }
    tyg_val Add(tyg_val a, tyg_val b) { return Bin(TYG_ADD, a, b); }
    tyg_val Sub(tyg_val a, tyg_val b) { return Bin(TYG_SUB, a, b); }
    tyg_val Mul(tyg_val a, tyg_val b) { return Bin(TYG_MUL, a, b); }
    tyg_val AndK(tyg_val v, int64_t k) { return Bin(TYG_AND, v, I64(k)); }
    tyg_val I8(int v) { return tyg_iconst(p, TYG_I8, v); }
    tyg_val I32(int64_t v) { return tyg_iconst(p, TYG_I32, v); }
    tyg_val AddrOf(tyg_addr a) { return tyg_addr_of(p, a); }
    // The address of the read-only bytes b (a string literal's, or a file name's).
    tyg_val BytesAddr(const string &b) { return AddrOf(tyg_at_sym(M.Bytes(b), 0)); }
    // An uninitialised 8-byte (i64) or 16-byte (header) local.
    tyg_local Slot() { return tyg_local_new(p, 8, 8); }
    tyg_local Slot16() { return tyg_local_new(p, 16, 8); }
    // The size-byte integer at `at`, widened to i64 (signed or not); the low byte of v stored at `at`.
    tyg_val LoadN(tyg_val at, int64_t size, bool sgn) {
        auto x = tyg_load(p, TgModule::ScalOfBytes((int)size), AtV(at), 0);
        return size == 8 ? x : tyg_convert(p, sgn ? TYG_SEXT : TYG_ZEXT, TYG_I64, x);
    }
    void StoreByte(tyg_val at, tyg_val v) {
        tyg_store(p, TYG_I8, AtV(at), tyg_convert(p, TYG_TRUNC, TYG_I8, v), 0);
    }
    tyg_val Cmp(tyg_cond c, tyg_val a, tyg_val b) { return tyg_cmp(p, c, a, b); }
    void Branch(tyg_val c, tyg_block a, tyg_block b) { tyg_branch(p, c, a, b); }
    tyg_block NewBlock() { return tyg_block_new(p); }
    void SetBlock(tyg_block b) { tyg_block_set(p, b); }
    // body() while cond() holds, each in a block of its own; on after.
    template <class C, class F> void WhileDo(C cond, F body) {
        auto head = NewBlock(), b = NewBlock(), done = NewBlock();
        tyg_jump(p, head);
        SetBlock(head);
        Branch(cond(), b, done);
        SetBlock(b);
        body();
        Jump(head);
        SetBlock(done);
    }
    // body(i) for i from `from` up to n (n read once); on after the last.
    template <class F> void Loop(tyg_val from, tyg_val n, F body) {
        auto il = Local(from), nl = Local(n);
        WhileDo([&] { return Cmp(TYG_SLT, LdL(il), LdL(nl)); }, [&] {
            body(LdL(il));
            if (!Ended()) StL(il, Add(LdL(il), I64(1)));
        });
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
        auto l = Slot();
        StL(l, v);
        return l;
    }
    TgModule &M;
    CodeGen &cg;
    FnSpec *sp;
    tyg_module *m;
    tyg_proc *p = nullptr;
    unordered_map<const VarDef *, tyg_local> locals;
    // Where a value is built, at the top of stk: mode 1 a T[] image (BuildInto returns its address), 2
    // a resizable of any kind (header at hdr), 3 a frame object (its struct at hdr), 4 a frame object's
    // bytes (the innermost count stored at hdr). A match, block or loop delivers modes 1 and 2 this
    // way.
    struct P;
    struct InPlace {
        int mode = 0;
        tyg_val stk = 0;
        TypeExpr *t = nullptr;
        tyg_addr hdr {};
        const P *fo = nullptr;   // Mode 3: the place, whose flags the struct's fields inherit.
    };
    struct LoopTargets {
        tyg_block brk, cnt;
        size_t depth;   // The stack scopes open outside the body (sscopes).
        tyg_local res = 0;        // Where a break's value goes, if wanted,
        TypeExpr *rt = nullptr;   // of this type.
        bool block = false;       // A block, which continue passes by.
        // A value built in place (an image or a resizable) rather than stored in res: each break builds
        // it at the same top of ip.stk, which was top0 where the value starts.
        InPlace ip;
        tyg_local top0 = 0;
        bool pref = false;        // Its value is a pool reference (GenPref).
    };
    vector<LoopTargets> loops;
    // The value at src copied to d as d.t is: a frame object, an image (a variant or fixed ADT adapted
    // to a variable-mode one), a resizable ADT, resizable struct or array (its elements).
    void CopyFrom(const P &src, const InPlace &d, Line l) {
        auto t = d.t;
        auto same = [&] { return cg.TEq(src.t, t); };
        if (d.mode == 3) return FoCopy(*d.fo, src, l);
        if (M.IsRb(t)) return RbFrom(d.stk, d.hdr, MemV(src.t, src.a, src.stk), t, l);
        if (d.mode == 1 && M.IsVm(t) && !same()) return (void)VmFrom(d.stk, src.a, src.t, t, l);
        if (M.IsRs(t) || (d.mode == 1 && (M.IsVs(t) || M.IsVm(t) || M.IsVv(t)))) {
            if (!same()) No(l, cat("value of type ", M.NameWhy(src.t), " as ", M.Name(t)));
            if (d.mode == 1) ImgCopy(d.stk, AddrOf(src.a), t);
            else RsCopy(d.stk, d.hdr, src, l);
            return;
        }
        auto v = ViewOf(src, l);
        if (d.mode == 1) {
            GenImg(nullptr, d.stk, t, l, &v);
            return;
        }
        SameElem(v.elem, t->arr->sub, l);
        StartHdr(d.stk, d.hdr);
        CopyView(d.stk, CountAt(d.hdr), t, v);
    }
    // The one entry to every builder: n's value built at d.
    tyg_val BuildInto(Node *n, const InPlace &d, Line l) {
        switch (d.mode) {
            case 1: return GenImg(n, d.stk, d.t, l);
            case 2: GenRz(n, d.stk, d.hdr, d.t, l); break;
            case 3: GenFo(n, *d.fo, l); break;
            default: GenFoBytes(n, d.stk, d.t, d.hdr, l);
        }
        return 0;
    }
    // Each result's channel (CodeGen::SigParams), its parameters kept in locals: an out-pointer in `a`
    // for a fixed result other than the C return value; for a resizable, its destination stack in `a`,
    // the count's out-parameter in `b`.
    struct Chan {
        tyg_local a = 0, b = 0;
        tyg_local top0 = 0;   // A destination's top on entry (DstTop0).
    };
    vector<Chan> chans;
    // Data stacks (§6.2) as the C backend uses them: gs_sp in a local, the thread's stack block
    // (gs_stks) read once per invocation (gblk). The body's stacks are GS(gs_sp + k), k counted up as
    // locals and temporaries take one (AllocStk); a callee is handed gs_sp + the indices in use. Every
    // scope restores the tops of the stacks it took, on every way out, from marks saved as each was
    // taken (CodeGen::CScope).
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
    // The globals of the C half's instance this body names, each with the local holding its address.
    // Their accessors are called once per invocation, in a block the entry block jumps to (gblk),
    // filled once the body is done: a body runs on one thread, whose instance does not change under it.
    vector<pair<string, tyg_local>> gaddrs;   // By accessor.
    tyg_block gblk = 0, gstart = 0;
    // By-value T[] parameters, whose locals hold their images' addresses; by-value T[>..] ones, whose
    // locals hold their headers, with the local holding each one's stack.
    set<const VarDef *> indirect;
    map<const VarDef *, tyg_local> rzstk;
    // A reusable pool's freelist (§5.4): its header { base, len } and the slot holding its stack, by
    // the pool's variable.
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
    // A place: memory holding a value of type t, and for a resizable array the stack its elements grow
    // on where known (else 0).
    struct P {
        tyg_addr a;
        TypeExpr *t;
        tyg_val stk = 0;
        bool enc = false;   // A varint in a bytes value: zigzag ULEB128 (ZigRead).
        // The tail of a resizable struct (FieldPlace): its elements start here, not where the header's
        // base says, and the header is the owner's, whose count it is.
        tyg_val rbase = 0;
        // A frame object that is the tail of a resizable struct (C.2): bytes as the other fields are,
        // its fixed fields packed where `a` is, its elements behind them, the count in the owner's
        // header ohdr.
        bool bfo = false;
        tyg_addr ohdr {};
        // A resizable struct's tail: a header made for it, its stack the struct's.
        bool tmphdr = false;
    };
    tyg_val BaseOf(const P &pl) { return pl.rbase ? pl.rbase : Ld(pl.a); }

    // The elements of an array or slice and their count (ViewOf).
    struct View {
        tyg_val elems, len;
        TypeExpr *elem;
        int64_t k;   // A fixed array's length, else -1.
    };

    // ---- Blocks and positions -------------------------------------------------------------------

    // Unreachable code after a jump or return goes into a block of its own: tinygen wants an open one.
    void Open() { if (tyg_block_terminated(p)) SetBlock(NewBlock()); }
    bool Ended() { return tyg_block_terminated(p) != 0; }
    void Jump(tyg_block b) { if (!Ended()) tyg_jump(p, b); }
    void Loc(Line l) { if (l.line > 0) tyg_loc(p, M.File(l.fileidx), l.line, 0); }

    // A local for a value of type t, aligned to the largest power of two up to 8 its size allows.
    tyg_local Temp(TypeExpr *t) {
        auto sz = M.Size(t);
        int64_t align = 1;
        while (align < 8 && align * 2 <= sz) align *= 2;
        return tyg_local_new(p, sz, align);
    }

    // ---- Building values in place ---------------------------------------------------------------

    // A resizable's header is (base, count); a build dispatches on the node.
    tyg_addr NewHdr() { return AtL(Slot16()); }
    static Node *StripCopy(Node *n) {
        auto c = Is<Call>(n);
        return c && c->builtin == B_COPY && c->ArgNodes().size() == 1 ? c->ArgNodes()[0] : n;
    }
    // If, block, match, early block, loop or function value n built at d, each arm's value by
    // BuildInto. A match, early block or loop only for a destination they deliver to. False for any
    // other n.
    bool BuildCtl(Node *n, const InPlace &d, Line l) {
        auto arm = [&](Node *v) { BuildInto(v, d, l); };
        if (auto ie = Is<IfExpr>(n); ie && ie->elseb) BuildIf(ie, l, arm);
        else if (auto b = Is<Block>(n); b && b->tail) BuildArm(b, l, arm);
        else if (auto c = Is<Call>(n); c && c->fvbody && c->fvbody->tail) BuildFv(c, l, arm);
        else if (d.mode > 2 || M.IsRs(d.t)) return false;
        else if (auto me = Is<MatchExpr>(n)) GenMatch(me, false, &d);
        else if (auto e = Is<EarlyBlock>(n)) GenEarly(e, false, &d);
        else if (auto lp = Is<LoopExpr>(n)) GenLoopX(lp, false, &d);
        else return false;
        return true;
    }
    // A call returning d.t (the same by name, or by TEq if !bymangle) is handed d to build its first
    // result at; false if n is not one.
    bool CallAt(Node *n, const InPlace &d, bool bymangle = true) {
        auto c = Is<Call>(n);
        if (!DestSpec(c)) return false;
        auto rt = DestSpec(c)->rets[0];
        if (bymangle ? cg.Mangle(rt) != cg.Mangle(d.t) : !cg.TEq(rt, d.t)) return false;
        if (d.mode == 1) CallImg(c, d.stk);
        else CallRz(c, d.stk, d.hdr);
        return true;
    }
    void CallRz(Call *c, tyg_val stk, tyg_addr hdr) {
        RzDst d { stk, hdr };
        rzdst = &d;
        GenCall(c);
        rzdst = nullptr;
    }
    void CallImg(Call *c, tyg_val stk) {
        imgdst = stk;
        GenCall(c);
        imgdst = 0;
    }
    // The header of a resizable about to be built at the top of stk: its base, no elements.
    tyg_val StartHdr(tyg_val stk, tyg_addr hdr) {
        auto top = Top(stk);
        St(hdr, top);
        St(CountAt(hdr), I64(0));
        return top;
    }
    // k zero bytes pushed.
    void PushZero(tyg_val stk, int64_t k) {
        if (k > 0) tyg_zero(p, AtV(StackReserve(stk, I64(k))), k);
    }
    void PushVal(tyg_val stk, TypeExpr *ft, const V &v) {
        auto top = Top(stk);
        Store(P { AtV(top), ft }, v);
        SetTop(stk, Add(top, I64(cg.FixedSize(ft))));
    }
    // x built as a value of type ft and pushed; false if the build ended.
    bool BuildField(tyg_val stk, TypeExpr *ft, Node *x) {
        auto v = GenAs(x, ft);
        if (Ended()) return false;
        if (!M.IsScalar(ft) && !v.mem) No(x->line, "aggregate value");
        PushVal(stk, ft, v);
        return true;
    }

    // ---- Data stacks ----------------------------------------------------------------------------

    tyg_val SpVal(Line l) {
        if (!hassp) No(l, "data stack without gs_sp");
        return LdL(spl);
    }
    // GS(gs_sp + k): the gs_stack (one pointer, its top) at that index.
    tyg_val Stk(int k, Line l) {
        auto sp0 = SpVal(l);
        if (!stksl) {
            stksl = Slot();
            M.NeedShim("gs_tg_stks");
        }
        return Add(LdL(stksl), Mul(Add(sp0, I64(k)), I64(8)));
    }
    tyg_val Top(tyg_val stk) { return Ld(AtV(stk)); }
    void SetTop(tyg_val stk, tyg_val top) { St(AtV(stk), top); }

    // The next free index, for a local (innermost non-statement scope) or a temporary (the
    // statement's), its top saved to restore (AllocStk(forlocal) in the C).
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
        for (auto mk = sc.marks.rbegin(); mk != sc.marks.rend(); ++mk)
            SetTop(Stk(mk->k, Line {}), LdL(mk->top));
    }
    // The restores of every scope from `depth` in, innermost first (a break's, continue's, return's).
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

    // A call handed gs_sp + the indices in use: none into the recursive cycle this function is in
    // (§7.8; CodeGen::NoStackAcrossCycleCall), which the C backend's allocation guarantees, this one's
    // may not.
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

    // Moves n bytes from src (maybe on stk itself) to the top of stk, which grows by them; where they
    // land.
    tyg_val PushBytes(tyg_val stk, tyg_val src, tyg_val n) {
        auto top = Top(stk);
        tyg_move_dyn(p, top, src, n);
        SetTop(stk, Add(top, n));
        return top;
    }
    // Moves `bytes` bytes of n elements from elems to the top of stk, counting them at len.
    void CopyBytes(tyg_val stk, tyg_addr len, tyg_val elems, tyg_val bytes, tyg_val n) {
        PushBytes(stk, elems, bytes);
        St(len, Add(Ld(len), n));
    }
    void CopyRun(tyg_val stk, tyg_addr len, TypeExpr *et, tyg_val elems, tyg_val n) {
        if (!cg.IsFix(et)) No(Line {}, "a copy of elements of variable size");
        CopyBytes(stk, len, elems, Mul(n, I64(cg.FixedSize(et))), n);
    }
    void SameElem(TypeExpr *a, TypeExpr *b, Line l) {
        if (cg.Mangle(a) != cg.Mangle(b)) No(l, "array of another element type");
    }
    tyg_val RunBytes(const View &v) {
        return cg.IsFix(v.elem) ? Mul(v.len, I64(cg.FixedSize(v.elem))) : Sub(RunEnd(v), v.elems);
    }
    // CopyRun of a view's elements, which may be images (their bytes to RunEnd).
    void CopyView(tyg_val stk, tyg_addr len, TypeExpr *, const View &v) {
        CopyBytes(stk, len, v.elems, RunBytes(v), v.len);
    }

    // Builds n's value, a resizable array of type t, at the top of stk, its header at hdr (CodeGen::
    // GenConstruct): a literal's elements one by one, a call's at its destination, else a copy of
    // elements.
    void GenRz(Node *n, tyg_val stk, tyg_addr hdr, TypeExpr *t, Line l) {
        if (M.IsRs(t)) return GenRs(n, stk, hdr, t, l);
        if (M.IsRb(t)) return GenRb(n, stk, hdr, t, l);
        auto et = t->arr->sub;
        auto fix = cg.IsFix(et);
        n = StripCopy(n);
        auto len = CountAt(hdr);
        if (BuildCtl(n, InPlace { 2, stk, t, hdr }, l)) return;
        if (auto al = Is<ArrayLit>(n)) {
            if (al->capexpr) No(l, "array literal of this kind");
            if (!al->fillval) {   // Elements (images each built behind the last) counted one by one.
                StartHdr(stk, hdr);
                for (auto e : al->elems) {
                    if (fix ? !BuildField(stk, et, e) : (GenImg(e, stk, et, l), Ended())) return;
                    St(len, Add(Ld(len), I64(1)));
                }
                return;
            }
            // [v; k] (CodeGen::GenArrayLit): v built once, then copied k times (k constant, §4.2).
            auto fc = Is<IntLit>(al->fillcount);
            if (!fc || fc->val < 0) No(l, "fill count");
            // v built once even for none (CodeGen::FreezeFill), then copied behind itself.
            if (!fix) {
                StartHdr(stk, hdr);
                auto saved = Top(stk);
                auto first = GenImg(al->fillval, stk, et, l);
                if (Ended()) return;
                if (fc->val == 0) SetTop(stk, saved);
                else if (fc->val > 1) {
                    auto fl = Local(first);
                    Loop(I64(1), I64(fc->val), [&](tyg_val) { ImgCopy(stk, LdL(fl), et); });
                }
                St(len, I64(fc->val));
                return;
            }
            auto esz = cg.FixedSize(et);
            // A relative element is built in each slot where it lies, its links measured from it.
            auto rel = SelfRelLit(al->fillval) ||
                (TgModule::IsRel(et) && !et->ref->pool && !Is<NullLit>(al->fillval));
            auto leaves = rel ? FillCapture(al) : vector<FillLeaf> {};
            auto one = rel ? tyg_local {} : Temp(et);
            if (!rel) Into(P { AtL(one), et }, al->fillval);
            if (Ended()) return;
            StartHdr(stk, hdr);
            auto top = Top(stk);
            if (rel) {
                tyg_zero(p, AtV(top), std::max<int64_t>(fc->val * esz, 1));
                FillStore(leaves, top, fc->val, al);
            } else {
                Loop(I64(0), I64(fc->val),
                     [&](tyg_val i) { tyg_copy(p, AtV(Add(top, Mul(i, I64(esz)))), AtL(one), esz); });
            }
            SetTop(stk, Add(top, I64(fc->val * esz)));
            St(len, I64(fc->val));
            return;
        }
        auto c = Is<Call>(n);
        auto s = Is<StrLit>(n);
        auto tobytes = c && c->builtin == B_TO_BYTES && c->ArgNodes().size() == 1;
        if (tobytes || IsStrCall(n) || s) {   // Bytes: a payload's, a text's, a string literal's.
            if (!IsU8(et)) No(l, "bytes of an array of this type");
            StartHdr(stk, hdr);
            auto an = c ? c->ArgNodes() : vector<Node *> {};
            if (tobytes) ToBytesInto(stk, len, an[0], l);
            else if (c) TextInto(stk, len, c, an, 0);
            else
                CopyRun(stk, len, et, BytesAddr(s->val), I64((int64_t)s->val.size()));
            return;
        }
        if (DestSpec(c) && IsGrow(DestSpec(c)->rets[0])) {
            SameElem(DestSpec(c)->rets[0]->arr->sub, et, l);
            return CallRz(c, stk, hdr);
        }
        auto pl = ElemsSource(n, l);
        if (!Ended()) CopyFrom(pl, InPlace { 2, stk, t, hdr }, l);
    }

    // ---- Resizable ADTs (TgModule::IsRb) --------------------------------------------------------

    // The bytes of the value of type t at base, its tail cnt elements: tag, fixed fields, tail.
    tyg_val RbBytes(TypeExpr *t, tyg_val base, tyg_val cnt) {
        if (t->kind == TY_STRUCT) {
            auto &fs = M.StFields(t);
            return Add(I64(fs.pre), RbBytes(fs.t[(size_t)fs.var], Add(base, I64(fs.pre)), cnt));
        }
        auto ei = M.EI(t);
        auto one = [&](int vi, bool tag) {
            auto sh = M.RbShapeOf(ei, vi);
            auto ts = tag ? M.TagSize(t) : 0;
            if (sh.img) {   // A variable-size variant: its image's size, walked.
                auto vt = t->kind == TY_VARIANT ? t : cg.VariantType(t, vi);
                return Add(I64(ts), ImgSize(vt, tag ? Add(base, I64(ts)) : base));
            }
            auto n = I64(ts + sh.pre);
            return sh.elem ? Add(n, Mul(cnt, I64(cg.FixedSize(sh.elem)))) : n;
        };
        if (t->kind == TY_VARIANT) return one(M.VariantIdx(t), false);
        return ByTag(t, LoadTag(AtV(base), t), [&](int vi) { return one(vi, true); });
    }

    // Builds n's value, a resizable ADT or variant of type t, at the top of stk, its header at hdr
    // (CodeGen:: GenConstruct, GenStructLit, VariantBehindTag): the tag, the fields in turn, the tail's
    // elements counted into the header. From a literal, a variant constant, a call handed stk, or a
    // fixed variant of the ADT.
    void GenRb(Node *n, tyg_val stk, tyg_addr hdr, TypeExpr *t, Line l) {
        auto len = CountAt(hdr);
        auto adt = t->kind == TY_ENUM;
        auto isst = t->kind == TY_STRUCT;
        auto ei = isst ? nullptr : M.EI(t);
        auto ts = adt ? M.TagSize(t) : 0;
        auto begin = [&](int vi) {   // The header at the value's start, its tag stored; its start.
            auto top = Local(StartHdr(stk, hdr));
            if (!adt) return top;
            StoreTag(AtV(LdL(top)), t, vi);
            SetTop(stk, Add(LdL(top), I64(ts)));
            return top;
        };
        // A literal's fields in turn, then its tail.
        auto fields = [&](StructLit *sl, const TgModule::FieldSet &fs, int tailfi) {
            for (size_t i = 0; i < fs.f.size(); i++)
                if (auto e = sl->InitFor((int)i); e && Is<SelfRef>(e))
                    No(l, "self reference in a resizable ADT");
            if (tailfi < 0) RunLit(sl, stk, fs);
            else LitWithTail(sl, stk, fs, tailfi, len, l);
        };
        if (StripCopy(n) != n) No(l, "copy of a resizable ADT");
        if (BuildCtl(n, InPlace { 2, stk, t, hdr }, l)) return;
        auto sl = Is<StructLit>(n);
        auto c = Is<Call>(n);
        if (CallAt(n, InPlace { 2, stk, t, hdr })) return;
        auto lit = sl && sl->exprtype && cg.TEq(sl->exprtype, t);
        if (isst) {   // Its fixed fields in turn, then its tail, a resizable ADT of its own.
            if (!lit || !sl->sinst || sl->variant) No(l, "resizable struct built from this value");
            begin(0);
            fields(sl, M.StFields(t), M.StFields(t).var);
            return;
        }
        if (lit && (!adt || sl->variant)) {
            auto vi = adt ? ei->en->VariantIndex(sl->variant) : M.VariantIdx(t);
            auto &fs = M.VarFields(ei, vi);
            auto sh = M.RbShapeOf(ei, vi);
            begin(vi);
            if (sh.img) return RunLit(sl, stk, fs);   // As a variable-size variant's.
            fields(sl, fs, sh.tailfi);
            return;
        }
        if (auto d = Is<Dot>(n); d && d->variantconst && adt && d->exprtype && cg.TEq(d->exprtype, t)) {
            begin(ei->en->VariantIndex(d->variantconst));
            return;
        }
        auto rt = DestSpec(c) ? DestSpec(c)->rets[0] : nullptr;
        // A call of one of its variants.
        auto ofadt = adt && rt && rt->kind == TY_VARIANT && M.EI(rt) == ei;
        if (ofadt && M.IsRb(rt)) {
            auto start = begin(M.VariantIdx(rt));   // Built behind the tag where the call puts it.
            CallRz(c, stk, hdr);
            if (!Ended()) St(hdr, LdL(start));
            return;
        }
        if (ofadt && M.IsVv(rt)) {
            begin(M.VariantIdx(rt));   // A variable-size variant, built behind the tag by the call.
            CallImg(c, stk);
            return;
        }
        // A value the ADT's variant is: its type is the variant's, not the ADT's the checker gave the
        // node.
        auto x = Gen1(n);
        if (Ended()) return;
        while (x.t && IsAddrRef(x.t)) x = Load(RefTarget(x), l);
        RbFrom(stk, hdr, x, t, l);
    }
    // A value x, a result held elsewhere (a forwarded call's), as the ADT or variant t at the top of
    // stk, its header at hdr: its bytes copied, an ADT's tag put in front of a variant's payload.
    void RbFrom(tyg_val stk, tyg_addr hdr, const V &x, TypeExpr *t, Line l) {
        auto len = CountAt(hdr);
        if (!x.mem || !x.t) No(l, cat("value of type ", M.NameWhy(t)));
        auto ts = t->kind == TY_ENUM ? M.TagSize(t) : 0;
        auto top = Top(stk);
        St(hdr, top);
        auto same = cg.Mangle(x.t) == cg.Mangle(t) && M.IsRb(x.t);
        if (!same) {
            if (t->kind != TY_ENUM || x.t->kind != TY_VARIANT || M.EI(x.t) != M.EI(t))
                No(l, cat("value of type ", M.NameWhy(x.t), " as ", M.Name(t)));
            StoreTag(AtV(top), t, M.VariantIdx(x.t));
        }
        // An ADT's bytes hold their tag; a variant's lie behind it.
        auto at = same ? top : Add(top, I64(ts));
        if (M.IsRb(x.t)) {
            auto xb = Ld(x.a);
            auto cnt = Ld(CountAt(x.a));
            SetTop(stk, at);
            PushBytes(stk, xb, RbBytes(same ? t : x.t, xb, cnt));
            St(len, cnt);
            return;
        }
        St(len, I64(0));
        if (!cg.IsFix(x.t)) {   // A variable-size variant's image, behind the tag.
            SetTop(stk, at);
            ImgCopy(stk, AddrOf(x.a), x.t);
            return;
        }
        auto sz = cg.FixedSize(x.t);
        if (sz) tyg_copy(p, AtV(at), x.a, sz);
        SetTop(stk, Add(at, I64(sz)));
    }
    // The place whose elements an array is built from. The checker types a popped or default fixed
    // array, to_bytes, from_bytes as the T[] they adapt to, but their own array lies there
    // (CodeGen::EmitBuiltin, B_POP).
    static bool IsStrCall(Node *n) {
        auto c = Is<Call>(n);
        return c && c->builtin == B_STR;
    }
    static bool OwnTypedCall(Node *n) {
        auto c = Is<Call>(n);
        return c && (c->builtin == B_POP || c->builtin == B_DEFAULT || c->builtin == B_FROM_BYTES ||
                     (c->builtin == B_TO_BYTES && c->ArgNodes().size() == 1));
    }
    P ElemsSource(Node *n, Line l) {
        if (OwnTypedCall(n)) {
            auto x = Gen1(n);
            if (Ended()) return P {};
            if (!x.mem) No(l, "popped value in no memory");
            return P { x.a, x.t, x.stk };
        }
        if (!LimCallResult(n)) return Place(n);
        auto x = Gen1(n);
        if (!Ended() && (!x.mem || !x.t || (x.t->kind != TY_ARRAY && x.t->kind != TY_SLICE)))
            No(n->line, "array result of this kind");
        return P { x.a, x.t };
    }
    // The function whose first result a call builds at a destination (CodeGen's in-place construction):
    // its own, or a dispatch's first case, whose results all cases share.
    FnSpec *DestSpec(Call *c) {
        if (!c || c->builtin >= 0) return nullptr;
        auto sp = c->spec ? c->spec : !c->dispatch.empty() ? c->dispatch[0] : nullptr;
        return !sp || sp->sf->isextern || sp->rets.empty() ? nullptr : sp;
    }
    // Where GenCall builds a call's first result instead of in a temporary: a resizable's stack and
    // header (rzdst), a T[] image's stack (imgdst).
    struct RzDst { tyg_val stk; tyg_addr hdr; };
    const RzDst *rzdst = nullptr;
    tyg_val imgdst = 0;
    static bool IsVarArr(TypeExpr *t) { return TgModule::IsVarArr(t); }
    bool IsImg(TypeExpr *t) { return M.IsImg(t); }

    // A T[] length prefix (EmitPrefixPatch): n, range-checked as the C does, stored at pre.
    void PatchPrefix(tyg_val pre, TypeExpr *t, tyg_val n, tyg_val stk) {
        auto w = TgModule::LenWidth(t);
        // A varint: one byte reserved, the elements moved up behind a count that needs more.
        if (w == 0) {
            auto nl = Local(n);
            auto cnt = [&] { return LdL(nl); };
            IfElse(Cmp(TYG_ULT, cnt(), I64(128)),
                   [&] { StoreByte(pre, cnt()); },
                   [&] {
                       auto tmp = Slot16();
                       auto msl = Local(UlebWrite(AddrOf(AtL(tmp)), cnt()));
                       auto msv = [&] { return LdL(msl); };
                       auto elems = Add(pre, I64(1));
                       auto top = Top(stk);
                       tyg_move_dyn(p, Add(elems, Sub(msv(), I64(1))), elems, Sub(top, elems));
                       tyg_copy_dyn(p, pre, AddrOf(AtL(tmp)), msv());
                       SetTop(stk, Add(top, Sub(msv(), I64(1))));
                   });
            return;
        }
        auto s = w == 1 ? TYG_I8 : w == 2 ? TYG_I16 : w == 4 ? TYG_I32 : TYG_I64;
        if (w < 8) {
            If(Cmp(TYG_UGT, n, I64((int64_t)((1ull << (8 * w)) - 1))), [&] {
                auto msg = string("array length exceeds storage range") + '\0';
                CallC("gs_panic", { tyg_type_ptr(m) }, { BytesAddr(msg) }, nullptr);
                tyg_unreachable(p);
            });
            n = tyg_convert(p, TYG_TRUNC, s, n);
        }
        tyg_store(p, s, AtV(pre), n, 0);
    }
    // Builds a T[] image of type t at the top of stk (CodeGen's variable array construction): n's value
    // (a call returning one builds at stk; else its elements as a resizable's, GenRz, behind a prefix
    // patched once counted), or the elements of `from`, or none. Its address.
    tyg_val GenImg(Node *n, tyg_val stk, TypeExpr *t, Line l, const View *from = nullptr) {
        if (TgModule::IsVarint(t)) {   // A varint field: its value zigzag ULEB128 at the top.
            if (!n || from) No(l, "varint without a value");
            auto top = Top(stk);
            auto x = GenAs(n, M.ast.inttypes[IS_I64]);
            if (Ended()) return top;
            SetTop(stk, Add(top, ZigWrite(top, x.v)));
            return top;
        }
        auto start = Local(Top(stk));
        auto at = [&] { return LdL(start); };
        if (n && !from && BuildCtl(n, InPlace { 1, stk, t, {} }, l)) return at();
        if (TgModule::IsVr(t)) {
            // A varint-width relative reference: the offset of the plain reference n is.
            if (from) No(l, "relative reference without a value");
            if (!n) {   // Declared without one: null, the zero byte.
                auto top = Top(stk);
                tyg_store(p, TYG_I8, AtV(top), tyg_iconst(p, TYG_I8, 0), 0);
                SetTop(stk, Add(top, I64(1)));
                return top;
            }
            auto x = GenAs(n, t);
            auto top = Top(stk);
            if (!Ended()) SetTop(stk, Add(top, RelOffsetInto(P { AtV(top), t }, x.v, l)));
            return top;
        }
        if (M.IsVm(t) || M.IsVv(t) || M.IsVs(t)) {
            if (!n || from) No(l, cat("value of type ", M.NameWhy(t)));
            return M.IsVm(t) ? GenVm(n, stk, t, l) : GenVs(n, stk, t, l);
        }
        if (CallAt(n, InPlace { 1, stk, t })) return at();
        if (!n && !from && M.VarImgOk(t)) No(l, "array of images without a value");
        auto rc = M.IsRc(t);
        if (rc && n && !from && RcPrelude(n, stk, t, l, at())) return at();
        SetTop(stk, Add(at(), I64(rc ? 8 : TgModule::PrefixBytes(t))));
        auto hdr = NewHdr();
        auto len = CountAt(hdr);
        if (n) {
            GenRz(n, stk, hdr, t, l);
        } else {
            StartHdr(stk, hdr);
            if (from) {
                SameElem(from->elem, t->arr->sub, l);
                CopyView(stk, len, t->arr->sub, *from);
            }
        }
        if (Ended()) return at();
        if (rc) RcSet(at(), t, Ld(len), Ld(len), l);
        else PatchPrefix(at(), t, Ld(len), stk);
        return at();
    }

    // A T[..] at `at`: its capacity and length stored.
    void RcSet(tyg_val at, TypeExpr *t, tyg_val cap, tyg_val cnt, Line l) {
        AbortUnless(Cmp(TYG_ULE, cap, I64(0xffffffffll)), "CAPRANGE", l);
        tyg_store(p, TYG_I32, AtV(at), tyg_convert(p, TYG_TRUNC, TYG_I32, cap), 0);
        LimSetLen(P { AtV(at), t }, cnt);
    }
    // A T[..] built at `at` at the top of stk (CodeGen::GenArrayLit): [..cap] its capacity, no
    // elements; another T[..] copied whole. True if n was one of them; any other is built as a
    // resizable's elements.
    bool RcPrelude(Node *n, tyg_val stk, TypeExpr *t, Line l, tyg_val at) {
        if (auto al = Is<ArrayLit>(n); al && al->capexpr) {
            auto cap = Index64(al->capexpr);
            if (!Ended()) {
                RcSet(at, t, cap, I64(0), l);
                SetTop(stk, Add(at, Add(I64(8), Mul(cap, I64(cg.FixedSize(t->arr->sub))))));
            }
            return true;
        }
        if (Is<ArrayLit>(n) || Is<StrLit>(n) || LimCallResult(n) || OwnTypedCall(n) || IsStrCall(n) ||
            !n->exprtype || !cg.TEq(n->exprtype, t))
            return false;
        auto src = Deref(Place(n));
        if (!Ended()) ImgCopy(stk, AddrOf(src.a), t);
        return true;
    }

    // to_bytes (CodeGen::EmitToBytes, AppendBytes): the payload's size ULEB128, then its bytes, each
    // handed to append.
    template <class F> void ToBytesVia(Node *a, Line l, F append) {
        auto v = ViewOf(Place(a), l);
        if (Ended()) return;
        auto sl = Slot16();
        StL(sl, v.elems);
        StL(sl, RunBytes(v), 8);
        auto pfx = Slot16();
        auto pn = Local(UlebWrite(AddrOf(AtL(pfx)), LdL(sl, 8)));
        append(AddrOf(AtL(pfx)), LdL(pn));
        append(LdL(sl), LdL(sl, 8));
    }
    // Appended at the top of stk to the u8 count at len.
    void ToBytesInto(tyg_val stk, tyg_addr len, Node *a, Line l) {
        ToBytesVia(a, l, [&](tyg_val from, tyg_val n) { CopyBytes(stk, len, from, n, n); });
    }
    // Appended to a limited u8 array, each part copied under the capacity check.
    void ToBytesIntoLim(const P &out, Node *a, Line l) {
        ToBytesVia(a, l, [&](tyg_val from, tyg_val n) {
            auto ol = Local(LimLen(out));
            AbortUnless(Cmp(TYG_SLE, Add(LdL(ol), n), LimCapOf(out)), "CAPACITY", l);
            tyg_move_dyn(p, ElemAddr(LimView(out), LdL(ol)), from, n);
            LimSetLen(out, Add(LdL(ol), n));
        });
    }

    V RzTemp(TypeExpr *t, Line l) { return MemV(t, NewHdr(), Stk(AllocStk(false, l), l)); }
    // A frame object's struct in a temporary, its tail on a stack of the statement's.
    V FoTemp(TypeExpr *t, Line l) {
        return MemV(t, AtL(tyg_local_new(p, M.Size(t), 8)), Stk(AllocStk(false, l), l));
    }
    // A type BuildTemp builds: an image, a resizable array or a frame object.
    bool TempOk(TypeExpr *t) {
        return M.FoOk(t) || (IsImg(t) && M.ImgOk(t)) || (IsGrow(t) && (M.RzOk(t) || M.VeOk(t)));
    }
    // n's value of type t built on a temporary stack of the statement's (an image at its top, the
    // others behind a header beside it); the builder is chosen by t.
    V BuildTemp(Node *n, TypeExpr *t, Line l) {
        if (M.FoOk(t)) {
            auto x = FoTemp(t, l);
            GenFo(n, P { x.a, t, x.stk }, l);
            return x;
        }
        if (!IsGrow(t) && !M.IsRs(t) && !M.IsRb(t)) {
            auto stk = Stk(AllocStk(false, l), l);
            return MemV(t, AtV(GenImg(n, stk, t, l)));
        }
        auto x = RzTemp(t, l);
        GenRz(n, x.stk, x.a, t, l);
        return x;
    }

    // ---- Conversions ----------------------------------------------------------------------------

    // A two-word header (a, then b) on a temporary.
    V PairV(TypeExpr *t, tyg_val a, tyg_val b) { return MemV(t, AtL(PairLocal(a, b))); }
    tyg_val BytesEq(tyg_val a, tyg_val b, tyg_val n) {
        auto d = CallC("memcmp", { tyg_type_ptr(m), tyg_type_ptr(m), IT(TYG_I64, false) }, { a, b, n },
                       IT(TYG_I32));
        return Cmp(TYG_EQ, d, I32(0));
    }
    // arm(vi), in a block entered when the tag in `tag` is variant vi, for each of n variants.
    template <class F> void TagSwitch(tyg_local tag, size_t n, F arm) {
        for (size_t vi = 0; vi < n; vi++) If(Cmp(TYG_EQ, LdL(tag), I64((int64_t)vi)), [&] { arm(vi); });
    }
    // The i64 that arm(vi) gives for the variant of the ADT that tag says (init for a corrupt tag).
    template <class F> tyg_val ByTag(TypeExpr *adt, tyg_val tag, F arm, int64_t init = 0) {
        auto res = Local(I64(init));
        auto n = M.EI(adt)->en->variants.size();
        TagSwitch(Local(tag), n, [&](size_t vi) { StL(res, arm((int)vi)); });
        return LdL(res);
    }

    // C's conversion of a scalar between the scalar types: what an assignment of one to the other does.
    tyg_val Conv(tyg_val v, TypeExpr *from, TypeExpr *to, Line l) {
        auto fs = M.Scal(from), ts = M.Scal(to);
        auto fflt = from->kind == TY_FLT, tflt = to->kind == TY_FLT;
        if (fflt && tflt)
            return fs == ts ? v : tyg_convert(p, fs == TYG_F32 ? TYG_FEXT : TYG_FTRUNC, ts, v);
        if (!fflt && tflt) {
            auto uns = from->kind == TY_BOOL || (from->kind == TY_INT && IsUnsigned(from->intstorage));
            if (fs != TYG_I64 && fs != TYG_I32)
                v = tyg_convert(p, uns ? TYG_ZEXT : TYG_SEXT, TYG_I32, v);
            return tyg_convert(p, uns ? TYG_UITOF : TYG_SITOF, ts, v);
        }
        if (fflt) No(l, "implicit float to integer conversion");
        if (fs == ts) return v;
        if (TgModule::ScalSize(ts) < TgModule::ScalSize(fs)) return tyg_convert(p, TYG_TRUNC, ts, v);
        return tyg_convert(p, TgModule::IsSigned(from) ? TYG_SEXT : TYG_ZEXT, ts, v);
    }

    // A value as type `want`: a reference read through where a value is wanted (reference transparency,
    // §3.8), a scalar converted.
    V Fit(V x, TypeExpr *want, Line l) {
        if (!want || want->kind == TY_VOID || !x.t) return x;
        // A relative reference too, as its slot's decoded address (RelLoad).
        while ((IsAddrRef(x.t) || (TgModule::IsRel(x.t) && M.RelOk(x.t, 1) && !x.mem)) &&
               want->kind != TY_REF)
            x = Load(RefTarget(x), l);
        if (M.IsScalar(x.t) && M.IsScalar(want)) {
            if ((x.t->kind == TY_REF || want->kind == TY_REF) && x.t->kind != want->kind)
                No(l, cat("reference conversion from ", TgModule::Name(x.t), " to ",
                          TgModule::Name(want)));
            if (x.t->kind != TY_REF) x.v = Conv(x.v, x.t, want, l);
            x.t = want;
            return x;
        }
        // An array or slice where a limited array is wanted: copied into one.
        if (TgModule::IsLim(want) && M.TypeOk(want) && x.mem &&
            (x.t->kind == TY_ARRAY || x.t->kind == TY_SLICE) && !cg.TEq(x.t, want))
            return LimFrom(x, want, l);
        if (want->kind == TY_ENUM && M.IsVm(want) && x.mem && M.VmOk(want) && x.t->kind != TY_REF &&
            (x.t->kind == TY_VARIANT || (x.t->kind == TY_ENUM && !M.IsVm(x.t))) &&
                M.EI(x.t) == M.EI(want)) {
            // A variant or fixed-mode ADT as a variable-mode one: its image on a temporary's stack.
            auto stk = Stk(AllocStk(false, l), l);
            return MemV(want, AtV(VmFrom(stk, x.a, x.t, want, l)));
        }
        if (x.t->kind == TY_VARIANT && want->kind == TY_ENUM && x.mem) {   // As its fixed-mode ADT.
            if (want->enu->varmode || !M.TypeOk(want) || M.EI(x.t) != M.EI(want))
                No(l, "ADT adaptation");
            auto r = MemTemp(want, true);
            StoreTag(r.a, want, M.VariantIdx(x.t));
            if (cg.FixedSize(x.t)) tyg_copy(p, AddrPlus(r.a, M.TagSize(want)), x.a, cg.FixedSize(x.t));
            return r;
        }
        if (x.t->kind == TY_ENUM && want->kind == TY_ENUM && x.mem && M.IsVm(x.t) && !M.IsVm(want) &&
            M.EI(x.t) == M.EI(want) && M.TypeOk(want)) {
            // A variable-mode value as fixed-mode (CodeGen::AdaptToFixed): its tag, then the payload.
            auto r = MemTemp(want, true);
            auto ei = cg.EIOf(want);
            auto tag = Local(LoadTag(x.a, x.t));
            tyg_store(p, M.TagScal(want), r.a, tyg_convert(p, TYG_TRUNC, M.TagScal(want), LdL(tag)), 0);
            TagSwitch(tag, ei->en->variants.size(), [&](size_t vi) {
                auto psz = cg.VariantLayout(ei, (int)vi).size;
                if (psz)
                    tyg_copy(p, AddrPlus(r.a, M.TagSize(want)), AddrPlus(x.a, M.TagSize(x.t)), psz);
            });
            return r;
        }
        // A fixed array where a slice is wanted: a view of where it lies; an array temporary (a call's
        // result) too: a view of it (§6.4).
        if (x.mem && x.t->kind == TY_ARRAY && want->kind == TY_SLICE &&
            cg.Mangle(x.t->arr->sub) == cg.Mangle(want->sub) &&
            (x.t->arr->akind == A_FIXED || (M.TypeOk(want) && (IsGrow(x.t) || IsVarArr(x.t))))) {
            auto v = ViewOf(P { x.a, x.t, x.stk }, l);
            return PairV(want, v.elems, v.len);
        }
        if (OptFit(x, want)) return x;
        if (M.IsScalar(x.t) != M.IsScalar(want) || !cg.TEq(x.t, want))
            No(l, cat("value conversion from ", TgModule::Name(x.t), " to ", TgModule::Name(want)));
        return x;
    }

    V Load(const P &pl, Line l) {
        V x;
        x.t = pl.t;
        if (pl.enc) x.v = ZigRead(AddrOf(pl.a));
        else if (TgModule::IsRel(pl.t)) x.v = RelLoad(pl, l);
        else if (M.IsScalar(pl.t)) x.v = tyg_load(p, M.Scal(pl.t), pl.a, 0);
        else if (!M.TypeOk(pl.t) && (M.ImgOk(pl.t) || M.RsOk(pl.t) || M.FoOk(pl.t))) {
            // A T[] or a resizable is read where it lies (its uses copy it).
            x.mem = true;
            x.a = pl.a;
            x.stk = pl.stk;
        } else {
            if (!M.TypeOk(pl.t)) No(l, "type");
            // An aggregate read is a copy: what is read must not change under a later operand.
            auto t = Temp(pl.t);
            tyg_copy(p, AtL(t), pl.a, M.Size(pl.t));
            x.mem = true;
            x.a = AtL(t);
        }
        return x;
    }

    void Store(const P &pl, const V &x) {
        if (Ended()) return;
        if (pl.enc) No(Line {}, "a store into a varint in a bytes value");
        // A Vr only into a temporary carrying a value across blocks (Size): the language never assigns
        // one.
        if (TgModule::IsVr(pl.t)) RelOffsetInto(pl, x.v, Line {});
        else if (TgModule::IsRel(pl.t)) RelStore(pl, x.v, Line {});
        // A scalar without a value comes from code after a return or break (a block never placed).
        else if (M.IsScalar(pl.t) && !x.v) return;
        else if (M.FoOk(pl.t) && x.mem && pl.stk && x.stk) FoCopy(pl, P { x.a, x.t, x.stk }, Line {});
        else if (M.IsScalar(pl.t)) tyg_store(p, M.Scal(pl.t), pl.a, x.v, 0);
        else if (!M.TypeOk(pl.t)) No(Line {}, cat("store of a ", M.Name(pl.t)));
        else tyg_move(p, pl.a, x.a, M.Size(pl.t));
    }

    // ---- Places ---------------------------------------------------------------------------------

    // What the reference at pl refers to: a fat reference's header, with its stack.
    P Step(const P &pl) {
        if (TgModule::IsRel(pl.t)) return P { AtV(RelLoad(pl, Line {})), pl.t->ref->sub };
        P r { AtV(Ld(pl.a)), pl.t->ref->sub };
        r.enc = TgModule::IsVarint(r.t);   // A varint in a bytes value, read-only.
        if (cg.IsFatRef(pl.t)) r.stk = Ld(AddrPlus(pl.a, 8));
        return r;
    }
    // Through references to what they refer to (§3.8; an optional one only where the checker narrowed
    // it).
    P Deref(P pl) {
        while (IsAddrRef(pl.t) || TgModule::IsRel(pl.t)) pl = Step(pl);
        return pl;
    }

    // ---- Relative references (§3.9) -------------------------------------------------------------

    // What an offset measures from (CodeGen::RelOrigin): the slot itself, or an `in pool` reference's
    // pool base less one, so that 0 stays null.
    tyg_val RelOrigin(const P &pl, Line l) {
        auto pool = pl.t->ref->pool;
        if (!pool) return AddrOf(pl.a);
        return Sub(Ld(VarPlace(const_cast<VarDef *>(pool), l).a), I64(1));
    }
    // The address the slot at pl decodes to, null for a 0 offset.
    tyg_val RelLoad(const P &pl, Line l) {
        auto s = M.RelScal(pl.t);
        // Signed from the slot, unsigned from a pool (CodeGen::RelParts).
        tyg_val off;
        if (TgModule::IsVr(pl.t)) {
            auto at = AddrOf(pl.a);
            off = pl.t->ref->pool ? UlebRead(at) : ZigRead(at);
        } else {
            off = tyg_load(p, s, pl.a, 0);
            if (s != TYG_I64) off = tyg_convert(p, pl.t->ref->pool ? TYG_ZEXT : TYG_SEXT, TYG_I64, off);
        }
        auto at = Add(RelOrigin(pl, l), off);
        if (!pl.t->ref->optional) return at;
        return tyg_select(p, Cmp(TYG_EQ, off, I64(0)), I64(0), at);
    }
    // The offset the slot at pl holds for the address v, zero for a null (CodeGen::RelOffset).
    tyg_val RelOffset(const P &pl, tyg_val v, Line l) {
        auto off = Sub(v, RelOrigin(pl, l));
        if (pl.t->ref->optional) {
            auto isnull = Cmp(TYG_EQ, v, I64(0));
            off = tyg_select(p, isnull, I64(0), off);
            // A link to its own slot would read back as null (RelOffset).
            if (!pl.t->ref->pool)
                AbortUnless(Bin(TYG_OR, isnull, Cmp(TYG_NE, off, I64(0))), "RELNULL", l);
        }
        return off;
    }
    // The address v as a varint-width offset (CodeGen::EmitRelStore): ULEB128 for an `in pool` one,
    // zigzag for a self-relative one; no range to check.
    tyg_val RelOffsetInto(const P &pl, tyg_val v, Line l) {
        auto off = RelOffset(pl, v, l);
        auto at = AddrOf(pl.a);
        return pl.t->ref->pool ? UlebWrite(at, off) : ZigWrite(at, off);
    }
    // The address v as the slot's offset, range-checked (EmitRelStoreAt).
    void RelStore(const P &pl, tyg_val v, Line l) {
        auto s = M.RelScal(pl.t);
        auto off = RelOffset(pl, v, l);
        auto bits = TgModule::ScalSize(s) * 8;
        if (bits < 64 && pl.t->ref->pool) {
            AbortUnless(Cmp(TYG_ULE, off, I64((int64_t)((1ull << bits) - 1))), "RELOFF", l);
        } else if (bits < 64) {
            auto lo = Cmp(TYG_SGE, off, I64(-(int64_t)(1ull << (bits - 1))));
            AbortUnless(Bin(TYG_AND, lo, Cmp(TYG_SLT, off, I64((int64_t)(1ull << (bits - 1))))),
                        "RELOFF", l);
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
        if (auto d = Is<Dot>(n); d && d->IsField())
            return FieldPlace(Deref(Place(d->obj)), d->fieldidx, n->line);
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
        return Spill(x);
    }

    // A variable: a local or parameter of this frame, one it was handed (a free variable, read through
    // its address), or a global.
    P VarPlace(VarDef *vd, Line l) {
        auto it = locals.find(vd);
        if (it == locals.end() && vd->isglobal) return GlobalPlace(vd, l);
        if (it == locals.end()) No(l, "variable from outside");
        auto ind = indirect.count(vd) != 0;
        P pl { ind ? AtV(LdL(it->second)) : AtL(it->second), vd->type };
        // A captured resizable: its header's address, its stack.
        if (auto s = rzstk.find(vd); s != rzstk.end() && (!ind || M.Resz(vd->type)))
            pl.stk = LdL(s->second);
        return pl;
    }

    P GlobalPlace(VarDef *d, Line l) {
        if (!M.GlobalOk(d)) No(l, cat("global of type ", M.NameWhy(d->type)));
        auto s = M.TypeOk(d->type) ? M.StaticGlobal(d) : TYG_NO_SYM;
        if (s != TYG_NO_SYM) return P { tyg_at_sym(s, 0), d->type };
        // The shims now, so that an abandoned body takes them back.
        auto at = LdL(AccessorLocal(M.Accessor(d)));
        // A T[], any image: the instance holds its address.
        if (IsImg(d->type)) return P { AtV(Ld(AtV(at))), d->type };
        P pl { AtV(at), d->type };
        if (IsGrow(d->type) || M.FoOk(d->type) || M.RsOk(d->type) || M.IsRb(d->type))
            pl.stk = LdL(AccessorLocal(M.StackAccessor(d)));
        return pl;
    }
    // The local an accessor's result is kept in, read once per invocation.
    tyg_local AccessorLocal(const string &name) {
        for (auto &[g, at] : gaddrs)
            if (g == name) return at;
        auto loc = Slot();
        gaddrs.emplace_back(name, loc);
        return loc;
    }

    // A fixed struct or array literal storing a non-null self-relative link at any depth: built where
    // it lies, as the C does (CodeGen::FixedLitAtLv), for the offsets to measure from.
    bool SelfRelLit(Node *n) {
        if (!SelfRelLitOk(n)) return false;
        auto stores = [&](TypeExpr *ft, Node *init) {
            if (!init) return false;
            // An `in pool` link is the pool's offset, but its `self` is the slot's.
            if (TgModule::IsRel(ft))
                return ft->ref->pool ? Is<SelfRef>(init) != nullptr : !Is<NullLit>(init);
            return SelfRelLit(init) && cg.TEq(init->exprtype, ft);
        };
        if (auto sl = Is<StructLit>(n)) {
            auto any = false;
            EachLitInit(sl, [&](Node *init, TypeExpr *ft, int64_t) { any = any || stores(ft, init); });
            return any;
        }
        auto al = Is<ArrayLit>(n);
        auto et = al->exprtype->arr->sub;
        if (al->fillval) return stores(et, al->fillval);
        for (auto e : al->elems)
            if (stores(et, e)) return true;
        return false;
    }
    // fn(init, type, offset) for the initialised fields of a struct, variant or fixed-mode ADT literal
    // (the offset past the tag, for the ADT), while the code goes on.
    template <class F> void EachLitInit(StructLit *sl, F fn) {
        auto t = sl->exprtype;
        auto ei = t->kind == TY_STRUCT ? nullptr : M.EI(t);
        auto &fs = ei ? M.VarFields(ei, t->kind == TY_VARIANT ? M.VariantIdx(t) :
                                    ei->en->VariantIndex(sl->variant))
                      : M.StFields(t);
        auto base = t->kind == TY_ENUM ? M.TagSize(t) : 0;
        for (size_t i = 0; i < fs.f.size() && !Ended(); i++)
            if (auto init = fs.f[i].ispad ? nullptr : sl->InitFor((int)i))
                fn(init, fs.t[i], base + fs.off[i]);
    }
    // A struct or array literal SelfRelInto can build.
    bool SelfRelLitOk(Node *n) {
        if (auto sl = Is<StructLit>(n)) {
            auto t = sl->exprtype;
            if (!t || !M.TypeOk(t)) return false;
            if (t->kind == TY_STRUCT) return sl->sinst && !sl->variant;
            return t->kind == TY_VARIANT || (t->kind == TY_ENUM && !t->enu->varmode && sl->variant);
        }
        auto al = Is<ArrayLit>(n);
        return al && !al->capexpr && al->exprtype && al->exprtype->kind == TY_ARRAY &&
               al->exprtype->arr->akind == A_FIXED && M.TypeOk(al->exprtype);
    }
    // The start of the literal being built in place, which `self` in its fields is (§3.9), or 0.
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
                if (!Ended()) FillStore(leaves, AddrOf(pl.a), cg.ArrSize(t->arr), al);
                return;
            }
            for (size_t i = 0; i < al->elems.size() && !Ended(); i++)
                Into(P { AddrPlus(pl.a, (int64_t)i * esz), et }, al->elems[i]);
            return;
        }
        auto sl = Is<StructLit>(lit);
        auto outer = selfbase;
        selfbase = AddrOf(pl.a);
        struct Back {
            tyg_val &at, was;
            ~Back() { at = was; }
        } back { selfbase, outer };
        if (t->kind == TY_ENUM) StoreTag(pl.a, t, M.EI(t)->en->VariantIndex(sl->variant));
        EachLitInit(sl, [&](Node *init, TypeExpr *ft, int64_t off) {
            Into(P { AddrPlus(pl.a, off), ft }, init);
        });
    }
    // A fill [v; k] of a value linking to itself (CodeGen::FreezeFill): v's leaves evaluated once, then
    // each slot built in place (FillStore).
    struct FillLeaf {
        TypeExpr *ft;
        int64_t off;
        bool self;
        tyg_local tmp;
        vector<FillLeaf> sub;   // An inner fill's leaves, repeated for the inner literal.
        ArrayLit *inner = nullptr;
    };
    vector<FillLeaf> FillCapture(ArrayLit *al) {
        vector<FillLeaf> leaves;
        if (!al->fillval) return leaves;
        auto capture = [&](Node *init, TypeExpr *ft, int64_t off) {
            if (Is<SelfRef>(init)) {
                leaves.push_back({ ft, off, true, 0 });
                return;
            }
            if (auto il = Is<ArrayLit>(init); il && il->fillval && SelfRelLit(init) &&
                Is<IntLit>(il->fillcount)) {
                FillLeaf lf { ft, off, false, 0 };
                lf.sub = FillCapture(il);
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
        if (auto sl = Is<StructLit>(al->fillval); sl && SelfRelLitOk(sl) &&
            sl->exprtype->kind == TY_STRUCT) {
            EachLitInit(sl, capture);
        } else {
            capture(al->fillval, al->exprtype->arr->sub, 0);
        }
        return leaves;
    }
    // The elements a literal has: its elements', or its fill's count.
    int64_t LitCount(ArrayLit *al) {
        return al->fillval ? Is<IntLit>(al->fillcount)->val : (int64_t)al->elems.size();
    }
    // k elements of a literal built where they stay, at the address in base: zeroed, then in place (a
    // fill's leaves captured already).
    void LitRunAt(ArrayLit *al, TypeExpr *et, tyg_local base, int64_t k,
                  const vector<FillLeaf> &leaves) {
        auto esz = cg.FixedSize(et);
        auto at = [&](int64_t off) { return AtV(LdL(base), off); };
        tyg_zero(p, at(0), std::max<int64_t>(k * esz, 1));
        if (al->fillval) FillStore(leaves, LdL(base), k, al);
        else
            for (int64_t i = 0; i < k && !Ended(); i++)
                Into(P { at(i * esz), et }, al->elems[(size_t)i]);
    }
    // k slots at base (zeroed), each built from the captured leaves.
    void FillStore(const vector<FillLeaf> &leaves, tyg_val base, int64_t k, ArrayLit *al) {
        auto esz = cg.FixedSize(al->exprtype->arr->sub);
        Loop(I64(0), I64(k), [&](tyg_val i) {
            auto slot = Add(base, Mul(i, I64(esz)));
            for (auto &lf : leaves) {
                if (lf.inner) {
                    FillStore(lf.sub, Add(slot, I64(lf.off)), cg.ArrSize(lf.inner->exprtype->arr),
                              lf.inner);
                    continue;
                }
                auto ld = [&] {
                    return tyg_load(p, TgModule::IsRel(lf.ft) ? TYG_I64 : M.Scal(lf.ft), AtL(lf.tmp),
                                    0);
                };
                Store(P { AtV(slot, lf.off), lf.ft },
                      lf.self ? ValV(lf.ft, slot)
                      : M.IsScalar(lf.ft) || TgModule::IsRel(lf.ft) ? ValV(lf.ft, ld()) :
                          MemV(lf.ft, AtL(lf.tmp)));
            }
        });
    }
    // Builds n's value at the place.
    void Into(const P &pl, Node *n) {
        if (SelfRelLit(n) && cg.TEq(n->exprtype, pl.t)) {
            SelfRelInto(pl, n);
            return;
        }
        if (auto al = Is<ArrayLit>(n);
            al && !al->fillval && !al->capexpr && pl.t->kind == TY_ARRAY && cg.HasRelRef(pl.t) &&
            cg.TEq(n->exprtype, pl.t) && M.TypeOk(pl.t) && cg.ArrSize(pl.t->arr) > 0 &&
            (pl.t->arr->akind == A_FIXED || cg.IsStaticLimited(pl.t))) {
            // Links built where they lie, so each offset measures from its slot (CodeGen::GenArrayLit).
            auto lim = pl.t->arr->akind != A_FIXED;
            auto et = pl.t->arr->sub;
            auto esz = cg.FixedSize(et);
            auto cnt = (int64_t)al->elems.size();
            if (cnt > cg.ArrSize(pl.t->arr)) No(n->line, "array literal length");
            tyg_zero(p, pl.a, M.Size(pl.t));
            for (int64_t i = 0; i < cnt && !Ended(); i++)
                Into(P { AddrPlus(pl.a, (lim ? M.LimWidth(pl.t) : 0) + i * esz), et },
                     al->elems[(size_t)i]);
            if (lim && !Ended()) LimSetLen(pl, I64(cnt));
            return;
        }
        auto x = GenAs(n, pl.t);
        if (M.IsScalar(pl.t) || x.mem) Store(pl, x);
        else if (!Ended()) No(n->line, "aggregate value");
    }

    // ---- Expressions ----------------------------------------------------------------------------

    V Gen(Node *n) {
        auto x = Gen1(n);
        // After a return or break the code using this value is still built: it needs a value.
        if (Ended() && !x.v && !x.mem && n->exprtype && M.IsScalar(n->exprtype))
            return Dummy(n->exprtype);
        if (Ended() || !x.t || !n->exprtype || n->exprtype->kind == TY_VOID) return x;
        return Fit(x, n->exprtype, n->line);
    }
    V GenAs(Node *n, TypeExpr *t) {
        if (t && t->kind == TY_SLICE) return GenSliceAs(n, t);
        if (t && t->kind == TY_REF && Is<NullLit>(n)) return Null(t, n->line);
        if (t && t->kind == TY_REF && cg.IsFatRef(t) && n->exprtype &&
            (IsGrow(n->exprtype) || M.FoOk(n->exprtype) || M.RsOk(n->exprtype) ||
             M.IsRb(n->exprtype))) {
            // A resizable where a reference to one is wanted: gs_rref, header address and stack.
            auto pl = Deref(Place(n));
            if (Ended()) return Dummy(t);
            if (!pl.stk || pl.tmphdr) No(n->line, "reference to an array of this kind");
            return PairV(t, AddrOf(pl.a), pl.stk);
        }
        if (t && IsAddrRef(t) && n->exprtype && IsImg(n->exprtype) && M.ImgOk(t->ref->sub)) {
            // A T[] (or variable-size struct) where a reference is wanted: its image's address.
            auto pl = Deref(Place(n));
            return ValV(t, Ended() ? I64(0) : AddrOf(pl.a));
        }
        return Fit(Gen(n), t, n->line);
    }
    tyg_val Truth(Node *n) { return OptCond(n) ? OptTest(n) : GenAs(n, M.ast.booltype).v; }
    V Dummy(TypeExpr *t) {
        V x;
        x.t = t;
        if (t && M.IsScalar(t))
            x.v = M.Scal(t) >= TYG_F32 ? tyg_fconst(p, M.Scal(t), 0) : tyg_iconst(p, M.Scal(t), 0);
        return x;
    }

    V Gen1(Node *n) {
        switch (n->nkind) {
            case NK_IntLit: {
                auto il = static_cast<IntLit *>(n);
                auto t = n->exprtype;
                if (t->kind == TY_FLT)
                    return ValV(t, tyg_fconst(p, M.Scal(t), il->uns ? (double)(uint64_t)il->val :
                                              (double)il->val));
                if (t->kind != TY_INT) No(n->line, "integer literal of this type");
                return ValV(t, tyg_iconst(p, M.Scal(t), il->val));
            }
            case NK_FltLit:
                if (n->exprtype->kind != TY_FLT) No(n->line, "float literal of this type");
                return ValV(n->exprtype,
                            tyg_fconst(p, M.Scal(n->exprtype), static_cast<FltLit *>(n)->val));
            case NK_NullLit: return Null(n->exprtype, n->line);
            case NK_SelfRef:   // `self` in a literal built where it lies: its start.
                if (!selfbase) No(n->line, "self outside a literal built in place");
                return ValV(n->exprtype, selfbase);
            case NK_BoolLit: return ValV(M.ast.booltype, I8(static_cast<BoolLit *>(n)->val ? 1 : 0));
            case NK_Ident: case NK_Index: return Load(Place(n), n->line);
            case NK_Dot: {
                auto d = static_cast<Dot *>(n);
                if (d->member == B_LEN) return GenLen(d->obj, n);
                if (d->member == B_CAP) return GenCap(d);
                if (d->variantconst) return GenVariantConst(d);
                if (!d->IsField()) No(n->line, "property or variant constant");
                return Load(Place(n), n->line);
            }
            case NK_ArrayLit: return GenArrayLit(static_cast<ArrayLit *>(n));
            case NK_SliceExpr: return GenSlice(static_cast<SliceExpr *>(n));
            case NK_StrLit: {
                // A string literal as a u8[:] (its bytes, read-only).
                auto t = n->exprtype;
                if (TgModule::IsLim(t)) return LimStr(static_cast<StrLit *>(n));
                auto &val = static_cast<StrLit *>(n)->val;
                // A u8[k] of exactly its bytes (§4.2): they, where they lie.
                if (t && t->kind == TY_ARRAY && t->arr->akind == A_FIXED && IsU8(t->arr->sub) &&
                    M.TypeOk(t) &&
                    (int64_t)val.size() == cg.FixedSize(t))
                    return MemV(t, tyg_at_sym(M.Bytes(val), 0));
                if (!t || t->kind != TY_SLICE || !IsU8(t->sub)) No(n->line, "string of this type");
                return PairV(t, BytesAddr(val), I64((int64_t)val.size()));
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
            default: No(n->line, cat("expression kind ", (int)n->nkind));
        }
    }

    V GenUnary(Unary *u) {
        auto et = u->exprtype;
        if (u->op == T_BITAND) {
            if (et && et->kind == TY_REF && cg.IsFatRef(et)) {
                // gs_rref: the header's address and its stack.
                auto pl = Deref(Place(u->child));
                if ((!IsGrow(pl.t) && !M.FoOk(pl.t) && !M.RsOk(pl.t) && !M.IsRb(pl.t)) || !pl.stk ||
                    pl.rbase ||
                    pl.tmphdr)
                    No(u->line, "reference to an array of this kind");
                return PairV(et, AddrOf(pl.a), pl.stk);
            }
            if (et && et->kind == TY_REF) {
                if (!IsPlainRef(et) && !M.OptRef(et) && !(TgModule::IsRel(et) && M.RelOk(et, 1)))
                    No(u->line, "reference kind");
                return ValV(et, AddrOf(Deref(Place(u->child)).a));
            }
            return Load(Deref(Place(u->child)), u->line);
        }
        if (u->op == T_NOT && OptCond(u->child))
            return ValV(M.ast.booltype, Cmp(TYG_EQ, OptTest(u->child), I8(0)));
        auto ot = cg.OperandT(u->child->exprtype);
        if (u->op == T_MINUS && (ot->kind == TY_STRUCT || ot->kind == TY_ARRAY)) {
            // Elementwise (CodeGen::GenElemwiseNegInto).
            auto o = ElemOperand(u->child, ot);
            if (Ended()) return Dummy(ot);
            auto r = MemTemp(ot, false);
            ElemLeaves(ot, 0, u->line, [&](TypeExpr *tt, int64_t off, const ElemDyn &dy) {
                tyg_store(p, M.Scal(tt), ElemAt(r.a, off, dy),
                          ScalarNeg(tt, ElemLeaf(o, tt, off, dy, u->line), u->line), 0);
            });
            return r;
        }
        if (!M.IsScalar(ot) || ot->kind == TY_REF) No(u->line, "unary operand type");
        auto x = GenAs(u->child, ot);
        switch (u->op) {
            case T_MINUS:
                if (ot->kind != TY_FLT && ot->kind != TY_INT) No(u->line, "negation type");
                x.v = ScalarNeg(ot, x.v, u->line);
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
    tyg_val FileArg(Line l) { return AddrOf(tyg_at_sym(M.FileName(l.fileidx), 0)); }
    tyg_val LineArg(Line l) { return I32(l.line); }

    // Calls a C shim or runtime function by name with scalar arguments (types for the signature).
    tyg_val CallC(const string &name, const vector<const tyg_type *> &pts,
                  const vector<tyg_val> &args, const tyg_type *ret) {
        auto sig = tyg_sig_new(m, ret, pts.data(), (int)pts.size(), 0);
        vector<tyg_arg> as(args.size());
        for (size_t i = 0; i < args.size(); i++) as[i].val = args[i];
        return tyg_call(p, sig, tyg_direct(M.Sym(name, TYG_SYM_FUNC | TYG_SYM_GLOBAL)), as.data(),
                        (int)as.size(), nullptr, nullptr);
    }
    const tyg_type *IT(tyg_scalar s, bool sgn = true) { return tyg_type_int(m, s, sgn); }

    // The runtime.h helper gs_<op>_<sfx> for t (checked arithmetic, division), through a shim.
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
        if (op == T_ANDAND || op == T_OROR) {
            auto t = tyg_local_new(p, 1, 1);
            auto lb = Cmp(TYG_NE, Truth(b->left), I8(0));
            tyg_store(p, TYG_I8, AtL(t), lb, 0);
            If(op == T_ANDAND ? lb : Cmp(TYG_EQ, lb, I8(0)), [&] {
                OpenScope(true);   // Its temporaries exist only when it runs.
                auto r = Truth(b->right);
                CloseScope();
                if (!Ended()) tyg_store(p, TYG_I8, AtL(t), Cmp(TYG_NE, r, I8(0)), 0);
            });
            return ValV(M.ast.booltype, tyg_load(p, TYG_I8, AtL(t), 0));
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
        auto arith = op == T_PLUS || op == T_MINUS || op == T_MUL || op == T_DIV || op == T_MOD;
        if (arith && b->exprtype && (b->exprtype->kind == TY_STRUCT || b->exprtype->kind == TY_ARRAY))
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
            auto n = AndK(r.v, IntBits(lt->intstorage) - 1);
            auto w = s == TYG_I64 ? l.v : tyg_convert(p, sgn ? TYG_SEXT : TYG_ZEXT, TYG_I64, l.v);
            auto res = Bin(op == T_SHL ? TYG_SHL : sgn ? TYG_ASHR : TYG_LSHR, w, n);
            return ValV(lt, s == TYG_I64 ? res : tyg_convert(p, TYG_TRUNC, s, res));
        }
        auto l = GenAs(b->left, lt);
        auto r = GenAs(b->right, lt);
        if (Ended()) return Dummy(lt);
        static const struct { TType op; tyg_cond sgn, uns, flt; } cmps[] = {
            { T_LT, TYG_SLT, TYG_ULT, TYG_FLT }, { T_GT, TYG_SGT, TYG_UGT, TYG_FGT },
            { T_LTEQ, TYG_SLE, TYG_ULE, TYG_FLE }, { T_GTEQ, TYG_SGE, TYG_UGE, TYG_FGE },
            { T_EQ, TYG_EQ, TYG_EQ, TYG_FEQ }, { T_NEQ, TYG_NE, TYG_NE, TYG_FNE } };
        for (auto &c : cmps)
            if (op == c.op)
                return ValV(M.ast.booltype, Cmp(isflt ? c.flt : sgn ? c.sgn : c.uns, l.v, r.v));
        if (isflt) {
            if (!arith) No(b->line, "float operator");
            return ValV(lt, ScalarArith(op, lt, l.v, r.v, b->line));
        }
        if (lt->kind == TY_INT) return ValV(lt, IntOp(op, lt, l.v, r.v, b->line));
        if (lt->kind == TY_BOOL && (op == T_BITAND || op == T_BITOR || op == T_XOR))
            return ValV(lt, Bin(op == T_BITAND ? TYG_AND : op == T_BITOR ? TYG_OR : TYG_XOR,
                                    l.v, r.v));
        No(b->line, "operator on this type");
    }

    // A bool from a body that jumps to `fail` on the first difference; != negates it.
    template <class F> V EqViaFail(Binary *b, F body) {
        auto res = tyg_local_new(p, 1, 1);
        auto fail = NewBlock(), done = NewBlock();
        body(fail);
        tyg_store(p, TYG_I8, AtL(res), I8(1), 0);
        tyg_jump(p, done);
        SetBlock(fail);
        tyg_store(p, TYG_I8, AtL(res), I8(0), 0);
        tyg_jump(p, done);
        SetBlock(done);
        return EqResult(b, tyg_load(p, TYG_I8, AtL(res), 0));
    }
    V EqResult(Binary *b, tyg_val v) {
        return ValV(M.ast.booltype, b->op == T_NEQ ? Cmp(TYG_EQ, v, I8(0)) : v);
    }

    // Structural equality (§4.5; CodeGen::EqX): a jump to fail on the first difference, else on past.
    // Fixed values field by field, an ADT's tag then its payload; floats as floats; a bytes value of
    // exact encodings by its bytes.
    void EqJump(const P &a, const P &b, tyg_block fail, Line l) {
        auto t = a.t;
        auto cmpw = [&](int64_t off, tyg_scalar s, tyg_cond c) {
            ContinueIf(Cmp(c, tyg_load(p, s, AddrPlus(a.a, off), 0),
                           tyg_load(p, s, AddrPlus(b.a, off), 0)), fail);
        };
        if (!cg.IsFix(t)) {
            if (!EqBitwise(t)) {
                // Floats or pads among its bytes: walked field by field, over a cursor in each
                // (CodeGen::EmitEqWalk).
                auto ca = Local(AddrOf(a.a)), cb = Local(AddrOf(b.a));
                EqWalk(t, ca, cb, fail, l);
                return;
            }
            auto pa = AddrOf(a.a), pb = AddrOf(b.a);
            ContinueIf(Cmp(TYG_EQ, ImgSize(t, pa), ImgSize(t, pb)), fail);
            ContinueIf(BytesEq(pa, pb, ImgSize(t, pa)), fail);
            return;
        }
        if (TgModule::IsRel(t)) return cmpw(0, M.RelScal(t), TYG_EQ);
        if (t->kind == TY_REF && cg.IsFatRef(t)) return cmpw(0, TYG_I64, TYG_EQ);
        if (M.IsScalar(t)) return cmpw(0, M.Scal(t), t->kind == TY_FLT ? TYG_FEQ : TYG_EQ);
        if (t->kind == TY_SLICE) {
            cmpw(0, TYG_I64, TYG_EQ);
            cmpw(8, TYG_I64, TYG_EQ);
            return;
        }
        if (!M.TypeOk(t)) No(l, cat("comparison of type ", M.NameWhy(t)));
        if (t->kind == TY_STRUCT || t->kind == TY_VARIANT) {
            for (auto &f : M.Fields(t))
                EqJump(P { AddrPlus(a.a, f.off), f.t }, P { AddrPlus(b.a, f.off), f.t }, fail, l);
            return;
        }
        if (t->kind == TY_ENUM) {
            ContinueIf(Cmp(TYG_EQ, LoadTag(a.a, t), LoadTag(b.a, t)), fail);
            auto tag = Local(LoadTag(a.a, t));
            TagSwitch(tag, cg.EIOf(t)->en->variants.size(), [&](size_t vi) {
                auto vt = cg.VariantType(t, (int)vi);
                if (!cg.FixedSize(vt)) return;
                EqJump(P { AddrPlus(a.a, M.TagSize(t)), vt }, P { AddrPlus(b.a, M.TagSize(t)), vt },
                       fail, l);
            });
            return;
        }
        if (t->kind == TY_ARRAY) {
            auto lim = TgModule::IsLim(t);
            auto va = lim ? LimView(a) : ViewOf(a, l), vb = lim ? LimView(b) : ViewOf(b, l);
            if (lim) ContinueIf(Cmp(TYG_EQ, va.len, vb.len), fail);
            EqElems(va, vb, fail, l);
            return;
        }
        No(l, cat("comparison of type ", M.NameWhy(t)));
    }
    // The bytes value at each cursor compared, both cursors left behind it (CodeGen::EmitEqWalk):
    // fixed parts by value, a varint by its bytes, an array by length then elements, a struct by
    // its fields, an ADT by tag then payload.
    void EqWalk(TypeExpr *t, tyg_local ca, tyg_local cb, tyg_block fail, Line l) {
        auto A = [&] { return LdL(ca); };
        auto B = [&] { return LdL(cb); };
        auto adv = [&](tyg_val n) {
            StL(ca, Add(A(), n));
            StL(cb, Add(B(), n));
        };
        auto bad = [&] { No(l, cat("comparison of type ", M.NameWhy(t))); };
        if (cg.IsFix(t)) {
            if (!M.TypeOk(t)) bad();
            EqJump(P { AtV(A()), t }, P { AtV(B()), t }, fail, l);
            adv(I64(cg.FixedSize(t)));
            return;
        }
        switch (t->kind) {
            case TY_INT: case TY_REF: {
                if (!TgModule::IsVarint(t) && !TgModule::IsVr(t)) bad();
                auto na = Local(UlebSize(A()));
                ContinueIf(Cmp(TYG_EQ, UlebSize(B()), LdL(na)), fail);
                ContinueIf(BytesEq(A(), B(), LdL(na)), fail);
                adv(LdL(na));
                return;
            }
            case TY_ARRAY: {
                auto sub = t->arr->sub;
                auto lena = Slot(), lenb = Slot();
                auto capa = Slot(), capb = Slot();
                auto rc = TgModule::IsLim(t);
                // The length (and a limited array's capacity) at a cursor, which moves past them.
                auto head = [&](tyg_local c, tyg_local len, tyg_local cap) {
                    P at { AtV(LdL(c)), t };
                    if (!rc && t->arr->akind != A_VAR) bad();
                    if (rc) StL(cap, LimCapOf(at));
                    auto v = rc ? LimView(at) : VarArrView(at);
                    StL(len, v.len);
                    StL(c, v.elems);
                };
                head(ca, lena, capa);
                head(cb, lenb, capb);
                ContinueIf(Cmp(TYG_EQ, LdL(lena), LdL(lenb)), fail);
                Loop(I64(0), LdL(lena), [&](tyg_val) { EqWalk(sub, ca, cb, fail, l); });
                if (rc) {
                    // Each side's unused slots.
                    auto esz = cg.FixedSize(sub);
                    for (auto [c, cap, len] :
                         { std::tuple { ca, capa, lena }, std::tuple { cb, capb, lenb } })
                        StL(c, Add(LdL(c), Mul(Sub(LdL(cap), LdL(len)), I64(esz))));
                }
                return;
            }
            case TY_STRUCT: case TY_VARIANT:
                cg.EachField(t, [&](TypeExpr *ft) { EqWalk(ft, ca, cb, fail, l); });
                return;
            case TY_ENUM: {
                ContinueIf(Cmp(TYG_EQ, LoadTag(AtV(A()), t), LoadTag(AtV(B()), t)), fail);
                auto tag = Local(LoadTag(AtV(A()), t));
                adv(I64(M.TagSize(t)));
                TagSwitch(tag, cg.EIOf(t)->en->variants.size(),
                          [&](size_t vi) { EqWalk(cg.VariantType(t, (int)vi), ca, cb, fail, l); });
                return;
            }
            default: bad();
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
    // Whether a bytes value's bytes are equal exactly when it is: no pad, no float, no fixed-mode ADT
    // (CodeGen::EmitEqBytes's bitwise).
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
        return EqViaFail(b, [&](tyg_block fail) {
            EqJump(pa, P { pb.a, pa.t, pb.stk }, fail, b->line);
        });
    }

    // Elementwise arithmetic on structs and fixed arrays (§6.1; CodeGen::GenElemwiseInto): each numeric
    // leaf of the result from the same leaf of each operand, or from a scalar operand. A leaf's place:
    // the constant offset from the callback, plus for an element of an array too big to unroll the
    // bytes from the array's start, kept in a local.
    struct ElemDyn {
        bool on;
        tyg_local loc;
    };
    tyg_addr ElemAt(tyg_addr a, int64_t off, const ElemDyn &dy) {
        return dy.on ? AtV(Add(AddrOf(a), LdL(dy.loc)), off) : AddrPlus(a, off);
    }
    // F(leaf type, offset, ElemDyn) for each scalar leaf: an array's elements unrolled up to 256,
    // beyond that in a counted loop (as CodeGen).
    template <class F>
    void ElemLeaves(TypeExpr *t, int64_t off, Line l, F leaf, ElemDyn dy = ElemDyn { false, 0 }) {
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
                    for (int64_t i = 0; i < n && !Ended(); i++)
                        ElemLeaves(t->arr->sub, off + i * esz, l, leaf, dy);
                    return;
                }
                Loop(I64(0), I64(n), [&](tyg_val i) {
                    auto at = Mul(i, I64(esz));
                    ElemLeaves(t->arr->sub, off, l, leaf,
                               ElemDyn { true, Local(dy.on ? Add(at, LdL(dy.loc)) : at) });
                });
                return;
            }
            default: No(l, cat("elementwise ", M.NameWhy(t)));
        }
    }
    tyg_val ScalarArith(TType op, TypeExpr *t, tyg_val a, tyg_val b, Line l) {
        static const pair<TType, TType> compound[] = { { T_PLUSEQ, T_PLUS }, { T_MINUSEQ, T_MINUS },
                                                       { T_MULEQ, T_MUL }, { T_DIVEQ, T_DIV },
                                                           { T_MODEQ, T_MOD } };
        for (auto &[eq, plain] : compound)
            if (op == eq) op = plain;
        if (t->kind != TY_FLT) return IntOp(op, t, a, b, l);
        if (op != T_PLUS && op != T_MINUS && op != T_MUL && op != T_DIV) {
            auto f32 = M.Scal(t) == TYG_F32;
            auto ft = tyg_type_scalar(m, M.Scal(t));
            vector<const tyg_type *> pts { ft, ft };
            auto sig = tyg_sig_new(m, ft, pts.data(), 2, 0);
            tyg_arg as[2] {};
            as[0].val = a;
            as[1].val = b;
            return tyg_call(p, sig,
                            tyg_direct(M.Sym(f32 ? "fmodf" : "fmod",
                                             TYG_SYM_FUNC | TYG_SYM_GLOBAL | TYG_SYM_FOREIGN)),
                            as, 2, nullptr, nullptr);
        }
        return Bin(op == T_PLUS ? TYG_FADD : op == T_MINUS ? TYG_FSUB : op == T_MUL ? TYG_FMUL :
                       TYG_FDIV, a, b);
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
            auto lv = ElemLeaf(lo, tt, off, dy, b->line), rv = ElemLeaf(ro, tt, off, dy, b->line);
            tyg_store(p, M.Scal(tt), ElemAt(r.a, off, dy), ScalarArith(b->op, tt, lv, rv, b->line), 0);
        });
        return r;
    }

    // a == b of arrays or slices (GenRangeEq, §4.5): the lengths, then the elements' bytes where that
    // is their equality (integers, bools).
    V GenArrayEq(Binary *b) {
        auto l = ViewOf(Place(b->left), b->line);
        auto r = ViewOf(Place(b->right), b->line);
        auto et = l.elem;
        if (cg.Mangle(et) != cg.Mangle(r.elem)) No(b->line, "array comparison of another element type");
        if (!(et->kind == TY_INT || et->kind == TY_BOOL) || et->intstorage == IS_VARINT)
            return EqViaFail(b, [&](tyg_block fail) {
                ContinueIf(Cmp(TYG_EQ, l.len, r.len), fail);
                EqElems(l, r, fail, b->line);
            });
        auto res = tyg_local_new(p, 1, 1);
        auto same = Cmp(TYG_EQ, l.len, r.len);
        tyg_store(p, TYG_I8, AtL(res), same, 0);
        If(same, [&] {
            tyg_store(p, TYG_I8, AtL(res),
                      BytesEq(l.elems, r.elems, Mul(l.len, I64(cg.FixedSize(et)))), 0);
        });
        return EqResult(b, tyg_load(p, TYG_I8, AtL(res), 0));
    }

    // An integer operation at t's width: unsigned and release-build signed arithmetic wraps, a debug
    // build's signed arithmetic checks (runtime.h), division and modulo check always (§6.2).
    tyg_val IntOp(TType op, TypeExpr *t, tyg_val l, tyg_val r, Line ln) {
        switch (op) {
            case T_PLUS: case T_MINUS: case T_MUL:
                if (TgModule::IsSigned(t) && M.checks)
                    return CheckedShim(op == T_PLUS ? "add" : op == T_MINUS ? "sub" : "mul", t,
                                       { l, r }, ln);
                return Bin(op == T_PLUS ? TYG_ADD : op == T_MINUS ? TYG_SUB : TYG_MUL, l, r);
            case T_DIV: return CheckedShim("div", t, { l, r }, ln);
            case T_MOD: return CheckedShim("mod", t, { l, r }, ln);
            case T_BITAND: return Bin(TYG_AND, l, r);
            case T_BITOR: return Bin(TYG_OR, l, r);
            case T_XOR: return Bin(TYG_XOR, l, r);
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
        if (tt->kind == TY_FLT) {
            if (st->kind == TY_BOOL) No(c->line, "bool cast");
            return ValV(tt, Conv(x.v, st, tt, c->line));
        }
        if (tt->kind != TY_INT || tt->intstorage == IS_VARINT) No(c->line, "cast target");
        if (st->kind == TY_BOOL) No(c->line, "bool cast");
        auto is = tt->intstorage;
        auto i64t = M.ast.inttypes[IS_I64];
        auto rng = IntRange(is);
        auto lo = rng.first, hi = rng.second;
        auto ptr = tyg_type_ptr(m), i32 = IT(TYG_I32), f64 = tyg_type_scalar(m, TYG_F64);
        // A runtime shim taking the abort message's file and line after the arguments.
        auto shim = [&](const char *name, vector<const tyg_type *> pts, vector<tyg_val> as, bool sgn) {
            M.NeedShim(name);
            pts.push_back(ptr);
            pts.push_back(i32);
            as.push_back(FileArg(c->line));
            as.push_back(LineArg(c->line));
            return CallC(name, pts, as, IT(TYG_I64, sgn));
        };
        tyg_val r;
        if (st->kind == TY_FLT) {
            auto d = M.Scal(st) == TYG_F32 ? tyg_convert(p, TYG_FEXT, TYG_F64, x.v) : x.v;
            auto isf32 = I32(M.Scal(st) == TYG_F32);
            if (c->unchecked) {
                M.NeedShim("gs_tg_f2iwrap");
                r = CallC("gs_tg_f2iwrap", { f64 }, { d }, IT(TYG_I64));
            } else if (is == IS_U64) {
                r = shim("gs_tg_f2u", { f64, i32 }, { d, isf32 }, false);
            } else {
                r = shim("gs_tg_f2i", { f64, i32, IT(TYG_I64), IT(TYG_I64), ptr },
                         { d, isf32, I64(lo), I64(hi), TypeNameArg(is) }, true);
            }
            return ValV(tt, Conv(r, i64t, tt, c->line));
        }
        auto fits = [&] {
            auto sr = IntRange(st->intstorage);
            return st->intstorage != IS_U64 && sr.first >= lo && sr.second <= hi;
        };
        if (c->unchecked || cg.TEq(st, tt) || fits() || (st->intstorage == IS_U64 && is == IS_U64))
            return ValV(tt, Conv(x.v, st, tt, c->line));
        // The checked narrowings of GS_RANGE and GS_RANGE_U: identities in a release build.
        if (st->intstorage == IS_U64) {
            auto u64 = IT(TYG_I64, false);
            r = shim("gs_tg_range_u", { u64, u64, ptr }, { x.v, I64(hi), TypeNameArg(is) }, false);
            return ValV(tt, Conv(r, M.ast.inttypes[IS_U64], tt, c->line));
        }
        auto i64 = IT(TYG_I64);
        r = shim("gs_tg_range", { i64, i64, i64, ptr },
                 { Conv(x.v, st, i64t, c->line), I64(lo), I64(hi), TypeNameArg(is) }, true);
        return ValV(tt, Conv(r, i64t, tt, c->line));
    }
    tyg_val TypeNameArg(IntStorage is) {
        return BytesAddr(cat(IntStorageName(is), string(1, '\0')));
    }

    V GenStructLit(StructLit *sl) {
        auto t = sl->exprtype;
        if (V x; InPlaceValue(sl, t, sl->line, x)) return x;
        auto adt = t->kind == TY_VARIANT || t->kind == TY_ENUM;
        if (!M.TypeOk(t) || (adt ? t->kind == TY_ENUM && !sl->variant : !sl->sinst || sl->variant ||
                             t->kind != TY_STRUCT))
            No(sl->line, "literal of this type");
        auto x = MemTemp(t, true);
        if (t->kind == TY_ENUM) StoreTag(x.a, t, M.EI(t)->en->VariantIndex(sl->variant));
        EachLitInit(sl, [&](Node *init, TypeExpr *ft, int64_t off) {
            // A self-relative link measures from the slot it lies in, which a literal built here and
            // copied out of is not.
            if (!adt && TgModule::IsRel(ft) && !ft->ref->pool && !Is<NullLit>(init))
                No(sl->line, "self-relative link in a literal");
            Into(P { AddrPlus(x.a, off), ft }, init);
        });
        return x;
    }

    // ---- Arrays and slices ----------------------------------------------------------------------

    // The elements of an array or slice at pl (references read through) and their count.
    View ViewOf(P pl, Line l) {
        pl = Deref(pl);
        if (pl.t->kind == TY_ARRAY && pl.t->arr->akind == A_FIXED && M.TypeOk(pl.t)) {
            auto k = cg.ArrSize(pl.t->arr);
            return { AddrOf(pl.a), I64(k), pl.t->arr->sub, k };
        }
        auto a8 = pl.a;
        a8.offset += 8;
        if ((pl.t->kind == TY_SLICE && M.TypeOk(pl.t)) || (IsGrow(pl.t) && M.RzOk(pl.t))) {
            // { data, len }, and gs_rhdr { base, len } alike.
            auto elem = pl.t->kind == TY_SLICE ? pl.t->sub : pl.t->arr->sub;
            return { BaseOf(pl), Ld(a8), elem, -1 };
        }
        if (TgModule::IsLim(pl.t) && M.LimHeld(pl.t)) return LimView(pl);
        // Elements of variable size: walked from the first (StepElem).
        if (IsGrow(pl.t) && M.VeOk(pl.t)) return { BaseOf(pl), Ld(a8), pl.t->arr->sub, -1 };
        if (IsArrayKind(pl.t, A_VAR) && (M.RzOk(pl.t) || M.VarImgOk(pl.t))) return VarArrView(pl);
        No(l, "array of this kind");
    }
    // A T[] image at pl: the length prefix, the elements behind it.
    View VarArrView(const P &pl) {
        auto w = TgModule::LenWidth(pl.t);
        auto base = AddrOf(pl.a);
        if (w == 0) return { Add(base, UlebSize(base)), UlebRead(base), pl.t->arr->sub, -1 };
        auto s = w == 1 ? TYG_I8 : w == 2 ? TYG_I16 : w == 4 ? TYG_I32 : TYG_I64;
        auto n = tyg_load(p, s, pl.a, 0);
        if (s != TYG_I64)
            n = tyg_convert(p, TgModule::SignedLen(pl.t) ? TYG_SEXT : TYG_ZEXT, TYG_I64, n);
        return { Add(base, I64(w)), n, pl.t->arr->sub, -1 };
    }
    tyg_val ElemAddr(const View &v, tyg_val i) {
        if (!cg.IsFix(v.elem)) No(Line {}, "an element of variable size by its index");
        return Add(v.elems, Mul(i, I64(cg.FixedSize(v.elem))));
    }

    // The element at `at`: a varint one zigzag-encoded there.
    P ElemPlace(tyg_val at, TypeExpr *et) {
        P r { AtV(at), et };
        r.enc = TgModule::IsVarint(et);
        return r;
    }
    // The element after the one at `at`: a fixed size on, or past its image's bytes.
    tyg_val StepElem(const View &v, tyg_val at) {
        return Add(at, cg.IsFix(v.elem) ? I64(cg.FixedSize(v.elem)) : ImgSize(v.elem, at));
    }

    // The end of a view's elements, each walked by its size where they vary (CodeGen's SizeFn).
    tyg_val RunEnd(const View &v) {
        if (cg.IsFix(v.elem)) return Add(v.elems, Mul(v.len, I64(cg.FixedSize(v.elem))));
        auto cl = Local(v.elems);
        Loop(I64(0), v.len, [&](tyg_val) { StL(cl, Add(LdL(cl), ImgSize(v.elem, LdL(cl)))); });
        return LdL(cl);
    }

    tyg_val Index64(Node *n) {
        auto it = cg.OperandT(n->exprtype);
        if (!it || it->kind != TY_INT || it->intstorage == IS_VARINT) No(n->line, "index type");
        return Conv(GenAs(n, it).v, it, M.ast.inttypes[IS_I64], n->line);
    }

    // GS_IDX: i in [0, n), or gs_idxfail with the Goose file and line.
    void BoundsCheck(tyg_val i, tyg_val n, Line l) {
        If(Cmp(TYG_UGE, i, n), [&] {
            CallC("gs_idxfail", { IT(TYG_I64), IT(TYG_I64), tyg_type_ptr(m), IT(TYG_I32) },
                  { i, n, FileArg(l), LineArg(l) }, nullptr);
            tyg_unreachable(p);
        });
    }

    // a[i] (CodeGen::IndexLoc): the check is elided by BCE (Index::nobc) or a constant within a fixed
    // array's length.
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
        auto i64 = M.ast.inttypes[IS_I64];
        if (ot->kind != TY_ARRAY || ot->arr->akind != A_FIXED)
            return ValV(i64, ViewOf(Place(obj), n->line).len);
        if (!Is<Ident>(obj) && !Is<Dot>(obj)) Gen(obj);   // What it does, it does.
        return ValV(i64, I64(cg.ArrSize(ot->arr)));
    }


    // The literal's elements in consecutive slots from dst, up to cap of them (a [v; k] exactly cap
    // when `exact`): v built once and copied. Their count.
    int64_t LitElemsAt(ArrayLit *al, tyg_addr dst, TypeExpr *et, int64_t cap, bool exact) {
        auto esz = cg.FixedSize(et);
        if (al->fillval) {
            auto il = Is<IntLit>(al->fillcount);
            if (!il || il->val < 0 || il->val > cap || (exact && il->val != cap))
                No(al->line, "fill count");
            auto one = Temp(et);
            Into(P { AtL(one), et }, al->fillval);
            if (!Ended())
                Loop(I64(0), I64(il->val), [&](tyg_val i) {
                    tyg_copy(p, AtV(Add(AddrOf(dst), Mul(i, I64(esz)))), AtL(one), esz);
                });
            return il->val;
        }
        auto count = (int64_t)al->elems.size();
        if (count > cap) No(al->line, "array literal length");
        for (int64_t i = 0; i < count; i++) {
            auto a = dst;
            a.offset += i * esz;
            Into(P { a, et }, al->elems[(size_t)i]);
        }
        return count;
    }
    // A literal's type set to t until the end of the scope, also when it is left to C.
    struct LitTypeGuard {
        ArrayLit *al;
        TypeExpr *saved;
        LitTypeGuard(ArrayLit *a, TypeExpr *t) : al(a), saved(a->exprtype) { a->exprtype = t; }
        ~LitTypeGuard() { al->exprtype = saved; }
    };

    // A value at a slice destination: a whole array there is a view of the array where it lies (§3.10),
    // or of a temporary if it is a value.
    V GenSliceAs(Node *n, TypeExpr *t) {
        if (!M.TypeOk(t)) No(n->line, "slice type");
        if (auto al = Is<ArrayLit>(n); al && !al->capexpr && cg.IsFix(t->sub)) {
            // A literal here is a temporary array, sliced whole (GenArrayLit).
            LitTypeGuard guard(al, t);
            return GenArrayLit(al);
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
        return PairV(t, v.elems, v.len);
    }

    // a[lo:hi] (CodeGen::GenSlice): the view, then the bounds (^k counts from the end), checked as the
    // C checks them unless BCE proved them.
    V GenSlice(SliceExpr *se) {
        auto v = ViewOf(Place(se->obj), se->line);
        auto bound = [&](Node *b, bool fromend, tyg_val dflt) {
            return !b ? dflt : fromend ? Sub(v.len, Index64(b)) : Index64(b);
        };
        auto lo = bound(se->lo, se->lo_from_end, I64(0));
        auto hi = bound(se->hi, se->hi_from_end, v.len);
        if (!se->nobc) {
            auto bad = Bin(TYG_OR, Cmp(TYG_SLT, lo, I64(0)), Cmp(TYG_SLT, hi, lo));
            If(Bin(TYG_OR, bad, Cmp(TYG_SGT, hi, v.len)), [&] {
                M.NeedShim("gs_tg_slicefail");
                CallC("gs_tg_slicefail", { tyg_type_ptr(m), IT(TYG_I32) },
                      { FileArg(se->line), LineArg(se->line) }, nullptr);
                tyg_unreachable(p);
            });
        }
        auto t = se->exprtype;
        // A slice the checker typed as the array it builds (a T[] result) is a view all the same.
        if (t && t->kind == TY_ARRAY && t->arr->akind != A_FIXED)
            t = M.ast.SliceOf(t->arr->sub, se->line);
        if (!t || t->kind != TY_SLICE || cg.Mangle(t->sub) != cg.Mangle(v.elem))
            No(se->line, "slice to this type");
        // Variable elements: whole-array slices only (the checker's rule).
        if (!cg.IsFix(v.elem)) return PairV(t, v.elems, v.len);
        return PairV(t, ElemAddr(v, lo), Sub(hi, lo));
    }

    // [a, b, c] and [v; k] as a fixed array.
    V GenArrayLit(ArrayLit *al) {
        auto t = al->exprtype;
        if (TgModule::IsLim(t)) return LimLit(al);
        if (V x; t && InPlaceValue(al, t, al->line, x)) return x;
        if (t && t->kind == TY_SLICE && M.TypeOk(t) && !al->capexpr && cg.IsFix(t->sub)) {
            // A literal in slice position: a temporary fixed array, sliced whole
            // (CodeGen::GenFixedArrayLit).
            auto fc = Is<IntLit>(al->fillcount);
            auto count = al->fillval ? (fc ? fc->val : -1) : (int64_t)al->elems.size();
            if (count < 0) No(al->line, "fill count");
            LitTypeGuard guard(al, M.ast.ArrayOf(t->sub, A_FIXED, al->line, count));
            auto av = GenArrayLit(al);
            if (Ended() || !av.mem) return Dummy(t);
            return PairV(t, AddrOf(av.a), I64(count));
        }
        if (!t || t->kind != TY_ARRAY || t->arr->akind != A_FIXED || !M.TypeOk(t) || al->capexpr)
            No(al->line, cat("array literal of type ", t ? M.NameWhy(t) : "?"));
        auto k = cg.ArrSize(t->arr);
        // Its links measure from the slot it lies in, which a temporary is not.
        if (SelfRelLit(al))
            No(al->line, al->fillval ? "fill of a value linking to itself" :
               "self-relative link in a literal");
        auto tmp = Temp(t);
        tyg_zero(p, AtL(tmp), M.Size(t));
        LitElemsAt(al, AtL(tmp), t->arr->sub, k, true);
        return MemV(t, AtL(tmp));
    }

    // ---- Limited arrays T[..k] ------------------------------------------------------------------
    // The C half's struct { lenT len; T e[k]; } (TgModule::LimOk), changed where it lies with the C
    // backend's checks and abort codes (CodeGen::CopyIntoLimited): growing past the capacity aborts
    // with GS_E_CAPACITY, and other arrays or slices are copied in under that check (§4.2). Slots past
    // the length are zero in a new value, as the C's `{0}`.

    tyg_addr LimLenAt(const P &pl) {
        auto a = pl.a;
        a.offset += M.IsRc(pl.t) ? 4 : 0;
        return a;
    }
    tyg_val LimLen(const P &pl) {
        auto s = M.LimLenScal(pl.t);
        auto n = tyg_load(p, s, LimLenAt(pl), 0);
        return s == TYG_I64 ? n : tyg_convert(p, TYG_ZEXT, TYG_I64, n);
    }
    void LimSetLen(const P &pl, tyg_val n) {
        auto s = M.LimLenScal(pl.t);
        tyg_store(p, s, LimLenAt(pl), s == TYG_I64 ? n : tyg_convert(p, TYG_TRUNC, s, n), 0);
    }
    int64_t LimCap(TypeExpr *t) { return cg.ArrSize(t->arr); }
    // The capacity at pl as an i64: the type's, or a T[..]'s own.
    tyg_val LimCapOf(const P &pl) {
        return M.IsRc(pl.t) ? tyg_convert(p, TYG_ZEXT, TYG_I64, tyg_load(p, TYG_I32, pl.a, 0)) :
            I64(LimCap(pl.t));
    }
    View LimView(const P &pl) {
        return { Add(AddrOf(pl.a), I64(M.LimWidth(pl.t))), LimLen(pl), pl.t->arr->sub, -1 };
    }
    tyg_val LimElemAddr(const P &pl, tyg_val i) { return ElemAddr(LimView(pl), i); }
    P LimTemp(TypeExpr *t) { return P { MemTemp(t, true).a, t }; }
    V GenCap(Dot *d) {
        auto ot = d->obj->exprtype;
        while (ot && IsPlainRef(ot)) ot = ot->ref->sub;
        if (!TgModule::IsLim(ot) || !M.LimHeld(ot)) No(d->line, "cap of this type");
        auto i64 = M.ast.inttypes[IS_I64];
        if (M.IsRc(ot)) return ValV(i64, LimCapOf(Deref(Place(d->obj))));
        if (!Is<Ident>(d->obj) && !Is<Dot>(d->obj)) Gen(d->obj);   // What it does, it does.
        return ValV(i64, I64(LimCap(ot)));
    }
    // v's elements appended to pl (CopyIntoLimited, EmitAppend): count and capacity check first, then
    // the elements (moved as memmove moves them) and the length.
    void LimAppendView(const P &pl, const View &v, Line l) {
        auto et = pl.t->arr->sub;
        if (cg.Mangle(v.elem) != cg.Mangle(et)) No(l, "array of another element type");
        auto nl = Local(v.len), ol = Local(LimLen(pl));
        auto total = [&] { return Add(LdL(ol), LdL(nl)); };
        AbortUnless(Cmp(TYG_SLE, total(), LimCapOf(pl)), "CAPACITY", l);
        tyg_move_dyn(p, LimElemAddr(pl, LdL(ol)), v.elems, Mul(LdL(nl), I64(cg.FixedSize(et))));
        LimSetLen(pl, total());
    }
    V LimFrom(const V &x, TypeExpr *want, Line l) {
        auto v = ViewOf(P { x.a, x.t, x.stk }, l);
        auto dst = LimTemp(want);
        LimAppendView(dst, v, l);
        return MemV(dst.t, dst.a);
    }
    V LimStr(StrLit *s) {
        auto t = s->exprtype;
        if (!M.TypeOk(t) || !IsU8(t->arr->sub) || (int64_t)s->val.size() > LimCap(t))
            No(s->line, "string of this type");
        auto dst = LimTemp(t);
        LimAppendView(dst, { BytesAddr(s->val),
                             I64((int64_t)s->val.size()), t->arr->sub, -1 }, s->line);
        return MemV(t, dst.a);
    }
    // [a, b], [v; n] and [..c] typed as a T[..k]: the elements in order into the first slots, the
    // length their count.
    V LimLit(ArrayLit *al) {
        auto t = al->exprtype;
        if (!M.TypeOk(t)) No(al->line, cat("array literal of type ", M.NameWhy(t)));
        auto dst = LimTemp(t);
        auto first = dst.a;
        first.offset += M.LimWidth(t);
        if (al->capexpr && (al->fillval || !al->elems.empty()))
            No(al->line, "array literal of this kind");
        auto count = al->capexpr ? 0 : LitElemsAt(al, first, t->arr->sub, LimCap(t), false);
        if (!Ended()) LimSetLen(dst, I64(count));
        return MemV(t, dst.a);
    }
    // A call returning a T[..k] that the checker typed as the array built from it (a T[], a resizable):
    // its result where it lies, to copy from.
    bool LimCallResult(Node *n) {
        auto c = Is<Call>(n);
        if (!c || c->builtin >= 0 || !n->exprtype) return false;
        auto sp = c->spec ? c->spec : !c->dispatch.empty() ? c->dispatch[0] : nullptr;
        if (!sp || sp->rets.empty()) return false;
        auto rt = sp->rets[0];
        auto arr = rt->kind == TY_ARRAY &&
                   (rt->arr->akind == A_LIMITED || rt->arr->akind == A_FIXED || IsVarArr(rt) ||
                    IsGrow(rt));
        return (arr || rt->kind == TY_SLICE) && !cg.TEq(n->exprtype, rt);
    }

    // The element a push or alloc stores: evaluated now into x, or, for a literal linking to itself,
    // left to be built in its slot (CodeGen's atslot). False when the block ended.
    bool PushOperand(Node *arg, TypeExpr *et, Line l, bool &atslot, V &x) {
        atslot = cg.HasRelRef(et) && SelfRelLitOk(arg) && cg.TEq(arg->exprtype, et);
        if (atslot) return true;
        x = GenAs(arg, et);
        if (Ended()) return false;
        if (!M.IsScalar(et) && !x.mem) No(l, "aggregate value");
        return true;
    }
    void StoreOperand(tyg_val at, TypeExpr *et, bool atslot, Node *arg, const V &x) {
        if (atslot) SelfRelInto(P { AtV(at), et }, arg);
        else Store(P { AtV(at), et }, x);
    }

    // v.push(x) (EmitPush): the element, then the capacity check, the slot, the length. Its value is
    // the slot's address.
    V LimPush(Call *c, const P &pl) {
        auto dp = discardpush;
        discardpush = false;
        if (!M.LimHeld(pl.t)) No(c->line, cat("push to ", M.NameWhy(pl.t)));
        auto et = pl.t->arr->sub;
        auto arg = c->ArgNodes()[1];
        bool atslot;
        V x;
        if (!PushOperand(arg, et, c->line, atslot, x)) return Dummy(c->exprtype);
        auto nl = Local(LimLen(pl));
        AbortUnless(Cmp(TYG_SLT, LdL(nl), LimCapOf(pl)), "CAPACITY", c->line);
        auto at = LimElemAddr(pl, LdL(nl));
        StoreOperand(at, et, atslot, arg, x);
        if (Ended()) return Dummy(c->exprtype);
        LimSetLen(pl, Add(LdL(nl), I64(1)));
        pushed = P { AtV(at), et };
        if (c->exprtype && c->exprtype->kind == TY_REF) return ValV(c->exprtype, at);
        if (!dp && c->exprtype && c->exprtype->kind != TY_VOID && M.TypeOk(et))
            return Load(*pushed, c->line);
        return V {};
    }

    // A reusable pool's array, its freelist's header and the freelist's stack.
    void PoolAt(VarDef *vd, Line l, P &pl, tyg_addr &fl, tyg_val &flstk) {
        bool local = vd && prefs.count(vd), pref = !local && vd && cg.PrefVar(vd);
        if (!local && !pref && (!vd || !(poolfl.count(vd) || (vd->isglobal && vd->reusable))))
            No(l, "pool of this kind");
        pl = VarPlace(vd, l);
        auto at = pl.a;
        if ((local || pref) && IsAddrRef(pl.t)) pl = Deref(pl);
        if (local) {
            fl = AtV(LdL(prefs[vd], 16));
            flstk = LdL(prefs[vd], 24);
        } else if (pref) {
            // gs_pref { hdr, stk, fl, flstk } where it lies: a local's, a global's, or a free
            // variable's.
            at.offset += 16;
            fl = AtV(Ld(at));
            at.offset += 8;
            flstk = Ld(at);
        } else if (vd->isglobal) {
            auto [h, st] = M.PoolAccessors(vd);
            fl = AtV(LdL(AccessorLocal(h)));
            flstk = LdL(AccessorLocal(st));
        } else {
            fl = AtL(poolfl[vd].first);
            flstk = LdL(poolfl[vd].second);
        }
    }
    // The pool a call's receiver names: its array, the freelist's header (length at +8) and stack.
    struct PoolRef {
        P pl;
        tyg_addr fl, fllen, len;
        tyg_val flstk;
    };
    PoolRef PoolOfCall(Call *c) {
        auto recv = c->ArgNodes()[0];
        if (auto u = Is<Unary>(recv); u && u->op == T_BITAND) recv = u->child;
        auto id = Is<Ident>(recv);
        PoolRef r;
        PoolAt(id ? id->vdef : nullptr, c->line, r.pl, r.fl, r.flstk);
        r.fllen = r.fl;
        r.fllen.offset += 8;
        r.len = r.pl.a;
        r.len.offset += 8;
        return r;
    }
    // A slice pool (§5.4; CodeGen::EmitSlicePool): runs of elements of a reusable array, its freelist
    // of (index, count) spans kept by the runtime's gs_spans_*.
    V GenSlicePool(Call *c) {
        auto an = c->ArgNodes();
        auto R = PoolOfCall(c);
        auto &pl = R.pl;
        auto &len = R.len;
        if (!IsGrow(pl.t) || !M.RzOk(pl.t) || !pl.stk) No(c->line, "slice pool of this kind");
        auto elem = pl.t->arr->sub;
        auto esz = cg.FixedSize(elem);
        auto pt = tyg_type_ptr(m);
        // gs_spans_<fn>(fl, &fllen, flstk, args...) of i64 arguments.
        auto spans = [&](const string &fn, vector<tyg_val> args, bool ret) {
            vector<const tyg_type *> ts(args.size(), IT(TYG_I64));
            ts.insert(ts.begin(), { pt, pt, pt });
            args.insert(args.begin(), { Ld(R.fl), AddrOf(R.fllen), R.flstk });
            return CallC("gs_spans_" + fn, ts, args, ret ? IT(TYG_I64) : nullptr);
        };
        auto elems = [&] { return Ld(pl.a); };
        // A slice's index in the pool: 0 for an empty one; else, where the checker cannot root it here,
        // checked to start a whole number of elements in and to end inside the length.
        auto indexof = [&](tyg_val data, tyg_val n) {
            auto il = Local(I64(0));
            If(Cmp(TYG_NE, n, I64(0)), [&] {
                auto off = Sub(data, elems());
                if (c->poolcheck && esz) {
                    auto k = Bin(TYG_UDIV, off, I64(esz));
                    auto bad = Bin(TYG_OR,
                                       Cmp(TYG_NE, Bin(TYG_UREM, off, I64(esz)), I64(0)),
                                       Bin(TYG_OR, Cmp(TYG_UGE, k, Ld(len)),
                                               Cmp(TYG_UGT, n, Sub(Ld(len), k))));
                    AbortUnless(Cmp(TYG_EQ, bad, tyg_iconst(p, TYG_I8, 0)), "POOLSLICE", c->line);
                }
                StL(il, Bin(TYG_SDIV, off, I64(std::max<int64_t>(esz, 1))));
            });
            return il;
        };
        auto slicelen = [&](Node *n) {
            auto nl = Local(Index64(n));
            auto max = esz ? Cmp(TYG_ULE, LdL(nl), I64((int64_t)((2048ull << 20) / (uint64_t)esz)))
                           : Cmp(TYG_SGE, LdL(nl), I64(0));
            AbortUnless(max, "SLICELEN", c->line);
            return nl;
        };
        auto slice = [&](Node *n) {
            auto x = GenAs(n, M.ast.SliceOf(elem, n->line));
            auto a8 = x.a;
            a8.offset += 8;
            auto data = Local(Ld(x.a));
            auto cnt = Local(Ld(a8));
            return std::make_pair(data, cnt);
        };
        if (c->builtin == B_FREE_SLICE) {
            auto dn = slice(an[1]);
            if (Ended()) return V {};
            auto il = indexof(LdL(dn.first), LdL(dn.second));
            spans("free", { LdL(il), LdL(dn.second) }, false);
            return V {};
        }
        // Where a run of n goes: past the end while the freelist is empty.
        auto place = [&](tyg_val n) {
            auto r = Local(Ld(len));
            If(Cmp(TYG_NE, Ld(R.fllen), I64(0)), [&] { StL(r, spans("alloc", { Ld(len), n }, true)); });
            return r;
        };
        auto extend = [&](tyg_val end) {
            auto ext = Local(Sub(end, Ld(len)));
            If(Cmp(TYG_SGT, LdL(ext), I64(0)), [&] {
                St(len, Add(Ld(len), LdL(ext)));
                SetTop(pl.stk, Add(Top(pl.stk), Mul(LdL(ext), I64(esz))));
            });
        };
        // Default values for count elements from first (EmitDefaultElems).
        auto defaults = [&](tyg_val first, tyg_val count) {
            if (!esz) return;
            if (!cg.HasFieldDefaults(elem)) {
                CallC("memset", { pt, IT(TYG_I32), IT(TYG_I64, false) },
                      { Add(elems(), Mul(first, I64(esz))), I32(0), Mul(count, I64(esz)) }, pt);
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
            auto dn = slice(an[1]);
            auto ol = dn.second;
            nl = slicelen(an[2]);
            if (Ended()) return Dummy(c->exprtype);
            il = indexof(LdL(dn.first), LdL(ol));
            IfElse(Cmp(TYG_SLE, LdL(nl), LdL(ol)),
                   [&] { spans("free", { Add(LdL(il), LdL(nl)), Sub(LdL(ol), LdL(nl)) }, false); },
                   [&] {
                       IfElse(Cmp(TYG_EQ, LdL(ol), I64(0)), [&] { StL(il, LdL(place(LdL(nl)))); }, [&] {
                           If(Cmp(TYG_NE, Add(LdL(il), LdL(ol)), Ld(len)), [&] {
                               auto ni = Local(spans("regrow", { Ld(len), LdL(il), LdL(ol), LdL(nl) },
                                                     true));
                               If(Cmp(TYG_NE, LdL(ni), LdL(il)), [&] {
                                   tyg_move_dyn(p, Add(elems(), Mul(LdL(ni), I64(esz))),
                                                Add(elems(), Mul(LdL(il), I64(esz))),
                                                    Mul(LdL(ol), I64(esz)));
                               });
                               StL(il, LdL(ni));
                           });
                       });
                       extend(Add(LdL(il), LdL(nl)));
                       defaults(Add(LdL(il), LdL(ol)), Sub(LdL(nl), LdL(ol)));
                   });
        }
        return PairV(M.ast.SliceOf(elem, c->line), Add(elems(), Mul(LdL(il), I64(esz))), LdL(nl));
    }

    // pool.alloc_index(x), alloc_ref(x) and free(i) (CodeGen::EmitAlloc, B_FREE): a slot from the
    // freelist, or a new one at the top that joins the array once its element is written.
    V GenPool(Call *c) {
        auto an = c->ArgNodes();
        auto R = PoolOfCall(c);
        auto &pl = R.pl;
        auto &len = R.len;
        auto &fllen = R.fllen;
        auto elem = pl.t->arr->sub;
        auto esz = cg.FixedSize(elem);
        if (c->builtin == B_FREE) {
            auto i = Index64(an[1]);
            if (Ended()) return V {};
            BoundsCheck(i, Ld(len), c->line);
            auto top = Top(R.flstk);
            St(AtV(top), i);
            SetTop(R.flstk, Add(top, I64(8)));
            St(fllen, Add(Ld(fllen), I64(1)));
            return V {};
        }
        // The value first, then the slot (the array cannot grow into a fresh one meanwhile).
        bool atslot;
        V x;
        if (!PushOperand(an[1], elem, c->line, atslot, x)) return Dummy(c->exprtype);
        auto il = Local(I64(0));
        IfElse(Cmp(TYG_SGT, Ld(fllen), I64(0)), [&] {
            St(fllen, Sub(Ld(fllen), I64(1)));
            auto ftop = Sub(Top(R.flstk), I64(8));
            SetTop(R.flstk, ftop);
            StL(il, Ld(AtV(ftop)));
        }, [&] { StL(il, Ld(len)); });
        auto slot = [&] { return Add(Ld(pl.a), Mul(LdL(il), I64(esz))); };
        StoreOperand(slot(), elem, atslot, an[1], x);
        If(Cmp(TYG_EQ, LdL(il), Ld(len)), [&] {
            St(len, Add(Ld(len), I64(1)));
            SetTop(pl.stk, Add(Top(pl.stk), I64(esz)));
        });
        if (c->builtin == B_ALLOC_INDEX)
            return ValV(c->exprtype, Conv(LdL(il), M.ast.inttypes[IS_I64], c->exprtype, c->line));
        auto r = slot();
        if (c->exprtype && c->exprtype->kind != TY_REF && !M.IsScalar(c->exprtype) && M.TypeOk(elem))
            return Load(P { AtV(r), elem }, c->line);
        return ValV(c->exprtype, r);
    }

    // A literal of relative-reference elements of type et, whose run is built where it lies (§4.3).
    ArrayLit *RelRunLit(Node *src, TypeExpr *et) {
        auto al = Is<ArrayLit>(src);
        if (!al || al->capexpr || !cg.HasRelRef(et) || !al->exprtype ||
            al->exprtype->kind != TY_ARRAY ||
            !cg.TEq(al->exprtype->arr->sub, et) || (al->fillval && !Is<IntLit>(al->fillcount)))
            return nullptr;
        return al;
    }
    // v.append(x) (EmitAppend): the source's elements (a string literal's bytes, or an array's, a
    // slice's, a literal's, evaluated whole) are copied after the length, capacity checked.
    V LimAppend(Call *c, const P &pl) {
        if (!M.LimHeld(pl.t)) No(c->line, cat("append to ", M.NameWhy(pl.t)));
        auto et = pl.t->arr->sub;
        auto src = StripCopy(c->ArgNodes()[1]);
        View v;
        if (auto al = RelRunLit(src, et)) {
            // Elements holding relative references are built in the free slots, once checked.
            auto k = LitCount(al);
            auto ol = Local(LimLen(pl));
            AbortUnless(Cmp(TYG_SLE, Add(LdL(ol), I64(k)), LimCapOf(pl)), "CAPACITY", c->line);
            auto leaves = FillCapture(al);
            if (Ended()) return V {};
            auto base = Local(LimElemAddr(pl, LdL(ol)));
            LitRunAt(al, et, base, k, leaves);
            if (Ended()) return V {};
            LimSetLen(pl, Add(LdL(ol), I64(k)));
            return V {};
        }
        if (auto s = Is<StrLit>(src)) {
            if (!IsU8(et)) No(c->line, "string of this type");
            v = { BytesAddr(s->val), I64((int64_t)s->val.size()), et, -1 };
        } else {
            if (Is<Call>(src) && src->exprtype && (IsGrow(src->exprtype) || IsVarArr(src->exprtype)))
                No(c->line, "append of this call");
            auto from = Place(src);
            if (Ended()) return V {};
            v = ViewOf(from, c->line);
        }
        LimAppendView(pl, v, c->line);
        return V {};
    }
    // pop, pop_n, resize and clear (codegen_builtins.h): only the length moves.
    V LimShrink(Call *c, const P &pl) {
        if (!M.LimHeld(pl.t)) No(c->line, cat(BuiltinByKind(c->builtin).name, " of ", M.NameWhy(pl.t)));
        auto an = c->ArgNodes();
        auto et = pl.t->arr->sub;
        switch (c->builtin) {
            case B_POP: {
                auto nl = Sub(LimLen(pl), I64(1));
                AbortUnless(Cmp(TYG_SGE, nl, I64(0)), "POP", c->line);
                LimSetLen(pl, nl);
                return Load(P { AtV(LimElemAddr(pl, LimLen(pl))), et }, c->line);
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
        auto nl = Local(nn), ol = Local(LimLen(pl));
        auto n = [&] { return LdL(nl); };
        auto o = [&] { return LdL(ol); };
        AbortUnless(Cmp(TYG_SGE, n(), I64(0)), "RESIZENEG", c->line);
        IfElse(Cmp(TYG_SLT, n(), o()), [&] { LimSetLen(pl, n()); }, [&] {
            If(Cmp(TYG_SGT, n(), o()), [&] {
                if (an.size() <= 2) {
                    Abort("RESIZEFILL", c->line);
                    return;
                }
                AbortUnless(Cmp(TYG_SLE, n(), LimCapOf(pl)), "CAPACITY", c->line);
                Loop(o(), n(), [&](tyg_val i) { Store(P { AtV(LimElemAddr(pl, i)), et }, fv); });
                LimSetLen(pl, n());
            });
        });
        return V {};
    }

    // ---- Calls ----------------------------------------------------------------------------------

    static tyg_arg ArgOfVal(tyg_val v) { return tyg_arg { v, {} }; }
    static tyg_arg ArgOfMem(tyg_addr m) { return tyg_arg { 0, m }; }
    static tyg_addr AddrPlus(tyg_addr a, int64_t off) {
        a.offset += off;
        return a;
    }
    // The count of the resizable whose header is at hdr.
    static tyg_addr CountAt(tyg_addr hdr) { return AddrPlus(hdr, 8); }
    // x stored to a temporary of its own.
    P Spill(const V &x) {
        P pl { AtL(Temp(x.t)), x.t };
        Store(pl, x);
        return pl;
    }
    // A 16-byte local holding two words.
    tyg_local PairLocal(tyg_val a, tyg_val b) {
        auto l = Slot16();
        StL(l, a);
        StL(l, b, 8);
        return l;
    }
    // Continues in a new block if c holds; else goes to `no`.
    void ContinueIf(tyg_val c, tyg_block no) {
        auto yes = NewBlock();
        Branch(c, yes, no);
        SetBlock(yes);
    }

    tyg_arg Arg(Node *n, TypeExpr *pt) {
        if (!M.TypeOk(pt)) No(n->line, "argument of this type");
        tyg_arg a {};
        auto x = GenAs(n, pt);
        if (TgModule::IsRel(pt)) {
            // A relative parameter takes the offset (CodeGen::EmitArg).
            if (Ended()) return a;
            auto slot = Temp(pt);
            Store(P { AtL(slot), pt }, x);
            return ArgOfVal(tyg_load(p, M.RelScal(pt), AtL(slot), 0));
        }
        return M.IsScalar(pt) ? ArgOfVal(x.v) : ArgOfMem(x.a);
    }

    // A large fixed value by value: the address of a copy of its own (CodeGen::SigParams).
    tyg_arg LargeArg(Node *n, TypeExpr *pt) {
        if (!M.TypeOk(pt)) No(n->line, "argument of this type");
        auto x = GenAs(n, pt);
        if (Ended()) return tyg_arg {};
        auto cp = Temp(pt);
        tyg_copy(p, AtL(cp), x.a, M.Size(pt));
        return ArgOfVal(AddrOf(AtL(cp)));
    }

    // Every result of the latest call (GenCall): the first as the call's value, the others where the
    // callee wrote them through out-pointers.
    vector<V> lastrets;

    // The call, and its C return value: rets[cret], none where cret < 0.
    V Result(FnSpec *callee, int cret, tyg_sig *sig, tyg_callee ce, vector<tyg_arg> &args) {
        auto n = (int)args.size();
        auto rt = cret < 0 ? nullptr : callee->rets[cret];
        if (rt && !M.IsScalar(rt)) {
            auto ra = AtL(Temp(rt));
            tyg_call(p, sig, ce, args.data(), n, nullptr, &ra);
            return MemV(rt, ra);
        }
        auto v = tyg_call(p, sig, ce, args.data(), n, nullptr, nullptr);
        return rt ? ValV(rt, v) : V {};
    }

    // default<T>() of zero bytes: a scalar's, or a fixed array's of such elements.
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
        auto b = c->builtin;
        auto an = c->ArgNodes();
        auto nargs = an.size();
        auto line = c->line;
        if (b == B_PRINT) return GenPrint(c);
        if (b == B_LEN && nargs == 1) return GenLen(an[0], c);
        if (b == B_PUSH && nargs == 2) return GenPush(c);
        if (b == B_APPEND && nargs == 2) return GenAppend(c);
        if (b == B_STR) return GenStr(c);
        if (b == B_POP || b == B_POP_N || b == B_RESIZE || b == B_CLEAR) return GenShrink(c);
        if (b == B_ASSERT && nargs == 1) {
            AbortUnless(Truth(an[0]), "ASSERT", line);
            return V {};
        }
        if (b == B_ABORT && nargs == 1) {
            auto v = ViewOf(Place(an[0]), line);
            if (Ended()) return V {};
            CallC("gs_abort_msg", { tyg_type_ptr(m), IT(TYG_I64), tyg_type_ptr(m), IT(TYG_I32) },
                  { v.elems, v.len, FileArg(line), LineArg(line) }, nullptr);
            return V {};
        }
        if (b == B_EXIT && nargs == 1) {
            auto x = GenAs(an[0], M.ast.inttypes[IS_I64]);
            if (Ended()) return V {};
            CallC("gs_exit", { IT(TYG_I64) }, { x.v }, nullptr);
            return V {};
        }
        if (b == B_FORMAT && nargs >= 1) return GenFormat(c);
        if (b == B_INDEX_OF && nargs == 2) {
            // The checker proved the reference is an element of this very array (§3.3): the distance in
            // whole elements, unchecked.
            auto v = ViewOf(Place(an[0]), line);
            if (!cg.IsFix(v.elem)) No(line, "index_of of elements of variable size");
            auto r = Gen(an[1]);
            if (Ended()) return Dummy(c->exprtype);
            auto esz = I64(std::max<int64_t>(cg.FixedSize(v.elem), 1));
            auto ix = Bin(TYG_SDIV, Sub(r.v, v.elems), esz);
            return ValV(c->exprtype, Conv(ix, M.ast.inttypes[IS_I64], c->exprtype, line));
        }
        if ((b == B_ALLOC_INDEX || b == B_ALLOC_REF || b == B_FREE) && nargs == 2) return GenPool(c);
        if (b == B_ALLOC_SLICE || b == B_REALLOC_SLICE || b == B_FREE_SLICE) return GenSlicePool(c);
        if (b == B_COPY && nargs == 1 && (!cg.IsFix(c->rettypes[0]) || M.TypeOk(c->rettypes[0]))) {
            // A copy on a temporary's stack for a variable-size value (an image, a resizable, a frame
            // object); at a slice destination, a view of the copy, not of its source.
            auto rt = c->rettypes[0];
            V x;
            if (cg.IsFix(rt)) {
                x = GenAs(an[0], rt);
                if (Ended()) return Dummy(c->exprtype);
                if (M.IsScalar(rt)) return x;
                auto tmp = Temp(rt);
                tyg_copy(p, AtL(tmp), x.a, M.Size(rt));
                x = MemV(rt, AtL(tmp));
            } else if (TempOk(rt)) {
                x = BuildTemp(an[0], rt, line);
            } else {
                No(line, cat("copy of type ", M.NameWhy(rt)));
            }
            if (Ended()) return Dummy(c->exprtype);
            if (c->exprtype && c->exprtype->kind == TY_SLICE) {
                auto v = ViewOf(P { x.a, x.t, x.stk }, line);
                return PairV(c->exprtype, v.elems, v.len);
            }
            return x;
        }
        if (b == B_TO_BYTES && nargs == 2) {   // Appended to a u8 resizable.
            auto out = Deref(Place(an[1]));
            if (TgModule::IsLim(out.t) && M.LimHeld(out.t) && IsU8(out.t->arr->sub)) {
                ToBytesIntoLim(out, an[0], line);
                return V {};
            }
            if (!IsGrow(out.t) || !IsU8(out.t->arr->sub) || !out.stk)
                No(line, "to_bytes into this kind");
            ToBytesInto(out.stk, CountAt(out.a), an[0], line);
            return V {};
        }
        if (b == B_FROM_BYTES) {   // Its array alone, where the flag is not wanted.
            lastrets = GenFromBytes(c);
            return lastrets[0];
        }
        if (b == B_TO_BYTES && nargs == 1) return BuildTemp(c, cg.GrowU8(), line);
        if (b == B_BYTES_OF && nargs == 1) {
            // The element region of an array or slice as a u8[:] (CodeGen::PayloadOf).
            auto rt = an[0]->exprtype;
            while (rt && IsAddrRef(rt)) rt = rt->ref->sub;
            if (!rt || (rt->kind != TY_ARRAY && rt->kind != TY_SLICE))
                No(line, "bytes_of of this type");
            auto v = ViewOf(Place(an[0]), line);
            if (Ended()) return Dummy(c->exprtype);
            return PairV(c->exprtype, v.elems, RunBytes(v));
        }
        auto dt = b == B_DEFAULT && c->rettypes.size() == 1 ? c->rettypes[0] : nullptr;
        if (dt && cg.IsFix(dt) && M.TypeOk(dt)) {
            // A fixed value from its type's defaults (CodeGen's B_DEFAULT): zero bytes where they all
            // are (EmitDefaultInto), else the default initializer, per element for an array.
            auto zero = ZeroDefault(c);
            if (zero && M.IsScalar(dt)) return Dummy(dt);
            P pl { AtL(Temp(dt)), dt };
            tyg_zero(p, pl.a, M.Size(dt));
            if (zero) return MemV(dt, pl.a);
            if (c->defaultinit && dt->kind == TY_ARRAY && dt->arr->akind == A_FIXED) {
                auto et = dt->arr->sub;
                auto esz = I64(cg.FixedSize(et));
                Loop(I64(0), I64(cg.ArrSize(dt->arr)), [&](tyg_val i) {
                    Into(P { AtV(Add(AddrOf(pl.a), Mul(i, esz))), et }, c->defaultinit);
                });
            } else if (c->defaultinit) {
                Into(pl, c->defaultinit);
            }
            if (Ended()) return Dummy(dt);
            return M.IsScalar(dt) ? Load(pl, line) : MemV(dt, pl.a);
        }
        if (b == B_HARDWARE_THREADS)
            return ValV(c->exprtype, CallC("gs_hardware_threads", {}, {}, IT(TYG_I64)));
        if ((b == B_EMBED_FILE || b == B_EMBED_SHADER) && c->shaderblob && c->rettypes.size() == 1) {
            // A read-only view of static data (CodeGen's B_EMBED_FILE): the data and length.
            auto &blob = *c->shaderblob;
            auto t = Temp(c->rettypes[0]);
            StL(t, BytesAddr(blob));
            StL(t, I64((int64_t)blob.size()), 8);
            return MemV(c->rettypes[0], AtL(t));
        }
        if (b == B_THREAD_WAIT) return GenThreadWait(c);
        if (b == B_THREAD_SPAWN) return GenThreadSpawn(c);
        if (b == B_QPUT) return GenQput(c);
        if (b == B_QGET || b == B_QPOLL) return GenQget(c, idst);
        if (b >= 0) No(line, cat("builtin ", BuiltinByKind(b).name));
        if (c->fvbody) return GenFvCall(c);
        if (!c->dispatch.empty()) return GenDispatch(c, idst, dst);
        if (!c->spec) No(line, "call kind");
        auto callee = c->spec;
        if (nargs < callee->argtypes.size()) No(line, "argument count");
        an.resize(callee->argtypes.size());
        if (callee->sf->isextern) return GenExternCall(c, callee, an);
        string why;
        if (!M.SigOk(callee, false, why)) No(line, cat("call to ", callee->sf->name, ": ", why));
        auto &ci = cg.sinfo[callee];
        vector<tyg_arg> args;
        for (size_t i = 0; i < an.size(); i++) {
            auto pt = callee->argtypes[i];
            if (cg.IsPoolParam(callee, i)) PrefArg(an[i], args);
            else if (M.RzParam(pt)) ValArg(an[i], pt, args);
            else args.push_back(cg.IsLargeFixed(pt) ? LargeArg(an[i], pt) : Arg(an[i], pt));
        }
        // The callee's free variables (CodeGen::EmitFvArg), by address.
        FvArgs(c, ci, args);
        lastrets.clear();
        if (Ended()) {
            for (auto r : callee->rets) lastrets.push_back(Dummy(r));
            return Dummy(callee->rets.empty() ? nullptr : callee->rets[0]);
        }
        auto rets = RetChans(callee, ci, idst, dst, args, line);
        if (ci.needssp) args.push_back(ArgOfVal(CalleeSp(callee, line)));
        auto sym = M.Sym(ci.cname, TYG_SYM_FUNC | TYG_SYM_GLOBAL);
        auto x = Result(callee, ci.cret, M.Sig(callee), tyg_direct(sym), args);
        RfCheck(callee);
        for (size_t i = 0; i < rets.size(); i++) {
            if ((int)i == ci.cret) lastrets.push_back(x);
            else if (M.IsScalar(rets[i].t)) lastrets.push_back(Load(P { rets[i].a, rets[i].t }, line));
            else lastrets.push_back(rets[i]);
        }
        return lastrets.empty() ? V {} : lastrets[0];
    }

    // A channel per result after the declared arguments (C.3), appended to args: an image's stack (idst
    // for the first), a resizable's stack (dst for the first) and count, a frame object's stack and
    // struct, an out-pointer for a fixed result the C does not return. Where each is.
    vector<V> RetChans(FnSpec *callee, CodeGen::SpecInfo &ci, tyg_val idst, const RzDst *dst,
                       vector<tyg_arg> &args, Line l) {
        vector<V> rets(callee->rets.size());
        for (size_t i = 0; i < callee->rets.size(); i++) {
            auto &r = rets[i];
            r.t = callee->rets[i];
            if (IsImg(r.t)) {   // Built whole at the top of the stack.
                auto stk = i == 0 && idst ? idst : Stk(AllocStk(false, l), l);
                r.mem = true;
                r.a = AtV(Top(stk));
                args.push_back(ArgOfVal(stk));
            } else if (M.FoOk(r.t)) {
                r = FoTemp(r.t, l);
                args.push_back(ArgOfVal(r.stk));
                args.push_back(ArgOfVal(AddrOf(r.a)));
            } else if (IsGrow(r.t) || M.IsRs(r.t) || M.IsRb(r.t)) {
                if (i == 0 && dst) r = MemV(r.t, dst->hdr, dst->stk);
                else r = RzTemp(r.t, l);
                auto len = CountAt(r.a);
                St(r.a, Top(r.stk));
                St(len, I64(0));
                args.push_back(ArgOfVal(r.stk));
                args.push_back(ArgOfVal(AddrOf(len)));
            } else if ((int)i != ci.cret) {
                r.a = AtL(Temp(r.t));
                args.push_back(ArgOfVal(AddrOf(r.a)));
                if (!M.IsScalar(r.t)) r.mem = true;
            }
        }
        return rets;
    }

    // A pool reference argument (CodeGen::GenPrefVal): gs_pref's four members.
    void PrefArg(Node *n, vector<tyg_arg> &args) {
        auto at = GenPref(n);
        for (int k = 0; k < 4; k++)
            args.push_back(ArgOfVal(Ended() ? I64(0) : Ld(AddrPlus(at, 8 * k))));
    }
    // gs_pref's members for a pool variable: header, its stack, free list, its stack.
    vector<tyg_val> PoolRefParts(const VarDef *vd, Line l) {
        P pl;
        tyg_addr fl;
        tyg_val flstk;
        PoolAt(const_cast<VarDef *>(vd), l, pl, fl, flstk);
        if (!pl.stk) No(l, "pool without its stack");
        return { AddrOf(pl.a), pl.stk, AddrOf(fl), flstk };
    }

    // Whether a variable holds a gs_pref: a pool reference variable, or a parameter handed a pool.
    bool IsPrefStore(const VarDef *vd) { return cg.PrefVar(vd) || (prefs.count(vd) && !vd->reusable); }
    // The pool reference a node gives (CodeGen::GenPrefVal), the address of its 32 bytes: a pool's,
    // another pool reference variable's, or what a construct choosing among pools builds, each arm into
    // one temporary (prefctl and prefloc hand it to the construct).
    bool prefctl = false;
    tyg_local prefloc = 0;
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
        auto parts = PoolRefParts(vd, n->line);
        auto res = tyg_local_new(p, 32, 8);
        for (int k = 0; k < 4; k++) StL(res, parts[k], 8 * k);
        return AtL(res);
    }
    // A pool reference built into the 32 bytes at dst.
    void PrefInto(tyg_addr dst, Node *n) {
        auto src = GenPref(n);
        if (!Ended()) tyg_copy(p, dst, src, 32);
    }

    // A by-value frame object, image or resizable argument: a copy of its own on a stack of the
    // statement's, as the C passes it (SigParams).
    void ValArg(Node *n, TypeExpr *pt, vector<tyg_arg> &args) {
        auto x = BuildTemp(n, pt, n->line);
        // A T[]: by the image's address.
        if (!x.stk) return args.push_back(ArgOfVal(AddrOf(x.a)));
        args.push_back(ArgOfMem(x.a));
        args.push_back(ArgOfVal(x.stk));
    }

    void FvArgs(Call *c, CodeGen::SpecInfo &ci, vector<tyg_arg> &args) {
        for (auto fv : ci.freevars) {
            auto v = c->FreeVarArg(fv);
            if (fv->reusable) {   // A pool, as gs_pref's members (CodeGen::SigParams).
                for (auto x : PoolRefParts(v, c->line)) args.push_back(ArgOfVal(x));
                continue;
            }
            auto pl = VarPlace(v, c->line);
            args.push_back(ArgOfVal(AddrOf(pl.a)));
            if (M.Resz(v->type)) {
                if (!pl.stk) No(c->line, "free variable without its stack");
                args.push_back(ArgOfVal(pl.stk));
            }
        }
    }

    // A call of a function value (§7.6; CodeGen::EmitFvCall): its body spliced in here in a scope of
    // its own, the parameters locals bound to the arguments. Its value is a copy (§4.1), so a slice of
    // it never points into a variable the body names.
    V GenFvCall(Call *c) {
        auto et = c->fvbody->exprtype;
        auto wantsval = !cg.IsVoidT(et);
        if (wantsval && !M.TypeOk(et)) {   // An image or a resizable: built on a temporary's stack.
            V x;
            if (InPlaceValue(c, et, c->line, x)) return x;
            No(c->line, cat("function value of type ", M.Name(et)));
        }
        if (c->fvparams.size() > c->args.size()) No(c->line, "function value parameters");
        if (Ended()) return Dummy(et);
        V x;
        InScope([&] {
            BindFvParams(c);
            x = GenBlock(c->fvbody, !wantsval);
            if (wantsval && x.mem && !Ended()) x.a = Spill(x).a;
        });
        if (!wantsval) return V {};
        return Ended() ? Dummy(et) : x;
    }

    // The element the last push added: what print renders of a push's value (a reference, §3.3).
    std::optional<P> pushed;
    bool discardpush = false;   // A push in statement position: its value unwanted.
    // v.push(x) (EmitPush): the receiver, then the element, which may grow the array itself, then its
    // slot claimed at the stack's top and the count raised. Its value is the element's address.
    V GenPush(Call *c) {
        auto dp = discardpush;
        discardpush = false;
        auto an = c->ArgNodes();
        auto pl = Deref(Place(an[0]));
        if (TgModule::IsLim(pl.t)) {
            discardpush = dp;
            return LimPush(c, pl);
        }
        auto la = CountAt(pl.a);
        // The value of a push whose element lies at `at`: its reference, or a varint decoded.
        auto counted = [&](tyg_val at) -> V {
            auto et = c->exprtype;
            if (et && et->kind != TY_REF && !cg.IsVoidT(et) && M.IsScalar(et))
                return Fit(Load(ElemPlace(at, pl.t->arr->sub), c->line), et, c->line);
            V r;
            r.t = et && et->kind == TY_REF ? et : nullptr;
            if (r.t) r.v = at;
            return r;
        };
        if (IsGrow(pl.t) && M.VeOk(pl.t) && pl.stk) {
            // An image element: built at the top of the array's own stack, then counted.
            auto at = GenImg(an[1], pl.stk, pl.t->arr->sub, c->line);
            if (Ended()) return Dummy(c->exprtype);
            St(la, Add(Ld(la), I64(1)));
            return counted(at);
        }
        if (!IsGrow(pl.t) || !M.RzOk(pl.t) || !pl.stk) No(c->line, "push to an array of this kind");
        auto et = pl.t->arr->sub;
        auto selfrel = SelfRelLit(an[1]) || (cg.HasRelRef(et) && SelfRelLitOk(an[1]));
        if (selfrel && cg.TEq(an[1]->exprtype, et)) {
            // Built in its slot at the top, then counted.
            auto slot = Top(pl.stk);
            auto sv = Local(slot);
            SelfRelInto(P { AtV(slot), et }, an[1]);
            if (Ended()) return Dummy(c->exprtype);
            auto at = LdL(sv);
            SetTop(pl.stk, Add(at, I64(cg.FixedSize(et))));
            St(la, Add(Ld(la), I64(1)));
            return counted(at);
        }
        auto x = GenAs(an[1], et);
        if (Ended()) return Dummy(c->exprtype);
        if (!M.IsScalar(et) && !x.mem) No(c->line, "aggregate value");
        auto top = Top(pl.stk);
        if (!cg.IsFix(et)) No(Line {}, "an element of variable size");
        PushVal(pl.stk, et, x);
        St(la, Add(Ld(la), I64(1)));
        V r;
        r.t = c->exprtype && c->exprtype->kind == TY_REF ? c->exprtype : nullptr;
        if (r.t) r.v = top;
        // The element itself, as a value, where one is wanted.
        pushed = P { AtV(top), et };
        bool wanted = !r.t && !dp && c->exprtype && c->exprtype->kind != TY_VOID && M.TypeOk(et);
        return wanted ? Load(*pushed, c->line) : r;
    }

    // v.append(x) (EmitAppend): x's elements built at v's top, their count added.
    V GenAppend(Call *c) {
        auto an = c->ArgNodes();
        auto pl = Deref(Place(an[0]));
        if (TgModule::IsLim(pl.t)) return LimAppend(c, pl);
        if (!IsGrow(pl.t) || !(M.RzOk(pl.t) || M.VeOk(pl.t)) || !pl.stk)
            No(c->line, "append to an array of this kind");
        AppendSrc(pl, an[1], c->line);
        return V {};
    }
    // The elements of src added to the resizable at pl: built at its top behind a header of their own
    // (GenRz).
    void AppendSrc(const P &pl, Node *src, Line l) {
        auto et = pl.t->arr->sub;
        auto len = CountAt(pl.a);
        if (auto ie = Is<IfExpr>(src); ie && ie->elseb) {
            BuildIf(ie, l, [&](Node *v) { AppendSrc(pl, v, l); });
            return;
        }
        src = StripCopy(src);
        if (auto al = RelRunLit(src, et)) {   // Its run is built at the receiver's top.
            auto k = LitCount(al);
            auto base = Slot();
            auto leaves = FillCapture(al);
            if (Ended()) return;
            StL(base, Top(pl.stk));
            LitRunAt(al, et, base, k, leaves);
            if (Ended()) return;
            SetTop(pl.stk, Add(LdL(base), I64(k * cg.FixedSize(et))));
            St(len, Add(Ld(len), I64(k)));
            return;
        }
        auto th = NewHdr();
        GenRz(src, pl.stk, th, pl.t, l);
        if (!Ended()) St(len, Add(Ld(len), Ld(CountAt(th))));
    }
    static bool IsGrow(TypeExpr *t) { return TgModule::IsGrow(t); }

    // Text of print, str and format (EmitStr, EmitFormatInto): each argument written in turn at the top
    // of a u8 resizable's stack, its count at len: a scalar by the runtime's gs_fmt_*, a u8 array or
    // slice copied, anything else rendered structurally (RenderInto). The call whose user format
    // overloads the value being rendered use (CodeGen::FmtContext).
    Call *fmtc = nullptr;
    TypeExpr *inlinet = nullptr;   // The type a render procedure's body renders inline.

    // A type's name as text renders it: the value's own, without a `const` of its place.
    static string TextName(TypeExpr *t) {
        auto s = TgModule::Name(t);
        if (t->cq && s.rfind("const ", 0) == 0) s = s.substr(6);
        return s;
    }

    // Literal bytes of element type et appended as CopyRun appends.
    void CopyLit(tyg_val stk, tyg_addr len, TypeExpr *et, const string &text) {
        CopyRun(stk, len, et, BytesAddr(text), I64((int64_t)text.size()));
    }
    void TextLit(tyg_val stk, tyg_addr len, const string &s) {
        CopyLit(stk, len, M.ast.inttypes[IS_U8], s);
    }

    // A user `format` overload rendering the value at pl (EmitUserFormat): handed the builder as a
    // gs_rref (its header and stack) and the value as the overload takes it.
    void UserFormat(tyg_val stk, tyg_addr len, P pl, FnSpec *sp, Line l) {
        string why;
        if (!M.SigOk(sp, false, why)) No(l, cat("format overload: ", why));
        auto &ci = cg.sinfo[sp];
        if (!ci.freevars.empty()) No(l, "format overload with free variables");
        auto pt = sp->argtypes[1];
        auto r = PairLocal(AddrOf(AddrPlus(len, -8)), stk);
        vector<tyg_arg> args { ArgOfMem(AtL(r)) };
        if (pl.enc || TgModule::IsVarint(pl.t)) {
            // A varint holds no i64: the overload is handed a temporary holding its value (RefArg).
            auto i64t = M.ast.inttypes[IS_I64];
            auto tmp = Temp(i64t);
            StL(tmp, Load(pl, l).v);
            pl = P { AtL(tmp), i64t };
        }
        tyg_arg v {};
        if (pt->kind == TY_REF && cg.IsFatRef(pt)) {   // A resizable by reference: gs_rref.
            if (!pl.stk) No(l, "format overload taking a resizable without its stack");
            v = ArgOfMem(AtL(PairLocal(AddrOf(pl.a), pl.stk)));
        } else if (pt->kind == TY_REF) {
            v = ArgOfVal(AddrOf(pl.a));
        } else if (M.IsScalar(pt)) {
            v = ArgOfVal(Load(P { pl.a, pt }, l).v);
        } else if (M.RzParam(pt)) {
            No(l, "format overload taking a variable-size value");
        } else if (cg.IsLargeFixed(pt)) {   // The address of a copy of its own (SigParams).
            auto cp = Temp(pt);
            tyg_copy(p, AtL(cp), pl.a, M.Size(pt));
            v = ArgOfVal(AddrOf(AtL(cp)));
        } else {
            v = ArgOfMem(pl.a);
        }
        args.push_back(v);
        if (ci.needssp) args.push_back(ArgOfVal(CalleeSp(sp, l)));
        auto sym = M.Sym(ci.cname, TYG_SYM_FUNC | TYG_SYM_GLOBAL);
        Result(sp, ci.cret, M.Sig(sp), tyg_direct(sym), args);
        RfCheck(sp);
    }

    // Where an argument of print, str or format is rendered from: a call's reference result where it
    // points, not as the value the checker decayed it to; anything else its place.
    P RenderPlace(Node *a) {
        auto fc = Is<Call>(a);
        if (fc && fc->builtin == B_PUSH) {
            pushed.reset();
            auto x = Gen1(a);
            if (Ended()) return P {};
            if (pushed) return *pushed;
            if (x.mem) return P { x.a, x.t, x.stk };
            if (!x.t || x.t->kind == TY_VOID) No(a->line, "place of no value");
            return Spill(x);
        }
        auto rs = !fc ? nullptr : fc->spec ? fc->spec : !fc->dispatch.empty() ? fc->dispatch[0] :
            nullptr;
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
        auto i64t = M.ast.inttypes[IS_I64];
        for (size_t i = from; i < an.size(); i++) {
            auto a = an[i];
            fmtc = cg.FmtContext(c, a);
            auto bt = a->exprtype;
            while (bt && IsPlainRef(bt)) bt = bt->ref->sub;
            if (fmtc && TgModule::IsVarint(bt)) {
                // A varint read out of storage is an i64 (§3.6): an i64 overload gets a temporary.
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
                // A function value's reference result is spliced in as a value, but the overload must
                // get the storage it names.
                if (auto fc = Is<Call>(a); fc && fc->fvbody && a->exprtype &&
                    a->exprtype->kind == TY_REF)
                    No(a->line, "format overload of a function value's reference result");
                auto pl = RenderPlace(a);
                if (Ended()) return;
                if (IsAddrRef(pl.t)) {   // A reference to a varint renders what it refers to.
                    auto d = Deref(pl);
                    if (d.enc) pl = d;
                }
                // The overload found for the argument's checked type, an i64 for a varint place.
                if (pl.enc || TgModule::IsVarint(pl.t))
                    UserFormat(stk, len, pl, cg.FmtSpecFor(fmtc, bt), a->line);
                else RenderInto(stk, len, pl, false, a->line);
                continue;
            }
            if (bt && ((bt->kind == TY_ARRAY && IsU8(bt->arr->sub)) ||
                       (bt->kind == TY_SLICE && IsU8(bt->sub)))) {
                if (auto s = Is<StrLit>(a)) {
                    CopyLit(stk, len, bt->kind == TY_ARRAY ? bt->arr->sub : bt->sub, s->val);
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

    // n bytes written at the top of stk by the runtime, added to the stack's top and the count at len.
    void Counted(tyg_val stk, tyg_addr len, tyg_val n) {
        SetTop(stk, Add(Top(stk), n));
        St(len, Add(Ld(len), n));
    }
    // A scalar x of type t formatted at the top of stk by the runtime's gs_fmt_*.
    void FmtScalar(tyg_val stk, tyg_addr len, TypeExpr *t, tyg_val x) {
        auto ptr = tyg_type_ptr(m);
        auto fmt = [&](const char *fn, auto ty, tyg_val v) {
            return CallC(fn, { ptr, ty }, { Top(stk), v }, IT(TYG_I64));
        };
        tyg_val n;
        if (t->kind == TY_INT && t->intstorage == IS_U64) {
            n = fmt("gs_fmt_u64", IT(TYG_I64, false), x);
        } else if (t->kind == TY_INT) {
            n = fmt("gs_fmt_i64", IT(TYG_I64), Conv(x, t, M.ast.inttypes[IS_I64], Line {}));
        } else if (t->kind == TY_FLT) {
            auto f32 = M.Scal(t) == TYG_F32;
            n = fmt(f32 ? "gs_fmt_f32" : "gs_fmt_f64", tyg_type_scalar(m, f32 ? TYG_F32 : TYG_F64), x);
        } else {
            n = fmt("gs_fmt_bool", IT(TYG_I64), tyg_convert(p, TYG_ZEXT, TYG_I64, x));
        }
        Counted(stk, len, n);
    }

    // `Name { a, b }`: the fields of pl's struct or variant, each where FieldPlace finds it.
    void RenderRun(tyg_val stk, tyg_addr len, string_view name, const P &pl, Line l, int depth) {
        auto &fs = M.RunOfType(pl.t);
        TextLit(stk, len, cat(name, " { "));
        auto first = true;
        for (size_t i = 0; i < fs.f.size(); i++) {
            if (fs.f[i].ispad) continue;
            if (!first) TextLit(stk, len, ", ");
            first = false;
            RenderInto(stk, len, FieldPlace(pl, (int)i, l, true), true, l, depth + 1);
        }
        TextLit(stk, len, " }");
    }

    // The value at pl rendered structurally, as CodeGen::RenderLoc renders it: references through to
    // what they refer to, a scalar by gs_fmt_*, u8 elements as their bytes (quoted when nested), other
    // arrays and slices as `[a, b]`, a struct as `Name { a, b }`.
    void RenderInto(tyg_val stk, tyg_addr len, P pl, bool nested, Line l, int depth = 0) {
        if (depth > 32) No(l, "text of a type this deep");
        // A reference renders what it refers to, a relative one decoded (CodeGen::RenderLoc).
        while (IsPlainRef(pl.t) || (TgModule::IsRel(pl.t) && !IsOptional(pl.t) && M.RelOk(pl.t, 1)))
            pl = Step(pl);
        auto t = pl.t;
        if (fmtc) {
            if (auto sp = cg.FmtSpecFor(fmtc, t)) {
                UserFormat(stk, len, pl, sp, l);
                return;
            }
        }
        if (M.OptRef(t) || (IsOptional(t) && M.RelOk(t))) {
            // `null`, or the pointee's text; a relative link's offset decodes to its address.
            auto at = TgModule::IsRel(t) ? RelLoad(pl, l) : Ld(pl.a);
            IfElse(Cmp(TYG_EQ, at, I64(0)), [&] { TextLit(stk, len, "null"); },
                   [&] { RenderInto(stk, len, Step(pl), nested, l, depth + 1); });
            return;
        }
        // A struct that reaches itself is rendered by a procedure of its own (CodeGen::EmitRenderCall),
        // which runs no overloads (the checker leaves none in such a type) and renders its first level
        // inline (inlinet).
        if ((t->kind == TY_STRUCT || t->kind == TY_ENUM) && M.Recursive(t)) {
            if (inlinet && cg.TEq(inlinet, t)) {
                inlinet = nullptr;
            } else {
                if ((pl.stk && !M.FoOk(t) && !M.RsOk(t)) || pl.enc || !M.RenderFnOk(t))
                    No(l, cat("text of type ", M.Name(t), " [recursive]"));
                auto ptr = tyg_type_ptr(m);
                // A frame object that is bytes is read as the C one first.
                if (pl.bfo) pl = FoView(pl);
                CallC(M.RenderFn(t), { ptr, ptr, ptr }, { stk, AddrOf(len), AddrOf(pl.a) }, nullptr);
                return;
            }
        }
        auto rb = M.IsRb(t);   // A resizable ADT or one of its variants, at its header.
        if (rb && (!M.RbOk(t) || pl.enc)) No(l, cat("text of type ", M.NameWhy(t)));
        switch (t->kind) {
            case TY_VARIANT:
                RenderVariant(stk, len, pl, l, depth);
                return;
            case TY_ENUM: {   // The variant the tag says, as its literal (CodeGen::RenderLoc).
                if (!rb && !M.TypeOk(t) && !M.VmOk(t)) break;
                auto tag = Local(LoadTag(rb ? AtV(Ld(pl.a)) : pl.a, t));
                TagSwitch(tag, M.EI(t)->en->variants.size(), [&](size_t vi) {
                    auto vt = cg.VariantType(t, (int)vi);
                    auto pa = AddrPlus(pl.a, M.TagSize(t));
                    P vp { pa, vt };
                    if (rb) {   // The payload behind the tag, its tail counted by the header's.
                        vp.a = AtL(PairLocal(Add(Ld(pl.a), I64(M.TagSize(t))), Ld(AddrPlus(pl.a, 8))));
                        vp.tmphdr = true;
                    } else if (!M.IsVm(t) && fmtc && !fmtc->fmtspecs.empty() && cg.FixedSize(vt)) {
                        // An overload may overwrite the value: its parts rendered from a copy.
                        auto cp = Temp(vt);
                        tyg_copy(p, AtL(cp), pa, cg.FixedSize(vt));
                        vp.a = AtL(cp);
                    }
                    RenderVariant(stk, len, vp, l, depth);
                });
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
            case TY_STRUCT:
                if (!rb && !M.VsOk(t) && !M.RsOk(t) && !M.FoOk(t) && !M.TypeOk(t)) break;
                RenderRun(stk, len, TextName(t), pl, l, depth);
                return;
            case TY_ARRAY: case TY_SLICE: {
                auto v = ViewOf(pl, l);
                if (IsU8(v.elem)) {
                    if (!nested) {
                        CopyRun(stk, len, v.elem, v.elems, v.len);
                        return;
                    }
                    Counted(stk, len, CallC("gs_fmt_quoted",
                                            { tyg_type_ptr(m), tyg_type_ptr(m), IT(TYG_I64) },
                                            { Top(stk), v.elems, v.len }, IT(TYG_I64)));
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

    // A variant's positional literal (CodeGen::RenderVariant): `E.V` for one without fields, else `V {
    // a, b }`.
    void RenderVariant(tyg_val stk, tyg_addr len, const P &pl, Line l, int depth) {
        auto t = pl.t;
        auto ei = cg.EIVar(t);
        auto &v = ei->en->variants[(size_t)M.VariantIdx(t)];
        if (v.fields.empty()) {
            TextLit(stk, len, cat(ei->en->name, ".", v.name));
            return;
        }
        if (!pl.tmphdr && !M.IsRb(t) && !(cg.IsFix(t) ? M.TypeOk(t) : M.VvOk(t)))
            No(l, cat("text of type ", M.NameWhy(t)));
        RenderRun(stk, len, v.name, pl, l, depth);
    }

    // gs_abort(GS_E_<what>) with the Goose file and line, through a shim; the block ends there.
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

    // pop, pop_n, resize and clear of a resizable (codegen_builtins.h): the elements end at the stack's
    // top, which moves with the count.
    V GenShrink(Call *c) {
        auto an = c->ArgNodes();
        auto pl = Deref(Place(an[0]));
        if (TgModule::IsLim(pl.t)) return LimShrink(c, pl);
        auto len = CountAt(pl.a);
        // Variable-size elements.
        if (IsGrow(pl.t) && M.VeOk(pl.t) && pl.stk && c->builtin == B_CLEAR) {
            St(len, I64(0));
            SetTop(pl.stk, BaseOf(pl));
            return V {};
        }
        if (!IsGrow(pl.t) || !M.RzOk(pl.t) || !pl.stk)
            No(c->line, cat(BuiltinByKind(c->builtin).name, " of an array of this kind"));
        auto et = pl.t->arr->sub;
        auto esz = I64(cg.FixedSize(et));
        // The address of element n.
        auto at = [&](tyg_val n) { return Add(BaseOf(pl), Mul(n, esz)); };
        switch (c->builtin) {
            case B_POP: {
                auto nl = Sub(Ld(len), I64(1));
                AbortUnless(Cmp(TYG_SGE, nl, I64(0)), "POP", c->line);
                St(len, nl);
                auto top = Sub(Top(pl.stk), esz);
                SetTop(pl.stk, top);
                return Load(P { AtV(top), et }, c->line);
            }
            case B_POP_N: {
                auto nn = Index64(an[1]);
                if (Ended()) return V {};
                auto ol = Ld(len);
                AbortUnless(Cmp(TYG_ULE, nn, ol), "POPN", c->line);
                St(len, Sub(ol, nn));
                SetTop(pl.stk, Sub(Top(pl.stk), Mul(nn, esz)));
                return V {};
            }
            case B_CLEAR:
                SetTop(pl.stk, BaseOf(pl));
                St(len, I64(0));
                return V {};
            default: break;
        }
        // resize(n[, fill]): shrink to n, or grow by copies of fill (evaluated before the length).
        auto nn = Index64(an[1]);
        V fv;
        if (an.size() > 2) {
            fv = GenAs(an[2], et);
            if (!M.IsScalar(et) && !fv.mem && !Ended()) No(c->line, "aggregate value");
        }
        if (Ended()) return V {};
        auto nl = Local(nn), ol = Local(Ld(len));
        auto n = [&] { return LdL(nl); };
        auto o = [&] { return LdL(ol); };
        AbortUnless(Cmp(TYG_SGE, n(), I64(0)), "RESIZENEG", c->line);
        IfElse(Cmp(TYG_SLT, n(), o()), [&] {
            St(len, n());
            SetTop(pl.stk, at(n()));
        }, [&] {
            If(Cmp(TYG_SGT, n(), o()), [&] {
                if (an.size() <= 2) {
                    Abort("RESIZEFILL", c->line);
                    return;
                }
                // The new slots through a cursor, the top written once after.
                auto top = Top(pl.stk);
                if (cg.FixedSize(et)) {
                    auto cnt = Bin(TYG_SDIV, Sub(at(n()), top), esz);
                    Loop(I64(0), cnt, [&](tyg_val k) {
                        Store(P { AtV(Add(top, Mul(k, esz))), et }, fv);
                    });
                }
                SetTop(pl.stk, at(n()));
                St(len, n());
            });
        });
        return V {};
    }

    // str(a, b, ...): a fresh u8[>..] on a stack of the statement's (or where GenRz builds it);
    // format(out, a, b, ...): appended to out.
    V GenStr(Call *c) {
        auto t = c->exprtype;
        auto slice = t && t->kind == TY_SLICE && IsU8(t->sub);
        if (t && M.RcOk(t) && IsU8(t->arr->sub)) return BuildTemp(c, t, c->line);   // u8[..] (GenRc).
        if (t && TgModule::IsLim(t) && M.TypeOk(t) && IsU8(t->arr->sub)) {
            // A static-capacity u8[..k]: rendered as a resizable's, then copied under the check.
            auto r = BuildTemp(c, cg.GrowU8(), c->line);
            if (Ended()) return Dummy(t);
            return LimFrom(r, t, c->line);
        }
        if (!slice && (!t || !IsGrow(t) || !IsU8(t->arr->sub)))
            No(c->line, cat("str of type ", t ? M.Name(t) : "?"));
        auto x = BuildTemp(c, slice ? cg.GrowU8() : t, c->line);
        if (!slice || Ended()) return x;
        // Where the checker typed it as the slice it is passed as: a view.
        return PairV(t, Ld(x.a), Ld(CountAt(x.a)));
    }
    V GenFormat(Call *c) {
        auto an = c->ArgNodes();
        auto pl = Deref(Place(an[0]));
        if (TgModule::IsLim(pl.t) && M.LimHeld(pl.t) && IsU8(pl.t->arr->sub)) {
            // Into a static-capacity u8[..k] (CodeGen::AppendBytes): rendered into a builder of the
            // statement's, then appended under the capacity check.
            auto b = RzTemp(cg.GrowU8(), c->line);
            auto blen = CountAt(b.a);
            St(b.a, Top(b.stk));
            St(blen, I64(0));
            TextInto(b.stk, blen, c, an, 1);
            if (Ended()) return V {};
            auto n = Ld(blen);
            AbortUnless(Cmp(TYG_SLE, Add(LimLen(pl), n), LimCapOf(pl)), "CAPACITY", c->line);
            auto v = LimView(pl);
            tyg_copy_dyn(p, Add(v.elems, LimLen(pl)), Ld(b.a), n);
            LimSetLen(pl, Add(LimLen(pl), n));
            return V {};
        }
        if (!IsGrow(pl.t) || !M.RzOk(pl.t) || !pl.stk || !IsU8(pl.t->arr->sub))
            No(c->line, "format into an array of this kind");
        TextInto(pl.stk, CountAt(pl.a), c, an, 1);
        return V {};
    }

    // gs_uleb_check: the ULEB128 at `at` bounded by `end`, shortest and within 64 bits: its byte count
    // into k (0 where it is none), its value into v.
    struct Uleb {
        tyg_local k, v;
    };
    Uleb UlebCheck(tyg_val at, tyg_val end) {
        Uleb r { Local(I64(0)), Local(I64(0)) };
        auto sh = Local(I64(0)), q = Local(at);
        auto head = NewBlock(), fail = NewBlock(), done = NewBlock();
        tyg_jump(p, head);
        SetBlock(head);
        ContinueIf(Cmp(TYG_ULT, LdL(q), end), fail);
        auto qa = LdL(q);
        auto b = LoadN(qa, 1, false);
        auto shv = LdL(sh);
        StL(q, Add(qa, I64(1)));
        ContinueIf(Cmp(TYG_SLE, shv, I64(63)), fail);
        auto bits = [&](int64_t mask) { return AndK(b, mask); };
        If(Cmp(TYG_EQ, shv, I64(63)), [&] { ContinueIf(Cmp(TYG_EQ, bits(0x7e), I64(0)), fail); });
        StL(r.v, Bin(TYG_OR, LdL(r.v), Bin(TYG_SHL, bits(0x7f), shv)));
        If(Cmp(TYG_NE, bits(0x80), I64(0)), [&] {
            StL(sh, Add(shv, I64(7)));
            tyg_jump(p, head);
        });
        // A redundant high zero group is no shortest form.
        If(Cmp(TYG_NE, shv, I64(0)), [&] { ContinueIf(Cmp(TYG_NE, b, I64(0)), fail); });
        StL(r.k, Sub(LdL(q), at));
        tyg_jump(p, done);
        SetBlock(fail);
        StL(r.k, I64(0));
        tyg_jump(p, done);
        SetBlock(done);
        return r;
    }
    // from_bytes<T[>..]>(bytes) (CodeGen::EmitFromBytes): the framing prefix has to count exactly the
    // rest of the slice; the payload is verified as an image of elements (plain fixed ones by size
    // alone, others by TgModule::VerifyFn) and copied. The array, and whether it was one.
    vector<V> GenFromBytes(Call *c) {
        auto t = c->rettypes.empty() ? nullptr : c->rettypes[0];
        // A T[] image result (a statement's own stack) as well as a resizable.
        auto isimg = t && IsVarArr(t) && M.RzOk(t);
        if (!t || !((IsGrow(t) && (M.RzOk(t) || M.VeOk(t))) || isimg))
            No(c->line, "from_bytes of this type");
        auto et = t->arr->sub;
        auto plain = cg.IsFix(et) && !cg.NeedsVerifyWalk(et) && cg.FixedSize(et) > 0;
        auto an = c->ArgNodes();
        P src0;
        if (Is<Call>(an[0]) && an[0]->exprtype && an[0]->exprtype->kind == TY_SLICE) {
            // A call's array result handed as the slice: viewed where it lies.
            auto x = Gen1(an[0]);
            if (!Ended() && (!x.mem || !x.t || x.t->kind != TY_ARRAY))
                No(c->line, "from_bytes of this source");
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
        auto len = Local(v.len), pay = Local(v.elems), pn = Local(I64(0)), okl = Local(I64(0)),
             cnt = Local(I64(0));
        auto uc = UlebCheck(v.elems, Add(v.elems, v.len));
        // Framed: a prefix that counts the rest of the slice exactly (pay, pn), and whole elements.
        auto framed = NewBlock();
        ContinueIf(Cmp(TYG_NE, LdL(uc.k), I64(0)), framed);
        ContinueIf(Cmp(TYG_SGE, LdL(uc.v), I64(0)), framed);
        ContinueIf(Cmp(TYG_EQ, LdL(uc.v), Sub(LdL(len), LdL(uc.k))), framed);
        StL(pay, Add(v.elems, LdL(uc.k)));
        StL(pn, LdL(uc.v));
        if (plain) {
            auto esz = I64(cg.FixedSize(et));
            ContinueIf(Cmp(TYG_EQ, Bin(TYG_SREM, LdL(pn), esz), I64(0)), framed);
            StL(cnt, Bin(TYG_SDIV, LdL(pn), esz));
        }
        StL(okl, I64(1));
        tyg_jump(p, framed);
        SetBlock(framed);
        if (!plain) {
            // Relative links to variable elements need a bitmap of element starts on a stack of its
            // own; one beyond what is reserved (GS_BM_MAX) is rejected.
            tyg_val bm = I64(0);
            if (!cg.IsFix(et) && cg.HasRelRefAny(et)) {
                auto bstk = Stk(AllocStk(false, c->line), c->line);
                bm = Top(bstk);
                auto bn = Local(Bin(TYG_SDIV, Add(LdL(pn), I64(7)), I64(8)));
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
        auto moveIn = [&] { PushBytes(r.stk, LdL(pay), LdL(pn)); };
        auto accepted = Cmp(TYG_NE, LdL(okl), I64(0));
        if (isimg) {
            // The prefix reserved, the payload behind it, the count patched in (CodeGen::OpenRzDest,
            // EmitPrefixPatch).
            auto start = Local(Top(r.stk));
            SetTop(r.stk, Add(LdL(start), I64(TgModule::PrefixBytes(t))));
            If(accepted, moveIn);
            // A rejected image holds no elements.
            If(Cmp(TYG_EQ, LdL(okl), I64(0)), [&] { StL(cnt, I64(0)); });
            PatchPrefix(LdL(start), t, LdL(cnt), r.stk);
            r.a = AtV(LdL(start));
        } else {
            auto rlen = CountAt(r.a);
            St(r.a, Top(r.stk));
            St(rlen, I64(0));
            If(accepted, [&] {
                moveIn();
                St(rlen, LdL(cnt));
            });
        }
        return { r, ValV(M.ast.booltype, tyg_convert(p, TYG_TRUNC, TYG_I8, LdL(okl))) };
    }
    vector<V> GenCallAll(Node *n, size_t count) {
        auto c = Is<Call>(n);
        if (c && c->builtin == B_FROM_BYTES && count == 2) return GenFromBytes(c);
        if (!(c && c->builtin == B_QPOLL && count == 2) && !DestSpec(c))
            No(n->line, "multiple results of this kind");
        GenCall(c);
        if (lastrets.size() != count) No(n->line, "result count");
        return lastrets;
    }

    // ---- Threads and queues (§11.2) -------------------------------------------------------------

    tyg_val QueueAddr(TypeExpr *t) {
        cg.usesthreads = true;
        return CallC(M.QueueAccessor(t), {}, {}, tyg_type_ptr(m));
    }
    // Whether a value of type t crosses a queue as this backend holds it (CodeGen's IsBytesT).
    bool QueueOk(TypeExpr *t) {
        if (!t || t->kind == TY_REF) return false;
        if (cg.IsFix(t)) return M.TypeOk(t);
        if (M.Resz(t)) return (IsGrow(t) && (M.RzOk(t) || M.VeOk(t))) || M.FoOk(t);
        return (M.IsRc(t) && M.RcOk(t)) || (IsVarArr(t) && M.RzOk(t)) || (M.IsVs(t) && M.VsOk(t));
    }
    // The bytes of one freelist unit of pool d (CodeGen::FlEntrySize, GS_SPAN_NODE of a slice pool).
    static int64_t FlEntry(const VarDef *d) { return d->reusable == RU_SLICES ? 512 : 8; }
    // Whether a thread program's global is one a spawn image carries.
    bool ThreadGlobalOk(VarDef *d) {
        if (!M.GlobalOk(d) || cg.PrefVar(d)) return false;
        auto t = d->type;
        if (cg.IsFix(t)) return M.TypeOk(t) && !d->reusable;
        if (M.Resz(t))
            return (IsGrow(t) && (M.RzOk(t) || M.VeOk(t)) && (!d->reusable || M.RzOk(t))) || M.FoOk(t);
        return IsImg(t) && !d->reusable;
    }
    // Reserves n bytes at the top of stk; their address.
    tyg_val StackReserve(tyg_val stk, tyg_val n) {
        auto top = Top(stk);
        SetTop(stk, Add(top, n));
        return top;
    }
    // A node's value bytes follow its gs_qnode header (next, size).
    static constexpr int64_t QNODE = 16;

    P RzValue(Node *n, TypeExpr *t, Line l) {
        auto x = BuildTemp(n, t, l);
        return P { x.a, t, x.stk };
    }
    // The flat image of the resizable or frame object at src, at the top of stk: [int64 count][a frame
    // object's fixed fields][tail elements] (CodeGen::EmitRzImage); its address.
    tyg_val RzImage(tyg_val stk, const P &src, TypeExpr *t) {
        auto start = StackReserve(stk, I64(8));
        auto cnt = AtV(start);
        St(cnt, I64(0));
        auto h = src.a;
        TypeExpr *et;
        if (M.FoOk(t)) {
            auto pre = M.FoHdrOff(t);
            auto top = StackReserve(stk, I64(pre));
            if (pre) tyg_copy(p, AtV(top), src.a, pre);
            h.offset += pre;
            et = cg.FoTailArr(t)->arr->sub;
        } else {
            et = t->arr->sub;
        }
        View v { Ld(h), Ld(AddrPlus(h, 8)), et, -1 };
        CopyView(stk, cnt, et, v);
        return start;
    }

    V GenThreadWait(Call *c) {
        cg.usesthreads = true;
        auto id = GenAs(c->ArgNodes()[0], M.ast.inttypes[IS_I64]).v;
        if (!Ended()) CallC("gs_thread_wait", { IT(TYG_I64), tyg_type_ptr(m), IT(TYG_I32) },
                            { id, FileArg(c->line), LineArg(c->line) }, nullptr);
        return V {};
    }

    // thread_spawn(worker, args...) (CodeGen::EmitThreadSpawn): arguments and the worker's globals
    // packed on a stack of the statement's, handed to the worker's thunk.
    V GenThreadSpawn(Call *c) {
        auto w = c->spec;
        auto an = c->ArgNodes();
        if (!w || an.size() != 1 + w->argtypes.size()) No(c->line, "thread_spawn arguments");
        string why;
        if (!M.SigOk(w, false, why)) No(c->line, cat("thread program: ", why));
        if (!cg.sinfo[w].freevars.empty()) No(c->line, "thread program with free variables");
        for (auto d : cg.ThreadGlobals(w))
            if (!ThreadGlobalOk(d))
                No(c->line, cat("thread program with a global of type ", M.NameWhy(d->type)));
        for (size_t i = 0; i < w->argtypes.size(); i++) {
            auto pt = w->argtypes[i];
            if (cg.IsPoolParam(w, i) ||
                (M.RzParam(pt) ? !(QueueOk(pt) || (M.IsRs(pt) && M.RsOk(pt))) : !M.TypeOk(pt)))
                No(c->line, cat("thread argument of type ", M.NameWhy(pt)));
        }
        cg.usesthreads = true;
        auto thunk = CallC(M.ThunkAccessor(w), {}, {}, tyg_type_ptr(m));
        auto stk = Stk(AllocStk(false, c->line), c->line);
        auto base = Top(stk);
        auto i64t = M.ast.inttypes[IS_I64];
        // [int64 size][payload]: the size patched in once `body` has appended the payload.
        auto sized = [&](auto body) {
            auto szp = StackReserve(stk, I64(8));
            body();
            if (!Ended()) St(AtV(szp), Sub(Top(stk), Add(szp, I64(8))));
        };
        for (size_t i = 0; i < w->argtypes.size(); i++) {
            auto pt = w->argtypes[i];
            auto arg = an[1 + i];
            if (M.IsRs(pt)) {
                // Built straight into the packet, prefix and tail as they lie, the count from its
                // header.
                sized([&] {
                    auto cntp = AtV(StackReserve(stk, I64(8)));
                    auto hdr = NewHdr();
                    GenRs(arg, stk, hdr, pt, c->line);
                    if (!Ended()) St(cntp, Ld(CountAt(hdr)));
                });
            } else if (M.Resz(pt)) {
                // The worker's thunk rebuilds the header on a stack of its own.
                sized([&] {
                    auto src = RzValue(arg, pt, c->line);
                    if (!Ended()) RzImage(stk, src, pt);
                });
            } else if (M.RzParam(pt)) {
                GenImg(arg, stk, pt, c->line);   // The packet holds the image whole.
            } else {
                auto top = Top(stk);
                Into(P { AtV(top), pt }, arg);
                if (!Ended()) SetTop(stk, Add(top, I64(cg.FixedSize(pt))));
            }
            if (Ended()) return Dummy(i64t);
        }
        for (auto d : cg.ThreadGlobals(w)) {
            // A fixed value's bytes, a resizable's flat image (with a pool's freelist after it), or a
            // T[]'s.
            sized([&] {
                auto pl = VarPlace(d, c->line);
                if (cg.IsFix(d->type)) {
                    auto size = cg.FixedSize(d->type);
                    auto at = StackReserve(stk, I64(size));
                    if (size) tyg_copy(p, AtV(at), pl.a, size);
                } else if (!M.Resz(d->type)) {
                    ImgCopy(stk, AddrOf(pl.a), d->type);
                } else {
                    RzImage(stk, pl, d->type);
                    if (!d->reusable) return;
                    // The freelist: its count, then its entries.
                    P hp;
                    tyg_addr fl;
                    tyg_val flstk;
                    PoolAt(d, c->line, hp, fl, flstk);
                    auto top = Top(stk);
                    auto flen = Ld(AtV(AddrOf(fl), 8));
                    St(AtV(top), flen);
                    auto bytes = Mul(flen, I64(FlEntry(d)));
                    tyg_move_dyn(p, Add(top, I64(8)), Ld(fl), bytes);
                    SetTop(stk, Add(Add(top, I64(8)), bytes));
                }
            });
        }
        M.NeedShim("gs_tg_thread_spawn");
        return ValV(i64t, CallC("gs_tg_thread_spawn", { tyg_type_ptr(m), tyg_type_ptr(m), IT(TYG_I64) },
                                { thunk, base, Sub(Top(stk), base) }, IT(TYG_I64)));
    }

    V GenQput(Call *c) {
        auto an = c->ArgNodes();
        auto t = an[0]->exprtype;
        if (!QueueOk(t)) No(c->line, cat("queue element of type ", M.NameWhy(t)));
        tyg_val ptr, size;
        if (cg.IsFix(t) && !M.IsScalar(t)) {
            // A fixed aggregate is read where it lies: a temporary of it could overflow a worker's
            // stack.
            auto pl = Place(an[0]);
            ptr = AddrOf(pl.a);
            size = I64(cg.FixedSize(t));
        } else if (M.Resz(t)) {
            auto src = RzValue(an[0], t, c->line);
            if (Ended()) return V {};
            auto stk = Stk(AllocStk(false, c->line), c->line);
            ptr = RzImage(stk, src, t);
            size = Sub(Top(stk), ptr);
        } else if (!cg.IsFix(t)) {
            auto stk = Stk(AllocStk(false, c->line), c->line);
            ptr = GenImg(an[0], stk, t, c->line);
            if (!Ended()) size = Sub(Top(stk), ptr);
        } else {
            auto x = GenAs(an[0], t);
            auto tmp = Temp(t);
            if (!Ended()) Store(P { AtL(tmp), t }, x);
            ptr = AddrOf(AtL(tmp));
            size = I64(cg.FixedSize(t));
        }
        if (Ended()) return V {};
        CallC("gs_qput", { tyg_type_ptr(m), tyg_type_ptr(m), IT(TYG_I64) },
              { QueueAddr(t), ptr, size }, nullptr);
        return V {};
    }

    // qget<T>() and qpoll<T>() (CodeGen's B_QGET): the oldest node's bytes, the node freed; a missed
    // poll gives the zero value and false (a poll of an image is left to C).
    V GenQget(Call *c, tyg_val idst) {
        auto t = c->rettypes[0];
        auto poll = c->builtin == B_QPOLL;
        if (!QueueOk(t) || (poll && !cg.IsFix(t) && !M.Resz(t)))
            No(c->line, cat("queue element of type ", M.NameWhy(t)));
        auto node = CallC(poll ? "gs_qpoll" : "gs_qget", { tyg_type_ptr(m) }, { QueueAddr(t) },
                          IT(TYG_I64));
        auto got = [&] { return Cmp(TYG_NE, node, I64(0)); };
        // The node is taken and freed where there is one.
        auto take = [&](auto body) {
            auto once = [&] {
                body();
                CallC("free", { tyg_type_ptr(m) }, { node }, nullptr);
            };
            if (poll) If(got(), once);
            else once();
        };
        auto result = [&](V x) {
            lastrets = { x };
            if (poll) lastrets.push_back(ValV(M.ast.booltype, got()));
            return x;
        };
        if (M.Resz(t)) {
            // Rebuilt from the image on a stack of the statement's: the zero value until a node is
            // read.
            auto stk = Stk(AllocStk(false, c->line), c->line);
            auto fo = M.FoOk(t);
            auto pre = fo ? M.FoHdrOff(t) : 0;
            auto osz = fo ? M.Size(t) : 16;
            auto obj = AtL(tyg_local_new(p, osz, 8));
            tyg_zero(p, obj, osz);
            St(AddrPlus(obj, pre), Top(stk));
            take([&] {
                auto image = Add(node, I64(QNODE + 8));
                St(AddrPlus(obj, pre + 8), Ld(AtV(node, QNODE)));
                if (pre) tyg_copy(p, obj, AtV(image), pre);
                auto n = Sub(Ld(AtV(node, 8)), I64(8 + pre));
                PushBytes(stk, Add(image, I64(pre)), n);
            });
            return result(MemV(t, obj, stk));
        }
        if (!cg.IsFix(t)) {
            auto stk = idst ? idst : Stk(AllocStk(false, c->line), c->line);
            auto size = Ld(AtV(node, 8));
            auto top = PushBytes(stk, Add(node, I64(QNODE)), size);
            CallC("free", { tyg_type_ptr(m) }, { node }, nullptr);
            return result(MemV(t, AtV(top)));
        }
        auto size = cg.FixedSize(t);
        auto at = AtL(Temp(t));
        if (poll) tyg_zero(p, at, std::max<int64_t>(size, 1));
        take([&] { if (size) tyg_copy(p, at, AtV(Add(node, I64(QNODE))), size); });
        return result(M.IsScalar(t) ? Load(P { at, t }, c->line) : MemV(t, at));
    }

    // An extern fn, through a trampoline the C half defines (it may be static, or a macro).
    V GenExternCall(Call *c, FnSpec *callee, vector<Node *> &an) {
        if (callee->rets.size() > 1) No(c->line, "extern results");
        lastrets.clear();
        for (auto r : callee->rets)
            if (!M.TypeOk(r)) No(c->line, "extern result type");
        vector<tyg_arg> args;
        for (size_t i = 0; i < an.size(); i++) {
            auto pt = callee->argtypes[i];
            auto isl = pt->kind == TY_SLICE;
            auto s = isl ? Is<StrLit>(an[i]) : nullptr;
            if (!s) {
                if (!M.TypeOk(pt)) No(c->line, isl ? "slice argument" : "extern parameter type");
                args.push_back(Arg(an[i], pt));
                continue;
            }
            if (!IsU8(pt->sub)) No(c->line, "slice argument");
            auto t = Slot16();
            StL(t, BytesAddr(s->val));
            StL(t, I64((int64_t)s->val.size()), 8);
            tyg_arg a {};
            a.mem = AtL(t);
            args.push_back(a);
        }
        // The C names of its types, now, while the C half can still take their declarations.
        for (auto t : callee->argtypes) cg.CT(t);
        for (auto t : callee->rets) cg.CT(t);
        // One trampoline per C name (a call may have a specialization of its own of the same extern); a
        // function the runtime object exports, or an export's own wrapper, needs none.
        auto direct = M.RuntimeApi(callee->sf->cname);
        for (auto sf : M.ast.functions)
            direct = direct || (sf->isexport && sf->cname == callee->sf->cname);
        auto &list = direct ? M.direct : M.externs;
        auto known = false;
        for (auto e : list) known = known || e->sf->cname == callee->sf->cname;
        if (!known) list.push_back(callee);
        if (Ended()) return Dummy(callee->rets.empty() ? nullptr : callee->rets[0]);
        auto sym = M.Sym(direct ? callee->sf->cname : cat("gs_tg_x_", callee->sf->cname),
                         TYG_SYM_FUNC | TYG_SYM_GLOBAL);
        return Result(callee, callee->rets.empty() ? -1 : 0, M.ExternSig(callee), tyg_direct(sym),
                      args);
    }

    // print(...) (§3.7): every argument is evaluated before any text is written (B_PRINT).
    static TypeExpr *PlainPointee(TypeExpr *t) {
        while (t && IsPlainRef(t)) t = t->ref->sub;
        return t;
    }
    // A u8 array or slice prints as its bytes (SimpleText).
    static bool PrintsAsBytes(TypeExpr *bt) {
        return bt && ((bt->kind == TY_ARRAY && IsU8(bt->arr->sub)) ||
                      (bt->kind == TY_SLICE && IsU8(bt->sub)));
    }
    V GenPrint(Call *c) {
        auto an = c->ArgNodes();
        // A value rendered structurally sends the whole line through a builder on a stack of the
        // statement's.
        auto simple = [&](Node *a) {
            if (Is<StrLit>(a)) return true;
            auto bt = PlainPointee(a->exprtype);
            auto fc = cg.FmtContext(c, a);
            if (bt && cg.FmtSpecFor(fc, bt)) return false;
            if (TgModule::IsVarint(bt) && cg.FmtSpecFor(fc, M.ast.inttypes[IS_I64])) return false;
            if (PrintsAsBytes(bt)) return true;
            auto ot = cg.OperandT(a->exprtype);
            return ot && M.IsScalar(ot) && ot->kind != TY_REF;
        };
        if (!std::all_of(an.begin(), an.end(), simple)) {
            auto b = RzTemp(cg.GrowU8(), c->line);
            auto len = AddrPlus(b.a, 8);
            St(b.a, Top(b.stk));
            St(len, I64(0));
            TextInto(b.stk, len, c, an, 0);
            if (Ended()) return V {};
            CallC("gs_out_bytes", { tyg_type_ptr(m), IT(TYG_I64) }, { Ld(b.a), Ld(len) }, nullptr);
            CallC("gs_out_nl", {}, {}, nullptr);
            return V {};
        }
        vector<std::function<void()>> outs;   // The calls, made once every argument is evaluated.
        auto bytes = [&](tyg_val ptr, tyg_val len) {
            outs.push_back([=] {
                CallC("gs_out_bytes", { tyg_type_ptr(m), IT(TYG_I64) }, { ptr, len }, nullptr);
            });
        };
        auto scalar = [&](const char *fn, tyg_val v, const tyg_type *ty) {
            outs.push_back([=] { CallC(fn, { ty }, { v }, nullptr); });
        };
        for (auto a : an) {
            if (auto s = Is<StrLit>(a)) {
                bytes(BytesAddr(s->val), I64((int64_t)s->val.size()));
            } else if (PrintsAsBytes(PlainPointee(a->exprtype))) {
                auto v = ViewOf(Place(a), a->line);
                bytes(v.elems, v.len);
            } else {
                auto ot = cg.OperandT(a->exprtype);
                if (ot->kind == TY_INT && ot->intstorage == IS_U64)
                    scalar("gs_out_uint", GenAs(a, ot).v, IT(TYG_I64, false));
                else if (ot->kind == TY_INT)
                    scalar("gs_out_int", GenAs(a, M.ast.inttypes[IS_I64]).v, IT(TYG_I64));
                else if (ot->kind == TY_FLT && M.Scal(ot) == TYG_F32)
                    scalar("gs_out_f32", GenAs(a, ot).v, tyg_type_scalar(m, TYG_F32));
                else if (ot->kind == TY_FLT)
                    scalar("gs_out_flt", GenAs(a, ot).v, tyg_type_scalar(m, TYG_F64));
                else
                    scalar("gs_out_bool", tyg_convert(p, TYG_ZEXT, TYG_I64, GenAs(a, ot).v),
                           IT(TYG_I64));
            }
        }
        if (Ended()) return V {};
        for (auto &o : outs) o();
        CallC("gs_out_nl", {}, {}, nullptr);
        return V {};
    }

    // ---- Optionals ------------------------------------------------------------------------------

    // Optionals (§3.8) as the C backend lowers them: a T? is the address a T& would be, 0 for null (a
    // fat reference with a null hdr), so a narrowed one is used as that reference (Binary::CgX,
    // GenTruth).

    // A reference that is an address: a plain one, or an optional one the checker has narrowed.
    static bool IsAddrRef(const TypeExpr *t) { return IsPlainRef(t) || TgModule::OptRef(t); }
    // A fat reference where one differing only in optionality is wanted (T& to T?, a narrowed T? to
    // T&).
    bool OptFit(V &x, TypeExpr *want) {
        if (x.t->kind != TY_REF || want->kind != TY_REF || !cg.IsFatRef(x.t) || !cg.IsFatRef(want) ||
            x.t->ref->lenstorage >= 0 || want->ref->lenstorage >= 0 ||
                !cg.TEq(x.t->ref->sub, want->ref->sub))
            return false;
        x.t = want;
        return true;
    }
    // The null of an optional (or of the checker's bare null type).
    V Null(TypeExpr *t, Line l) {
        if (t && t->kind == TY_REF && cg.IsFatRef(t)) {
            auto tmp = Slot16();
            tyg_zero(p, AtL(tmp), 16);
            return MemV(t, AtL(tmp));
        }
        if (!t || t->kind != TY_REF || (t->ref->lenstorage >= 0 && !M.RelOk(t, 1)))
            No(l, "null of this type");
        return ValV(t, I64(0));
    }
    tyg_val Addr(const V &x, Line l) {
        if (!x.t || x.t->kind != TY_REF || (x.t->ref->lenstorage >= 0 && !M.RelOk(x.t, 1)))
            No(l, "address of this value");
        return x.mem ? Ld(x.a) : x.v;
    }
    // A reference as a test or identity sees it: a variable's own value, even where narrowed; null;
    // anything else's value.
    tyg_val RefAddr(Node *n) {
        if (Is<NullLit>(n)) return I64(0);
        V x;
        if (auto id = Is<Ident>(n); id && id->vdef && id->vdef->type && id->vdef->type->kind == TY_REF)
            x = Load(VarPlace(id->vdef, n->line), n->line);
        else
            x = Gen(n);
        return Ended() ? I64(0) : Addr(x, n->line);
    }
    // Whether a condition is an optional's null test: an optional variable or a value of optional type.
    bool OptCond(Node *n) {
        if (auto id = Is<Ident>(n); id && id->vdef && id->vdef->type && IsOptional(id->vdef->type))
            return true;
        return n->exprtype && IsOptional(n->exprtype);
    }
    tyg_val OptTest(Node *n) { return Cmp(TYG_NE, RefAddr(n), I64(0)); }
    // `a == null`, `a != null`, and identity `a .== b`, `a .!= b`: the addresses, left first.
    V GenRefEq(Binary *b) {
        auto l = RefAddr(b->left);
        auto r = RefAddr(b->right);
        return ValV(M.ast.booltype, Cmp(b->op == T_EQ || b->op == T_DOTEQ ? TYG_EQ : TYG_NE, l, r));
    }
    // ---- Control --------------------------------------------------------------------------------

    // The body in a stack scope of its own (a binder's or a block's stacks are given back on this
    // path).
    template <class F> void InScope(F body) {
        tyg_scope_begin(p);
        OpenScope(false);
        body();
        CloseScope();
        tyg_scope_end(p);
    }
    // A block's value is its tail's (none in statement position); a function body's tail with a result
    // is returned (tailret).
    TypeExpr *armwant = nullptr;   // What GenArm wants of a block arm's tail.
    V GenBlock(Block *b, bool discard, bool tailret = false) {
        auto want = armwant;
        armwant = nullptr;
        V x;
        InScope([&] {
            for (auto s : b->stmts) Stmt(s);
            if (!b->tail) return;
            if (discard && !tailret) return Stmt(b->tail);
            Open();
            Loc(b->tail->line);
            if (tailret) return DoReturn({ b->tail }, b->tail->line);
            x = want ? GenSliceAs(b->tail, want) : Gen(b->tail);
            if (x.t && (IsGrow(x.t) || (IsImg(x.t) && !TgModule::IsVr(x.t))))
                No(b->tail->line, "array value of a block");
        });
        return x;
    }

    // ---- Varints in bytes values (§3.6: ULEB128, values zigzag) ---------------------------------

    template <class F> void DoWhile(F body) {
        auto head = NewBlock(), again = NewBlock(), done = NewBlock();
        tyg_jump(p, head);
        SetBlock(head);
        Branch(body(), again, done);
        SetBlock(again);
        tyg_jump(p, head);
        SetBlock(done);
    }
    tyg_val UlebRead(tyg_val at) {
        auto v = Local(I64(0)), sh = Local(I64(0)), q = Local(at);
        DoWhile([&] {
            auto qa = LdL(q);
            auto b = LoadN(qa, 1, false);
            StL(v, Bin(TYG_OR, LdL(v), Bin(TYG_SHL, AndK(b, 0x7f), LdL(sh))));
            StL(q, Add(qa, I64(1)));
            StL(sh, Add(LdL(sh), I64(7)));
            return Cmp(TYG_NE, AndK(b, 0x80), I64(0));
        });
        return LdL(v);
    }
    // A varint value: (u >> 1) ^ -(u & 1) (gs_zig_read).
    tyg_val ZigRead(tyg_val at) {
        auto u = UlebRead(at);
        return Bin(TYG_XOR, Bin(TYG_LSHR, u, I64(1)), tyg_un(p, TYG_NEG, AndK(u, 1)));
    }
    // The bytes of the ULEB128 at `at` (gs_uleb_size).
    tyg_val UlebSize(tyg_val at) {
        auto q = Local(at);
        DoWhile([&] {
            auto qa = LdL(q);
            StL(q, Add(qa, I64(1)));
            return Cmp(TYG_NE, AndK(LoadN(qa, 1, false), 0x80), I64(0));
        });
        return Sub(LdL(q), at);
    }
    // Writes the varint value v at `at`, zigzag ULEB128 (gs_zig_write); its byte count.
    tyg_val ZigWrite(tyg_val at, tyg_val v) {
        return UlebWrite(at,
                         Bin(TYG_XOR, Bin(TYG_SHL, v, I64(1)),
                                 Bin(TYG_ASHR, v, I64(63))));
    }
    // Writes the unsigned uv at `at`, ULEB128 (gs_uleb_write); its byte count.
    tyg_val UlebWrite(tyg_val at, tyg_val uv0) {
        auto u = Local(uv0), q = Local(at);
        DoWhile([&] {
            auto uv = LdL(u);
            auto b = AndK(uv, 0x7f);
            auto rest = Bin(TYG_LSHR, uv, I64(7));
            StL(u, rest);
            auto qa = LdL(q);
            StL(q, Add(qa, I64(1)));
            auto more = Cmp(TYG_NE, rest, I64(0));
            IfElse(more, [&] { StoreByte(qa, Bin(TYG_OR, b, I64(0x80))); }, [&] {
                StoreByte(qa, b);
            });
            return more;
        });
        return Sub(LdL(q), at);
    }

    // An arm whose value `build` makes in place: its statements, then its tail built there.
    template <class F> void BuildArm(Node *n, Line l, F build) {
        if (auto b = Is<Block>(n)) {
            InScope([&] {
                for (auto st : b->stmts) Stmt(st);
                if (Ended()) return;
                if (!b->tail) No(l, "arm without a value");
                Open();
                Loc(b->tail->line);
                build(b->tail);
            });
            return;
        }
        OpenScope(true);
        build(n);
        CloseScope();
    }
    void BindFvParams(Call *c) {
        for (size_t i = 0; i < c->fvparams.size(); i++) {
            Declare(c->fvparams[i], c->args[i]->line);
            Init(c->fvparams[i], c->args[i], c->args[i]->line);
        }
    }
    // A function value's call built in place: its parameters bound in a scope, then its body as an arm.
    template <class F> void BuildFv(Call *c, Line l, F build) {
        if (c->fvparams.size() > c->args.size()) No(c->line, "function value parameters");
        InScope([&] {
            BindFvParams(c);
            if (!Ended()) BuildArm(c->fvbody, l, build);
        });
    }
    template <class F> void BuildIf(IfExpr *ie, Line l, F build) {
        IfElse(Truth(ie->cond), [&] { BuildArm(ie->thenb, l, build); }, [&] {
            BuildArm(ie->elseb, l, build);
        });
    }

    // An arm runs or not: what it takes on a stack is its own.
    V GenArm(Node *n, bool discard, TypeExpr *want = nullptr) {
        // A slice wanted: an array arm is a view of the array where it lies (§6.4), not of a copy.
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
        if (x.t && (IsGrow(x.t) || (IsImg(x.t) && !TgModule::IsVr(x.t))))
            No(n->line, "array value of an arm");
        CloseScope();
        return x;
    }

    // An image or resizable value of n built on a temporary's stack (GenImg, GenRz); false if et is
    // neither.
    bool InPlaceValue(Node *n, TypeExpr *et, Line l, V &x) {
        if (M.FoOk(et) || !(TempOk(et) || M.RbOk(et))) return false;
        x = BuildTemp(n, et, l);
        return true;
    }
    // A value wanted of a type neither scalar nor held in a register: in place if an image or a
    // resizable.
    bool WantInPlace(Node *n, bool discard, V &x) {
        auto et = n->exprtype;
        if (discard || !et || et->kind == TY_VOID || M.TypeOk(et) || M.VrOk(et)) return false;
        return InPlaceValue(n, et, n->line, x);
    }

    // What every valued control construct shares: its result temporary (or a pool-reference slot).
    struct Ctl {
        tyg_local res = 0;
        bool pr = false, valued = false;
        TypeExpr *et = nullptr;
        V w;   // The value, where n was built in place.
    };
    // Starts construct n: true if built in place (value in k.w), else k is set up. `gate` false: n
    // gives no value (an if without else, a loop without a break value).
    bool CtlBegin(Ctl &k, Node *n, bool discard, const InPlace *ip, bool gate) {
        k.pr = prefctl;
        k.res = prefloc;
        prefctl = false;
        if (gate && !ip && !k.pr && WantInPlace(n, discard, k.w)) return true;
        auto et = n->exprtype;
        k.valued = gate && !ip && !discard && et && et->kind != TY_VOID;   // A value of a type held.
        if (k.valued && !M.TypeOk(et) && !M.VrOk(et)) No(n->line, cat("value of type ", M.NameWhy(et)));
        // Carried between blocks, a relative reference is the plain one: an offset is from its own
        // slot.
        k.et = et && TgModule::IsRel(et) && !TgModule::IsVr(et) ?
            M.ast.RefTo(et->ref->sub, Line {}, et->ref->optional) : et;
        if (k.valued && !k.pr) k.res = Temp(k.et);
        return false;
    }
    void CtlArm(const Ctl &k, const InPlace *ip, Node *body, Line l) {
        if (ip) BuildArm(body, l, [&](Node *v) { BuildInto(v, *ip, l); });
        else if (k.valued && k.pr)
            BuildArm(body, body->line, [&](Node *v) { PrefInto(AtL(k.res), v); });
        else {   // The value stored in the construct's temporary.
            auto x = GenArm(body, !k.valued, k.valued ? k.et : nullptr);
            if (k.valued && !Ended() && x.t) Store(P { AtL(k.res), k.et }, Fit(x, k.et, body->line));
        }
    }
    V CtlValue(const Ctl &k, Line l) { return k.valued ? Load(P { AtL(k.res), k.et }, l) : V {}; }
    // The break/continue target of a loop or block delivering k.
    LoopTargets CtlTarget(const Ctl &k, tyg_block brk, tyg_block cnt, bool block, const InPlace *ip) {
        LoopTargets lt { brk, cnt, sscopes.size(), k.res, k.valued ? k.et : nullptr, block };
        lt.pref = k.valued && k.pr;
        if (ip) {
            lt.ip = *ip;
            lt.top0 = Local(Top(ip->stk));
        }
        return lt;
    }

    V GenIf(IfExpr *i, bool discard) {
        Ctl k;
        if (CtlBegin(k, i, discard, nullptr, i->elseb != nullptr)) return k.w;
        auto arm = [&](Node *n) { CtlArm(k, nullptr, n, i->line); };
        auto c = Truth(i->cond);
        if (i->elseb) IfElse(c, [&] { arm(i->thenb); }, [&] { arm(i->elseb); });
        else If(c, [&] { arm(i->thenb); });
        return CtlValue(k, i->line);
    }

    // `block { ... }` (EarlyBlock::CgAny): its tail's value, or a break's.
    V GenEarly(EarlyBlock *e, bool discard, const InPlace *ip = nullptr) {
        Ctl k;
        if (CtlBegin(k, e, discard, ip, true)) return k.w;
        auto done = NewBlock();
        loops.push_back(CtlTarget(k, done, done, true, ip));
        CtlArm(k, ip, e->body, e->line);
        loops.pop_back();
        Jump(done);
        SetBlock(done);
        return CtlValue(k, e->line);
    }

    // `loop { ... }`, its value a break's.
    V GenLoopX(LoopExpr *lp, bool discard, const InPlace *ip = nullptr) {
        Ctl k;
        auto brkval = std::any_of(lp->breaks.begin(), lp->breaks.end(), [](Break *b) {
            return b->val;
        });
        if (CtlBegin(k, lp, discard, ip, brkval)) return k.w;
        auto body = NewBlock(), done = NewBlock();
        tyg_jump(p, body);
        SetBlock(body);
        loops.push_back(CtlTarget(k, done, body, false, ip));
        GenBlock(lp->body, true);
        loops.pop_back();
        Jump(body);
        SetBlock(done);
        return CtlValue(k, lp->line);
    }

    // Branches to `hit` where c holds, else on in a block of its own.
    void HitElse(tyg_val c, tyg_block hit) {
        auto next = NewBlock();
        Branch(c, hit, next);
        SetBlock(next);
    }
    // An integer match (MatchExpr::CgAny): the scrutinee evaluated once, the arms tested in order.
    V GenMatch(MatchExpr *me, bool discard, const InPlace *ip = nullptr) {
        Ctl k;
        if (CtlBegin(k, me, discard, ip, true)) return k.w;
        if (auto adt = AdtOf(me->scrutinee->exprtype)) return GenAdtMatch(me, adt, k, ip);
        auto ot = cg.OperandT(me->scrutinee->exprtype);
        if (!ot || ot->kind != TY_INT || ot->intstorage == IS_VARINT)
            No(me->line, cat("match over type ", ot ? M.NameWhy(ot) : string("?")));
        auto x0 = Conv(GenAs(me->scrutinee, ot).v, ot, M.ast.inttypes[IS_I64], me->line);
        if (Ended()) return k.valued ? Dummy(k.et) : V {};
        auto xl = Local(x0);
        auto done = NewBlock();
        auto wild = false;
        for (auto &a : me->arms) {
            if (a.binder) No(me->line, "match arm with a binding");
            auto hit = NewBlock(), next = NewBlock();
            wild = a.pat.kind == P_WILDCARD;
            if (wild) tyg_jump(p, hit);
            // lo <= x <= hi (lo <= hi at the type's order) as one unsigned compare.
            for (auto &r : a.ranges)
                HitElse(Cmp(TYG_ULE, Sub(LdL(xl), I64(r.lo)),
                            I64((int64_t)((uint64_t)r.hi - (uint64_t)r.lo))), hit);
            if (!wild) tyg_jump(p, next);
            SetBlock(hit);
            CtlArm(k, ip, a.body, me->line);
            Jump(done);
            SetBlock(next);
            if (wild) break;
        }
        Jump(done);   // Past the last arm, or the wildcard's never-entered fall-through.
        SetBlock(done);
        return CtlValue(k, me->line);
    }

    // ---- Fixed-mode ADTs ------------------------------------------------------------------------
    // Fixed-mode ADTs and variants as the C backend builds and reads them (TgModule's ADT section has
    // the layout): `match` and case-function dispatch (CodeGen::EmitDispatch) are chains of tag
    // compares in variant order (no switch), with GS_UNREACHABLE's abort past the last under --debug.

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
    tyg_val LoadTag(tyg_addr a, TypeExpr *adt) {
        return tyg_convert(p, TYG_ZEXT, TYG_I64, tyg_load(p, M.TagScal(adt), a, 0));
    }
    // A payload-less variant constant (Dot::CgX).
    V GenVariantConst(Dot *d) {
        auto t = d->exprtype;
        if (M.RbOk(t) || M.VmOk(t)) {
            V x;
            if (!InPlaceValue(d, t, d->line, x)) No(d->line, "variant constant");
            return x;
        }
        if (!M.TypeOk(t)) No(d->line, cat("variant constant of type ", M.NameWhy(t)));
        auto x = MemTemp(t, true);
        if (t->kind == TY_ENUM) StoreTag(x.a, t, d->einst->en->VariantIndex(d->variantconst));
        return x;
    }
    // Past the last variant: a corrupt tag, reported under --debug (GS_UNREACHABLE), else assumed away.
    void BadTag(Line l) {
        if (M.checks) Abort("TAG", l);
        else tyg_unreachable(p);
    }
    // The ADT value a match or dispatch reads, in memory, its tag read once into a local.
    struct Scrut {
        V v;
        TypeExpr *adt;
        tyg_local tag;
    };
    // The scrutinee at a (a temporary if the code ended before it was evaluated).
    Scrut ScrutAt(TypeExpr *adt, tyg_addr a) {
        if (Ended()) a = AtL(Temp(M.ast.inttypes[IS_I64]));
        return Scrut { MemV(adt, a), adt, Local(LoadTag(a, adt)) };
    }
    Scrut AdtScrutinee(Node *n, TypeExpr *adt) {
        // A resizable or variable-mode value is read where it lies (a resizable's header holds its
        // address).
        if (M.RbOk(adt)) {
            auto pl = Deref(Place(n));
            return ScrutAt(adt, Ended() ? tyg_addr {} : AtV(Ld(pl.a)));
        }
        if (M.VmOk(adt)) return ScrutAt(adt, Deref(Place(n)).a);
        if (!M.TypeOk(adt)) No(n->line, cat("match over type ", M.NameWhy(adt)));
        auto x = GenAs(n, adt);
        return ScrutAt(adt, x.a);
    }
    void TagIs(const Scrut &s, int vi, tyg_block hit) {
        HitElse(Cmp(TYG_EQ, LdL(s.tag), I64(vi)), hit);
    }
    void CopyPayload(const Scrut &s, int vi, tyg_addr to) {
        auto sz = cg.FixedSize(cg.VariantType(s.adt, vi));
        if (sz) tyg_copy(p, to, AddrPlus(s.v.a, M.TagSize(s.adt)), sz);
        else tyg_zero(p, to, 1);
    }

    V GenAdtMatch(MatchExpr *me, TypeExpr *adt, const Ctl &k, const InPlace *ip) {
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
            InScope([&] {
                if (a.binder) BindAdtArm(me, adt, s, a);
                Loc(a.body->line);   // A breakpoint on the arm's line stops here, bound.
                CtlArm(k, ip, a.body, me->line);
            });
            Jump(done);
            SetBlock(next);
            if (wild) break;
        }
        if (!wild) BadTag(me->line);
        else tyg_unreachable(p);
        SetBlock(done);
        return CtlValue(k, me->line);
    }
    // An arm's binder: a payload by reference (variable mode only) or by value (§8.1).
    template <class A> void BindAdtArm(MatchExpr *me, TypeExpr *adt, const Scrut &s, const A &a) {
        auto vi = adt->enu->en->VariantIndex(a.variants[0]);
        auto vt = cg.VariantType(adt, vi);
        auto pay = s.v.a;
        pay.offset += M.TagSize(adt);
        // The C binds no resizable payload, and by value only a resizable ADT's others.
        if (M.IsRb(adt) && (a.pat.byref || cg.IsResz(vt)))
            No(me->line, "match arm binding of a resizable ADT");
        if (a.pat.byref) {
            if (!M.IsVm(adt)) No(me->line, "match arm binding by reference");
            locals[a.binder] = Local(AddrOf(pay));
            if (!IsAddrRef(a.binder->type)) indirect.insert(a.binder);
        } else if (!cg.IsFix(vt)) {
            Declare(a.binder, me->line);   // A variable payload, copied onto the binder's own stack.
            StL(locals[a.binder], ImgCopy(LdL(rzstk[a.binder]), AddrOf(pay), vt));
        } else {
            Declare(a.binder, me->line);
            CopyPayload(s, vi, AtL(locals[a.binder]));
        }
    }

    // A case-function call dispatched on its ADT argument's tag (§8.2): every argument evaluated once,
    // in order, then the variant's case called with its payload by value and the others shared.
    V GenDispatch(Call *c, tyg_val idst, const RzDst *dst) {
        auto sp0 = c->dispatch[0];
        size_t pos = c->dispatcharg;
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
                if (cg.Mangle(sp->rets[i]) != cg.Mangle(sp0->rets[i]))
                    No(c->line, "dispatch with results of different types");
            for (size_t i = 0; i < sp->argtypes.size(); i++) {
                if (i != pos && cg.Mangle(sp->argtypes[i]) != cg.Mangle(sp0->argtypes[i]))
                    No(c->line, "dispatch with arguments of different types");
                if (cg.IsPoolParam(sp, i) != cg.IsPoolParam(sp0, i))
                    No(c->line, "dispatch with a pool reference");
            }
            // A variable-size case takes its payload by value as an image: a copy of the variable-mode
            // value's own (below).
            auto cpt = sp->argtypes[pos];
            if (M.RzParam(cpt) && !(M.IsVv(cpt) && !M.Resz(cpt) && M.IsVm(adt)))
                No(c->line, "dispatch to a case of this kind");
        }
        // The shared arguments, each kept in a local of its own: the arms are blocks of their own.
        Scrut s {};
        tyg_local copied = 0;
        auto havecopy = false;
        vector<vector<tyg_arg>> shared(an.size());   // The others' arguments, built once.
        auto copyval = [&] {
            // The whole variable-mode value copied before the later arguments run
            // (CodeGen::EmitDispatch).
            if (Ended() || !M.IsVm(adt) ||
                !std::any_of(c->dispatch.begin(), c->dispatch.end(), [&](FnSpec *k) {
                    return k->argtypes[pos]->kind != TY_REF;
                }))
                return;
            copied = Local(ImgCopy(Stk(AllocStk(false, c->line), c->line), AddrOf(s.v.a), adt));
            havecopy = true;
        };
        for (size_t i = 0; i < an.size(); i++) {
            auto pt = sp0->argtypes[i];
            if (i == pos) {
                // Cases taking the variant by reference get the value where it lies, its payload by
                // address.
                s = pt->kind == TY_REF ? ScrutAt(adt, Deref(Place(an[i])).a) : AdtScrutinee(an[i], adt);
                copyval();
            } else if (cg.IsPoolParam(sp0, i)) {
                // A pool reference, kept as its 32 bytes (GenPrefVal).
                auto l = tyg_local_new(p, 32, 8);
                PrefInto(AtL(l), an[i]);
                for (int k = 0; k < 4 && !Ended(); k++) shared[i].push_back(ArgOfVal(LdL(l, 8 * k)));
            } else if (M.RzParam(pt)) {
                ValArg(an[i], pt, shared[i]);
            } else {
                if (!M.TypeOk(pt)) No(an[i]->line, "argument of this type");
                auto x = GenAs(an[i], pt);
                auto l = Temp(pt);
                if (Ended()) continue;
                if (M.IsScalar(pt)) tyg_store(p, M.Scal(pt), AtL(l), x.v, 0);
                else tyg_copy(p, AtL(l), x.a, M.Size(pt));
                shared[i].push_back(M.IsScalar(pt) ? ArgOfVal(tyg_load(p, M.Scal(pt), AtL(l), 0))
                                    : cg.IsLargeFixed(pt) ? ArgOfVal(AddrOf(AtL(l))) :
                                        ArgOfMem(AtL(l)));
            }
        }
        auto cret = cg.sinfo[sp0].cret;
        auto rt = cret >= 0 ? sp0->rets[(size_t)cret] : nullptr;
        tyg_local res = rt ? Temp(rt) : 0;
        lastrets.clear();
        if (Ended()) {
            for (auto r : sp0->rets) lastrets.push_back(Dummy(r));
            return lastrets.empty() ? V {} : lastrets[0];
        }
        vector<tyg_arg> chanargs;
        auto rets = RetChans(sp0, cg.sinfo[sp0], idst, dst, chanargs, c->line);
        auto done = NewBlock();
        auto nvar = adt->enu->en->variants.size();
        vector<tyg_block> arms;
        for (size_t vi = 0; vi < nvar; vi++) {
            arms.push_back(NewBlock());
            TagIs(s, (int)vi, arms.back());
        }
        BadTag(c->line);
        for (size_t vi = 0; vi < nvar; vi++) {
            SetBlock(arms[vi]);
            auto sp = c->dispatch[vi];
            auto &ci = cg.sinfo[sp];
            vector<tyg_arg> args;
            for (size_t i = 0; i < an.size(); i++) {
                auto pt = sp->argtypes[i];
                if (i == pos && pt->kind == TY_REF && pt->ref->sub->kind == TY_VARIANT) {
                    args.push_back(ArgOfVal(Add(AddrOf(s.v.a), I64(M.TagSize(adt)))));
                } else if (i == pos && M.IsVv(pt)) {
                    args.push_back(ArgOfVal(Add(LdL(copied), I64(M.TagSize(adt)))));
                } else if (i == pos) {
                    if (pt->kind != TY_VARIANT) No(c->line, "dispatch to a case of this kind");
                    auto t = Temp(pt);
                    auto cs = s;
                    if (havecopy) cs.v.a = AtV(LdL(copied));
                    CopyPayload(cs, (int)vi, AtL(t));
                    args.push_back(cg.IsLargeFixed(pt) ? ArgOfVal(AddrOf(AtL(t))) : ArgOfMem(AtL(t)));
                } else {
                    args.insert(args.end(), shared[i].begin(), shared[i].end());
                }
            }
            FvArgs(c, ci, args);
            args.insert(args.end(), chanargs.begin(), chanargs.end());
            if (ci.needssp) args.push_back(ArgOfVal(CalleeSp(sp, c->line)));
            auto sym = M.Sym(ci.cname, TYG_SYM_FUNC | TYG_SYM_GLOBAL);
            auto x = Result(sp, ci.cret, M.Sig(sp), tyg_direct(sym), args);
            if (rt) Store(P { AtL(res), rt }, x);
            RfCheck(sp);
            Jump(done);
        }
        SetBlock(done);
        for (size_t i = 0; i < rets.size(); i++) {
            if ((int)i == cret) lastrets.push_back(Load(P { AtL(res), rt }, c->line));
            else if (M.IsScalar(rets[i].t))
                lastrets.push_back(Load(P { rets[i].a, rets[i].t }, c->line));
            else lastrets.push_back(rets[i]);
        }
        return lastrets.empty() ? V {} : lastrets[0];
    }

    // ---- Statements -----------------------------------------------------------------------------

    // A statement's temporaries' stacks are free again at its end.
    void Stmt(Node *n) {
        Open();
        Loc(n->line);
        OpenScope(true);
        Stmt1(n);
        CloseScope();
    }
    // A break's value, delivered as its target wants it (CodeGen::GenExitValue, LandValue).
    void BreakValue(Break *b, const LoopTargets &t) {
        if (t.pref) {
            PrefInto(AtL(t.res), b->val);
        } else if (t.ip.mode) {
            // Built at the top, which something under construction may have moved up: the value then
            // moves down to where the exit's value starts.
            auto st = Local(Top(t.ip.stk));
            BuildInto(b->val, t.ip, b->line);
            if (Ended()) return;
            auto start = LdL(st), top0 = LdL(t.top0);
            If(Cmp(TYG_NE, start, top0), [&] {
                auto n = Sub(Top(t.ip.stk), start);
                tyg_move_dyn(p, top0, start, n);
                SetTop(t.ip.stk, Add(top0, n));
                if (t.ip.mode == 2) St(t.ip.hdr, top0);
            });
        } else if (t.rt) {
            Into(P { AtL(t.res), t.rt }, b->val);
        } else {
            Stmt1(b->val);
        }
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
            case NK_LoopExpr:
                GenLoopX(static_cast<LoopExpr *>(n), true);
                return;
            case NK_EarlyBlock:
                GenEarly(static_cast<EarlyBlock *>(n), true);
                return;
            case NK_MatchExpr:
                GenMatch(static_cast<MatchExpr *>(n), true);
                return;
            case NK_ForLoop: return StmtFor(static_cast<ForLoop *>(n));
            case NK_Return: {
                auto r = static_cast<Return *>(n);
                if (!sp) No(n->line, "return in a global initializer");
                if (r->target != sp->sf) return GenFromReturn(r);
                return DoReturn(r->vals, r->line);
            }
            case NK_Break: {
                // To the innermost loop or block (CodeGen::GenBreakPath): its value first, then the
                // stacks of the scopes left.
                auto b = static_cast<Break *>(n);
                if (loops.empty()) No(n->line, "break target");
                auto t = loops.back();
                if (b->val) {
                    BreakValue(b, t);
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
            case NK_Block:
                GenBlock(static_cast<Block *>(n), true);
                return;
            case NK_IfExpr:
                GenIf(static_cast<IfExpr *>(n), true);
                return;
            default: {
                auto pc = Is<Call>(n);
                discardpush = pc && pc->builtin == B_PUSH;
                Gen(n);
                discardpush = false;
                return;
            }
        }
    }

    // A nonfixed result built at the top of dst, from val or from x (a forwarded call's value): a T[]'s
    // image, a resizable's elements (their count stored at len) or a frame object (its struct at len).
    void BuildAt(Node *val, V *x, tyg_val dst, TypeExpr *t, tyg_addr len, Line l) {
        auto fo = M.FoOk(t), img = !fo && IsImg(t);
        auto hdr = fo || img ? len : NewHdr();
        P fp { hdr, t, dst };
        InPlace d { fo ? 3 : img ? 1 : 2, dst, t, hdr, &fp };
        if (!x) BuildInto(val, d, l);
        else CopyFrom(P { x->a, x->t, x->stk }, d, l);
        if (d.mode == 2 && !Ended()) St(len, Ld(CountAt(hdr)));
    }

    // return, return x, return a, b (results the C does not return are written through their channels
    // as they are made), or return f() forwarding all of a call's results (§7.1). A resizable result is
    // built at its destination stack, its count written through its out-parameter. Then every scope's
    // stacks are restored, and the C return value returned.
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
        auto selfrel = [&](Node *v, TypeExpr *t) { return SelfRelLit(v) && cg.TEq(v->exprtype, t); };
        auto scalar = [&](const V &x, TypeExpr *t) {
            if (!M.IsScalar(t) && !x.mem) No(l, "aggregate value");
        };
        if (rets.size() == 1 && cret == 0) {
            if (vals.size() == 1 && selfrel(vals[0], rets[0])) {   // Built where the result lies.
                auto tmp = AtL(Temp(rets[0]));
                SelfRelInto(P { tmp, rets[0] }, vals[0]);
                if (Ended()) return;
                Leave();
                tyg_ret_mem(p, tmp);
                return;
            }
            auto x = GenAs(vals.at(0), rets[0]);
            if (Ended()) return;
            scalar(x, rets[0]);
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
        // Where result i goes: the C return value's temporary, or through its out-pointer.
        auto dest = [&](size_t i) {
            return (int)i == cret ? first : P { AtV(LdL(chans[i].a)), rets[i] };
        };
        for (size_t i = 0; i < rets.size(); i++) {
            if (IsImg(rets[i]) || M.Resz(rets[i])) {
                auto dst = LdL(chans[i].a);
                SetTop(dst, LdL(chans[i].top0));
                auto rl = M.Resz(rets[i]) ? AtV(LdL(chans[i].b)) : tyg_addr {};
                BuildAt(xs.empty() ? vals[i] : nullptr, xs.empty() ? nullptr : &xs[i], dst, rets[i],
                        rl, l);
            } else if (xs.empty() && selfrel(vals[i], rets[i])) {
                SelfRelInto(dest(i), vals[i]);
            } else {
                auto x = xs.empty() ? GenAs(vals[i], rets[i]) : Fit(xs[i], rets[i], l);
                if (Ended()) return;
                scalar(x, rets[i]);
                Store(dest(i), x);
            }
            if (Ended()) return;
        }
        Leave();
        if (cret < 0) tyg_ret(p, 0);
        else if (M.IsScalar(rets[cret])) tyg_ret(p, tyg_load(p, M.Scal(rets[cret]), first.a, 0));
        else tyg_ret_mem(p, first.a);
    }

    // ---- return from (§7.9) ---------------------------------------------------------------------
    // As the C half does it (CodeGen::GenFromReturn, EmitRfCheck): the values go into the target's
    // thread-local channels (gs_lret_<tid>_<i>), gs_rf is set to its id and every frame on the way
    // returns at once; after a call that can propagate one, a nonzero gs_rf is caught by its target or
    // passed on. Both thread-locals are the C half's, reached through its exported accessors.

    // A result built on a destination stack rather than returned in a register or slot.
    bool NonFixedRet(TypeExpr *t) { return IsImg(t) || M.Resz(t); }
    tyg_val RfAddr() {
        M.NeedShim("gs_tg_rf");
        return LdL(AccessorLocal("gs_tg_rf"));
    }
    // A target's channel for result i: gs_lret_ (a fixed value, or a resizable's count), gs_fdst_ (a
    // nonfixed one's destination stack) or gs_fval_ (where its value starts there).
    tyg_addr ChanAt(FnSpec *t, size_t i, const char *kind) {
        cg.EnsureFromChannels(t);
        auto tid = cg.fromids[t];
        auto name = cat("gs_tg_", kind, "_", tid, "_", i);
        M.NeedShim(name, cat("void *", name, "(void) { return (void *)&gs_", kind, "_", tid, "_", i,
                             "; }\n"));
        auto rt = t->rets[i];
        M.channels[name] = string(kind) == "lret" && !IsGrow(rt) && !M.IsRs(rt) && !M.IsRb(rt) ?
            M.Size(rt) : 8;
        return AtV(LdL(AccessorLocal(name)));
    }
    tyg_addr LretAt(FnSpec *t, size_t i) { return ChanAt(t, i, "lret"); }
    // A target's saves of its gs_fdst_ channels, restored as it returns.
    vector<pair<tyg_addr, tyg_local>> fdstsaves;
    void Leave() {
        RestoreFrom(0);
        for (auto &[at, sav] : fdstsaves) St(at, LdL(sav));
    }
    // Return at once with a dummy C return value (CodeGen::PropagateReturn).
    void Propagate() {
        Leave();
        auto cret = cg.sinfo[sp].cret;
        if (cret < 0) return tyg_ret(p, 0);
        auto rt = sp->rets[cret];
        if (M.IsScalar(rt)) return tyg_ret(p, Dummy(rt).v);
        auto t = Temp(rt);
        tyg_zero(p, AtL(t), M.Size(rt));
        tyg_ret_mem(p, AtL(t));
    }
    void GenFromReturn(Return *r) {
        auto t = r->targetspec;
        auto l = r->line;
        if (!sp || !t || !cg.fromids.count(t) || !cg.sinfo[sp].hasrf) No(l, "return from");
        auto &rets = t->rets;
        auto &vals = r->vals;
        for (auto rt : rets)
            if (!M.RetOk(rt)) No(l, cat("return from with a result of type ", M.Name(rt)));
        // A nonfixed value lands at its channel's top, which the catch moves to where the target's
        // caller expects it.
        for (size_t i = 0; i < rets.size(); i++)
            if (NonFixedRet(rets[i])) St(ChanAt(t, i, "fval"), Top(Ld(ChanAt(t, i, "fdst"))));
        vector<V> xs;
        if (vals.size() == 1 && rets.size() > 1) xs = GenCallAll(vals[0], rets.size());
        else if (vals.size() != rets.size()) No(l, "result count");
        for (size_t i = 0; i < rets.size(); i++) {
            if (NonFixedRet(rets[i])) {
                BuildAt(xs.empty() ? vals[i] : nullptr, xs.empty() ? nullptr : &xs[i],
                        Ld(ChanAt(t, i, "fdst")),
                        rets[i], M.Resz(rets[i]) ? LretAt(t, i) : tyg_addr {}, l);
            } else {
                auto x = xs.empty() ? GenAs(vals[i], rets[i]) : Fit(xs[i], rets[i], l);
                if (!Ended() && !M.IsScalar(rets[i]) && !x.mem) No(l, "aggregate value");
                if (!Ended()) Store(P { LretAt(t, i), rets[i] }, x);
            }
            if (Ended()) return;
        }
        tyg_store(p, TYG_I32, AtV(RfAddr()), I32(cg.fromids[t]), 0);
        Propagate();
    }
    // The value a catching frame returns as its own for result i: it lies on our destination behind
    // what the calls it unwound built there (CodeGen::LandValue), then the channel's header.
    void LandChannel(size_t i) {
        auto rt = sp->rets[i];
        auto dst = LdL(chans[i].a), top0 = LdL(chans[i].top0);
        auto start = Ld(ChanAt(sp, i, "fval"));
        auto n = Sub(Top(dst), start);
        tyg_move_dyn(p, top0, start, n);
        SetTop(dst, Add(top0, n));
        if (IsGrow(rt) || M.IsRs(rt) || M.IsRb(rt)) {
            St(AtV(LdL(chans[i].b)), Ld(LretAt(sp, i)));
        } else if (M.FoOk(rt)) {
            // The frame object, its tail's base moved with it.
            auto rl = LdL(chans[i].b);
            tyg_copy(p, AtV(rl), LretAt(sp, i), M.Size(rt));
            auto hb = AtV(rl, M.FoHdrOff(rt));
            St(hb, Add(top0, Sub(Ld(hb), start)));
        }
    }
    // After a call that can propagate a return (CodeGen::EmitRfCheck).
    void RfCheck(FnSpec *callee) {
        if (!cg.sinfo[callee].hasrf || cg.norfcheck || Ended()) return;
        auto rfa = RfAddr();
        auto rf = tyg_load(p, TYG_I32, AtV(rfa), 0);
        auto i32 = [&](int v) { return I32(v); };
        If(Cmp(TYG_NE, rf, i32(0)), [&] {
            if (sp && callee->needs.count(sp))
                If(Cmp(TYG_EQ, rf, i32(cg.fromids[sp])), [&] {
                    tyg_store(p, TYG_I32, AtV(rfa), i32(0), 0);   // Nothing in flight again.
                    auto &rets = sp->rets;
                    auto cret = cg.sinfo[sp].cret;
                    for (size_t i = 0; i < rets.size(); i++) {
                        if ((int)i == cret) continue;
                        if (NonFixedRet(rets[i])) LandChannel(i);
                        else
                            Store(P { AtV(LdL(chans[i].a)), rets[i] },
                                  Load(P { LretAt(sp, i), rets[i] }, Line {}));
                    }
                    Leave();
                    if (cret < 0) tyg_ret(p, 0);
                    else if (M.IsScalar(rets[cret]))
                        tyg_ret(p, Load(P { LretAt(sp, cret), rets[cret] }, Line {}).v);
                    else tyg_ret_mem(p, LretAt(sp, cret));
                });
            if (sp && cg.sinfo[sp].hasrf) {
                Propagate();
            } else {
                M.NeedShim("gs_tg_rfpanic");
                CallC("gs_tg_rfpanic", {}, {}, nullptr);
                tyg_unreachable(p);
            }
        });
    }

    void DebugVar(decltype(&tyg_debug_var) fn, VarDef *d, const tyg_dtype *dt, tyg_local loc) {
        fn(p, string(d->name).c_str(), dt, AtL(loc), M.File(d->line.fileidx), d->line.line);
    }
    // A local's header or image, empty, with a stack of its own (rzstk): a resizable's header, a frame
    // object's struct, or a T[] local's image address (built by StmtVarDecl).
    void Declare(VarDef *d, Line l) {
        auto t = d->type;
        auto hdr = (IsGrow(t) && (M.RzOk(t) || M.VeOk(t))) || M.RsOk(t) || M.RbOk(t);
        auto img = !hdr && !M.FoOk(t) &&
                   ((IsVarArr(t) && (M.RzOk(t) || M.VarImgOk(t))) || M.VsOk(t) || M.VmOk(t) ||
                    M.VvOk(t) ||
                    M.RcOk(t) || M.VrOk(t));
        if ((hdr || img || M.FoOk(t)) && !cg.PrefVar(d)) {
            auto stk = Stk(AllocStk(true, l), l);
            auto sl = Local(stk);
            tyg_local loc;
            if (img) {
                loc = Local(Top(stk));
                indirect.insert(d);
            } else {
                // The tail's elements go on the stack, the tail empty.
                loc = tyg_local_new(p, hdr ? 16 : M.Size(t), 8);
                if (!hdr) tyg_zero(p, AtL(loc), M.Size(t));
                StL(loc, Top(stk), hdr ? 0 : M.FoHdrOff(t));
                if (hdr) StL(loc, I64(0), 8);
            }
            locals[d] = loc;
            rzstk[d] = sl;
            DebugVar(tyg_debug_var, d, img ? tyg_dt_pointer(m, M.DT(t)) : M.DT(t), loc);
            if (hdr && d->reusable) {
                // Its freelist: free slot indices on a stack of their own.
                auto flstk = Stk(AllocStk(true, l), l);
                auto fl = Slot16();
                auto fsl = Local(flstk);
                StL(fl, Top(flstk));
                StL(fl, I64(0), 8);
                poolfl[d] = { fl, fsl };
            }
            return;
        }
        if (!M.TypeOk(t)) No(l, cat("variable type of ", d->name, ": ", M.NameWhy(t)));
        // A pool reference is a gs_pref: the fat reference and the pool's freelist (CodeGen::VarCT).
        auto loc = cg.PrefVar(d) ? tyg_local_new(p, 32, 8) : Temp(t);
        locals[d] = loc;
        DebugVar(tyg_debug_var, d, M.DT(t), loc);
    }

    void StmtVarDecl(VarDecl *vd) {
        if (vd->isglobal) No(vd->line, "declaration kind");
        if (vd->reusable)
            for (auto d : vd->defs)
                if (!IsGrow(d->type) || !M.RzOk(d->type) || cg.PrefVar(d))
                    No(vd->line, "reusable pool of this kind");
        auto l = vd->line;
        if (vd->defs.size() > 1 && vd->inits.size() == 1) {
            // let a, b = f(): the locals, then the call into them.
            for (auto d : vd->defs) Declare(d, l);
            auto xs = GenCallAll(vd->inits[0], vd->defs.size());
            for (size_t i = 0; i < xs.size(); i++) {
                auto d = vd->defs[i];
                auto t = d->type;
                // The local's header or image was left empty at its stack's top by Declare, where the
                // result is built.
                if (!Ended() && (IsVarArr(t) || M.IsVs(t) || (M.IsVm(t) && !M.IsRb(t)) || M.FoOk(t) ||
                                 M.RsOk(t) ||
                                 M.IsRb(t) || IsGrow(t))) {
                    if (IsGrow(t) && (!IsGrow(xs[i].t) ||
                                      cg.Mangle(xs[i].t->arr->sub) != cg.Mangle(t->arr->sub)))
                        No(l, "array result of another type");
                    BuildAt(nullptr, &xs[i], LdL(rzstk[d]), t, AtL(locals[d], M.FoOk(t) ? 0 : 8), l);
                    continue;
                }
                auto x = Fit(xs[i], t, l);
                if (!M.IsScalar(t) && !x.mem && !Ended()) No(l, "aggregate value");
                Store(P { AtL(locals[d]), t }, x);
            }
            return;
        }
        if (!vd->inits.empty() && vd->inits.size() != vd->defs.size()) No(l, "declaration kind");
        // Every local first, so that a resizable one's stack lies under what its initializer takes.
        for (auto d : vd->defs) Declare(d, l);
        for (size_t i = 0; i < vd->defs.size(); i++)
            Init(vd->defs[i], vd->inits.empty() ? nullptr : vd->inits[i], l);
    }
    // A declared local's first value: init's, or zero bytes (an empty array) where there is none.
    void Init(VarDef *d, Node *init, Line l) {
        auto t = d->type;
        auto a = AtL(locals[d]);
        auto stk = [&] { return LdL(rzstk[d]); };
        if (IsImg(t)) {
            auto img = GenImg(init, stk(), t, l);
            if (!Ended()) St(a, img);
        } else if (IsGrow(t) || M.RsOk(t) || M.IsRb(t) || M.FoOk(t)) {
            if (!init && M.IsRb(t)) No(l, "resizable ADT without a value");
            P pl { a, t, stk() };
            if (init) BuildInto(init, InPlace { M.FoOk(t) ? 3 : 2, pl.stk, t, a, &pl }, l);
        } else if (cg.PrefVar(d)) {
            if (!init) tyg_zero(p, a, 32);
            else PrefInto(a, init);
        } else if (!init) {
            tyg_zero(p, a, M.Size(t));
        } else {
            Into(P { a, t }, init);
        }
    }

    // A plain `=` into pl (Assign::CgStmt): a resizable's stack top goes back to where its elements
    // start, the new value is built there.
    void AssignWhole(Assign *a, P pl) {
        auto l = a->line;
        auto t = pl.t;
        // The stack's top goes back to `top`, where the old value began; a->rhs is built there.
        auto rebuild = [&](tyg_val top, const InPlace &d) {
            SetTop(pl.stk, top);
            BuildInto(a->rhs, d, l);
        };
        if (IsGrow(t)) {
            if (!(M.RzOk(t) || M.VeOk(t)) || !pl.stk) No(l, "assignment to an array of this kind");
            auto th = pl.rbase ? NewHdr() : pl.a;   // A resizable struct's tail: a header of its own.
            rebuild(BaseOf(pl), InPlace { 2, pl.stk, t, th });
            if (pl.rbase && !Ended()) St(AddrPlus(pl.a, 8), Ld(AddrPlus(th, 8)));
            return;
        }
        if (pl.bfo && pl.stk) {   // A frame object tail of a resizable struct, as bytes (GenFoAsBytes).
            rebuild(AddrOf(pl.a), InPlace { 4, pl.stk, t, AddrPlus(pl.ohdr, 8) });
            return;
        }
        if (M.FoOk(t) && pl.stk) {   // Its innermost tail's elements given back.
            rebuild(Ld(AddrPlus(pl.a, M.FoHdrOff(t))), InPlace { 3, pl.stk, t, pl.a, &pl });
            return;
        }
        if (pl.tmphdr || M.RsOk(t) || M.IsRb(t)) {   // A tmphdr place: a resizable struct's tail.
            if (!pl.stk || (!M.RsOk(t) && !M.RbOk(t))) No(l, "assignment to a resizable of this kind");
            rebuild(Ld(pl.a), InPlace { 2, pl.stk, t, pl.a });
            if (pl.tmphdr && !Ended()) St(AddrPlus(pl.ohdr, 8), Ld(AddrPlus(pl.a, 8)));   // Its count.
            return;
        }
        if (IsAddrRef(t)) {
            auto d = Deref(pl);
            if (M.IsRc(d.t)) pl = d;
            t = pl.t;
        }
        if (M.IsRc(t)) {
            // A T[..]: the value as one, its length and elements copied in under the capacity.
            if (!M.RcOk(t)) No(l, "assignment to an array of this kind");
            View v;
            auto rhs = StripCopy(a->rhs);
            if (Is<Ident>(rhs) || Is<Dot>(rhs) || Is<Index>(rhs)) {
                v = ViewOf(Place(rhs), l);   // A place's elements are read where they lie.
            } else {
                auto stk = Stk(AllocStk(false, l), l);
                v = LimView(P { AtV(GenImg(a->rhs, stk, t, l)), t });
            }
            if (Ended()) return;
            if (cg.Mangle(v.elem) != cg.Mangle(t->arr->sub)) No(l, "array of another element type");
            auto nl = Local(v.len);
            AbortUnless(Cmp(TYG_SLE, LdL(nl), LimCapOf(pl)), "CAPACITY", l);
            tyg_move_dyn(p, LimView(pl).elems, v.elems, Mul(LdL(nl), I64(cg.FixedSize(t->arr->sub))));
            LimSetLen(pl, LdL(nl));
            return;
        }
        // The destination's address is taken before the value runs, as the C does.
        if (SelfRelLit(a->rhs) && cg.TEq(a->rhs->exprtype, t)) {
            SelfRelInto(pl, a->rhs);   // A literal linking to itself is built where it lies.
            return;
        }
        Into(pl, a->rhs);
    }

    void StmtAssign(Assign *a) {
        auto pl = Place(a->lval);
        if (a->pointee) pl = Deref(pl);
        if (a->op == T_DOTASSIGN) {
            // A reference re-pointed (CodeGen::GenRebind): its slot takes the new address.
            if (auto id = Is<Ident>(a->lval); id && id->vdef && !a->pointee && IsPrefStore(id->vdef)) {
                PrefInto(pl.a, a->rhs);   // A pool reference rebound takes the pool's freelist too.
                return;
            }
            auto rel = TgModule::IsRel(pl.t) && M.RelOk(pl.t, 1);
            if (!rel && (!(IsPlainRef(pl.t) || M.OptRef(pl.t)) || !M.TypeOk(pl.t)))
                No(a->line, cat("rebind of type ", M.NameWhy(pl.t)));
            Into(pl, a->rhs);
            return;
        }
        if (a->op == T_ASSIGN) return AssignWhole(a, pl);
        pl = Deref(pl);
        auto t = pl.t;
        if ((t->kind == TY_STRUCT || t->kind == TY_ARRAY) && M.TypeOk(t) &&
            (a->op == T_PLUSEQ || a->op == T_MINUSEQ || a->op == T_MULEQ || a->op == T_DIVEQ ||
             a->op == T_MODEQ)) {
            // Elementwise, each leaf from its old value, read before the operand runs.
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
        static const pair<TType, TType> ops[] = {
            { T_PLUSEQ, T_PLUS }, { T_MINUSEQ, T_MINUS }, { T_MULEQ, T_MUL }, { T_DIVEQ, T_DIV },
            { T_MODEQ, T_MOD }, { T_ANDEQ, T_BITAND }, { T_OREQ, T_BITOR }, { T_XOREQ, T_XOR },
            { T_SHLEQ, T_SHL }, { T_SHREQ, T_SHR } };
        TType op = a->op;
        for (auto &[eq, o] : ops)
            if (eq == a->op) op = o;
        if (op == a->op) No(a->line, "assignment operator");
        tyg_val r;
        if (op == T_SHL || op == T_SHR) {
            if (t->kind != TY_INT) No(a->line, "shift type");
            auto n = GenAs(a->rhs, M.ast.inttypes[IS_I64]).v;
            auto sgn = TgModule::IsSigned(t);
            auto s = M.Scal(t);
            n = AndK(n, IntBits(t->intstorage) - 1);
            auto w = s == TYG_I64 ? old : tyg_convert(p, sgn ? TYG_SEXT : TYG_ZEXT, TYG_I64, old);
            auto res = Bin(op == T_SHL ? TYG_SHL : sgn ? TYG_ASHR : TYG_LSHR, w, n);
            r = s == TYG_I64 ? res : tyg_convert(p, TYG_TRUNC, s, res);
        } else {
            auto rv = GenAs(a->rhs, t).v;
            if (t->kind == TY_FLT) {
                if (op != T_PLUS && op != T_MINUS && op != T_MUL && op != T_DIV)
                    No(a->line, "float compound assignment");
                auto f = op == T_PLUS ? TYG_FADD : op == T_MINUS ? TYG_FSUB : op == T_MUL ? TYG_FMUL :
                    TYG_FDIV;
                r = Bin(f, old, rv);
            } else if (t->kind == TY_INT) {
                r = IntOp(op, t, old, rv, a->line);
            } else if (op == T_BITAND || op == T_BITOR || op == T_XOR) {
                r = Bin(op == T_BITAND ? TYG_AND : op == T_BITOR ? TYG_OR : TYG_XOR, old, rv);
            } else {
                No(a->line, "compound assignment type");
            }
        }
        if (!Ended()) tyg_store(p, M.Scal(t), pl.a, r, 0);
    }

    // The loop variable d takes v (of type from) at its own type.
    void StoreConv(VarDef *d, tyg_val v, TypeExpr *from, Line l) {
        tyg_store(p, M.Scal(d->type), AtL(locals[d]), Conv(v, from, d->type, l), 0);
    }
    // A loop's body, then on at its step block.
    void LoopBody(ForLoop *f, tyg_block done, tyg_block step) {
        loops.push_back({ done, step, sscopes.size() });
        GenBlock(f->body, true);
        loops.pop_back();
        Jump(step);
        SetBlock(step);
        Loc(f->line);
    }

    // A counted loop over a range or a count (§6.5): the counter runs at the range's own type, the
    // binding is a copy of it at its own.
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
        if (f->idxdef) ix = Local(I64(0));
        if (f->vdef) Declare(f->vdef, f->line);
        if (f->idxdef) Declare(f->idxdef, f->line);
        auto head = NewBlock(), body = NewBlock(), step = NewBlock(), done = NewBlock();
        tyg_jump(p, head);
        SetBlock(head);
        Loc(f->line);
        auto cv = tyg_load(p, s, AtL(ctr), 0);
        Branch(Cmp(TgModule::IsSigned(ct) ? TYG_SLT : TYG_ULT, cv, tyg_load(p, s, AtL(hiv), 0)), body,
               done);
        SetBlock(body);
        if (f->vdef) StoreConv(f->vdef, cv, ct, f->line);
        if (f->idxdef) StoreConv(f->idxdef, LdL(ix), M.ast.inttypes[IS_I64], f->line);
        LoopBody(f, done, step);
        tyg_store(p, s, AtL(ctr), Add(tyg_load(p, s, AtL(ctr), 0), tyg_iconst(p, s, 1)), 0);
        if (f->idxdef) StL(ix, Add(LdL(ix), I64(1)));
        tyg_jump(p, head);
        SetBlock(done);
        tyg_scope_end(p);
    }

    // for x in a / for &x in a / for x, i in a over a fixed array or slice (ForLoop::CgStmt): the
    // length and the elements are read again at every iteration where the path to them leads (a body
    // may re-point a slice, §6.5); anything that is not a path runs once, into a temporary. The binding
    // is the element's address if the checker typed it a reference, else a copy.
    void StmtForEach(ForLoop *f) {
        P once {};
        auto path = CheckFreePath(f->iter);
        if (!path) once = Place(f->iter);
        auto at = [&]() { return path ? Place(f->iter) : once; };
        auto gi = Local(I64(0));
        // Variable-size elements are walked with a cursor (StepElem), the variable their address.
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
            auto loc = Slot();
            locals[f->vdef] = loc;
            // A reference variable holds the address itself; a value one is read through it.
            if (!IsAddrRef(f->vdef->type)) indirect.insert(f->vdef);
            DebugVar(tyg_debug_var, f->vdef, tyg_dt_pointer(m, M.DT(ielem)), loc);
        } else if (f->vdef) {
            Declare(f->vdef, f->line);
        }
        if (f->idxdef) Declare(f->idxdef, f->line);
        auto head = NewBlock(), body = NewBlock(), step = NewBlock(), done = NewBlock();
        tyg_local wb = 0;
        if (walk) {
            cl = Slot();
            wb = Slot();
            auto e0 = ViewOf(at(), f->line).elems;
            StL(cl, e0);
            StL(wb, e0);
        }
        tyg_jump(p, head);
        SetBlock(head);
        Loc(f->line);
        auto v = ViewOf(at(), f->line);
        if (walk && path) {
            // The path re-pointed: the cursor walks to the same index from the new start.
            If(Cmp(TYG_NE, v.elems, LdL(wb)), [&] {
                StL(wb, v.elems);
                StL(cl, v.elems);
                Loop(I64(0), LdL(gi), [&](tyg_val) {
                    auto c1 = LdL(cl);
                    StL(cl, Add(c1, ImgSize(ielem, c1)));
                });
            });
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
        if (f->idxdef) StoreConv(f->idxdef, i, M.ast.inttypes[IS_I64], f->line);
        LoopBody(f, done, step);
        StL(gi, Add(LdL(gi), I64(1)));
        if (walk) {
            auto c0 = LdL(cl);
            StL(cl, Add(c0, ImgSize(ielem, c0)));
        }
        tyg_jump(p, head);
        SetBlock(done);
        tyg_scope_end(p);
    }

    // ---- Frame objects --------------------------------------------------------------------------

    // A tail's header at th: init built on stk, or empty at its top.
    void TailHdr(Node *init, tyg_val stk, tyg_addr th, TypeExpr *ft, Line l) {
        if (init) {
            GenRz(init, stk, th, ft, l);
        } else {
            St(th, Top(stk));
            St(AddrPlus(th, 8), I64(0));
        }
    }

    // A frame object built at pl, its tail on pl.stk (CodeGen's GenConstruct).
    void GenFo(Node *n, P pl, Line l) {
        InPlace d { 3, pl.stk, pl.t, pl.a, &pl };
        if (BuildCtl(n, d, l)) return;
        auto sl = Is<StructLit>(n);
        // Any other: a call's result or variable, copied.
        if (!sl && n->exprtype && cg.TEq(n->exprtype, pl.t)) {
            P src;
            if (Is<Call>(n)) {
                auto x = Gen1(n);
                src = P { x.a, x.t, x.stk };
            } else {
                src = Deref(Place(n));
            }
            if (!Ended()) CopyFrom(src, d, l);
            return;
        }
        if (!sl || !sl->sinst || sl->variant) No(l, cat("value of type ", M.NameWhy(pl.t)));
        auto &fs = M.StFields(pl.t);
        for (size_t i = 0; i < fs.f.size() && !Ended(); i++) {
            if (fs.f[i].ispad) continue;
            auto init = sl->InitFor((int)i);
            auto fp = FieldPlace(pl, (int)i, l);
            if ((int)i != fs.var) {
                if (init) Into(fp, init);
                else tyg_zero(p, fp.a, M.Size(fp.t));
            } else if (!M.FoOk(fp.t)) {
                TailHdr(init, pl.stk, fp.a, fp.t, l);
            } else if (init) {   // A nested frame object, its tail on the same stack.
                GenFo(init, fp, l);
            } else {
                tyg_zero(p, fp.a, M.Size(fp.t));
                St(AddrPlus(fp.a, M.FoHdrOff(fp.t)), Top(pl.stk));
            }
        }
    }

    // The frame object at src copied to dst: its fixed fields, then its tail's elements.
    void FoCopy(const P &dst, const P &src, Line l) {
        if (src.bfo) return FoCopy(dst, FoView(src), l);
        auto &fs = M.StFields(dst.t);
        auto pre = fs.coff[(size_t)fs.var];
        if (!dst.stk || !src.stk) No(l, "frame object without its stack");
        auto tl = FieldPlace(src, fs.var, l);
        if (M.FoOk(tl.t)) {
            if (pre) tyg_move(p, dst.a, src.a, pre);
            FoCopy(FieldPlace(dst, fs.var, l), tl, l);
            return;
        }
        auto v = ViewOf(tl, l);
        // The tail's start and count first: the source may be anywhere.
        auto el = tyg_local_new(p, 24, 8);
        StL(el, v.elems);
        StL(el, v.len, 8);
        StL(el, RunBytes(v), 16);
        if (pre) tyg_move(p, dst.a, src.a, pre);
        auto dt = FieldPlace(dst, fs.var, l);
        auto len = AddrPlus(dt.a, 8);
        St(dt.a, Top(dst.stk));
        St(len, I64(0));
        CopyBytes(dst.stk, len, LdL(el), LdL(el, 16), LdL(el, 8));
    }

    // ---- Frame objects as bytes -----------------------------------------------------------------

    // Frame object `obj`'s fixed fields stored to the bytes at `at` (pads zeroed) or loaded from them.
    void FoBytesCopy(TypeExpr *t, tyg_addr obj, tyg_addr at, bool tobytes) {
        auto &fs = M.StFields(t);
        auto packed = [&](size_t i) { return AddrPlus(at, fs.off[i]); };
        for (size_t i = 0; i < (size_t)fs.var; i++) {
            if (fs.f[i].ispad) {
                if (tobytes && fs.f[i].padsize > 0) tyg_zero(p, packed(i), fs.f[i].padsize);
                continue;
            }
            auto n = cg.FixedSize(fs.t[i]);
            auto o = AddrPlus(obj, fs.coff[i]);
            if (tobytes) tyg_copy(p, packed(i), o, n);
            else tyg_copy(p, o, packed(i), n);
        }
        auto tail = fs.t[(size_t)fs.var];
        if (cg.IsFrameObj(tail))
            FoBytesCopy(tail, AddrPlus(obj, fs.coff[(size_t)fs.var]), AddrPlus(at, fs.pre), tobytes);
    }
    // The bytes frame object at src read as a C one (CodeGen::FoView): its innermost tail's header
    // points at the elements where they lie.
    P FoView(const P &src) {
        auto t = src.t;
        auto tmp = AtL(tyg_local_new(p, M.Size(t), 8));
        tyg_zero(p, tmp, M.Size(t));
        FoBytesCopy(t, tmp, src.a, false);
        auto hb = AddrPlus(tmp, M.FoHdrOff(t));
        St(hb, Add(AddrOf(src.a), I64(cg.FoBytesPrefix(t))));
        St(AddrPlus(hb, 8), Ld(AddrPlus(src.ohdr, 8)));
        return P { tmp, t, src.stk };
    }
    // A literal's fields before its tail field, then the tail (a frame object's fields as bytes, or
    // elements behind a header of its own, or a resizable ADT), its count stored at len.
    void LitWithTail(StructLit *sl, tyg_val stk, const TgModule::FieldSet &fs, int tail, tyg_addr len,
                     Line l) {
        RunLit(sl, stk, fs, (size_t)tail);
        if (Ended()) return;
        auto tt = fs.t[(size_t)tail];
        auto init = sl->InitFor(tail);
        if (!init && (M.FoOk(tt) || M.IsRb(tt))) No(l, "omitted tail");
        if (M.FoOk(tt)) GenFoBytes(init, stk, tt, len, l);
        else {   // Behind a header of its own, its count stored at len.
            auto th = NewHdr();
            TailHdr(init, stk, th, tt, l);
            if (!Ended()) St(len, Ld(CountAt(th)));
        }
    }
    // Builds n's value, of frame object type t, at the top of stk as the tail of a value that is not
    // one: its fields as bytes, its innermost tail's count into olen (CodeGen::GenFoAsBytes). Anything
    // but a literal is built as a frame object behind room left for the fields, stored there after.
    void GenFoBytes(Node *n, tyg_val stk, TypeExpr *t, tyg_addr olen, Line l) {
        if (BuildCtl(n, InPlace { 4, stk, t, olen }, l)) return;
        if (auto sl = Is<StructLit>(n); sl && sl->sinst && !sl->variant && cg.TEq(sl->exprtype, t)) {
            LitWithTail(sl, stk, M.StFields(t), M.StFields(t).var, olen, l);
            return;
        }
        auto at = Local(Top(stk));
        SetTop(stk, Add(LdL(at), I64(cg.FoBytesPrefix(t))));
        auto h = AtL(tyg_local_new(p, M.Size(t), 8));
        GenFo(n, P { h, t, stk }, l);
        if (Ended()) return;
        FoBytesCopy(t, h, AtV(LdL(at)), true);
        St(olen, Ld(AddrPlus(h, M.FoHdrOff(t) + 8)));
    }

    // ---- Resizable structs ----------------------------------------------------------------------
    // A struct with a variable-size prefix and a resizable tail (TgModule::RsOk) is one image on a data
    // stack, its header a gs_rhdr: the prefix packed as a variable-size struct's, the tail's elements
    // behind it, the count in the header.

    // A resizable struct built at the top of stk, its header at hdr (CodeGen::GenConstruct).
    void GenRs(Node *n, tyg_val stk, tyg_addr hdr, TypeExpr *t, Line l) {
        n = StripCopy(n);
        if (BuildCtl(n, InPlace { 2, stk, t, hdr }, l)) return;
        auto len = CountAt(hdr);
        if (auto sl = Is<StructLit>(n); sl && sl->sinst && !sl->variant && cg.TEq(sl->exprtype, t)) {
            auto start = Local(Top(stk));
            LitWithTail(sl, stk, M.StFields(t), M.StFields(t).last, len, l);
            if (!Ended()) St(hdr, LdL(start));
            return;
        }
        if (CallAt(n, InPlace { 2, stk, t, hdr })) return;
        auto pl = Deref(Place(n));
        if (!Ended()) CopyFrom(pl, InPlace { 2, stk, t, hdr }, l);
    }
    // The image at src copied to the top of stk, header at hdr, bytes as they lie.
    void RsCopy(tyg_val stk, tyg_addr hdr, const P &src, Line l) {
        auto &fs = M.StFields(src.t);
        auto last = fs.last;
        auto tail = fs.t[(size_t)last];
        auto from = Ld(src.a);
        auto n = Ld(AddrPlus(src.a, 8));
        // To where the elements start: past a frame object tail's fields.
        auto estart = FieldAddr(src.t, from, last);
        if (M.FoOk(tail)) estart = Add(estart, I64(cg.FoBytesPrefix(tail)));
        auto elem = IsGrow(tail) ? tail->arr->sub : cg.FoTailArr(tail)->arr->sub;
        auto bytes = Add(Sub(estart, from), Mul(n, I64(cg.FixedSize(elem))));
        St(hdr, PushBytes(stk, from, bytes));
        St(CountAt(hdr), n);
    }

    // ---- Variable-size structs ------------------------------------------------------------------

    // A variable-size struct (TgModule::VsOk) is a bytes value as a T[] is (§6.1): its locals,
    // parameters and results are an image on a stack, a uint8_t * at its start; what differs is here:
    // its size and its fields' addresses walk the variable-size fields before them.

    // The bytes of the image of type t at `at` (CodeGen::SizeX).
    tyg_val ImgSize(TypeExpr *t, tyg_val at) {
        if (cg.IsFix(t)) return I64(cg.FixedSize(t));
        // Zigzag has ULEB128's shape.
        if (TgModule::IsVarint(t) || TgModule::IsVr(t)) return UlebSize(at);
        if (M.IsVm(t)) {
            // The tag, and its variant's payload: a variable one walked, only on its own tag's path.
            auto ts = M.TagSize(t);
            return ByTag(t, LoadTag(AtV(at), t), [&](int vi) {
                return Add(ImgSize(cg.VariantType(t, vi), Add(at, I64(ts))), I64(ts));
            }, ts);
        }
        if (M.IsRc(t))
            return Add(Mul(LimCapOf(P { AtV(at), t }), I64(cg.FixedSize(t->arr->sub))), I64(8));
        if (IsVarArr(t)) {
            auto v = ViewOf(P { AtV(at), t }, Line {});
            return Add(Sub(v.elems, at), RunBytes(v));
        }
        return Sub(FieldAddr(t, at, -1), at);
    }
    // The address of field fi of the image of t (a struct or variant) at `at`, or its end where fi < 0
    // (CodeGen::FieldPtr): a constant offset until a variable-size field, then a cursor past each.
    tyg_val FieldAddr(TypeExpr *t, tyg_val at, int fi) {
        auto &fs = M.RunOfType(t);
        auto n = fi < 0 ? (int)fs.f.size() : fi;
        int64_t off = 0;
        auto cur = at;
        for (int i = 0; i < n; i++) {
            if (fs.f[(size_t)i].ispad) {
                off += TgModule::PadSize(fs.f[(size_t)i]);
                continue;
            }
            auto ft = fs.t[(size_t)i];
            if (cg.IsFix(ft)) {
                off += cg.FixedSize(ft);
                continue;
            }
            if (off) cur = Add(cur, I64(off));
            off = 0;
            cur = Add(cur, ImgSize(ft, cur));
        }
        return off ? Add(cur, I64(off)) : cur;
    }
    // Field fi of the struct or variant at base, whatever its representation: a fixed one at its C
    // offset; a bytes value's (image's) walked (CodeGen::FieldPtr, BytesLoc); a resizable's, behind the
    // header base.a, at its offset in the bytes at its base; a frame object's at its C offset, or
    // packed in bytes (base.bfo). A resizable's tail is a place of its own (CodeGen::FieldLocAt).
    // `read`: a tail needs no stack, and a resizable ADT's variant has one.
    P FieldPlace(const P &base, int fi, Line l, bool read = false) {
        auto t = base.t;
        if (t->kind != TY_STRUCT && t->kind != TY_VARIANT) No(l, "field of this type");
        auto &fs = M.RunOfType(t);
        auto ft = fs.t[(size_t)fi];
        auto tail = cg.IsResz(ft);
        auto plain = [&](tyg_val at) {
            P r { AtV(at), ft };
            r.enc = TgModule::IsVarint(ft);
            return r;
        };
        // The tail at `at` in bytes whose count is in the header hdr: a frame object as bytes, a
        // resizable's elements, or a resizable ADT through a header made for it (the struct's count,
        // which is not its own).
        auto tailat = [&](tyg_val at, tyg_addr hdr) {
            if (M.IsRb(ft)) {
                auto th = Slot16();
                StL(th, at);
                StL(th, Ld(CountAt(hdr)), 8);
                P r { AtL(th), ft, base.stk };
                r.tmphdr = true;
                r.ohdr = base.tmphdr ? base.ohdr : hdr;
                return r;
            }
            if (M.FoOk(ft)) {
                P r { AtV(at), ft, base.stk };
                r.bfo = true;
                r.ohdr = hdr;
                return r;
            }
            if (!base.stk && !read) No(l, "resizable without its stack");
            P r { hdr, ft, base.stk };
            r.rbase = at;
            return r;
        };
        if (base.tmphdr || M.RsOk(t) || (M.IsRb(t) && (t->kind == TY_VARIANT || M.RbOk(t)))) {
            if (tail && t->kind == TY_VARIANT && !read) No(l, "tail of a resizable variant");
            auto at = FieldAddr(t, Ld(base.a), fi);   // Behind the header's base.
            return tail ? tailat(at, base.a) : plain(at);
        }
        if (M.FoOk(t)) {
            if (base.bfo) {
                auto at = AddrPlus(base.a, fs.off[(size_t)fi]);
                return tail ? tailat(AddrOf(at), base.ohdr) : P { at, ft };
            }
            if (tail && !base.stk && !read) No(l, "frame object without its stack");
            return P { AddrPlus(base.a, fs.coff[(size_t)fi]), ft, tail ? base.stk : 0 };
        }
        if (!cg.IsFix(t)) {
            if (!M.VsOk(t) && !M.VvOk(t)) No(l, "field of this type");
            return plain(FieldAddr(t, AddrOf(base.a), fi));
        }
        if (!M.TypeOk(t)) No(l, "field of this type");
        return P { AddrPlus(base.a, fs.off[(size_t)fi]), ft };
    }

    // Copies the image of type t at src (maybe on stk, below its top) to the top of stk; its address.
    tyg_val ImgCopy(tyg_val stk, tyg_val src, TypeExpr *t) {
        return PushBytes(stk, src, ImgSize(t, src));
    }
    // A variant or fixed-mode ADT at `src` adapted to the variable-mode ADT t at the top of stk
    // (CodeGen::GenVarEnumFromLoc): the tag, then the payload; its address.
    tyg_val VmFrom(tyg_val stk, tyg_addr src, TypeExpr *srct, TypeExpr *t, Line l) {
        if (M.EI(srct) != M.EI(t) ||
            (srct->kind != TY_VARIANT && !(srct->kind == TY_ENUM && !srct->enu->varmode)))
            No(l, cat("value of type ", M.NameWhy(t)));
        auto start = Local(Top(stk));
        auto base = LdL(start);
        auto ts = M.TagSize(t);
        SetTop(stk, Add(base, I64(ts)));
        auto ei = M.EI(t);
        auto copyfix = [&](int64_t sz, tyg_addr from) {
            if (sz) tyg_copy(p, AtV(StackReserve(stk, I64(sz))), from, sz);
        };
        if (srct->kind == TY_VARIANT) {
            StoreTag(AtV(base), t, M.VariantIdx(srct));
            if (cg.IsFix(srct)) copyfix(cg.FixedSize(srct), src);
            else ImgCopy(stk, AddrOf(src), srct);
            return LdL(start);
        }
        auto tag = Local(LoadTag(src, srct));
        tyg_store(p, M.TagScal(t), AtV(base), tyg_convert(p, TYG_TRUNC, M.TagScal(t), LdL(tag)), 0);
        TagSwitch(tag, ei->en->variants.size(),
                  [&](size_t vi) { copyfix(cg.VariantLayout(ei, (int)vi).size, AddrPlus(src, ts)); });
        return LdL(start);
    }

    // `self` is a variant's value start while its fields are built (§3.9).
    struct SelfScope {
        tyg_val &slot, was;
        SelfScope(tyg_val &s, tyg_val now) : slot(s), was(s) { s = now; }
        ~SelfScope() { slot = was; }
    };

    // Builds n's value, of the variable-mode ADT t, at the top of stk (CodeGen::GenConstruct); its
    // address. A variant literal as its tag then its fields, a call handed stk, else a copy.
    tyg_val GenVm(Node *n, tyg_val stk, TypeExpr *t, Line l) {
        auto start = Local(Top(stk));
        auto at = [&] { return LdL(start); };
        if (CallAt(n, InPlace { 1, stk, t })) return at();
        auto sl = Is<StructLit>(n);
        auto d = Is<Dot>(n);
        auto var = sl && sl->variant ? sl->variant : d && d->variantconst ? d->variantconst : nullptr;
        if (!var) {
            auto pl = Deref(Place(n));
            if (!Ended()) CopyFrom(pl, InPlace { 1, stk, t }, l);
            return at();
        }
        auto ei = cg.EIOf(t);
        auto vi = ei->en->VariantIndex(var);
        auto vt = cg.VariantType(t, vi);
        auto fixed = cg.IsFix(vt);
        if (!fixed && !sl) No(l, cat("variant constant of type ", M.NameWhy(t)));
        // The tag, then the payload: a fixed one zero bytes, a literal's fields stored in place; a
        // variable one its fields built in turn.
        auto base = at();
        auto size = M.TagSize(t) + (fixed ? cg.FixedSize(vt) : 0);
        SetTop(stk, Add(base, I64(size)));
        if (fixed) tyg_zero(p, AtV(base), size);
        StoreTag(AtV(base), t, vi);
        if (!sl) return at();
        SelfScope scope(selfbase, base);
        if (!fixed) RunLit(sl, stk, M.VarFields(ei, vi));
        else EachLitInit(sl, [&](Node *x, TypeExpr *ft, int64_t o) {
            Into(P { AtV(at(), o), ft }, x);
        });
        return at();
    }

    // A variable-size struct's or variant's value at the top of stk: a literal's fields in turn, a call
    // handed stk, else a copy whole.
    tyg_val GenVs(Node *n, tyg_val stk, TypeExpr *t, Line l) {
        auto start = Local(Top(stk));
        auto isvv = t->kind == TY_VARIANT;
        if (!isvv) n = StripCopy(n);
        auto sl = Is<StructLit>(n);
        if (sl && cg.TEq(sl->exprtype, t) && (isvv || (sl->sinst && !sl->variant))) {
            RunLit(sl, stk, M.RunOfType(t));
        } else if (!CallAt(n, InPlace { 1, stk, t }, false)) {
            auto pl = Deref(Place(n));
            if (!Ended()) CopyFrom(pl, InPlace { 1, stk, t }, l);
        }
        return LdL(start);
    }
    // A literal's fields in layout order (CodeGen::GenFieldInits), each built at the top in turn: a
    // fixed one evaluated, then stored; a variable-size one built there; explicit padding zeroed.
    void RunLit(StructLit *sl, tyg_val stk, const TgModule::FieldSet &fs, size_t count = (size_t)-1) {
        for (size_t i = 0; i < fs.f.size() && i < count && !Ended(); i++) {
            auto ft = fs.t[i];
            auto init = sl->InitFor((int)i);
            if (fs.f[i].ispad) PushZero(stk, fs.f[i].padsize);
            else if (cg.IsFix(ft) && !init) PushZero(stk, cg.FixedSize(ft));
            else if (cg.IsFix(ft)) BuildField(stk, ft, init);
            else if (init) GenImg(init, stk, ft, init->line);
            else if (TgModule::IsVr(ft)) PushZero(stk, 1);   // An omitted optional: the one zero byte.
            else No(sl->line, "omitted variable-size field");
        }
    }

    // ---- The procedure --------------------------------------------------------------------------

    void Emit() {
        auto &si = cg.sinfo[sp];
        auto sym = M.Sym(si.cname, TYG_SYM_FUNC | TYG_SYM_GLOBAL);
        auto sig = M.Sig(sp);
        if (!sig) No(sp->sf->line, cat("signature: ", tyg_error(m) ? tyg_error(m) : "?"));
        NewProcBegin(sym, sig, sp->sf->line);
        auto file = M.File(sp->sf->line.fileidx);
        auto ptrto = [&](const tyg_dtype *dt) { return tyg_dt_pointer(m, dt); };
        vector<const tyg_dtype *> pdt;
        for (size_t i = 0; i < sp->argtypes.size(); i++) {
            auto t = sp->argtypes[i];
            if (cg.IsPoolParam(sp, i)) {
                for (int k = 0; k < 4; k++) pdt.push_back(ptrto(M.DT(M.ast.inttypes[IS_U8])));
                continue;
            }
            pdt.push_back((M.RzParam(t) && !M.Resz(t)) || cg.IsLargeFixed(t) ? ptrto(M.DT(t)) :
                          M.DT(t));
            if (M.RzParam(t) && M.Resz(t)) pdt.push_back(ptrto(M.StackDT()));
        }
        for (auto fv : si.freevars) {
            pdt.push_back(ptrto(M.DT(fv->type)));
            if (M.Resz(fv->type)) pdt.push_back(ptrto(M.StackDT()));
            if (fv->reusable) {
                pdt.push_back(ptrto(M.DT(fv->type)));
                pdt.push_back(ptrto(M.StackDT()));
            }
        }
        auto cret = si.cret;
        auto fdt = tyg_dt_func(m, cret < 0 ? tyg_dt_void(m) : M.DT(sp->rets[cret]), pdt.data(),
                               (int)pdt.size(), 0);
        tyg_debug_proc(p, string(sp->sf->qname).c_str(), file, sp->sf->line.line, fdt);
        Loc(sp->sf->line);
        int pi = 0;
        for (size_t i = 0; i < sp->argtypes.size(); i++) {
            auto t = sp->argtypes[i];
            pi += cg.IsPoolParam(sp, i) ? 4 : M.RzParam(t) && M.Resz(t) ? 2 : 1;
        }
        auto param = [&] { return Local(tyg_param(p, pi++)); };
        // Free variables (§7.5): the address of the variable in its frame, read through as a by-value
        // T[] parameter is; the debugger sees the variable itself (tyg_debug_var_ref).
        for (auto fv : si.freevars) {
            auto loc = param();
            locals[fv] = loc;
            indirect.insert(fv);
            DebugVar(tyg_debug_var_ref, fv, M.DT(fv->type), loc);
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
                // gs_pref { hdr, stk, fl, flstk }: a fat reference, then the freelist's header and
                // stack.
                auto loc = tyg_local_new(p, 32, 8);
                for (int k = 0; k < 4; k++) StL(loc, tyg_param(p, pi++), 8 * k);
                locals[vd] = prefs[vd] = loc;
                DebugVar(tyg_debug_param, vd, M.DT(t), loc);
                continue;
            }
            auto pv = tyg_param(p, pi++);
            if ((M.RzParam(t) && !M.Resz(t)) || cg.IsLargeFixed(t)) {
                // A by-value T[]'s image, a copy the caller made, or a large fixed value's caller's
                // copy: read through its address.
                auto loc = Local(pv);
                locals[vd] = loc;
                indirect.insert(vd);
                DebugVar(tyg_debug_param, vd, ptrto(M.DT(t)), loc);
                continue;
            }
            auto psize = M.RzParam(t) && (IsGrow(t) || M.IsRs(t)) ? 16 : M.Size(t);
            auto loc = tyg_local_new(p, psize, 8);
            locals[vd] = loc;
            if (M.IsScalar(t)) tyg_store(p, M.Scal(t), AtL(loc), pv, 0);
            else tyg_copy(p, AtL(loc), AtV(pv), psize);
            DebugVar(tyg_debug_param, vd, M.DT(t), loc);
            if (M.RzParam(t)) rzstk[vd] = param();   // A by-value T[>..]: its header, then its stack.
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
        if (si.needssp) {
            spl = param();
            hassp = true;
        }
        // Each destination's top on entry: a return builds its result there, also one nested in another
        // return's half-built value.
        for (size_t i = 0; i < sp->rets.size(); i++) {
            if (!NonFixedRet(sp->rets[i])) continue;
            chans[i].top0 = Local(Top(LdL(chans[i].a)));
        }
        NewProcBody();
        if (cg.fromids.count(sp)) {
            // A `return from` target: values in flight to it are built on its destinations (EmitSpec).
            for (size_t i = 0; i < sp->rets.size(); i++) {
                if (!NonFixedRet(sp->rets[i])) continue;
                auto at = ChanAt(sp, i, "fdst");
                auto sav = Local(Ld(at));
                St(at, LdL(chans[i].a));
                fdstsaves.emplace_back(at, sav);
            }
        }
        // The tail of a body with a result is returned as the C does (EmitSpec).
        auto body = sp->body;
        auto tail = body->tail;
        auto tailret = !sp->rets.empty() && tail && tail->exprtype && tail->exprtype->kind != TY_VOID &&
                       !(Is<IfExpr>(tail) && !Is<IfExpr>(tail)->elseb) && !Is<Return>(tail) &&
                       !Is<Break>(tail) && !Is<Continue>(tail);
        GenBlock(body, true, tailret);
        // The checker's rule: control cannot reach the end of a function with a result.
        if (!Ended()) {
            if (sp->rets.empty()) tyg_ret(p, 0);
            else tyg_trap(p);
        }
        Finish(sp->sf->line);
    }

    // The render procedure of t (CodeGen::EmitRenderFn): void(stk, &len, &value).
    void EmitRender(const string &name, TypeExpr *t) {
        auto ptr = tyg_type_ptr(m);
        const tyg_type *pts[3] = { ptr, ptr, ptr };
        NewProcBegin(M.Sym(name, TYG_SYM_FUNC | TYG_SYM_GLOBAL), tyg_sig_new(m, nullptr, pts, 3, 0),
                     {});
        // The entry block as a function's, for the globals a link reads (a pool's base).
        NewProcBody();
        inlinet = t;
        RenderInto(tyg_param(p, 0), AtV(tyg_param(p, 1)), P { AtV(tyg_param(p, 2)), t }, true, Line {});
        if (!Ended()) tyg_ret(p, 0);
        Finish(Line {});
    }

    // ---- The verifier of an image's elements ----------------------------------------------------
    // gs_tg_verify_<T>(p, n, bm) (CodeGen::VerifyFn): the element count of the image at p, or -1 if its
    // n bytes are not one. The walks mirror CodeGen::EmitVerifyWalk over a cursor `vq`; failure goes to
    // `vfail`.
    tyg_val vP = 0, vN = 0, vBM = 0, vE = 0;
    tyg_local vq = 0;
    tyg_block vfail = 0;
    TypeExpr *velem = nullptr;

    tyg_val VQ() { return LdL(vq); }
    void VSetQ(tyg_val x) { StL(vq, x); }
    void VAddQ(tyg_val x) { VSetQ(Add(VQ(), x)); }
    tyg_val VAt() { return Add(vP, VQ()); }
    // Fail where `a c b`, else go on.
    void VFail(tyg_cond c, tyg_val a, tyg_val b) { HitElse(Cmp(c, a, b), vfail); }
    void VNeed(int64_t bytes) { VFail(TYG_SLT, Sub(vN, VQ()), I64(bytes)); }   // Fewer bytes left.
    void VSkip(int64_t bytes) {
        VNeed(bytes);
        VAddQ(I64(bytes));
    }
    // The ULEB128 at q (gs_uleb_check), which must be one.
    Uleb VUleb() {
        auto uc = UlebCheck(VAt(), vE);
        VFail(TYG_EQ, LdL(uc.k), I64(0));
        return uc;
    }
    // body() while q < n.
    template <class F> void VWhileQ(F body) { WhileDo([&] { return Cmp(TYG_SLT, VQ(), vN); }, body); }
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

    // A self-relative reference at q holding `off` (CodeGen::EmitVerifyLink): it must reach the start
    // of an element, carrying the tag where typed at a variant.
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
            // Bound the untrusted offset before adding it.
            VFail(TYG_SLT, LdL(offl), tyg_un(p, TYG_NEG, LdL(v)));
            VFail(TYG_SGE, LdL(offl), Sub(vN, LdL(v)));
            StL(v, Add(LdL(v), LdL(offl)));
            if (cg.IsFix(velem)) {
                auto esz = I64(std::max<int64_t>(cg.FixedSize(velem), 1));
                VFail(TYG_NE, Bin(TYG_SREM, LdL(v), esz), I64(0));
            } else {   // GS_BM_GET(bm, v)
                auto byte = LoadN(Add(vBM, Bin(TYG_LSHR, LdL(v), I64(3))), 1, false);
                auto bit = Bin(TYG_AND, Bin(TYG_LSHR, byte, AndK(LdL(v), 7)), I64(1));
                VFail(TYG_EQ, bit, I64(0));
            }
            if (variant) {
                VFail(TYG_SLT, Sub(vN, LdL(v)), I64(skip));
                VFail(TYG_NE, LoadN(Add(vP, LdL(v)), skip, false), I64(vi));
            }
        };
        if (rt->ref->optional) If(Cmp(TYG_NE, LdL(offl), I64(0)), check);
        else check();
    }

    // The checks and cursor advance for one value of type t at q (CodeGen::EmitVerifyWalk).
    void VWalk(TypeExpr *t, bool links) {
        if (!cg.NeedsVerifyWalk(t)) {
            if (cg.FixedSize(t) > 0) VSkip(cg.FixedSize(t));
            return;
        }
        auto walk = [this, links](TypeExpr *e) { return [=](tyg_val) { VWalk(e, links); }; };
        switch (t->kind) {
            case TY_BOOL:
                VNeed(1);
                VFail(TYG_UGT, LoadN(VAt(), 1, false), I64(1));
                VAddQ(I64(1));
                return;
            case TY_INT:   // A varint: only its length matters.
                VAddQ(LdL(VUleb().k));
                return;
            case TY_REF: {
                auto w = (IntStorage)t->ref->lenstorage;
                auto off = Slot(), k = Slot();
                if (w == IS_VARINT) {
                    auto uc = VUleb();
                    StL(k, LdL(uc.k));
                    if (links) {   // gs_zig_check
                        auto u = LdL(uc.v), neg = tyg_un(p, TYG_NEG, AndK(u, 1));
                        StL(off, Bin(TYG_XOR, Bin(TYG_LSHR, u, I64(1)), neg));
                    }
                } else {
                    VNeed(CodeGen::IntSize(w));
                    StL(k, I64(CodeGen::IntSize(w)));
                    if (links) StL(off, LoadN(VAt(), CodeGen::IntSize(w), !t->ref->pool));
                }
                if (links) VLink(t, LdL(off));
                VAddQ(LdL(k));
                return;
            }
            case TY_ARRAY: {
                auto &a = *t->arr;
                auto nl = Slot();
                switch (a.akind) {
                    case A_FIXED:
                        Loop(I64(0), I64(cg.ArrSize(t->arr)), walk(a.sub));
                        return;
                    case A_VAR: {
                        auto ls = cg.LenStore(t->arr);
                        if (ls == IS_VARINT) {
                            auto uc = VUleb();
                            VFail(TYG_SLT, LdL(uc.v), I64(0));   // above INT64_MAX
                            StL(nl, LdL(uc.v));
                            VAddQ(LdL(uc.k));
                        } else {
                            VNeed(CodeGen::IntSize(ls));
                            StL(nl, LoadN(VAt(), CodeGen::IntSize(ls), false));
                            VAddQ(I64(CodeGen::IntSize(ls)));
                        }
                        auto per = I64(std::max<int64_t>(cg.MinBytes(a.sub), 1));
                        VFail(TYG_SLT, LdL(nl), I64(0));
                        VFail(TYG_SGT, LdL(nl), Bin(TYG_SDIV, Sub(vN, VQ()), per));
                        if (!cg.NeedsVerifyWalk(a.sub)) VAddQ(Mul(LdL(nl), I64(cg.FixedSize(a.sub))));
                        else Loop(I64(0), LdL(nl), walk(a.sub));
                        return;
                    }
                    case A_LIMITED: {
                        // Only the live slots were written, so only they are walked; q skips the
                        // capacity.
                        auto s = Local(VQ()), cap = Slot();
                        auto stat = cg.IsFix(t);
                        if (stat) {
                            auto ls = CodeGen::IntSize(cg.LenStore(t->arr));
                            VNeed(cg.FixedSize(t));
                            StL(nl, LoadN(VAt(), ls, false));
                            VFail(TYG_SLT, LdL(nl), I64(0));
                            VFail(TYG_SGT, LdL(nl), I64(cg.ArrSize(t->arr)));
                            VAddQ(I64(ls));
                        } else {
                            VNeed(8);
                            StL(cap, LoadN(VAt(), 4, false));
                            StL(nl, LoadN(Add(VAt(), I64(4)), 4, false));
                            VFail(TYG_SGT, LdL(nl), LdL(cap));
                            auto esz = I64(std::max<int64_t>(cg.FixedSize(a.sub), 1));
                            VFail(TYG_SGT, LdL(cap), Bin(TYG_SDIV, Sub(Sub(vN, VQ()), I64(8)), esz));
                            VAddQ(I64(8));
                        }
                        if (cg.NeedsVerifyWalk(a.sub)) Loop(I64(0), LdL(nl), walk(a.sub));
                        auto cb = Mul(LdL(cap), I64(cg.FixedSize(a.sub)));
                        VSetQ(Add(LdL(s), stat ? I64(cg.FixedSize(t)) : Add(I64(8), cb)));
                        return;
                    }
                    default: No(Line {}, "verifier of this array");
                }
                return;
            }
            case TY_STRUCT: case TY_VARIANT: {   // The fields (CodeGen::EmitVerifyFields).
                // A fixed one's offsets may include padding a walk would miss; bytes lay out in turn.
                auto &fs = M.RunOfType(t);
                auto fix = cg.IsFix(t);
                tyg_local start = fix ? Local(VQ()) : 0;
                if (fix) VNeed(cg.FixedSize(t));
                for (size_t i = 0; i < fs.f.size(); i++) {
                    if (fs.f[i].ispad) {
                        if (!fix && fs.f[i].padsize > 0) VAddQ(I64(fs.f[i].padsize));
                        continue;
                    }
                    if (fix && !cg.NeedsVerifyWalk(fs.t[i])) continue;
                    if (fix) VSetQ(Add(LdL(start), I64(fs.off[i])));
                    VWalk(fs.t[i], links);
                }
                if (fix) VSetQ(Add(LdL(start), I64(cg.FixedSize(t))));
                return;
            }
            case TY_ENUM: {   // The tag is range-checked in both modes (a match has no default).
                auto ei = cg.EIOf(t);
                auto tsz = CodeGen::IntSize(cg.TagStore(ei->en));
                auto nv = (int64_t)ei->en->variants.size();
                auto fix = !t->enu->varmode;
                auto s = Local(VQ()), tag = Slot();
                auto after = NewBlock();
                VNeed(fix ? cg.FixedSize(t) : tsz);
                StL(tag, LoadN(VAt(), tsz, false));
                VFail(TYG_UGE, LdL(tag), I64(nv));
                VAddQ(I64(tsz));
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
        NewProcBegin(M.Sym(name, TYG_SYM_FUNC | TYG_SYM_GLOBAL), tyg_sig_new(m, i64t, pts, 3, 0), {});
        NewProcBody();
        velem = elem;
        vP = tyg_param(p, 0);
        vN = tyg_param(p, 1);
        vBM = tyg_param(p, 2);
        vE = Add(vP, vN);
        vfail = NewBlock();
        // Zero-byte elements have no recoverable count: only the empty image is an array of them.
        if (cg.MinBytes(elem) <= 0) {
            tyg_ret(p, tyg_select(p, Cmp(TYG_EQ, vN, I64(0)), I64(0), I64(-1)));
        } else {
            auto fixed = cg.IsFix(elem);
            auto rel = cg.HasRelRefAny(elem);
            vq = Slot();
            auto cnt = Local(I64(0));
            VSetQ(I64(0));
            VWhileQ([&] {
                if (!fixed && rel) {   // GS_BM_SET(bm, q)
                    auto at = Add(vBM, Bin(TYG_LSHR, VQ(), I64(3)));
                    auto bit = Bin(TYG_SHL, I64(1), AndK(VQ(), 7));
                    auto old = LoadN(at, 1, false);
                    StoreByte(at, Bin(TYG_OR, old, bit));
                }
                VWalk(elem, fixed && rel);
                StL(cnt, Add(LdL(cnt), I64(1)));
            });
            VFail(TYG_NE, VQ(), vN);
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

    // The entry block (gblk): the addresses of the globals and the stack block, then on to the body.
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

    // gs_init_globals for a whole program build (CodeGen::EmitGlobalInit): the initializers in order
    // with gs_sp fixed at 0, a resizable or T[] global built on its own stack. No sp: what refers to
    // the function's own results (return, return from) is unsupported.
    void EmitInit(tyg_sym sym, Line line) {
        NewProcBegin(sym, tyg_sig_new(m, nullptr, nullptr, 0, 0), line);
        tyg_debug_proc(p, "gs_init_globals", M.File(line.fileidx), line.line,
                       tyg_dt_func(m, tyg_dt_void(m), nullptr, 0, 0));
        Loc(line);
        spl = Local(I64(0));
        hassp = true;
        NewProcBody();
        auto stackinit = [&](tyg_val stk) {
            M.NeedShim("gs_tg_stack_init");
            CallC("gs_tg_stack_init", { tyg_type_ptr(m) }, { stk }, nullptr);
        };
        auto emptyhdr = [&](tyg_val at, tyg_val stk) {
            St(AtV(at), Top(stk));
            St(AtV(at, 8), I64(0));
        };
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
                    stackinit(stk);
                    if (fo) {   // A frame object: its tail on the global's own stack.
                        GenFo(init, P { AtV(at), d->type, stk }, init->line);
                    } else if (grow) {   // CodeGen::InitGlobalStack: an empty array, then its elements.
                        emptyhdr(at, stk);
                        GenRz(init, stk, AtV(at), d->type, init->line);
                        if (d->reusable && !Ended()) {   // Its freelist, empty, on a stack of its own.
                            auto [h, st] = M.PoolAccessors(d);
                            auto flstk = LdL(AccessorLocal(st));
                            stackinit(flstk);
                            emptyhdr(LdL(AccessorLocal(h)), flstk);
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

// The backend: chooses the specializations it takes as CodeGen asks, then compiles those into one
// object.
struct TinyGenBackend : CodeGen::SpecBackend {
    Ast &ast;
    bool checks;
    string source;
    vector<FnSpec *> taken;
    vector<pair<FnSpec *, string>> left;   // Specializations left to C, and why.
    unique_ptr<TgModule> mod;
    string_view runtime_ext_text;
    string runtime_decls;   // runtime.h and runtime_ext.h (TgModule::RuntimeApi).
    // Left to C whatever they hold (--tinygen-skip): what tests calls across the two halves with.
    vector<string> skip;

    TinyGenBackend(Ast &_ast, bool _checks, const string &_source, string_view _ext, string _decls)
        : ast(_ast), checks(_checks), source(_source), runtime_ext_text(_ext),
          runtime_decls(std::move(_decls)) {}

    // What a procedure asked of the module for the C half, to take back.
    static string Why(const TgUnsupported &u) {
        return u.line.line > 0 ? cat(u.why, " (line ", u.line.line, ")") : u.why;
    }
    // Runs emit() building fn's procedure: "" when it compiled, else why not. A construct not supported
    // anywhere in the body abandons the procedure (tyg_proc_abort) and takes back what it asked of the
    // module for the C half (shims, trampolines), so the whole body goes to C.
    template <class F> string Attempt(TgFn &fn, F emit) {
        auto nshims = mod->shims.size(), nexterns = mod->externs.size(), ndirect = mod->direct.size();
        try {
            emit();
        } catch (TgUnsupported &u) {
            if (fn.p) tyg_proc_abort(fn.p);
            mod->shims.resize(nshims);
            mod->externs.resize(nexterns);
            mod->direct.resize(ndirect);
            return Why(u);
        }
        return "";
    }

    tyg_sym FSym(const string &name) { return mod->Sym(name, TYG_SYM_FUNC | TYG_SYM_GLOBAL); }
    tyg_sig *SigOf(const tyg_type *ret, const vector<const tyg_type *> &ps) {
        return tyg_sig_new(mod->m, ret, ps.data(), (int)ps.size(), 0);
    }
    void EndProc(tyg_proc *p, const string &name) {
        if (tyg_proc_end(p) == 0) return;
        throw CompileError { cat("tinygen: ", name, ": ", tyg_error(mod->m) ? tyg_error(mod->m) :
                                 "?") };
    }
    tyg_val CallSig(tyg_proc *p, tyg_sig *sig, tyg_sym fn, const vector<tyg_val> &vals) {
        vector<tyg_arg> as;
        for (auto v : vals) as.push_back(TgFn::ArgOfVal(v));
        return tyg_call(p, sig, tyg_direct(fn), as.data(), (int)as.size(), nullptr, nullptr);
    }
    // A call of the object's procedure `name`.
    tyg_val CallProc(tyg_proc *p, const string &name, const tyg_type *ret,
                     const vector<const tyg_type *> &ps,
                     const vector<tyg_val> &vals) {
        return CallSig(p, SigOf(ret, ps), FSym(name), vals);
    }
    tyg_val Acc(tyg_proc *p, const string &name) {
        return CallProc(p, name, tyg_type_ptr(mod->m), {}, {});
    }
    // A zero-filled data symbol, thread-local if tls.
    tyg_sym DataSym(const string &name, int64_t size, int64_t align, bool tls) {
        auto sym = tyg_symbol(mod->m, name.c_str(), tls ? TYG_SYM_TLS : 0);
        tyg_data(mod->m, sym, tls ? TYG_SECTION_TBSS : TYG_SECTION_BSS, nullptr,
                 std::max<int64_t>(size, 1), align);
        return sym;
    }
    // An accessor `name`: a procedure returning the address of `target`.
    void DefAccessor(const string &name, tyg_sym target) {
        auto p = tyg_proc_begin(mod->m, FSym(name), SigOf(tyg_type_ptr(mod->m), {}));
        tyg_ret(p, tyg_addr_of(p, tyg_at_sym(target, 0)));
        EndProc(p, name);
    }
    // The body of a wrapper p of sp passing its parameters on to `callee` (gs_sp 0 after them if
    // zerosp) and returning the result; a slice as the C wrapper gives it: the base, its length stored
    // through an extra out-pointer parameter.
    void ForwardCall(tyg_proc *p, FnSpec *sp, tyg_sig *sig, tyg_sym callee, bool zerosp, bool slice) {
        vector<tyg_arg> args;
        for (size_t i = 0; i < sp->argtypes.size(); i++) {
            tyg_arg a {};
            if (mod->IsScalar(sp->argtypes[i])) a.val = tyg_param(p, (int)i);
            else a.mem = tyg_at_val(tyg_param(p, (int)i), 0);
            args.push_back(a);
        }
        if (zerosp) args.push_back(TgFn::ArgOfVal(tyg_iconst(p, TYG_I64, 0)));
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
                auto out = tyg_at_val(tyg_param(p, (int)sp->argtypes.size()), 0);
                tyg_store(p, TYG_I64, out, tyg_load(p, TYG_I64, tyg_at_local(t, 8), 0), 0);
                tyg_ret(p, tyg_load(p, TYG_I64, tyg_at_local(t, 0), 0));
            } else {
                tyg_ret_mem(p, ra);
            }
        }
    }

    // Builds sp into the module: "" when it compiled, else why not. A body that compiled tells the C
    // half's stack accounting (codegen_stacks.h) the stacks it owns and what it hands its callees.
    string Build(CodeGen &cg, FnSpec *sp) {
        string why;
        // A simd function is its baseline version here (§7.12).
        if (!mod->SigOk(sp, true, why)) return cat("interface: ", why);
        TgFn fn(*mod, sp);
        if (auto w = Attempt(fn, [&] { fn.Emit(); }); !w.empty()) return w;
        cg.stackown[sp] = fn.maxown;
        cg.stackcalls[sp] = fn.stackcalls;
        return "";
    }

    // The specializations whose body compiles, built straight into the module; the rest are left to C.
    // The halves call each other with gs_sp as two C functions do, so what one takes does not depend on
    // the other.
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

    // The render procedures the functions asked for (TgModule::RenderFn), each possibly asking for
    // more.
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

    // After the C half is made: the extern fns the object calls count as used, so it declares them.
    set<FnSpec *> declared;   // Extern fns the C half declares already.

    void Compile(CodeGen &cg) {
        if (!mod) return;
        declared.insert(cg.usedexterns.begin(), cg.usedexterns.end());
        for (auto list : { &mod->externs, &mod->direct })
            for (auto sp : *list)
                if (std::find(cg.usedexterns.begin(), cg.usedexterns.end(), sp) == cg.usedexterns.end())
                    cg.usedexterns.push_back(sp);
    }

    // The C the object calls into (appended to the C half): the shims, and a trampoline per extern fn.
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
    // nothing asked of the C half that the support object (TgSupportBody) or the object does not hold
    // (globals, `return from` channels of fixed types, extern trampolines), and a plain main. Else why
    // not.
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
            if (!ExternHeld(sp)) return Refuse(why, cat("extern ", sp->sf->cname));
        if (!GlobalsOk(cg, why)) return false;
        // An unused gs_init_globals stays in the object, with what it asked of the C half.
        if (!cg.noglobalinit && !BuildInit(cg, why)) return false;
        BuildRenders();
        for (auto &sh : mod->shims)
            if (TgFixedShim(sh.first).empty() && !OwnAccessor(sh.first)) {
                initsym = TYG_NO_SYM;
                return Refuse(why, cat("C half's ", sh.first));
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

    // Whether a whole program can call an extern fn through its gs_tg_x_ trampoline: the support object
    // holds one (TgSupportExterns), or the object does (ForwardExterns) for a symbol no runtime header
    // defines. Sets the reason a whole-program build is not possible; false.
    static bool Refuse(string &why, string reason) {
        why = std::move(reason);
        return false;
    }
    bool ExternHeld(FnSpec *sp) {
        auto &name = sp->sf->cname;
        for (auto &x : TgSupportExterns())
            if (name == x.name) return true;
        return mod->rtdecls.find(cat(" ", name, "(")) == string_view::npos &&
               mod->rtdecls.find(cat("*", name, "(")) == string_view::npos;
    }

    // The trampolines the object holds itself: gs_tg_x_<name> calls the C symbol with the same
    // parameters.
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

    // The queues (eight bytes of data with an accessor, started by main) and the worker thunks the C
    // half holds in a mixed build: a thunk takes the spawn image, opens the thread program's data
    // stacks, unpacks the arguments and calls the worker (CodeGen::EmitThreadThunk).
    void EmitThreads(CodeGen &cg) {
        auto m = mod->m;
        auto ptr = tyg_type_ptr(m);
        for (auto &[acc, q] : mod->queueacc) DefAccessor(acc, DataSym(cat("gs_", q), 8, 8, false));
        for (auto &[acc, sp] : mod->thunkacc) {
            auto &ki = cg.sinfo[sp];
            auto tsym = FSym(cg.thunks[sp]);
            auto p = tyg_proc_begin(m, tsym, SigOf(nullptr, { ptr }));
            TgFn fn(*mod, nullptr);   // Its IR shorthands, and the sizes of image arguments.
            fn.p = p;
            auto cur = fn.Local(tyg_param(p, 0));   // The packet, read past each argument and global.
            auto ld = [&](tyg_val a, int64_t off = 0) { return fn.Ld(fn.AtV(a, off)); };
            auto st = [&](tyg_val a, int64_t off, tyg_val v) { fn.St(fn.AtV(a, off), v); };
            auto add = [&](tyg_val a, int64_t k) { return fn.Add(a, fn.I64(k)); };
            auto stackinit = [&](tyg_val stk) {
                mod->NeedShim("gs_tg_stack_init");
                CallProc(p, "gs_tg_stack_init", nullptr, { ptr }, { stk });
            };
            // The array at `at`, [int64 size][count][pre bytes of fields][n bytes of elements], into
            // the header at dst (+ pre), its elements onto the stack whose top cell is at `stk`.
            auto unpack = [&](tyg_val at, tyg_addr dst, tyg_val stk, int64_t pre, tyg_val n) {
                auto top = ld(stk);
                auto hdr = dst, hlen = dst;
                hdr.offset += pre;
                hlen.offset += pre + 8;
                fn.St(hdr, top);
                fn.St(hlen, ld(at, 8));
                if (pre) tyg_copy(p, dst, fn.AtV(at, 16), pre);
                tyg_move_dyn(p, top, add(at, 16 + pre), n);
                st(stk, 0, fn.Add(top, n));
            };
            CallProc(p, "gs_tg_stack_block", nullptr, { tyg_type_int(m, TYG_I64, 1) },
                     { fn.I64(cg.ProgramStacks({ sp })) });
            vector<tyg_arg> args;
            for (auto t : sp->argtypes) {
                auto at = fn.LdL(cur);
                tyg_arg a {};
                tyg_val size;
                if (mod->Resz(t)) {   // A header (or the frame object) and a stack of the thunk's own.
                    auto fo = mod->FoOk(t);
                    auto pre = fo ? mod->FoHdrOff(t) : 0;
                    auto osz = fo ? mod->Size(t) : 16;
                    auto obj = fn.AtL(tyg_local_new(p, osz, 8));
                    auto stk = tyg_addr_of(p, fn.AtL(tyg_local_new(p, 8, 8)));
                    stackinit(stk);
                    tyg_zero(p, obj, osz);
                    auto sz = ld(at);
                    unpack(at, obj, stk, pre, fn.Sub(sz, fn.I64(8 + pre)));
                    a.mem = obj;
                    args.push_back(a);
                    args.push_back(TgFn::ArgOfVal(stk));
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
            // The worker's globals, each [int64 size][bytes], into this thread's instance of them.
            for (auto d : cg.ThreadGlobals(sp)) {
                auto at = fn.LdL(cur);
                if (cg.IsFix(d->type)) {
                    auto size = cg.FixedSize(d->type);
                    if (size)
                        tyg_copy(p, fn.AtV(Acc(p, mod->accessors.at(d))), fn.AtV(add(at, 8)), size);
                    fn.StL(cur, add(at, 8 + size));
                    continue;
                }
                auto sz = ld(at), img = add(at, 8);
                auto gaddr = Acc(p, mod->accessors.at(d)), stk = Acc(p, mod->stkaccessors.at(d));
                auto fo = mod->FoOk(d->type);
                stackinit(stk);
                if (fo || mod->Resz(d->type)) {
                    // A pool: [count][elements][freelist count][entries].
                    auto pre = fo ? mod->FoHdrOff(d->type) : 0;
                    auto pool = !fo && d->reusable;
                    auto n = pool ? fn.Mul(ld(img), fn.I64(cg.FixedSize(d->type->arr->sub)))
                                  : fn.Sub(sz, fn.I64(8 + pre));
                    unpack(at, fn.AtV(gaddr), stk, pre, n);
                    if (pool) {
                        auto [fh, fs] = mod->PoolAccessors(d);
                        auto fl = Acc(p, fh), flstk = Acc(p, fs);
                        stackinit(flstk);
                        auto ftop = ld(flstk), fat = fn.Add(img, add(n, 8)), fcnt = ld(fat);
                        auto fbytes = fn.Mul(fcnt, fn.I64(TgFn::FlEntry(d)));
                        st(fl, 0, ftop);
                        st(fl, 8, fcnt);
                        tyg_move_dyn(p, ftop, add(fat, 8), fbytes);
                        st(flstk, 0, fn.Add(ftop, fbytes));
                    }
                } else {   // A T[]: the instance holds its address.
                    auto top = ld(stk);
                    st(gaddr, 0, top);
                    tyg_move_dyn(p, top, img, sz);
                    st(stk, 0, fn.Add(top, sz));
                }
                fn.StL(cur, fn.Add(img, sz));
            }
            if (ki.needssp) args.push_back(TgFn::ArgOfVal(fn.I64(0)));
            tyg_call(p, mod->Sig(sp), tyg_direct(FSym(ki.cname)), args.data(), (int)args.size(),
                     nullptr, nullptr);
            tyg_ret(p, 0);
            EndProc(p, cg.thunks[sp]);
            DefAccessor(acc, tsym);
        }
    }

    // Whether the object writes an `export fn`'s wrapper (CodeGen::EmitExports): a plain specialization
    // whose parameters and result cross by value. Else why not.
    bool ExportOk(CodeGen &cg, SFunction *sf, string &why) {
        auto fail = [&] { return Refuse(why, "export fns"); };
        if (sf->specs.size() != 1 || !cg.sinfo.count(sf->specs[0])) return fail();
        auto sp = sf->specs[0];
        auto &si = cg.sinfo[sp];
        if (!si.freevars.empty() || sp->rets.size() > 1 || !mod->SigOk(sp, false, why)) return fail();
        for (size_t i = 0; i < sp->argtypes.size(); i++) {
            auto t = sp->argtypes[i];
            if (cg.IsPoolParam(sp, i) || mod->RzParam(t) || cg.IsLargeFixed(t) || !mod->TypeOk(t))
                return fail();
        }
        if (!sp->rets.empty() && (si.cret != 0 || !mod->TypeOk(sp->rets[0]))) return fail();
        return true;
    }

    // The C symbol of each `export fn` (CodeGen::EmitExports), calling it with gs_sp 0.
    void EmitExports(CodeGen &cg) {
        auto ptr = tyg_type_ptr(mod->m);
        for (auto sf : ast.functions) {
            if (!sf->isexport) continue;
            auto sp = sf->specs[0];
            auto rt = sp->rets.empty() ? nullptr : sp->rets[0];
            auto slice = rt && rt->kind == TY_SLICE;
            vector<const tyg_type *> ps;
            for (auto t : sp->argtypes) ps.push_back(mod->AT(t));
            if (slice) ps.push_back(ptr);
            auto sig = SigOf(!rt ? nullptr : slice ? ptr : mod->AT(rt), ps);
            auto p = tyg_proc_begin(mod->m, FSym(sf->cname), sig);
            ForwardCall(p, sp, mod->Sig(sp), FSym(cg.sinfo[sp].cname), cg.sinfo[sp].needssp, slice);
            EndProc(p, sf->cname);
        }
    }

    // ---- Globals of a whole program -------------------------------------------------------------
    // With no C half the object holds the globals: each member of gs_globals_t a zero-filled data
    // symbol under its Goose name, a resizable's or T[]'s dedicated stack another beside it; `let` data
    // is the object's already (TgModule::StaticGlobal). There are no threads, so main's instance is the
    // only one. Functions reach a global through its accessor as in a mixed build, which the object now
    // defines.

    // Whether every global is one the object can hold: else why not.
    bool GlobalsOk(CodeGen &cg, string &why) {
        for (auto g : ast.globals)
            for (auto d : g->defs) {
                // Its bytes are not laid out: no accessor can reach it.
                bool stat = cg.gstatic.count(d);
                if (stat ? mod->accessors.count(d) > 0 : !mod->GlobalOk(d))
                    return Refuse(why, cat("global ", d->name,
                                           stat ? " as static data" : " of this kind"));
            }
        return true;
    }

    // Whether the object defines a program-specific shim itself: an accessor or a `return from`
    // channel.
    bool OwnAccessor(const string &name) {
        if (mod->channels.count(name) || mod->queueacc.count(name) || mod->thunkacc.count(name))
            return true;
        auto has = [&](auto &table) {
            for (auto &[d, n] : table)
                if (n == name) return true;
            return false;
        };
        for (auto &[d, n] : mod->flaccessors)
            if (n.first == name || n.second == name) return true;
        return has(mod->accessors) || has(mod->stkaccessors);
    }

    // gs_init_globals into the object, if its initializers compile and need no more data stacks than
    // the C's (stackneed[nullptr]). Else why not.
    tyg_sym initsym = TYG_NO_SYM;
    int64_t initstacks = 0;   // The data stacks gs_tg_init_globals uses at once.
    bool BuildInit(CodeGen &cg, string &why) {
        auto sym = mod->Sym("gs_tg_init_globals", TYG_SYM_FUNC);
        TgFn fn(*mod, nullptr);
        Line line;
        for (auto g : ast.globals)
            if (!g->inits.empty()) {
                line = g->line;
                break;
            }
        if (auto w = Attempt(fn, [&] { fn.EmitInit(sym, line); }); !w.empty())
            return Refuse(why, cat("global initializer: ", w));
        int64_t need = fn.maxown;
        for (auto &c : fn.stackcalls) need = std::max(need, c.offset + cg.stackneed[c.callee]);
        // This one's temporaries may need more than the C's count; main asks the runtime for them.
        if (need > cg.rtinit[0] && cg.maxstacks > 0 && need > cg.maxstacks)
            return Refuse(why, "global initializers' data stacks");
        initstacks = need;
        for (auto sp : mod->externs)   // Extern fns: as for any function of a whole program.
            if (!ExternHeld(sp)) return Refuse(why, cat("extern ", sp->sf->cname));
        initsym = sym;
        return true;
    }

    // The globals' storage, their debug entries, and the accessors the functions call.
    void EmitGlobalStorage(CodeGen &cg) {
        auto m = mod->m;
        map<const VarDef *, tyg_sym> gsyms, ssyms;
        map<const VarDef *, pair<tyg_sym, tyg_sym>> flsyms;
        // A thread program's instance of the globals it names is thread-local (§11.2); its thunk fills
        // it.
        auto tls = false;
        for (auto &[acc, sp] : mod->thunkacc) tls = tls || !cg.ThreadGlobals(sp).empty();
        auto def = [&](const string &name, int64_t size, int64_t align, VarDef *d,
                       const tyg_dtype *dt) {
            auto sym = DataSym(cat("gs_g_", name), size, align, tls);
            if (dt)
                tyg_debug_global(m, string(d->name).c_str(), dt, sym, mod->File(d->line.fileidx),
                                 d->line.line);
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
                    flsyms[d] = { def(cat(name, "_fl"), 16, 8, d, nullptr),
                                  def(cat("gs_gstk_", name, "_fl"), 8, 8, d, nullptr) };
            }
        for (auto &[d, n] : mod->accessors) DefAccessor(n, gsyms.at(d));
        for (auto &[d, n] : mod->stkaccessors) DefAccessor(n, ssyms.at(d));
        for (auto &[d, n] : mod->flaccessors) {
            DefAccessor(n.first, flsyms.at(d).first);
            DefAccessor(n.second, flsyms.at(d).second);
        }
        // The `return from` channels: thread-local in a program with threads, as in the C half.
        auto threaded = cg.usesthreads || !cg.queues.empty();
        for (auto &sh : mod->shims) {
            auto it = mod->channels.find(sh.first);
            if (it == mod->channels.end()) continue;
            DefAccessor(sh.first, DataSym(cat("gs_", sh.first.substr(6)), it->second, 8, threaded));
        }
    }

    // The C main of a whole program (CodeGen::EmitProgramInit, EmitMain): the runtime started with the
    // program's stack counts, the globals initialized, fn main() run. In the debugger it is at fn
    // main().
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
        // The main program's stacks, or what the initializers use if more.
        auto stacks = std::max(cg.rtinit[0], initstacks);
        CallProc(p, "gs_tg_start", nullptr, { i32, ptr, i64, i64, i64 },
                 { tyg_param(p, 0), tyg_param(p, 1), I(stacks), I(cg.rtinit[1] + stacks - cg.rtinit[0]),
                   I(cg.rtinit[2]) });
        for (auto &[acc, q] : mod->queueacc) CallProc(p, "gs_qinit", nullptr, { ptr }, { Acc(p, acc) });
        if (initsym != TYG_NO_SYM) CallSig(p, SigOf(nullptr, {}), initsym, {});
        auto &mi = cg.sinfo[mainsp];
        CallSig(p, mod->Sig(mainsp), FSym(mi.cname), mi.needssp ? vector<tyg_val> { I(0) } :
                vector<tyg_val> {});
        CallProc(p, "gs_tg_end", nullptr, {}, {});
        tyg_ret(p, tyg_iconst(p, TYG_I32, 0));
        EndProc(p, "main");
    }

    bool Write(const string &path, string &err) {
        if (tyg_module_write(mod->m, path.c_str()) == 0) return true;
        err = tyg_error(mod->m) ? tyg_error(mod->m) : "?";
        return false;
    }
};

}  // namespace goose
#endif
