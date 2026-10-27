// bitflags (docs/stdlib.md): a set of enum variants in one integer.
//
//   enum Permission { Read, Write, Execute }
//   type Permissions = bitflags::BitFlags<Permission, u8>;
//
//   var mode = Permissions { .. }.with_all([Permission.Read, Permission.Write]);
//   mode = mode.with(Permission.Execute).without(Permission.Read);
//   if mode.has(Permission.Write) { ... }
//   mode.bits                                                   // the raw integer
//   let mode, known = bitflags::of_bits<Permission, u8>(byte);  // known: every set bit was a flag
//   mode.has(Weekday.Monday)                                    // compile error: wrong enum
//
// Variant i is bit i. B is the storage: u8 holds up to 8 flags, u16 16,
// u32 32, u64 64. == compares sets. Every function takes the set first,
// so it can be called as a method; Permissions { .. } is the empty set.
// There is no | or &, since Goose has no operator overloading.
//
// bit_of is inlined and the C compiler folds it, so mode.has(flag) costs
// the same as a hand-written mask test.

namespace bitflags;

import std;

struct BitFlags<E, B> { bits: B = 0 }

// The flag's bit: 0 for the first variant, 1 for the next, and so on.
inline fn bit_of<E>(flag: E) -> i64 {
    var index = 0;
    const for <N, V> in variants<E> {
        match flag { V => { return index; }, _ => {} }
        index++;
    }
    return index;
}

// How many flags E has.
fn count<E>() -> i64 {
    var total = 0;
    const for <N, V> in variants<E> { total++; }
    return total;
}

// One set and one flag.
fn has<E, B>(set: BitFlags<E, B>, flag: E) -> bool { (set.bits & (1 << bit_of(flag))) != 0 }
fn with<E, B>(set: BitFlags<E, B>, flag: E) -> BitFlags<E, B> { BitFlags<E, B> { bits: set.bits | (1 << bit_of(flag)) } }
fn without<E, B>(set: BitFlags<E, B>, flag: E) -> BitFlags<E, B> { BitFlags<E, B> { bits: set.bits & ~(1 << bit_of(flag)) } }
fn toggle<E, B>(set: BitFlags<E, B>, flag: E) -> BitFlags<E, B> { BitFlags<E, B> { bits: set.bits ^ (1 << bit_of(flag)) } }

// The set plus every flag in the array.
fn with_all<E, B>(set: BitFlags<E, B>, flags: E[:]) -> BitFlags<E, B> {
    var result = set;
    for flag in flags { result = with(result, flag); }
    return result;
}

// all() is the set of every flag; the empty set is BitFlags<E, B> { .. }.
fn is_empty<E, B>(set: BitFlags<E, B>) -> bool { set.bits == 0 }
fn all<E, B>() -> BitFlags<E, B> {
    var set = BitFlags<E, B> { .. };
    const for <N, V> in variants<E> { set = with(set, V); }
    return set;
}

// Two sets.
fn union<E, B>(a: BitFlags<E, B>, b: BitFlags<E, B>) -> BitFlags<E, B> { BitFlags<E, B> { bits: a.bits | b.bits } }
fn intersection<E, B>(a: BitFlags<E, B>, b: BitFlags<E, B>) -> BitFlags<E, B> { BitFlags<E, B> { bits: a.bits & b.bits } }
fn difference<E, B>(a: BitFlags<E, B>, b: BitFlags<E, B>) -> BitFlags<E, B> { BitFlags<E, B> { bits: a.bits & ~b.bits } }
fn has_all<E, B>(set: BitFlags<E, B>, wanted: BitFlags<E, B>) -> bool { (set.bits & wanted.bits) == wanted.bits }
fn has_any<E, B>(set: BitFlags<E, B>, wanted: BitFlags<E, B>) -> bool { (set.bits & wanted.bits) != 0 }

// A set from a raw integer. The bool is false if a bit was set that is no
// flag: a strict decoder rejects that, a lenient one ignores it.
fn of_bits<E, B>(bits: B) -> BitFlags<E, B>, bool {
    let known = all<E, B>().bits;
    return BitFlags<E, B> { bits: bits & known }, (bits & ~known) == 0;
}

// Calls F(flag) for each flag in the set, first variant first.
fn each<E, B, F>(set: BitFlags<E, B>) {
    const for <N, V> in variants<E> {
        if has(set, V) { F(V); }
    }
}

// Appends the flag names: {Read, Write}, or {} when empty. print and str
// do not find a format overload on a generic type, so add one per type:
//
//   fn format(out: u8[>..]&, mode: Permissions) { bitflags::format_names(out, mode); }
fn format_names<E, B>(out: u8[>..]&, set: BitFlags<E, B>) {
    out.push('{');
    var first = true;
    const for <N, V> in variants<E> {
        if has(set, V) {
            if !first { format(out, ", "); }
            format(out, N);
            first = false;
        }
    }
    out.push('}');
}
