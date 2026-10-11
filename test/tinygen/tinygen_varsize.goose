// The tinygen backend: variable-size structs, one image of their fields
// laid out inline (a u8[] field and the fields after it at offsets the
// image's own lengths decide), as the C backend lays them out, so that
// test/tinygen_backend.py can leave either side to C: locals built from
// literals, copies and calls; fields read behind variable-size ones;
// fixed fields written; nested such structs; by-value and by-reference
// parameters; results built at the caller's destination (alone and beside
// a fixed result); rendered by print and str.

struct Employee { id: i32, name: u8[], dept: u8[u8], salary: i64 }
struct Team { lead: Employee, tag: u16, motto: u8[] }

fn hire(id: i32, name: const u8[:], salary: i64) -> Employee {
    return Employee { id: id, name: name, dept: "eng", salary: salary };
}

fn raise(e: Employee&, by: i64) { e.salary += by; }

fn pay(e: Employee) -> i64 { return e.salary + e.name.len; }

fn describe(e: Employee&) -> u8[] { str(e.name, "/", e.dept, " #", e.id, " $", e.salary) }

fn initials(t: Team&) -> u8[] {
    var out: u8[>..] = [];
    out.push(t.lead.name[0]);
    out.push(t.motto[0]);
    return out;
}

fn pick(a: Employee&, b: Employee&, first: bool) -> Employee {
    if first { return copy(a); }
    return copy(b);
}

fn with_count(e: Employee&) -> Employee, i64 { return copy(e), e.name.len; }

fn main() {
    var ada = Employee { id: 1, name: "Ada", dept: "math", salary: 100 };
    let bob = hire(2, "Bob", 90);
    print(ada.id, " ", ada.name, " ", ada.dept, " ", ada.salary);
    print(bob.name, " ", bob.dept, " ", bob.salary, " ", bob.name.len);
    raise(ada, 5);
    ada.id = 7;
    print(ada);
    print(pay(copy(ada)), " ", pay(hire(3, "Cy", 1)));
    print(describe(bob));
    let saved = copy(ada);
    raise(ada, 1);
    print(saved.salary, " ", ada.salary);
    let team = Team { lead: copy(bob), tag: 42, motto: "ship it" };
    print(team.lead.name, " ", team.tag, " ", team.motto, " ", team.lead.salary);
    print(initials(team));
    print(team);
    let p = pick(ada, bob, false);
    print(p.name, " ", pick(ada, bob, true).name);
    let e, n = with_count(bob);
    print(e.name, " ", n);
    for i in 3 {
        let tmp = hire(i as i32, "tmp", i * 10);
        print(str(tmp));
    }
}
