// runtime-debug: bitflags on three enums: Permission (three flags in a
// byte), Weekday (seven, so bit 7 is unused) and Month (twelve in a u16).
// One block per function.
import std;
import bitflags;

enum Permission { Read, Write, Execute }
type Permissions = bitflags::BitFlags<Permission, u8>;

enum Weekday { Monday, Tuesday, Wednesday, Thursday, Friday, Saturday, Sunday }
type Weekdays = bitflags::BitFlags<Weekday, u8>;

enum Month { January, February, March, April, May, June, July, August, September, October, November, December }
type Months = bitflags::BitFlags<Month, u16>;

// print needs a format overload per flag type.
fn format(out: u8[>..]&, mode: Permissions) { bitflags::format_names(out, mode); }
fn format(out: u8[>..]&, days: Weekdays) { bitflags::format_names(out, days); }
fn format(out: u8[>..]&, months: Months) { bitflags::format_names(out, months); }

fn read_write() -> Permissions { bitflags::with(bitflags::with(Permissions { .. }, Permission.Read), Permission.Write) }
fn weekend() -> Weekdays { bitflags::with(bitflags::with(Weekdays { .. }, Weekday.Saturday), Weekday.Sunday) }

fn main() {
    let none = Permissions { .. };
    assert(none.bits == 0);
    assert(bitflags::is_empty(none));
    assert(!bitflags::has(none, Permission.Read));
    print("empty set: ", none, " bits ", none.bits);

    assert(bitflags::bit_of(Permission.Read) == 0);
    assert(bitflags::bit_of(Permission.Write) == 1);
    assert(bitflags::bit_of(Permission.Execute) == 2);
    assert(bitflags::bit_of(Weekday.Sunday) == 6);
    assert(bitflags::bit_of(Month.December) == 11);
    print("bit_of: Read ", bitflags::bit_of(Permission.Read), " Write ", bitflags::bit_of(Permission.Write),
          " Execute ", bitflags::bit_of(Permission.Execute), " Sunday ", bitflags::bit_of(Weekday.Sunday),
          " December ", bitflags::bit_of(Month.December));

    assert(bitflags::count<Permission>() == 3);
    assert(bitflags::count<Weekday>() == 7);
    assert(bitflags::count<Month>() == 12);
    print("count: Permission ", bitflags::count<Permission>(), " Weekday ", bitflags::count<Weekday>(),
          " Month ", bitflags::count<Month>());

    let read = bitflags::with(none, Permission.Read);
    assert(read.bits == 1);
    let read_execute = bitflags::with(read, Permission.Execute);
    assert(read_execute.bits == 5);
    assert(read.bits == 1);                                         // with returns a new set
    assert(bitflags::with(read, Permission.Read) == read);          // a flag already in the set
    print("with: ", read, " ", read.bits, " then ", read_execute, " ", read_execute.bits);

    let both = Permissions { .. }.with_all([Permission.Read, Permission.Write]);
    assert(both == read_write());
    assert(both.with_all([]) == both);                              // no flags: unchanged
    assert(read.with_all([Permission.Write, Permission.Write]) == both);   // a flag twice: once
    assert(Months { .. }.with_all([Month.January, Month.December]).bits == 2049);
    print("with_all: ", both, " from [Read, Write]; ", Months { .. }.with_all([Month.January, Month.December]));

    let chained = Permissions { .. }.with(Permission.Read).with(Permission.Write).without(Permission.Read);
    assert(chained == bitflags::with(Permissions { .. }, Permission.Write));
    assert(chained.has(Permission.Write));
    assert(!chained.is_empty());
    print("method form: ", chained, " has Write ", chained.has(Permission.Write));

    let mode = read_write();
    assert(bitflags::has(mode, Permission.Read));
    assert(bitflags::has(mode, Permission.Write));
    assert(!bitflags::has(mode, Permission.Execute));
    print("has: ", mode, " Read ", bitflags::has(mode, Permission.Read), " Write ", bitflags::has(mode, Permission.Write),
          " Execute ", bitflags::has(mode, Permission.Execute));

    let read_only = bitflags::without(mode, Permission.Write);
    assert(read_only.bits == 1);
    assert(!bitflags::has(read_only, Permission.Write));
    assert(bitflags::without(mode, Permission.Execute) == mode);    // a flag not in the set
    assert(bitflags::is_empty(bitflags::without(read_only, Permission.Read)));
    print("without: ", mode, " minus Write is ", read_only, ", minus Read is ", bitflags::without(read_only, Permission.Read));

    let toggled = bitflags::toggle(mode, Permission.Execute);
    assert(toggled.bits == 7);
    assert(bitflags::toggle(toggled, Permission.Execute) == mode);
    assert(bitflags::toggle(mode, Permission.Read).bits == 2);
    print("toggle: ", mode, " Execute on ", toggled, ", Read off ", bitflags::toggle(mode, Permission.Read));

    assert(bitflags::all<Permission, u8>().bits == 7);
    assert(bitflags::all<Weekday, u8>().bits == 127);
    assert(bitflags::all<Month, u16>().bits == 4095);
    assert(bitflags::has_all(bitflags::all<Month, u16>(), bitflags::with(Months { .. }, Month.December)));
    print("all: ", bitflags::all<Permission, u8>(), " ", bitflags::all<Permission, u8>().bits,
          ", ", bitflags::all<Month, u16>(), " ", bitflags::all<Month, u16>().bits);

    let saturday_first = bitflags::with(bitflags::with(Weekdays { .. }, Weekday.Saturday), Weekday.Sunday);
    let sunday_first = bitflags::with(bitflags::with(Weekdays { .. }, Weekday.Sunday), Weekday.Saturday);
    assert(saturday_first == sunday_first);
    assert(weekend() == Weekdays { bits: 96 });
    assert(weekend() != Weekdays { .. });
    print("==: ", saturday_first, " == ", sunday_first, " ", saturday_first == sunday_first, ", bits ", weekend().bits);

    let friday = bitflags::with(Weekdays { .. }, Weekday.Friday);
    let long_weekend = bitflags::union(friday, weekend());
    assert(long_weekend.bits == 112);
    assert(bitflags::union(weekend(), weekend()) == weekend());
    assert(bitflags::union(weekend(), Weekdays { .. }) == weekend());
    print("union: ", friday, " + ", weekend(), " = ", long_weekend);

    let sunday_monday = bitflags::with(bitflags::with(Weekdays { .. }, Weekday.Sunday), Weekday.Monday);
    let shared = bitflags::intersection(weekend(), sunday_monday);
    assert(shared == bitflags::with(Weekdays { .. }, Weekday.Sunday));
    assert(bitflags::is_empty(bitflags::intersection(weekend(), friday)));
    print("intersection: ", weekend(), " & ", sunday_monday, " = ", shared, ", & ", friday, " = ", bitflags::intersection(weekend(), friday));

    let saturday = bitflags::difference(weekend(), sunday_monday);
    assert(saturday == bitflags::with(Weekdays { .. }, Weekday.Saturday));
    assert(bitflags::difference(weekend(), weekend()) == Weekdays { .. });
    assert(bitflags::difference(weekend(), friday) == weekend());
    print("difference: ", weekend(), " - ", sunday_monday, " = ", saturday);

    assert(bitflags::has_all(mode, read));
    assert(bitflags::has_all(mode, mode));
    assert(!bitflags::has_all(read, mode));
    assert(bitflags::has_all(read, none));                          // every set has all of the empty set
    print("has_all: ", mode, " has all of ", read, " ", bitflags::has_all(mode, read), "; ", read, " has all of ", mode, " ", bitflags::has_all(read, mode));

    let write_execute = bitflags::with(bitflags::with(none, Permission.Write), Permission.Execute);
    let execute = bitflags::with(none, Permission.Execute);
    assert(bitflags::has_any(mode, write_execute));
    assert(!bitflags::has_any(mode, execute));
    assert(!bitflags::has_any(mode, none));                         // no set has any of the empty set
    print("has_any: ", mode, " has any of ", write_execute, " ", bitflags::has_any(mode, write_execute), ", of ", execute, " ", bitflags::has_any(mode, execute));

    let from_three, three_known = bitflags::of_bits<Permission, u8>(3);
    assert(three_known && from_three == mode);
    let from_zero, zero_known = bitflags::of_bits<Permission, u8>(0);
    assert(zero_known && bitflags::is_empty(from_zero));
    let from_nine, nine_known = bitflags::of_bits<Permission, u8>(9);
    assert(!nine_known && from_nine == read);                       // bit 3 dropped, Read kept
    let all_days, all_days_known = bitflags::of_bits<Weekday, u8>(127);
    assert(all_days_known && all_days == bitflags::all<Weekday, u8>());
    let top_bit, top_bit_known = bitflags::of_bits<Weekday, u8>(128);
    assert(!top_bit_known && bitflags::is_empty(top_bit));
    let year, year_known = bitflags::of_bits<Month, u16>(0xFFFF);
    assert(!year_known && year == bitflags::all<Month, u16>());
    print("of_bits: 3 -> ", from_three, " known ", three_known, "; 9 -> ", from_nine, " known ", nine_known,
          "; 128 -> ", top_bit, " known ", top_bit_known, "; 0xFFFF -> ", year.bits, " known ", year_known);

    var seen: u8[>..] = [];
    bitflags::each(long_weekend) { day => format(seen, " ", day); };
    assert(seen == " Weekday.Friday Weekday.Saturday Weekday.Sunday");
    var visits = 0;
    bitflags::each(Months { .. }) { month => visits++; };
    assert(visits == 0);
    print("each:", seen);

    assert(str(mode) == "{Read, Write}");
    assert(str(none) == "{}");
    assert(str(bitflags::all<Month, u16>()) == "{January, February, March, April, May, June, July, August, September, October, November, December}");
    print("format_names: ", mode, " ", none, " ", weekend());

    print("bitflags: seventeen functions on Permission, Weekday and Month passed");
}
