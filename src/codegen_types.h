// Goose compiler -- CodeGen's types (definitions of CodeGen members, codegen.h): type utilities
// and packed layouts (C.2), C naming, mangled type identities with their C types written on
// demand, the minimum sizes of bytes values, default values (§4.2) and structural equality (§4.5).
#pragma once

namespace goose {

// ------------------------------------------------------------------
// Type utilities on concrete (post-typecheck) types. Sizes of fixed and
// limited arrays were evaluated during checking; assert rather than
// re-evaluate.

inline bool CodeGen::TEq(TypeExpr *a, TypeExpr *b) {
    if (a == b) return true;
    if (a->kind != b->kind) return false;
    switch (a->kind) {
        case TY_INT:  return a->intstorage == b->intstorage;
        case TY_FLT:  return a->fltstorage == b->fltstorage;
        case TY_BOOL: case TY_VOID: case TY_FN: return true;
        case TY_STRUCT: return SI(a) == SI(b);
        case TY_ENUM:   return EIOf(a) == EIOf(b) && a->enu->varmode == b->enu->varmode;
        case TY_ARRAY: {
            auto &x = *a->arr, &y = *b->arr;
            if (x.akind != y.akind || !TEq(x.sub, y.sub)) return false;
            switch (x.akind) {
                case A_FIXED: case A_LIMITED: return ArrSize(a->arr) == ArrSize(b->arr);
                case A_VAR:     return LenStore(a->arr) == LenStore(b->arr);
                default:        return true;
            }
        }
        case TY_SLICE: return TEq(a->sub, b->sub);
        case TY_REF:
            return TEq(a->ref->sub, b->ref->sub) && a->ref->optional == b->ref->optional &&
                   a->ref->lenstorage == b->ref->lenstorage;
        case TY_VARIANT:
            return a->var->variant == b->var->variant && TEq(a->var->adt, b->var->adt);
        default: assert(false); return false;
    }
}

// Instantiations: the typechecker filled the per-TypeExpr caches for every
// type it touched; fall back to an argument-equality search for clones.
inline StructInst *CodeGen::SI(TypeExpr *t) {
    assert(t->kind == TY_STRUCT);
    if (t->struc->inst) return t->struc->inst;
    for (auto inst : t->struc->st->insts) {
        if (inst->args.size() != t->struc->args.size()) continue;
        auto ok = true;
        for (size_t i = 0; i < inst->args.size(); i++)
            ok &= TEq(inst->args[i], t->struc->args[i]);
        if (ok) return t->struc->inst = inst;
    }
    assert(false);
    return nullptr;
}

inline EnumInst *CodeGen::EIOf(TypeExpr *t) {
    assert(t->kind == TY_ENUM);
    if (t->enu->inst) return t->enu->inst;
    for (auto inst : t->enu->en->insts) {
        if (inst->args.size() != t->enu->args.size()) continue;
        auto ok = true;
        for (size_t i = 0; i < inst->args.size(); i++)
            ok &= TEq(inst->args[i], t->enu->args[i]);
        if (ok) return t->enu->inst = inst;
    }
    assert(false);
    return nullptr;
}

inline EnumInst *CodeGen::EIVar(TypeExpr *t) {   // For TY_VARIANT.
    assert(t->kind == TY_VARIANT);
    return EIOf(t->var->adt);
}

inline span<const FieldRun> CodeGen::FieldRuns(TypeExpr *t) {
    switch (t->kind) {
        case TY_STRUCT: return RunsOf(SI(t));
        case TY_ENUM: return RunsOf(EIOf(t));
        case TY_VARIANT: {
            auto ei = EIVar(t);
            return RunsOf(ei, ei->en->VariantIndex(t->var->variant));
        }
        default: return {};
    }
}

inline SizeClass CodeGen::Cls(TypeExpr *t) {
    switch (t->kind) {
        case TY_INT: return t->intstorage == IS_VARINT ? SC_VARIABLE : SC_FIXED;
        case TY_REF: return t->ref->lenstorage == IS_VARINT ? SC_VARIABLE : SC_FIXED;
        case TY_STRUCT: return SI(t)->sclass;
        case TY_ENUM: return t->enu->varmode ? EIOf(t)->varclass : SC_FIXED;
        case TY_ARRAY:
            switch (t->arr->akind) {
                case A_FIXED:   return SC_FIXED;
                case A_VAR:     return SC_VARIABLE;
                case A_LIMITED: return ArrSize(t->arr) >= 0 ? SC_FIXED : SC_VARIABLE;
                default:        return SC_RESIZABLE;
            }
        case TY_VARIANT: {
            auto c = SC_FIXED;
            EachField(t, [&](TypeExpr *ft) { c = std::max(c, Cls(ft)); });
            return c;
        }
        default: return SC_FIXED;
    }
}

inline TypeExpr *CodeGen::FoTailArr(TypeExpr *t) {
    auto si = SI(t);
    auto ft = si->ftypes[LastRealField(si->st->fields)];
    return IsFrameObj(ft) ? FoTailArr(ft) : ft;
}

// Where the innermost tail's elements start in that bytes layout (FoBytes),
// in which a bare pad inserts nothing, as in any variable-class layout.
inline int64_t CodeGen::FoBytesPrefix(TypeExpr *t) {
    auto si = SI(t);
    int64_t n = 0;
    for (size_t i = 0; i < si->st->fields.size(); i++) {
        auto &f = si->st->fields[i];
        if (f.ispad) { n += f.padsize > 0 ? f.padsize : 0; continue; }
        auto ft = si->ftypes[i];
        if (IsResz(ft)) return n + (IsFrameObj(ft) ? FoBytesPrefix(ft) : 0);
        n += FixedSize(ft);
    }
    assert(false);   // A frame object ends in its tail.
    return n;
}

inline bool CodeGen::IsFatRef(TypeExpr *t) {
    return t->kind == TY_REF && t->ref->lenstorage < 0 && IsResz(t->ref->sub);
}

// Whether a value of type `t` carries a data stack inside it: a fat
// reference, or one reachable from the value -- in a field, a payload, an
// element, or behind a slice or a plain reference. Code handed such a value
// can grow that stack without the value's name saying which stack it is
// (SyncReach). A nominal type reached again on the way
// through itself has no further fat reference to offer, so `open` cuts the
// recursion there.
inline bool CodeGen::HoldsFatRef(TypeExpr *t) {
    set<const void *> open;
    return HoldsFatRefIn(t, open);
}

inline bool CodeGen::HoldsFatRefIn(TypeExpr *t, set<const void *> &open) {
    switch (t->kind) {
        case TY_REF: return IsFatRef(t) || HoldsFatRefIn(t->ref->sub, open);
        case TY_SLICE: return HoldsFatRefIn(t->sub, open);
        case TY_ARRAY: return HoldsFatRefIn(t->arr->sub, open);
        default:
            // A run's types vector belongs to one instantiation (or one of
            // its variants), so it identifies what was entered.
            for (auto &run : FieldRuns(t)) {
                if (!open.insert(run.ftypes).second) continue;
                for (auto ft : *run.ftypes) if (ft && HoldsFatRefIn(ft, open)) return true;
            }
            return false;
    }
}

// The stored length-field type of an array (A_VAR: declared or u32
// default; A_LIMITED with static capacity: smallest fitting type).
inline IntStorage CodeGen::LenStore(TypeArray *a) {
    switch (a->akind) {
        case A_VAR:
            return a->lenstorage < 0 ? IS_U32 : (IntStorage)a->lenstorage;
        case A_LIMITED: {
            auto k = ArrSize(a);
            assert(k >= 0);
            return k <= 255 ? IS_U8 : k <= 65535 ? IS_U16 :
                   k <= (int64_t)UINT32_MAX ? IS_U32 : IS_U64;
        }
        default: assert(false); return IS_U32;
    }
}

inline const char *CodeGen::IntCT(IntStorage s) {
    switch (s) {
        case IS_I8:  return "int8_t";
        case IS_I16: return "gs_i16";
        case IS_I32: return "gs_i32";
        case IS_U8:  return "uint8_t";
        case IS_U16: return "gs_u16";
        case IS_U32: return "gs_u32";
        case IS_U64: return "gs_u64";
        default:     return "gs_i64";
    }
}

// The runtime arithmetic helper suffix per integer type (runtime.h).
inline const char *CodeGen::IntSfx(IntStorage s) {
    switch (s) {
        case IS_I8:  return "i8";
        case IS_I16: return "i16";
        case IS_I32: return "i32";
        case IS_U8:  return "u8";
        case IS_U16: return "u16";
        case IS_U32: return "u32";
        case IS_U64: return "u64";
        default:     return "i64";
    }
}

// StructInst / (EnumInst, variant).

// The alignment `pad` (bare form) gives the next field: its scalar width.
inline int64_t CodeGen::PadAlign(TypeExpr *t) {
    switch (t->kind) {
        case TY_INT: return t->intstorage == IS_VARINT ? 1 : IntSize(t->intstorage);
        case TY_FLT: return t->fltstorage == FS_F32 ? 4 : 8;
        case TY_REF: return t->ref->lenstorage >= 0 ?
                            (t->ref->lenstorage == IS_VARINT ? 1
                                                             : IntSize((IntStorage)t->ref->lenstorage))
                            : 8;
        case TY_SLICE: return 8;
        default: return 1;
    }
}

inline CodeGen::Layout CodeGen::LayoutFields(const vector<Field> &fields,
                                             const vector<TypeExpr *> &ftypes) {
    Layout lo;
    int64_t off = 0;
    for (size_t i = 0; i < fields.size(); i++) {
        auto &f = fields[i];
        if (f.ispad) {
            lo.offs.push_back(off);
            if (f.padsize >= 0) {
                off += f.padsize;
            } else {
                // Bare pad: align the next real field to its own size.
                TypeExpr *nt = nullptr;
                for (auto j = i + 1; j < fields.size(); j++)
                    if (!fields[j].ispad) { nt = ftypes[j]; break; }
                auto a = nt ? PadAlign(nt) : 1;
                off = (off + a - 1) / a * a;
            }
            continue;
        }
        lo.offs.push_back(off);
        off += FixedSize(ftypes[i]);
    }
    lo.size = off;
    return lo;
}

inline const CodeGen::Layout &CodeGen::StructLayout(StructInst *si) {
    auto key = pair<const void *, int>(si, -1);
    auto it = layouts.find(key);
    if (it != layouts.end()) return it->second;
    return layouts[key] = LayoutFields(si->st->fields, si->ftypes);
}

inline const CodeGen::Layout &CodeGen::VariantLayout(EnumInst *ei, int vi) {
    auto key = pair<const void *, int>(ei, vi);
    auto it = layouts.find(key);
    if (it != layouts.end()) return it->second;
    return layouts[key] = LayoutFields(ei->en->variants[vi].fields, ei->vftypes[vi]);
}

inline int64_t CodeGen::FixedSize(TypeExpr *t) {
    switch (t->kind) {
        case TY_INT:  assert(t->intstorage != IS_VARINT); return IntSize(t->intstorage);
        case TY_FLT:  return t->fltstorage == FS_F32 ? 4 : 8;
        case TY_BOOL: return 1;
        case TY_REF:
            if (t->ref->lenstorage >= 0) {
                assert(t->ref->lenstorage != IS_VARINT);
                return IntSize((IntStorage)t->ref->lenstorage);
            }
            return IsResz(t->ref->sub) ? 16 : 8;
        case TY_SLICE: return 16;
        case TY_STRUCT: {
            // An empty struct takes the one byte C gives it.
            auto sz = StructLayout(SI(t)).size;
            return sz ? sz : 1;
        }
        case TY_ENUM: {
            assert(!t->enu->varmode);
            auto ei = EIOf(t);
            int64_t maxp = 0;
            for (size_t vi = 0; vi < ei->en->variants.size(); vi++)
                maxp = std::max(maxp, VariantLayout(ei, (int)vi).size);
            return TagSize(ei->en) + maxp;
        }
        case TY_VARIANT: {
            auto ei = EIVar(t);
            return VariantLayout(ei, ei->en->VariantIndex(t->var->variant)).size;
        }
        case TY_ARRAY: {
            auto &a = *t->arr;
            switch (a.akind) {
                case A_FIXED:   return ArrSize(t->arr) * FixedSize(a.sub);
                case A_LIMITED: return IntSize(LenStore(t->arr)) +
                                       ArrSize(t->arr) * FixedSize(a.sub);
                default: assert(false); return 0;
            }
        }
        default: assert(false); return 0;
    }
}

// ------------------------------------------------------------------
// Naming: one global identifier space for types, functions, globals, and
// static data; per-function spaces for locals seeded from it.

// A name from the program as a C identifier. Every one of them carries the
// same suffix, which is what keeps the generated file free of collisions
// without a list of names to avoid: not the C keywords, not the runtime's own
// gs_ names, not the temporaries and labels below, and -- the reason a list
// could never be right -- not whatever the platform's headers happen to
// declare or define as a macro, which is not the same set on Windows as it is
// on Linux or macOS. Struct fields and enum variants get it too: those live in
// their own C namespace, but a macro does not respect namespaces.
//
// The one name that crosses into real C untouched is an `extern fn`'s symbol,
// which is the C name the declaration gives and never comes through here.
inline string CodeGen::Sanitize(string_view name) {
    string s(name);
    if (s.empty()) s = "_";
    // `result#1`, a name the parser makes that no program can write
    // (Parser::ParseMultiAssign); a local's C name is unique either way.
    for (auto &c : s) if (c == '#') c = '_';
    return s + "_g";
}

// A namespaced declaration (docs/design/namespaces.md): the namespace, the
// leaf, the suffix above, then the namespace's length. The digits are what
// keep the mapping injective -- `a::b_c` and `a_b::c` differ in them, and no
// global name's C form ends in a digit -- without a list of characters to
// replace.
inline string CodeGen::Sanitize(string_view ns, string_view name) {
    if (ns.empty()) return Sanitize(name);
    return cat(ns, "_", name, "_g", ns.size());
}

inline string CodeGen::Unique(string base) {
    if (!used.count(base)) { used.insert(base); return base; }
    for (auto n = 2;; n++) {
        auto s = cat(base, "_", n);
        if (!used.count(s)) { used.insert(s); return s; }
    }
}

// ------------------------------------------------------------------
// Mangled type identities and their C types, written on demand (CT). The
// mangle keys every per-type artifact: the backend's procedures and symbols,
// the typedefs; the C name is the uniquified mangle.

// The raw spelling does not tell every pair of types apart: `E<F..>` and
// `E<F>..` both spell E_g_F_g_vm, `E<G.B>` and `E<G>.B` both E_g_G_g_B_p,
// and `A<B>` spells what a struct named A_g_B does. The first type to spell
// one keeps it; a type of other parts (SameMangle) spelling it later gets a
// suffix, `_m1` and up, past every mangle already handed out. Parts are
// mangled here too, so a type built of told-apart ones is told apart too.
inline string CodeGen::Mangle(TypeExpr *t) {
    auto raw = RawMangle(t);
    auto &owners = mangleowners[raw];
    for (auto &[o, m] : owners)
        if (SameMangle(o, t)) return m;
    auto m = raw;
    for (auto n = 1; mangles.count(m); n++) m = cat(raw, "_m", n);
    mangles.insert(m);
    owners.push_back({ t, m });
    return m;
}

// Whether two types are made of the parts RawMangle spells: TEq, but for
// a nominal type its declaration and arguments rather than its instance,
// so that instances whose arguments differ only in `const`, which shares a
// layout, keep sharing their C types too.
inline bool CodeGen::SameMangle(TypeExpr *a, TypeExpr *b) {
    if (a == b) return true;
    if (a->kind != b->kind) return false;
    auto sameargs = [&](const vector<TypeExpr *> &x, const vector<TypeExpr *> &y) {
        if (x.size() != y.size()) return false;
        for (size_t i = 0; i < x.size(); i++)
            if (!SameMangle(x[i], y[i])) return false;
        return true;
    };
    switch (a->kind) {
        case TY_INT:
            return a->intstorage == b->intstorage && !a->ix == !b->ix &&
                   (!a->ix || (a->ix->arr == b->ix->arr && a->ix->upto == b->ix->upto));
        case TY_STRUCT:
            return a->struc->st == b->struc->st && sameargs(a->struc->args, b->struc->args);
        case TY_ENUM:
            return a->enu->en == b->enu->en && a->enu->varmode == b->enu->varmode &&
                   sameargs(a->enu->args, b->enu->args);
        case TY_VARIANT:
            return a->var->variant == b->var->variant && SameMangle(a->var->adt, b->var->adt);
        case TY_ARRAY: {
            auto &x = *a->arr, &y = *b->arr;
            if (x.akind != y.akind || !SameMangle(x.sub, y.sub)) return false;
            switch (x.akind) {
                case A_FIXED: case A_LIMITED: return ArrSize(a->arr) == ArrSize(b->arr);
                case A_VAR:                   return LenStore(a->arr) == LenStore(b->arr);
                default:                      return true;
            }
        }
        case TY_SLICE: return SameMangle(a->sub, b->sub);
        case TY_REF:
            return a->ref->optional == b->ref->optional &&
                   a->ref->lenstorage == b->ref->lenstorage &&
                   SameMangle(a->ref->sub, b->ref->sub);
        default: return TEq(a, b);
    }
}

inline string CodeGen::RawMangle(TypeExpr *t) {
    switch (t->kind) {
        case TY_INT: {
            if (t->intstorage == IS_VARINT) return "vi";
            if (!t->ix) return IntSfx(t->intstorage);
            // An index type is a C integer, but its array is part of its
            // identity, which per-type artifacts such as a verifier depend on.
            auto [it, fresh] = ixmangles.try_emplace(t->ix->arr, (int)ixmangles.size());
            (void)fresh;
            return cat(IntSfx(t->intstorage), t->ix->upto ? "b" : "x", it->second);
        }
        case TY_FLT: return t->fltstorage == FS_F32 ? "f32" : "f64";
        case TY_BOOL: return "b";
        case TY_STRUCT: {
            auto s = Sanitize(t->struc->st->ns, t->struc->st->name);
            for (auto a : t->struc->args) Append(s, "_", Mangle(a));
            return s;
        }
        case TY_ENUM: {
            auto s = Sanitize(t->enu->en->ns, t->enu->en->name);
            for (auto a : t->enu->args) Append(s, "_", Mangle(a));
            if (t->enu->varmode) s += "_vm";
            return s;
        }
        case TY_VARIANT: {
            auto s = Mangle(t->var->adt);
            Append(s, "_", t->var->variant->name, "_p");
            return s;
        }
        case TY_ARRAY: {
            auto &a = *t->arr;
            switch (a.akind) {
                case A_FIXED:   return cat("a", ArrSize(t->arr), "_", Mangle(a.sub));
                case A_VAR:     return cat("v", IntStorageName(LenStore(t->arr)), "_",
                                           Mangle(a.sub));
                case A_LIMITED: return ArrSize(t->arr) >= 0
                                    ? cat("l", ArrSize(t->arr), "_", Mangle(a.sub))
                                    : cat("lv_", Mangle(a.sub));
                case A_GROW:    return cat("gw_", Mangle(a.sub));
                default:        return cat("gx_", Mangle(a.sub));
            }
        }
        case TY_SLICE: return cat("sl_", Mangle(t->sub));
        case TY_REF: {
            auto &r = *t->ref;
            string s = r.optional ? "o" : "r";
            if (r.lenstorage >= 0) Append(s, "l", IntStorageName(r.lenstorage));
            Append(s, "_", Mangle(r.sub));
            return s;
        }
        default: assert(false); return "void";
    }
}

inline void CodeGen::EmitCoreTypes() {
    if (corebuiltins) return;
    corebuiltins = true;
    // A resizable value is a frame header (C.2): the data pointer (element
    // region for arrays, struct start for resizable-tailed structs) plus
    // the tail element count. A reference to one points at the header.
    tdecls +=
        "typedef struct { uint8_t *base; int64_t len; } gs_rhdr;\n"
        "typedef struct { gs_rhdr *hdr; gs_stack *stk; } gs_rref;\n"
        "typedef struct { gs_rhdr *hdr; gs_stack *stk; gs_rhdr *fl; gs_stack *flstk; } "
        "gs_pref;\n\n";
    used.insert("gs_rhdr");
    used.insert("gs_rref");
    used.insert("gs_pref");
}

// The C type of a fixed-class value (also refs/slices to anything). Emits
// the typedef on first use. Never valid for bytes-class values.
inline string CodeGen::CT(TypeExpr *t) {
    switch (t->kind) {
        case TY_INT:  assert(t->intstorage != IS_VARINT); return IntCT(t->intstorage);
        case TY_FLT:  return t->fltstorage == FS_F32 ? "gs_f32" : "gs_f64";
        case TY_BOOL: return "uint8_t";
        case TY_VOID: return "void";
        case TY_REF: {
            auto &r = *t->ref;
            if (r.lenstorage >= 0) {
                assert(r.lenstorage != IS_VARINT);
                return IntCT((IntStorage)r.lenstorage);   // Stored offset form.
            }
            EmitCoreTypes();
            if (IsResz(r.sub)) return "gs_rref";
            // A pointer needs only the pointee's name, so a node type can
            // hold references to itself and the body can come later.
            auto name = NameCT(t);
            if (cdefined.insert(Mangle(t)).second) {
                auto sub = IsBytesT(r.sub) ? string("uint8_t") :
                           StructLike(r.sub) ? NameCT(r.sub) : CT(r.sub);
                Append(tdecls, "typedef ", sub, " *", name, " GS_ALIGN1;\n");
            }
            return name;
        }
        default: break;
    }
    auto name = NameCT(t);
    auto m = Mangle(t);
    if (cdefined.count(m)) return name;
    // A variant's struct is a member of its enum's union. Emitting the enum
    // emits each variant's body ahead of the enum's own, also where a payload
    // names the enum again (through a slice), so a variant starts there.
    if (t->kind == TY_VARIANT && EIVar(t)->allfixed) {
        CT(t->var->adt);
        if (cdefined.count(m)) return name;
    }
    cdefined.insert(m);   // Before the body: recursion terminates via refs.
    string d;
    switch (t->kind) {
        case TY_SLICE: {
            if (StructLike(t->sub) && !IsBytesT(t->sub)) {
                // Like a reference, a slice needs only its element's name, and
                // the element's body may hold this slice (a node type with a
                // slice of its children): the typedef goes first, the body
                // after it.
                Append(tdecls, "typedef struct { ", NameCT(t->sub), " *data; int64_t len; } ",
                       name, ";\n\n");
                slicects.insert(name);
                CT(t->sub);
                return name;
            }
            auto e = IsBytesT(t->sub) ? string("uint8_t") : CT(t->sub);
            Append(d, "typedef struct { ", e, " *data; int64_t len; } ", name, ";\n");
            slicects.insert(name);
            break;
        }
        case TY_ARRAY: {
            auto &a = *t->arr;
            auto e = CT(a.sub);
            if (a.akind == A_FIXED) {
                auto k = ArrSize(t->arr);
                Append(d, "typedef struct { ", e, " e[", k ? k : 1, "]; } ", name, ";\n");
            } else {
                assert(a.akind == A_LIMITED && ArrSize(t->arr) >= 0);
                Append(d, "typedef struct { ", IntCT(LenStore(t->arr)), " len; ", e, " e[",
                       ArrSize(t->arr) ? ArrSize(t->arr) : 1, "]; } ", name, ";\n");
            }
            break;
        }
        case TY_STRUCT: {
            auto si = SI(t);
            Append(d, "struct ", name, " {\n");
            if (EmptyLayout(si->st->fields)) Append(d, "    uint8_t gs_empty;\n");
            else EmitCFields(d, si->st->fields, si->ftypes);
            Append(d, "};\n");
            break;
        }
        case TY_VARIANT: {
            auto ei = EIVar(t);
            auto vi = ei->en->VariantIndex(t->var->variant);
            auto &v = ei->en->variants[vi];
            Append(d, "struct ", name, " {\n");
            if (EmptyLayout(v.fields)) Append(d, "    uint8_t gs_empty;\n");
            else EmitCFields(d, v.fields, ei->vftypes[vi]);
            Append(d, "};\n");
            break;
        }
        case TY_ENUM: {
            assert(!t->enu->varmode);
            auto ei = EIOf(t);
            EnsureTagEnum(ei);
            Append(d, "struct ", name, " {\n    ", IntCT(TagStore(ei->en)), " tag;\n");
            string members;
            for (size_t vi = 0; vi < ei->en->variants.size(); vi++) {
                if (EmptyLayout(ei->en->variants[vi].fields)) continue;
                auto vt = VariantType(t, (int)vi);
                Append(members, "        ", CT(vt), " v_",
                       Sanitize(ei->en->variants[vi].name), ";\n");
            }
            if (!members.empty()) Append(d, "    union {\n", members, "    } u;\n");
            Append(d, "};\n");
            break;
        }
        default: assert(false);
    }
    tdecls += d;
    tdecls += "\n";
    return name;
}

// Fixed-class kinds that C declares as a named struct: they get a
// forward typedef the moment they are named (NameCT), so a reference to
// one needs only the name and the body follows when the type is first
// needed by value or through a pointer (PointeeLv).
inline bool CodeGen::StructLike(TypeExpr *t) {
    return t->kind == TY_STRUCT || t->kind == TY_VARIANT ||
           (t->kind == TY_ENUM && !t->enu->varmode);
}

// The C name of a fixed-class type, assigned (and, for struct-like kinds,
// forward-declared) on first sight; CT emits the body.
inline string CodeGen::NameCT(TypeExpr *t) {
    auto m = Mangle(t);
    auto it = ctypes.find(m);
    if (it != ctypes.end()) return it->second;
    EmitCoreTypes();
    auto name = Unique(m);
    ctypes[m] = name;
    if (StructLike(t)) Append(tdecls, "typedef struct ", name, " ", name, ";\n");
    return name;
}

inline void CodeGen::EmitCFields(string &d, const vector<Field> &fields,
                                 const vector<TypeExpr *> &ftypes) {
    // A frame object's tail is not part of the packed layout: it is the
    // tail's own header (or nested frame object) after the fixed fields.
    auto n = fields.size();
    while (n > 0 && ftypes[n - 1] && IsResz(ftypes[n - 1])) n--;
    vector<Field> pre(fields.begin(), fields.begin() + n);
    vector<TypeExpr *> pret(ftypes.begin(), ftypes.begin() + n);
    auto lo = LayoutFields(pre, pret);
    auto npad = 0;
    for (size_t i = 0; i < n; i++) {
        auto &f = fields[i];
        if (f.ispad) {
            auto next = i + 1 < n ? lo.offs[i + 1] : lo.size;
            auto sz = next - lo.offs[i];
            if (sz > 0) Append(d, "    uint8_t gs_pad", npad++, "[", sz, "];\n");
            continue;
        }
        Append(d, "    ", CT(ftypes[i]), " ", Sanitize(f.name), ";\n");
    }
    for (auto i = n; i < fields.size(); i++) {
        EmitCoreTypes();
        Append(d, "    ", RzCT(ftypes[i]), " ", Sanitize(fields[i].name), ";\n");
    }
}

inline void CodeGen::EnsureTagEnum(EnumInst *ei) {
    if (tagenums.count(ei)) return;
    tagenums.insert(ei);
    // Use the base (mode-less) mangle as the constant prefix, told apart
    // from another instance's as a mangle is (Mangle).
    string raw = Sanitize(ei->en->ns, ei->en->name);
    for (auto a : ei->args) Append(raw, "_", Mangle(a));
    auto base = raw;
    for (auto n = 1; !tagbases.insert(base).second; n++) base = cat(raw, "_m", n);
    tagprefix[ei] = base;
    string d = "enum {\n";
    for (size_t vi = 0; vi < ei->en->variants.size(); vi++)
        Append(d, "    ", base, "_", Sanitize(ei->en->variants[vi].name), "_k = ", vi,
               ",\n");
    d += "};\n\n";
    tdecls += d;
}

inline TypeExpr *CodeGen::VariantType(TypeExpr *enumtype, int vi) {
    auto ei = EIOf(enumtype);
    auto key = pair<EnumInst *, int>(ei, vi);
    auto it = varianttypes.find(key);
    if (it != varianttypes.end()) return it->second;
    auto t = ast.VariantTypeOf(enumtype, &ei->en->variants[vi], enumtype->line);
    // The fixed-mode base made for a variable-mode enum names the same
    // instantiation.
    if (t->var->adt != enumtype) t->var->adt->enu->inst = ei;
    return varianttypes[key] = t;
}

// ------------------------------------------------------------------
// Sizes of bytes-class values.

// The fewest bytes any value of this type can occupy: what bounds the
// element count a length field may claim, before a single element is walked.
// With `zero`, the size of the all-zeroes value instead, whose arrays are
// empty and whose ADTs are at variant 0: what a missed qpoll writes so the
// value stays valid.
inline int64_t CodeGen::MinBytes(TypeExpr *t, bool zero) {
    if (IsFix(t)) return FixedSize(t);
    switch (t->kind) {
        case TY_INT: case TY_REF: return 1;   // varint / varint-width relative ref
        case TY_ARRAY:
            switch (t->arr->akind) {
                case A_VAR:     return PrefixBytes(LenStore(t->arr));
                case A_LIMITED: return 8;     // runtime capacity: [cap][len]
                default:        return 0;
            }
        case TY_STRUCT: case TY_VARIANT: {
            int64_t n = 0;
            for (auto &run : FieldRuns(t)) {
                for (size_t i = 0; i < run.fields->size(); i++) {
                    auto &f = (*run.fields)[i];
                    if (f.ispad) { if (f.padsize > 0) n += f.padsize; continue; }
                    n += MinBytes((*run.ftypes)[i], zero);
                }
            }
            return n;
        }
        case TY_ENUM: {
            auto ei = EIOf(t);
            if (zero) return TagSize(ei->en) + MinBytes(VariantType(t, 0), true);
            int64_t m = INT64_MAX;
            for (size_t vi = 0; vi < ei->en->variants.size(); vi++)
                m = std::min(m, MinBytes(VariantType(t, (int)vi)));
            return TagSize(ei->en) + m;
        }
        default: return 0;
    }
}

// ------------------------------------------------------------------
// The verifier from_bytes runs over an untrusted image
// (docs/design/serialization.md §5).

// Does a value of this type hold a relative reference at any depth? Unlike
// HasRelRef this looks through variable-size types too, since an image's
// elements are exactly the types HasRelRef does not reach.
inline bool CodeGen::HasRelRefAny(TypeExpr *t) {
    return HasRelRefOf(t, true, [&](TypeExpr *x) { return FieldRuns(x); });
}

// Whether the verifier has anything to say about a value of this type, or
// can simply skip its bytes: a fixed value with no ADT tag to range-check,
// no boolean to validate, limited-array length to bound or relative reference to follow is
// opaque payload either way.
inline bool CodeGen::NeedsVerifyWalk(TypeExpr *t) {
    if (!IsFix(t)) return true;
    switch (t->kind) {
        case TY_BOOL: return true;
        case TY_INT:  return t->ix != nullptr;         // checked against its array
        case TY_REF:  return t->ref->lenstorage >= 0;
        case TY_ENUM: return true;                     // the tag is range-checked
        case TY_ARRAY:
            return t->arr->akind == A_LIMITED || NeedsVerifyWalk(t->arr->sub);
        case TY_STRUCT: case TY_VARIANT:
            return AnyField(t, [&](TypeExpr *ft) { return NeedsVerifyWalk(ft); });
        default: return false;
    }
}

// ------------------------------------------------------------------
// default<T>() (§4.2): all-zero bytes are the default of every fixed type
// -- numbers, false, null, empty slices and limited arrays, variant 0 --
// except where a field declares its own default, which the checker
// builds as a literal of the call's own (Call::defaultinit).

// Does any field at any depth of a fixed type declare a default value? An
// empty array has no fields.
inline bool CodeGen::HasFieldDefaults(TypeExpr *t) {
    switch (t->kind) {
        case TY_STRUCT: case TY_ENUM: case TY_VARIANT: {
            if (t->kind == TY_ENUM && t->enu->varmode) return false;
            auto runs = FieldRuns(t);
            if (t->kind == TY_ENUM) runs = runs.first(1);   // Variant 0 is the default variant.
            for (auto &run : runs)
                for (size_t i = 0; i < run.fields->size(); i++) {
                    auto &f = (*run.fields)[i];
                    if (!f.ispad && (f.defaultval || HasFieldDefaults((*run.ftypes)[i])))
                        return true;
                }
            return false;
        }
        case TY_ARRAY:
            return t->arr->akind == A_FIXED && ArrSize(t->arr) > 0 &&
                   HasFieldDefaults(t->arr->sub);
        default: return false;
    }
}

// ------------------------------------------------------------------
// Structural equality (§4.5): gap-free fixed types without floats compare
// as bytes.

inline bool CodeGen::BitwiseEq(TypeExpr *t) {
    if (!IsFix(t)) return false;
    switch (t->kind) {
        case TY_STRUCT: case TY_VARIANT:
            for (auto &run : FieldRuns(t))
                for (size_t i = 0; i < run.fields->size(); i++)
                    if ((*run.fields)[i].ispad || !BitwiseEq((*run.ftypes)[i])) return false;
            return true;
        case TY_ENUM: return false;      // Uninitialized trailing payload area.
        case TY_ARRAY:
            if (t->arr->akind == A_LIMITED) return false;   // Uninitialized slots.
            return BitwiseEq(t->arr->sub);
        case TY_FLT: return false;       // IEEE: -0.0 == 0.0, NaN != NaN.
        default: return true;
    }
}

}  // namespace goose
