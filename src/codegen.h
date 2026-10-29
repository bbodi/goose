// Goose compiler -- the program analyses and type services the backend (tinygen_backend.h)
// compiles from. Goose's own C generator lived here; what is left of it is what tinygen's
// procedures, the export header and the C units the driver writes around tinygen's C still read:
//
// * the live specializations and their call interfaces (CollectSpecs: C names, free variables,
//   whether a body takes gs_sp, `return ... from` targets), the globals' names, dedicated data
//   stacks and static initializers (NameGlobals, StaticInitX), the globals a thread program copies;
// * the size classes and packed layouts of the types (Appendix C: IsFix, FixedSize, FieldRuns,
//   LayoutFields, the tags of ADTs), their mangled identities (Mangle), and their C spellings
//   (CT), which the extern unit, an --include header and the export header are written against
//   (codegen_types.h);
// * the static count of data stacks the main program and each worker use at once, from what the
//   backend's bodies report (codegen_stacks.h);
// * the shared libraries the dynamic externs bind from (DlLibs).
//
// Members are defined across codegen_types.h, codegen_program.h and codegen_stacks.h.
#pragma once

namespace goose {

struct CodeGen {
    Ast &ast;

    string tdecls;      // The C types CT has written, packed.
    // The globals a thread program uses, in declaration order: what a spawn
    // copies into the worker's instance (TinyGenBackend's spawn and thunk).
    unordered_map<FnSpec *, vector<VarDef *>> threadglobals;
    vector<VarDef *> &ThreadGlobals(FnSpec *entry);
    // Extern fns called by live code, in the order their first call is
    // emitted: the prototypes follow it, and a set of pointers would put
    // them in heap-address order, which differs from run to run.
    vector<FnSpec *> usedexterns;
    // The library a dynamic extern binds from, with the spelling each
    // platform opens: the sidecars' (§7.10), the program's over the
    // package's for a platform either says anything about, and empty where
    // neither does, which leaves the runtime its derived spelling.
    DylibSpelling DlSpelling(FnSpec *sp) {
        DylibSpelling r { sp->sf->dylib };
        auto pkg = ast.filepkg[sp->sf->line.fileidx];
        for (auto p : { pkg, 0 }) {
            for (auto &d : ast.packages[p].dylibs) {
                if (d.name != r.name) continue;
                for (auto plat : { "_windows", "_macos", "_linux" }) {
                    auto &s = d.On(plat).empty() ? d.any : d.On(plat);
                    if (!s.empty()) r.On(plat) = s;
                }
            }
        }
        return r;
    }
    static bool SameDylib(const DylibSpelling &a, const DylibSpelling &b) {
        return a.name == b.name && a.onwindows == b.onwindows &&
               a.onmacos == b.onmacos && a.onlinux == b.onlinux;
    }
    // The libraries the used externs bind from, each spelling once, in the
    // order their first declaration comes in the program (file order, then
    // source order).
    vector<DylibSpelling> DlLibs(const vector<FnSpec *> &externs) {
        vector<pair<Line, DylibSpelling>> firsts;
        for (auto sp : externs) {
            if (sp->sf->dylib.empty()) continue;
            auto l = sp->sf->line;
            auto d = DlSpelling(sp);
            auto it = std::find_if(firsts.begin(), firsts.end(),
                                   [&](auto &f) { return SameDylib(f.second, d); });
            if (it == firsts.end()) firsts.emplace_back(l, d);
            else if (pair(l.fileidx, l.line) < pair(it->first.fileidx, it->first.line))
                it->first = l;
        }
        std::stable_sort(firsts.begin(), firsts.end(), [](auto &a, auto &b) {
            return pair(a.first.fileidx, a.first.line) < pair(b.first.fileidx, b.first.line);
        });
        vector<DylibSpelling> libs;
        for (auto &f : firsts) libs.push_back(f.second);
        return libs;
    }
    bool usesthreads = false;
    bool library = false;
    // Measurement only, and unsound: emit the whole `return ... from`
    // machinery but none of the post-call discriminant checks, so a program
    // that never takes a long-distance return still prints the right answer
    // and the checks' cost can be priced (--unsafe-no-rf-check).
    bool norfcheck = false;

    [[noreturn]] void Fail(Line l, const string &msg) {
        auto s = cat("codegen: ", msg);
        if (l.fileidx >= 0 && l.fileidx < (int)ast.sources.size())
            s = cat(ast.sources[l.fileidx].first, ":", l.line, ": ", s);
        throw CompileError { s };
    }

    // A C string literal (quotes included) for arbitrary bytes; non-printables
    // as 3-digit octal so following characters can never extend an escape.
    // MSVC takes at most 16380 bytes in one literal, so a long string becomes
    // adjacent literals, a line of its text or at most 4000 bytes each.
    static string CStr(string_view v) {
        string s = "\"";
        auto split = v.size() > 4000;
        size_t piece = 0;
        for (size_t i = 0; i < v.size(); i++) {
            auto c = v[i];
            auto u = (uint8_t)c;
            if (c == '"' || c == '\\') { s += '\\'; s += c; }
            else if (u >= 32 && u < 127) s += c;
            else {
                char buf[8];
                snprintf(buf, sizeof(buf), "\\%03o", u);
                s += buf;
            }
            if (split && (c == '\n' || ++piece == 4000) && i + 1 < v.size()) {
                s += "\"\n    \"";
                piece = 0;
            }
        }
        s += '"';
        return s;
    }

    // ------------------------------------------------------------------
    // Type utilities on concrete (post-typecheck) types. Sizes of fixed and
    // limited arrays were evaluated during checking; assert rather than
    // re-evaluate.
    //
    // These read what the checker left behind and create nothing: TypeCheck's
    // ClassOf and GetStructInst instantiate a type and validate it, which is
    // the checking half of the same questions and must not happen here.
    // TEq is not TypeEq either: it is equality of C representation, so it
    // ignores `const`, which no C type carries (§9.5).

    int64_t ArrSize(TypeArray *a) {
        assert(a->size >= 0 || !a->sizeexpr);
        return a->size;
    }

    bool TEq(TypeExpr *a, TypeExpr *b);
    StructInst *SI(TypeExpr *t);
    EnumInst *EIOf(TypeExpr *t);
    EnumInst *EIVar(TypeExpr *t);
    // The field runs of a nominal type (ast.h FieldRun); empty for every
    // other kind.
    span<const FieldRun> FieldRuns(TypeExpr *t);
    // As the checker's, over the runs this pass reads rather than builds.
    template<typename F> bool AnyField(TypeExpr *t, F f) { return AnyFieldOf(FieldRuns(t), f); }
    template<typename F> void EachField(TypeExpr *t, F f) { EachFieldOf(FieldRuns(t), f); }
    SizeClass Cls(TypeExpr *t);

    bool IsFix(TypeExpr *t)  { return Cls(t) == SC_FIXED; }
    bool IsResz(TypeExpr *t) { return Cls(t) == SC_RESIZABLE; }
    bool IsBytesT(TypeExpr *t) { return Cls(t) != SC_FIXED; }
    // This is a C backend storage/ABI choice, not a language size class:
    // packed layout, copying, indexing and lifetime rules stay unchanged.
    static constexpr int64_t NATIVE_VALUE_LIMIT = 4096;
    bool IsLargeFixed(TypeExpr *t) { return IsFix(t) && FixedSize(t) > NATIVE_VALUE_LIMIT; }
    bool NeedsStack(TypeExpr *t) { return IsBytesT(t) || IsLargeFixed(t); }
    // A limited array of static capacity, `T[..k]`: a C value of a length
    // and k slots, which any other array or slice of T constructs by copy
    // (§4.2, AdaptToFixed).
    bool IsStaticLimited(TypeExpr *t) {
        return t->kind == TY_ARRAY && t->arr->akind == A_LIMITED && IsFix(t);
    }
    // A resizable-tailed struct with an all-fixed prefix is a frame object
    // (C.2): a C struct of its fixed fields plus its tail's own gs_rhdr (or
    // nested frame object), held in the owning frame like a fixed value;
    // only the innermost tail's elements occupy a data stack. A gs_rref to
    // one carries the object's address in `hdr`.
    bool IsFrameObj(TypeExpr *t) { return t->kind == TY_STRUCT && SI(t)->frameobj; }
    // The C type of a resizable value's header: a frame object's own struct,
    // a gs_rhdr for anything else.
    string RzCT(TypeExpr *t) { return IsFrameObj(t) ? CT(t) : "gs_rhdr"; }
    TypeExpr *FoTailArr(TypeExpr *t);
    // The same type as the tail of a value that is not a frame object keeps
    // its fixed fields as bytes in that value, like the enclosing fields.
    int64_t FoBytesPrefix(TypeExpr *t);
    bool IsFatRef(TypeExpr *t);
    bool HoldsFatRef(TypeExpr *t);
    bool HoldsFatRefIn(TypeExpr *t, set<const void *> &open);
    bool IsVoidT(TypeExpr *t) { return !t || t->kind == TY_VOID; }

    IntStorage LenStore(TypeArray *a);

    static int64_t IntSize(IntStorage s) { return IntBits(s) / 8; }

    static const char *IntCT(IntStorage s);
    static const char *IntSfx(IntStorage s);

    IntStorage TagStore(SEnum *en) { return en->variants.size() <= 256 ? IS_U8 : IS_U16; }
    int64_t TagSize(SEnum *en) { return IntSize(TagStore(en)); }

    // Packed layout of a run of fields: byte offsets aligned with the fields
    // vector (pads get their own offset), plus the total size. Fixed types
    // only; the same code computes the static prefix of variable structs.
    struct Layout {
        vector<int64_t> offs;
        int64_t size = 0;
    };
    map<pair<const void *, int>, Layout> layouts;   // StructInst / (EnumInst, variant).

    int64_t PadAlign(TypeExpr *t);
    Layout LayoutFields(const vector<Field> &fields, const vector<TypeExpr *> &ftypes);
    const Layout &StructLayout(StructInst *si);
    const Layout &VariantLayout(EnumInst *ei, int vi);
    int64_t FixedSize(TypeExpr *t);

    // ------------------------------------------------------------------
    // Naming: one global identifier space for types, functions, globals, and
    // static data; per-function spaces for locals seeded from it.

    set<string> used;

    string Sanitize(string_view name);
    string Sanitize(string_view ns, string_view name);
    string Unique(string base);

    // ------------------------------------------------------------------
    // Mangled type identities and on-demand C type emission. The mangle keys
    // every per-type artifact (typedef, size fn, eq fn); the C name is the
    // uniquified mangle.

    string Mangle(TypeExpr *t);
    string RawMangle(TypeExpr *t);
    bool SameMangle(TypeExpr *a, TypeExpr *b);
    map<VarDef *, int> ixmangles;   // Index types' arrays, numbered for Mangle.
    // Per raw spelling, the types it was handed out for (one of each set of
    // parts, SameMangle) with the mangle each got; and every mangle handed out.
    unordered_map<string, vector<pair<TypeExpr *, string>>> mangleowners;
    unordered_set<string> mangles;

    unordered_map<string, string> ctypes;   // mangle -> emitted C type name.
    bool corebuiltins = false;

    void EmitCoreTypes();
    string CT(TypeExpr *t);
    static bool StructLike(TypeExpr *t);
    string NameCT(TypeExpr *t);
    set<string> cdefined;    // Mangled names whose C body CT has written.
    set<string> slicects;    // The C typedef names of slice types.

    void EmitCFields(string &d, const vector<Field> &fields, const vector<TypeExpr *> &ftypes);

    // Tag constants, one enum per EnumInst: <Mangle>_<Variant>_k.
    set<EnumInst *> tagenums;
    unordered_map<EnumInst *, string> tagprefix;
    unordered_set<string> tagbases;   // The prefixes taken.

    void EnsureTagEnum(EnumInst *ei);

    // The TY_VARIANT type for (enum type, variant index); cached per pair.
    map<pair<EnumInst *, int>, TypeExpr *> varianttypes;

    TypeExpr *VariantType(TypeExpr *enumtype, int vi);

    // ------------------------------------------------------------------
    // Sizes of bytes-class values.

    int64_t MinBytes(TypeExpr *t, bool zero = false);

    // ------------------------------------------------------------------
    // The verifier from_bytes runs over an untrusted image
    // (docs/design/serialization.md §5): which types it walks.

    bool HasRelRefAny(TypeExpr *t);
    bool NeedsVerifyWalk(TypeExpr *t);

    // ------------------------------------------------------------------
    // default<T>() (§4.2): all-zero bytes are the default of every fixed type
    // -- numbers, false, null, empty slices and limited arrays, variant 0 --
    // except where a field declares its own default, which the checker
    // builds as a literal of the call's own (Call::defaultinit).

    bool HasFieldDefaults(TypeExpr *t);

    // ------------------------------------------------------------------
    // Structural equality (§4.5): gap-free fixed types without floats compare as bytes.

    bool BitwiseEq(TypeExpr *t);

    // ------------------------------------------------------------------
    // Per-specialization call interface. Signature shape (C.3 order):
    //   [declared params (resizable by-value ones add a gs_stack*)]
    //   [free-variable references, §7.5]
    //   [out-pointers for fixed returns after the first]
    //   [destination stacks for nonfixed returns]
    //   [int64_t gs_sp].
    // The C return value is the first fixed return, else void; a `return ...
    // from` discriminant travels in the thread-local gs_rf, not the signature.

    struct SpecInfo {
        string cname;
        vector<VarDef *> freevars;
        set<const VarDef *> globals; // Transitively touched stack/fat-reference globals.
        bool needssp = false;
        bool hasrf = false;
        bool readsalltops = false;   // Transitively reads every data-stack top.
        int cret = -1;               // Ret index returned as the C value.
    };
    unordered_map<FnSpec *, SpecInfo> sinfo;
    vector<FnSpec *> livespecs;

    // Data stack accounting (codegen_stacks.h). What each compiled function
    // asks for (TinyGenBackend::Build): the calls it hands a stack index, with
    // that index relative to its own, and the most stacks its own body opens.
    // The nullptr function is what runs with gs_sp fixed at 0.
    struct StackCall {
        FnSpec *callee;
        int offset;
        Line line;
    };
    unordered_map<FnSpec *, vector<StackCall>> stackcalls;
    unordered_map<FnSpec *, int> stackown;
    // The most stacks a function can have in use at once, its callees
    // included (BoundStacks): a constant, since no call into a recursive
    // cycle is made with a stack in use (§7.8).
    unordered_map<FnSpec *, int64_t> stackneed;
    // Regions outside the indexed block: the globals' dedicated stacks
    // gs_init_globals reserves for the main program, and what each worker's
    // thunk reserves for its arguments and its copy of the globals.
    int globalregions = 0;
    unordered_map<FnSpec *, int> thunkregions;
    // What main hands gs_rt_init (the main program's stacks, its regions, a
    // worker's), and whether the globals' initializers leave nothing to do
    // (TinyGenBackend::EmitMain).
    int64_t rtinit[3] = { 0, 0, 0 };
    bool noglobalinit = false;
    // GS_MAX_STACKS as the program is configured (main.cpp): the most data
    // stacks one thread program may use at once, zero for no limit.
    int64_t maxstacks = 0;
    void BoundStacks();
    int64_t ProgramStacks(const vector<FnSpec *> &roots);
    vector<FnSpec *> MainRoots();
    vector<pair<string, FnSpec *>> WorkerEntries();
    void CheckStackLimit(const string &what, int64_t stacks, Line ln);
    // The runtime configuration a report assumes (main.cpp).
    struct StackConfig {
        uint64_t reserve, gap, budget;
        int64_t maxstacks;
    };
    // The configuration the program is built with (main.cpp, StackConfigOf), which a backend
    // without the C preprocessor reads instead of GS_STACK_RESERVE and the others.
    StackConfig stackcfg { 0, 0, 0, 0 };
    void StackReport(FILE *out, const StackConfig &cfg);

    // Long-distance return targets (§7.9), numbered: the backend names their channels by it.
    unordered_map<FnSpec *, int> fromids;

    bool IsPoolParam(FnSpec *sp, size_t i);

    void CollectSpecs();
    // The members a by-value parameter of type t (a pool reference where
    // pool is set) is passed as, each a (C type, member name); none where it
    // is passed whole.
    vector<pair<string, string>> ParamFields(TypeExpr *t, bool pool);

    // ------------------------------------------------------------------
    // Globals (§11.1). Their names hold for the whole run: GS_GL->name (a member of the program
    // instance's block) or the static data of a constant one; the dedicated stack of a resizable
    // or bytes global, and a reusable pool's freelist, by the same convention.
    unordered_map<const VarDef *, string> gnames;
    unordered_map<const VarDef *, string> gstks;
    unordered_map<const VarDef *, pair<string, string>> gpools;
    // `let` globals of a const type with a compile-time initializer, which are static data
    // (§9.5), with that initializer, which a later one naming it is spelled out from.
    map<const VarDef *, Node *> gstatic;
    // Spelling out more elements of a fill or a string than this would trade startup work for
    // size; such a value keeps its runtime initialization.
    static constexpr int64_t MAXSTATICELEMS = 256;
    bool StaticInitX(Node *n, TypeExpr *t, string &out);
    static string IntStr(int64_t v);
    static string FltStr(double v, bool f32);
    void NameGlobals();
    void GlobalStacks();

    // ------------------------------------------------------------------
    // Values and nodes.

    // A reference variable with reusable-pool provenance is a gs_pref (§5.4).
    bool PrefVar(const VarDef *vd);
    string VarCT(const VarDef *vd);
    static bool IsCtl(Node *n);
    // An operand's type as an operator sees it: a spliced reference reads as its pointee.
    TypeExpr *OperandT(TypeExpr *t);
    bool HasStmts(Node *n);
    // A fixed type holding relative references at any depth, whose values are built where they
    // end up (§3.9).
    bool HasRelRef(TypeExpr *t);
    bool NeedsResultLocation(TypeExpr *t) { return IsFix(t) && t->kind != TY_REF && HasRelRef(t); }
    TypeExpr *AdtFrom(Node *n);
    // How many elements an array literal writes: its fill count, which the
    // checker evaluated into an IntLit (§4.2), or its element list's length.
    static int64_t LitCount(ArrayLit *al) {
        if (!al->fillval) return (int64_t)al->elems.size();
        assert(Is<IntLit>(al->fillcount));
        return ((IntLit *)al->fillcount)->val;
    }
    // The bytes a length prefix of this storage reserves ahead of the
    // elements. A varint takes the one byte that covers counts under 128.
    int64_t PrefixBytes(IntStorage ls) { return ls == IS_VARINT ? 1 : IntSize(ls); }
    vector<Node *> CallArgNodes(Call *c, size_t nparams);
    // A named result (§7.3), found structurally on the final body.
    const VarDef *NamedResult(Block *fnbody, SFunction *target, size_t nrets, size_t resultidx);

    // ------------------------------------------------------------------
    // Queues, threads and text (§11.2, §3.7).

    unordered_map<string, string> queues;   // Element type mangle -> queue name.
    string QueueFor(TypeExpr *t);
    // A worker's entry thunk, by name.
    unordered_map<FnSpec *, string> thunks;
    string EnsureThreadThunk(FnSpec *sp);
    int ThunkRegions(FnSpec *sp);
    TypeExpr *growu8 = nullptr;
    TypeExpr *GrowU8();
    Call *FmtContext(Call *c, Node *arg);
    FnSpec *FmtSpecFor(Call *c, TypeExpr *t);
    bool SimpleText(Call *c, TypeExpr *t);

    // ------------------------------------------------------------------
    // Driver.

    // The program's C types (tdecls), for the extern unit and the --include headers written against
    // them (TinygenExternSource, TinygenUnitHeader in main.cpp), and the export header's prototypes.
    string types;
    string exportprotos;
    string ExportHeader();
    void StartStacks();
    void ExportProtos();

    // The analyses the backend compiles from: the live specializations and their call interfaces,
    // the globals' names, data stacks and static initializers.
    CodeGen(Ast &_ast, bool _norfcheck = false, bool _library = false, StackConfig _stackcfg = {})
        : ast(_ast), library(_library), norfcheck(_norfcheck), maxstacks(_stackcfg.maxstacks),
          stackcfg(_stackcfg) {
        CollectSpecs();
        NameGlobals();
    }

    // Once the backend compiled every live specialization (and told stackown and stackcalls what
    // each holds): the data stack counts the program starts the runtime with (rtinit), the export
    // header's prototypes, and the C types of the extern fns, those the backend calls (`externs`)
    // among them, which an --include header is written against.
    void Finish(const vector<FnSpec *> &externs, bool headers) {
        GlobalStacks();
        StartStacks();
        ExportProtos();
        // A header can implement any declared extern, even one no call reaches.
        for (auto sp : ast.fnspecs) {
            if (!sp->sf->isextern) continue;
            for (auto t : sp->argtypes) {
                CT(t);
                // CT only forward-declares a struct behind a pointer; an
                // adapter needs its fields too. Builders use gs_rref instead.
                if (t->kind == TY_REF && !IsResz(t->ref->sub)) CT(t->ref->sub);
            }
            for (auto t : sp->rets) CT(t);
        }
        EmitCoreTypes();
        CT(ast.SliceOf(ast.inttypes[IS_U8], Line {}));
        // An --include header reads a struct an extern takes by reference.
        if (headers)
            for (auto sp : externs)
                for (auto t : sp->argtypes)
                    if (t->kind == TY_REF && StructLike(t->ref->sub) && !IsResz(t->ref->sub) &&
                        !IsBytesT(t->ref->sub))
                        CT(t->ref->sub);
        types = cat("\n/* ---- types ---- */\n#pragma pack(push, 1)\n", tdecls, "#pragma pack(pop)\n");
    }
};

}  // namespace goose
