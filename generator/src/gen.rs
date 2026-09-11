//! Writing Xag programs that are worth running.
//!
//! Everything here produces a program the compiler accepts. A generator that
//! writes programs the checker rejects is testing diagnostics, which is a
//! different job — and a rejected program is reported as a fault in *this*
//! file rather than counted as a finding.
//!
//! Text is written straight into one reused buffer. No intermediate strings,
//! no formatting machinery: at forty milliseconds a case the cost is all in
//! processes, and the generator should never become the reason it is not.

use crate::rng::Rng;

/// Every whole-number type there is. A size is always written, so the generator
/// has to choose one — and two different ones never meet in an expression,
/// because nothing converts on its own.
const WHOLE: [&str; 10] = [
    "int8", "int16", "int32", "int64", "int128", "uint8", "uint16", "uint32", "uint64",
    "uint128",
];

fn signed(which: u8) -> bool {
    which < 5
}

/// A literal that fits, whatever was written. `int8` holds 127 and no more, and
/// nothing unsigned holds anything below zero.
fn literal_range(which: u8) -> (i64, i64) {
    let low = if signed(which) {
        if which == 0 { -100 } else { -1000 }
    } else {
        0
    };
    let high = if which == 0 || which == 5 { 100 } else { 1000 };
    (low, high)
}

/// IEEE 754 binary, at the widths this machine's compiler can represent.
/// `bin128` is missing on purpose: there is no type behind it yet.
const BINARY: [&str; 4] = ["bin16", "bin32", "bin64", "bin128"];

/// IEEE 754 decimal, which counts in tens and keeps the places it was given.
const DECIMAL: [&str; 3] = ["deci32", "deci64", "deci128"];

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum Ty {
    Whole(u8),
    Real(u8),
    Deci(u8),
    Bool,
    Str,
}

impl Ty {
    fn written(self) -> &'static str {
        match self {
            Ty::Whole(which) => WHOLE[which as usize],
            Ty::Real(which) => BINARY[which as usize],
            Ty::Deci(which) => DECIMAL[which as usize],
            Ty::Bool => "bool",
            Ty::Str => "str",
        }
    }
}

/// What `count` answers with, which is the one whole type the generator does
/// not get to choose.
const COUNTED: Ty = Ty::Whole(3);

#[derive(Clone)]
struct Var {
    name: String,
    ty: Ty,
    mutable: bool,
    /// How many places it has, when it is a `many` of `ty` rather than one of
    /// them. A `many` never stands where its element type would, so everything
    /// that picks a name by type has to look past these.
    many: Option<u32>,
    /// Whether it may be grown. A `many-growing`, which is a second type: the
    /// length recorded above is what it was *made* with and never falls, so an
    /// index below it stays safe however much it grows. Nothing counts a growth
    /// toward that, because a growth written inside an arm that does not run
    /// never happens.
    grows: bool,
    /// How many places each of *those* has, when this is a `many` of a `many`.
    /// Every row the same length, so that reaching into one is safe wherever it
    /// is reached from — the language allows rows of different lengths, and a
    /// generated program that indexed the short one would be this file's
    /// mistake rather than a finding.
    inner: Option<u32>,
    moved: bool,
    /// Lent out right now. Nothing may be handed over, changed or lent again
    /// while this is set, which is the whole of what the region pass checks.
    lent: bool,
    /// Which struct it is, when it is one. Like `many`, a struct never stands
    /// where one of the things it holds would.
    group: Option<usize>,
    /// Which `one-of` it is, when it is one. Nothing may be done with one but
    /// ask which case it is in: showing it, writing it out and comparing two
    /// are all the same impossible question, and all refused.
    sum: Option<usize>,
    /// Which of the things it holds have gone on their own. A field is known
    /// where it is written, so this is tracked one at a time — which is the
    /// whole of what a struct's ownership has that an array's does not.
    parts_moved: Vec<String>,
}

/// One of the things a struct holds: a plain value, or another struct, which is
/// what makes a group of items nest.
#[derive(Clone, Copy)]
enum Held {
    Plain(Ty),
    Group(usize),
    /// A borrow of one of these, held by the struct for as long as the struct
    /// lives. Nothing had ever written such a struct — no example, no test, and
    /// not this file — and it did not build at all: the checker took it, both
    /// interpreters answered, and LLVM's own verifier refused the module.
    ///
    /// Text included, which asks a second question the others do not: who ends
    /// it. A borrow ends nothing, so the struct has to leave it alone and
    /// whoever lent it has to still have it afterwards. Every case checks its
    /// own allocation balance, so a struct that freed what it borrowed would be
    /// a finding rather than a silence.
    Lent(Ty),
    /// One that may hold nothing. Built, asked, and let go of — and nothing
    /// wrote one until now, so a value went straight into a field shaped
    /// `{ is it there, what it is }` and the module came out ill-formed.
    Maybe(Ty),
}

/// A struct the program declared, and what it holds in the order it holds it.
struct Shape {
    name: String,
    fields: Vec<(String, Held)>,
}

/// A `one-of` the program declared, and what it may be. `None` is a case that
/// carries nothing, which is written bare where a value goes.
struct Sum {
    name: String,
    cases: Vec<(String, Option<Ty>)>,
}

struct Fun {
    name: String,
    params: Vec<Ty>,
    answers: Ty,
}

pub struct Writer<'a> {
    rng: &'a mut Rng,
    out: &'a mut String,
    scopes: Vec<Vec<Var>>,
    funs: Vec<Fun>,
    shapes: Vec<Shape>,
    sums: Vec<Sum>,
    consts: Vec<Var>,
    next_name: u32,
    /// What the file was written from, so that what it says can say so.
    seed: u64,
    /// Names a loop steps itself, which nothing may borrow into a struct. The
    /// borrow would outlive the statement that took it and the next turn would
    /// change what it points at — refused, and rightly, but the refusal would
    /// be this file's mistake rather than a finding.
    ///
    /// Kept apart from `lent` on purpose: a counter may still be read, lent for
    /// the length of a call, and assigned to. It is only holding a borrow of one
    /// *across* the step that cannot work.
    stepped: Vec<String>,
    indent: usize,
    size: u32,
}

/// A whole program, written into `out`.
///
/// `size` is roughly how many statements START gets. It matters more than it
/// looks: every case pays about 170ms for macOS to scan a freshly linked
/// binary before it will run it once, which dwarfs compiling and running put
/// together. That cost is per *binary*, not per statement, so the way to test
/// more per second is to ask each binary to carry more.
pub fn generate(seed: u64, size: u32, out: &mut String) {
    out.clear();
    let mut rng = Rng::from_seed(seed);
    let mut writer = Writer {
        rng: &mut rng,
        out,
        scopes: Vec::new(),
        funs: Vec::new(),
        shapes: Vec::new(),
        sums: Vec::new(),
        consts: Vec::new(),
        next_name: 0,
        seed,
        stepped: Vec::new(),
        indent: 0,
        size,
    };
    writer.program();
}

impl<'a> Writer<'a> {
    fn pad(&mut self) {
        for _ in 0..self.indent {
            self.out.push_str("    ");
        }
    }

    fn fresh(&mut self) -> String {
        let name = format!("v{}", self.next_name);
        self.next_name += 1;
        name
    }

    fn declare(&mut self, var: Var) {
        self.scopes.last_mut().unwrap().push(var);
    }

    /// Everything a name could mean here, constants included.
    fn visible(&self, ty: Ty, want_mutable: bool) -> Vec<&Var> {
        let mut seen: Vec<&Var> = Vec::new();
        for scope in &self.scopes {
            for var in scope {
                if var.ty == ty && var.many.is_none() && var.group.is_none()
                   && var.sum.is_none() && !var.moved && !var.lent &&
                   (!want_mutable || var.mutable) {
                    seen.push(var);
                }
            }
        }
        if !want_mutable {
            for var in &self.consts {
                if var.ty == ty && var.many.is_none() && var.group.is_none() && var.sum.is_none() {
                    seen.push(var);
                }
            }
        }
        seen
    }

    fn pick_whole(&mut self) -> Ty {
        // Every family gets a turn, so the oracle sees all four.
        match self.rng.below(10) {
            0..=1 => Ty::Real(self.rng.below(BINARY.len() as u32) as u8),
            2..=3 => Ty::Deci(self.rng.below(DECIMAL.len() as u32) as u8),
            _ => Ty::Whole(self.rng.below(WHOLE.len() as u32) as u8),
        }
    }

    fn numeric(ty: Ty) -> bool {
        matches!(ty, Ty::Whole(_) | Ty::Real(_) | Ty::Deci(_))
    }

    /// A whole type some visible name already has, so an expression can be
    /// built out of more than literals.
    fn whole_in_scope(&mut self) -> Option<Ty> {
        let mut seen: Vec<Ty> = Vec::new();
        for scope in &self.scopes {
            for var in scope {
                if Self::numeric(var.ty) && var.many.is_none() && var.group.is_none() && var.sum.is_none() && !var.moved && !var.lent {
                    seen.push(var.ty);
                }
            }
        }
        for var in &self.consts {
            if Self::numeric(var.ty) && var.many.is_none() && var.group.is_none() && var.sum.is_none() {
                seen.push(var.ty);
            }
        }
        if seen.is_empty() {
            return None;
        }
        let at = self.rng.below(seen.len() as u32) as usize;
        Some(seen[at])
    }

    fn pick_name(&mut self, ty: Ty) -> Option<String> {
        // Names are copied out before the generator is asked to choose, so that
        // reading the scope and drawing a number are not asking at once.
        let seen: Vec<String> =
            self.visible(ty, false).iter().map(|var| var.name.clone()).collect();
        if seen.is_empty() {
            return None;
        }
        let at = self.rng.below(seen.len() as u32) as usize;
        Some(seen[at].clone())
    }

    // ---- the program

    fn program(&mut self) {
        // A file is three blocks, and a generated one is a file like any other.
        // What it says is what wrote it and what asked for it, so a case kept
        // from a failing run says where it came from.
        self.out.push_str("READ_ME {\nWritten by xag-oracle from seed ");
        self.out.push_str(&self.seed.to_string());
        self.out.push_str(".\n}\n\nPREP {\n");

        // Something to hand a `str` to, so that moves and their drop flags get
        // written as well as read.
        self.out
            .push_str("fn.nothing 'consume' [str 't'] {\n    print.stdout['t' \\n];\n}\n\n");
        self.out.push_str(
            "fn.nothing 'look' [loan.str 't'] {\n    print.stdout[(count['t']) \\n];\n}\n\n");
        self.out.push_str(
            "fn.nothing 'edit' [loanmut.str 't'] {\n    set 't' = ['t' *!*];\n}\n\n");

        // Three generics, written into every program whether or not anything
        // calls one. A generic nobody calls is written out not at all, which is
        // itself worth generating.
        //
        // They are fixed rather than random because what a generic body may do
        // depends on every type it is ever called with, and the generator does
        // not know that when it writes the body. These three do only what every
        // type can do — be looked at, be asked about, be handed back.

        // Every kind it could be handed, so no call can fail to be covered.
        self.out.push_str(concat!(
            "fn.str 'describe' [loan.any 'v'] {\n",
            "    whichever 'v' {\n",
            "        is number     { give [convert-to-str['v']]; }\n",
            "        is bool       { give [convert-to-str['v']]; }\n",
            "        is str        { give [convert-to-str[count['v']]]; }\n",
            "        is many       { give [convert-to-str[count['v']]]; }\n",
            "        is struct     { give [str:*group*]; }\n",
            "        is or-nothing { give [str:*maybe*]; }\n",
            "    }\n}\n\n"));

        // Walking a struct nobody wrote this function to know about, and
        // asking each field what it is on the way past.
        self.out.push_str(concat!(
            "fn.str 'parts-of' [loan.any 'v'] {\n",
            "    var.mut.str 'out' = [str:*<*];\n",
            "    loop.parts 'p' = ['v'] {\n",
            "        set 'out' = ['out' 'p'.name str:*=* (describe[loan 'p'.value]) str:* *];\n",
            "    }\n",
            "    give ['out' str:*>*];\n}\n\n"));

        // The floor: what can be done with a blank when nothing narrows it.
        self.out.push_str("fn.any 'same' [any 'v'] { give ['v']; }\n\n");
        self.funs.push(Fun {
            name: "consume".to_string(),
            params: vec![Ty::Str],
            answers: COUNTED, // never called for its answer
        });

        let constants = self.rng.below(3);
        for _ in 0..constants {
            let name = self.fresh();
            let ty = if self.rng.chance(70) { self.pick_whole() } else { Ty::Str };
            self.out.push_str("const.");
            self.out.push_str(ty.written());
            self.out.push_str(" '");
            self.out.push_str(&name);
            self.out.push_str("' = [");
            self.literal(ty);
            self.out.push_str("];\n");
            self.consts.push(Var { name, ty, mutable: false, many: None, moved: false, lent: false, group: None, sum: None, parts_moved: Vec::new(), inner: None, grows: false });
        }
        if constants > 0 {
            self.out.push('\n');
        }

        // At least two, most of the time: one struct is the program that cannot
        // tell a right answer from a wrong one, and two is what lets a group of
        // items nest.
        let shapes = self.rng.below(3) + 1;
        for _ in 0..shapes {
            self.shape();
        }
        if shapes > 0 {
            self.out.push('\n');
        }

        // A type that is one of several things, sometimes. Not every program:
        // one that declares one and never asks which case it is in says less
        // about the type than a shorter program that does.
        let sums = if self.rng.chance(45) { self.rng.below(2) + 1 } else { 0 };
        for _ in 0..sums {
            self.sum();
        }
        if sums > 0 {
            self.out.push('\n');
        }

        let functions = self.rng.below(3) + self.size / 12;
        for _ in 0..functions {
            self.function();
        }

        // Everything that lasts the whole program has been written by here.
        self.out.push_str("}\n\nSTART {\n");
        self.scopes.push(Vec::new());
        self.indent = 1;
        let statements = self.rng.below(self.size / 2 + 1) + self.size / 2 + 1;
        self.body(statements);
        self.finish_scope();
        self.scopes.pop();
        self.out.push_str("}\n");
    }

    /// `struct 'v3' [int64 'v4', str 'v5']` — two or three things, of types the
    /// rest of the generator already knows how to write.
    fn shape(&mut self) {
        let name = self.fresh();
        let count = self.rng.below(2) + 2;
        let mut fields: Vec<(String, Held)> = Vec::new();
        self.out.push_str("struct '");
        self.out.push_str(&name);
        self.out.push_str("' [");
        for i in 0..count {
            if i > 0 {
                self.out.push_str(", ");
            }
            // One of the ones already declared, sometimes, so that a group of
            // items has somewhere to nest. Only an earlier one: a struct that
            // holds itself has no size.
            let held = if !self.shapes.is_empty() && self.rng.chance(25) {
                Held::Group(self.rng.below(self.shapes.len() as u32) as usize)
            } else if self.rng.chance(20) {
                let ty = match self.rng.below(10) {
                    0..=1 => Ty::Str,
                    2 => Ty::Bool,
                    _ => self.pick_whole(),
                };
                Held::Lent(ty)
            } else if self.rng.chance(18) {
                let ty = match self.rng.below(10) {
                    0..=3 => Ty::Str,
                    4 => Ty::Bool,
                    _ => self.pick_whole(),
                };
                Held::Maybe(ty)
            } else {
                Held::Plain(match self.rng.below(10) {
                    0..=2 => Ty::Str,
                    3 => Ty::Bool,
                    _ => self.pick_whole(),
                })
            };
            let field = self.fresh();
            match held {
                Held::Plain(ty) => self.out.push_str(ty.written()),
                Held::Lent(ty) => {
                    self.out.push_str("loan.");
                    self.out.push_str(ty.written());
                }
                Held::Maybe(ty) => {
                    self.out.push_str("or-nothing.");
                    self.out.push_str(ty.written());
                }
                Held::Group(which) => {
                    let name = self.shapes[which].name.clone();
                    self.out.push_str(&name);
                }
            }
            self.out.push_str(" '");
            self.out.push_str(&field);
            self.out.push('\'');
            fields.push((field, held));
        }
        self.out.push_str("]\n");
        self.shapes.push(Shape { name, fields });
    }

    /// `one-of 'v3' [int64 'v4', str 'v5', nothing 'v6']` — two to four cases,
    /// at least one of which carries something. A case may hold text or a
    /// struct, which is what makes the letting-go worth checking: what the live
    /// case holds goes when the value does, and which case that is is read
    /// where it ends.
    fn sum(&mut self) {
        let name = self.fresh();
        let count = self.rng.below(3) + 2;
        let mut cases: Vec<(String, Option<Ty>)> = Vec::new();
        self.out.push_str("one-of '");
        self.out.push_str(&name);
        self.out.push_str("' [");
        for i in 0..count {
            if i > 0 {
                self.out.push_str(", ");
            }
            // The last case carries nothing sometimes, and never the first —
            // a `one-of` of nothing but empties is a choice with no values in
            // it, which is a thing to write but a dull one to check.
            // Text sometimes, which is what makes the letting-go worth
            // checking: a case holding one owns it, and it goes when the value
            // does. A case holding a struct or a `many` is the same walk one
            // level further down, and is asked about by the tests rather than
            // here — writing a struct value after a colon is a shape this file
            // does not otherwise build.
            let held = if i > 0 && self.rng.chance(25) {
                None
            } else if self.rng.chance(25) {
                Some(Ty::Str)
            } else if self.rng.chance(15) {
                Some(Ty::Bool)
            } else {
                Some(self.pick_whole())
            };
            let case = self.fresh();
            self.out.push_str(match held {
                Some(ty) => ty.written(),
                None => "nothing",
            });
            self.out.push_str(" '");
            self.out.push_str(&case);
            self.out.push('\'');
            cases.push((case, held));
        }
        self.out.push_str("]\n");
        self.sums.push(Sum { name, cases });
    }

    /// `var.v3 'v7' = [v4:*12*];` — one of the things it may be, said where the
    /// value is made.
    fn sum_declaration(&mut self) {
        if self.sums.is_empty() {
            self.declaration();
            return;
        }
        let which = self.rng.below(self.sums.len() as u32) as usize;
        let at = self.rng.below(self.sums[which].cases.len() as u32) as usize;
        let (case, held) = self.sums[which].cases[at].clone();
        let type_name = self.sums[which].name.clone();
        let name = self.fresh();
        // Never `mut`: nothing here writes over a `one-of` afterwards, and a
        // `mut` nothing uses is `W0003`.
        self.pad();
        self.out.push_str("var.");
        self.out.push_str(&type_name);
        self.out.push_str(" '");
        self.out.push_str(&name);
        self.out.push_str("' = [");
        self.out.push_str(&case);
        if let Some(ty) = held {
            self.out.push(':');
            if ty == Ty::Str {
                // One item, and text is several of them joined — which inside
                // brackets is a group, and a group holds one value.
                // Written, not taken from another name: what goes into a case
                // is handed over, so a name would have to say `move` and then
                // be gone — bookkeeping this file would have to carry for a
                // value it can write in place instead.
                self.out.push('*');
                self.word();
                self.out.push('*');
            } else {
                // Bracketed, because the colon takes one item and an expression
                // is one item only when something says where it ends —
                // `v6:*1* ^ *2*` reads as `(v6:*1*) ^ *2*`, which asks a power
                // of a `one-of`.
                self.out.push('(');
                self.expr(ty, 1);
                self.out.push(')');
            }
        }
        self.out.push_str("];\n");
        self.declare(Var {
            name,
            ty: Ty::Bool, // never asked: nothing picks a `one-of` by its type
            mutable: false,
            many: None,
            moved: false,
            lent: false,
            group: None,
            sum: Some(which),
            parts_moved: Vec::new(),
            inner: None,
            grows: false,
        });
    }

    /// `when 'v7' { is v4 'v9' { … } … }` — every case, once each, which is
    /// what the compiler insists on and the only way to reach what is inside.
    fn sum_when(&mut self) {
        let mut seen: Vec<(String, usize)> = Vec::new();
        for scope in &self.scopes {
            for var in scope {
                if let Some(which) = var.sum {
                    if !var.moved && !var.lent {
                        seen.push((var.name.clone(), which));
                    }
                }
            }
        }
        if seen.is_empty() {
            self.print();
            return;
        }
        let at = self.rng.below(seen.len() as u32) as usize;
        let (name, which) = seen[at].clone();
        let cases = self.sums[which].cases.clone();
        self.pad();
        self.out.push_str("when '");
        self.out.push_str(&name);
        self.out.push_str("' {\n");
        self.indent += 1;
        for (case, held) in &cases {
            self.pad();
            self.out.push_str("is ");
            self.out.push_str(case);
            match held {
                Some(ty) => {
                    let bound = self.fresh();
                    self.out.push_str(" '");
                    self.out.push_str(&bound);
                    self.out.push_str("' {\n");
                    self.indent += 1;
                    // Lent for the arm, so it is read and nothing else.
                    self.pad();
                    self.out.push_str("print.stdout['");
                    self.out.push_str(&bound);
                    self.out.push_str("' \\n];\n");
                    self.indent -= 1;
                    let _ = ty;
                }
                None => {
                    self.out.push_str(" {\n");
                    self.indent += 1;
                    self.pad();
                    self.out.push_str("print.stdout[str:*none* \\n];\n");
                    self.indent -= 1;
                }
            }
            self.pad();
            self.out.push_str("}\n");
        }
        self.indent -= 1;
        self.pad();
        self.out.push_str("}\n");
    }

    /// Names holding a struct that nobody is using for anything else.
    fn groups(&mut self, want_mutable: bool, whole: bool) -> Vec<(String, usize)> {
        let mut seen = Vec::new();
        for scope in &self.scopes {
            for var in scope {
                if let Some(which) = var.group {
                    if var.many.is_none() && !var.moved && !var.lent
                        && (!want_mutable || var.mutable)
                        && (!whole || var.parts_moved.is_empty())
                    {
                        seen.push((var.name.clone(), which));
                    }
                }
            }
        }
        seen
    }

    fn pick_group(&mut self, want_mutable: bool, whole: bool) -> Option<(String, usize)> {
        let seen = self.groups(want_mutable, whole);
        if seen.is_empty() {
            return None;
        }
        let at = self.rng.below(seen.len() as u32) as usize;
        Some(seen[at].clone())
    }

    /// Every way into a struct that ends at a plain value, written as the path
    /// it takes: `v4`, or `v4.v7` where one of them holds another struct.
    /// Where each of the things a struct holds can be reached, and what is
    /// there. `writable` leaves out the borrowed ones: a borrow may be read
    /// through and never written through or taken out, so a `set` or a `move`
    /// reaching one is a program the compiler rightly refuses — which would be
    /// this file's mistake rather than a finding.
    fn paths(&self, which: usize, gone: &[String], depth: u32,
             writable: bool) -> Vec<(String, Ty)> {
        let mut out = Vec::new();
        for (field, held) in &self.shapes[which].fields {
            if depth == 0 && gone.contains(field) {
                continue;
            }
            match held {
                Held::Plain(ty) => out.push((field.clone(), *ty)),
                Held::Lent(ty) if !writable => out.push((field.clone(), *ty)),
                Held::Lent(_) => {}
                // Reached only by asking. Showing one would not say which of
                // the two it is, and that is refused where it is written.
                Held::Maybe(_) => {}
                Held::Group(inner) if depth < 2 => {
                    for (rest, ty) in self.paths(*inner, gone, depth + 1, writable) {
                        out.push((format!("{field}.{rest}"), ty));
                    }
                }
                Held::Group(_) => {}
            }
        }
        out
    }

    /// One of the things a struct holds that has not gone anywhere.
    /// Something of this type to borrow from, for a struct to hold. Unlike
    /// every other picker this does not look past a name already lent: two
    /// read loans of one thing are two read loans of one thing, and a struct
    /// with two borrowed fields of one type has to come from somewhere.
    fn lendable_copy(&mut self, ty: Ty) -> Option<String> {
        let mut seen: Vec<String> = Vec::new();
        for scope in &self.scopes {
            for var in scope {
                if var.ty == ty && var.many.is_none() && var.group.is_none() && var.sum.is_none() && !var.moved
                    && !self.stepped.contains(&var.name)
                {
                    seen.push(var.name.clone());
                }
            }
        }
        if seen.is_empty() {
            return None;
        }
        let at = self.rng.below(seen.len() as u32) as usize;
        Some(seen[at].clone())
    }

    /// Whether every borrow this struct holds has something to borrow from.
    /// A struct is declared at the top of the file and built later on, so the
    /// building has to ask rather than assume.
    fn buildable(&self, which: usize, depth: u32) -> bool {
        for (_, held) in &self.shapes[which].fields {
            match held {
                Held::Lent(ty) => {
                    let mut found = false;
                    for scope in &self.scopes {
                        for var in scope {
                            if var.ty == *ty && var.many.is_none() && var.group.is_none() && var.sum.is_none()
                                && !var.moved
                                && !self.stepped.contains(&var.name)
                            {
                                found = true;
                            }
                        }
                    }
                    if !found {
                        return false;
                    }
                }
                Held::Group(inner) if depth < 3 => {
                    if !self.buildable(*inner, depth + 1) {
                        return false;
                    }
                }
                _ => {}
            }
        }
        true
    }

    fn pick_field(&mut self, name: &str, which: usize, want: Option<Ty>,
                  writable: bool) -> Option<(String, Ty)> {
        let gone: Vec<String> = self
            .scopes
            .iter()
            .flatten()
            .find(|v| v.name == name)
            .map(|v| v.parts_moved.clone())
            .unwrap_or_default();
        let here: Vec<(String, Ty)> = self
            .paths(which, &gone, 0, writable)
            .into_iter()
            .filter(|(_, ty)| want.map_or(true, |w| *ty == w))
            .collect();
        if here.is_empty() {
            return None;
        }
        let at = self.rng.below(here.len() as u32) as usize;
        Some(here[at].clone())
    }

    fn note_part_moved(&mut self, name: &str, field: &str) {
        for scope in self.scopes.iter_mut() {
            if let Some(var) = scope.iter_mut().find(|v| v.name == name) {
                var.parts_moved.push(field.to_string());
                return;
            }
        }
    }

    /// `var.v3 'v9' = [*1* *hello*];`
    fn group_declaration(&mut self) {
        if self.shapes.is_empty() {
            self.print();
            return;
        }
        let which = self.rng.below(self.shapes.len() as u32) as usize;
        if !self.buildable(which, 0) {
            self.print();
            return;
        }
        let mutable = self.rng.chance(60);
        let name = self.fresh();
        let shape_name = self.shapes[which].name.clone();
        self.pad();
        self.out.push_str("var.");
        if mutable {
            self.out.push_str("mut.");
        }
        self.out.push_str(&shape_name);
        self.out.push_str(" '");
        self.out.push_str(&name);
        self.out.push_str("' = [");
        self.group_items(which);
        self.out.push_str("];\n");
        self.declare(Var {
            name: name.clone(),
            ty: Ty::Bool, // never asked for: everything picking by type looks past a group
            mutable,
            many: None,
            moved: false,
            lent: false,
            group: Some(which), sum: None,
            parts_moved: Vec::new(),
            inner: None,
            grows: false,
        });
        // `mut` asks for something that then happens, most of the time. This
        // was the largest source of `W0003` by a long way: thirty warnings to
        // one program, every one of them right, and every one of them noise on
        // top of whatever the case was actually written to find.
        if mutable && self.rng.chance(80) {
            self.set_a_field(&name, which);
        }
    }

    /// One item for each of the things it holds, in order — and a group of
    /// items where one of them is itself a struct.
    fn group_items(&mut self, which: usize) {
        let held: Vec<Held> = self.shapes[which].fields.iter().map(|(_, h)| *h).collect();
        for (i, one) in held.iter().enumerate() {
            if i > 0 {
                self.out.push(' ');
            }
            match one {
                // Every one of them is a place of its own, so text is written
                // whole rather than as pieces that would join anywhere else.
                Held::Plain(ty) => self.expr(*ty, if *ty == Ty::Str { 0 } else { 1 }),
                Held::Maybe(ty) => {
                    // Either a value or none, because both have to be written:
                    // an absence writes its own pair, and a value has to be
                    // wrapped on the way in.
                    if self.rng.chance(35) {
                        self.out.push_str("nothing");
                    } else {
                        self.expr(*ty, if *ty == Ty::Str { 0 } else { 1 });
                    }
                }
                Held::Lent(ty) => {
                    // Whatever is here to borrow from. `buildable` has already
                    // said there is something, so this cannot come back empty.
                    let name = self.lendable_copy(*ty).unwrap_or_default();
                    self.out.push_str("loan '");
                    self.out.push_str(&name);
                    self.out.push('\'');
                    // Held for as long as the struct is, so nothing may hand it
                    // over, change it, or lend it for writing from here on.
                    self.markLent(&name, true);
                }
                Held::Group(inner) => {
                    // Named where it is made: a word before a bracket is a
                    // call, where a name before one is an index.
                    let name = self.shapes[*inner].name.clone();
                    self.out.push_str(&name);
                    self.out.push('[');
                    self.group_items(*inner);
                    self.out.push(']');
                }
            }
        }
    }

    /// `print.stdout['v9'.v4 \n];`
    fn group_read(&mut self) {
        let Some((name, which)) = self.pick_group(false, false) else {
            self.print();
            return;
        };
        let Some((field, ty)) = self.pick_field(&name, which, None, false) else {
            self.print();
            return;
        };
        self.pad();
        self.out.push_str("print.stdout['");
        self.out.push_str(&name);
        self.out.push_str("'.");
        self.out.push_str(&field);
        if ty == Ty::Str {
            self.out.push_str(" \\n];\n");
        } else {
            self.out.push_str(" \\n];\n");
        }
    }

    /// `set 'v9'.v4 = […];` — one of them, leaving the rest where they were.
    fn group_set(&mut self) {
        let Some((name, which)) = self.pick_group(true, false) else {
            self.print();
            return;
        };
        self.set_a_field(&name, which);
    }

    /// `set 'v3'.v4 = […]` — one of the things a struct holds, written. A field
    /// the struct only borrows is not one of them: a borrow may be read
    /// through and never written through, so reaching one would be a program
    /// the compiler rightly refuses.
    fn set_a_field(&mut self, name: &str, which: usize) {
        let name = name.to_string();
        let Some((field, ty)) = self.pick_field(&name, which, None, true) else {
            self.print();
            return;
        };
        self.pad();
        self.out.push_str("set '");
        self.out.push_str(&name);
        self.out.push_str("'.");
        self.out.push_str(&field);
        self.out.push_str(" = [");
        self.expr(ty, if ty == Ty::Str { 0 } else { 1 });
        self.out.push_str("];\n");
    }

    /// `consume[move 'v9'.v5];` — one of them handed over on its own, which is
    /// the thing a struct has that an array does not.
    fn group_part_moved(&mut self) {
        let Some((name, which)) = self.pick_group(false, false) else {
            self.print();
            return;
        };
        let Some((field, _)) = self.pick_field(&name, which, Some(Ty::Str), true) else {
            self.print();
            return;
        };
        // Only one written at the top, because what is tracked here is which of
        // this struct's own things have gone.
        if field.contains('.') {
            self.print();
            return;
        }
        self.pad();
        self.out.push_str("consume[move '");
        self.out.push_str(&name);
        self.out.push_str("'.");
        self.out.push_str(&field);
        self.out.push_str("];\n");
        self.note_part_moved(&name, &field);
    }

    fn function(&mut self) {
        let name = format!("f{}", self.funs.len());
        let count = self.rng.below(3);
        let mut params = Vec::new();
        // One whole type throughout: what it takes, what it works in, and what
        // it answers with, since none of them convert into each other.
        let ty = self.pick_whole();
        self.out.push_str("fn.");
        self.out.push_str(ty.written());
        // Marked where it is named, bare where it is called.
        self.out.push_str(" '");
        self.out.push_str(&name);
        self.out.push_str("' [");
        self.scopes.push(Vec::new());
        for i in 0..count {
            if i > 0 {
                self.out.push_str(", ");
            }
            let param = self.fresh();
            self.out.push_str(ty.written());
            self.out.push_str(" '");
            self.out.push_str(&param);
            self.out.push('\'');
            params.push(ty);
            self.declare(Var { name: param, ty, mutable: false, many: None, moved: false, lent: false, group: None, sum: None, parts_moved: Vec::new(), inner: None, grows: false });
        }
        self.out.push_str("] {\n");
        self.indent = 1;
        let statements = self.rng.below(3) + 1;
        self.body(statements);
        self.pad();
        self.out.push_str("give [");
        self.expr(ty, 2);
        self.out.push_str("];\n}\n\n");
        self.scopes.pop();
        self.indent = 0;
        self.funs.push(Fun { name, params, answers: ty });
    }

    fn body(&mut self, statements: u32) {
        for _ in 0..statements {
            self.statement();
        }
    }

    // Anything a scope still owns is handed away or simply left to end, which
    // is the drop path either way.
    fn finish_scope(&mut self) {
        let owned: Vec<String> = self
            .scopes
            .last()
            .unwrap()
            .iter()
            .filter(|v| v.ty == Ty::Str && v.many.is_none() && v.group.is_none() && !v.moved && !v.lent)
            .map(|v| v.name.clone())
            .collect();
        for name in owned {
            if self.rng.chance(40) {
                let guarded = self.rng.chance(50);
                if guarded {
                    self.pad();
                    self.out.push_str("if ");
                    self.condition();
                    self.out.push_str(" {\n");
                    self.indent += 1;
                }
                self.pad();
                self.out.push_str("consume[move '");
                self.out.push_str(&name);
                self.out.push_str("'];\n");
                if guarded {
                    self.indent -= 1;
                    self.pad();
                    self.out.push_str("}\n");
                }
                if let Some(var) =
                    self.scopes.last_mut().unwrap().iter_mut().find(|v| v.name == name)
                {
                    var.moved = true;
                }
            }
        }
    }

    fn statement(&mut self) {
        // A `one-of` only where the program declared one, and then often: the
        // point of writing one is asking which case it is in, and a value
        // nobody asks about says nothing about the type.
        if !self.sums.is_empty() && self.rng.chance(14) {
            if self.rng.chance(45) {
                self.sum_declaration();
            } else {
                self.sum_when();
            }
            return;
        }
        match self.rng.below(25) {
            0..=3 => self.declaration(),
            4 => self.assignment(),
            5..=6 => self.print(),
            7 => self.branch(),
            8 => {
                if self.rng.chance(30) {
                    self.while_loop()
                } else {
                    self.counted_loop()
                }
            }
            9 => self.lending(),
            10 => self.lending_number(),
            11..=12 => self.array_declaration(),
            13 => self.array_set(),
            14 => self.array_read(),
            15..=16 => self.group_declaration(),
            17 => self.group_read(),
            18 => self.group_set(),
            19 => self.group_part_moved(),
            20 => self.describe_call(),
            21 => self.parts_call(),
            22 => self.same_call(),
            23 => self.group_holds(),
            24 => self.array_grow(),
            _ => self.print(),
        }
    }

    /// Any name at all, whatever it holds — a number, text, several of
    /// something, or a group. `describe` takes every one of them, which is the
    /// point of asking it.
    fn anything_lendable(&mut self) -> Option<String> {
        let mut seen: Vec<String> = Vec::new();
        for scope in &self.scopes {
            for var in scope {
                if !var.moved && !var.lent && var.parts_moved.is_empty()
                   && var.sum.is_none() {
                    seen.push(var.name.clone());
                }
            }
        }
        if seen.is_empty() {
            return None;
        }
        let at = self.rng.below(seen.len() as u32) as usize;
        Some(seen[at].clone())
    }

    /// `print.stdout[(describe[loan 'v3']) \n];` — one generic written out once
    /// per type anything is handed to it at, and a `whichever` inside choosing
    /// a different arm in every copy.
    fn describe_call(&mut self) {
        let name = match self.anything_lendable() {
            Some(name) => name,
            None => return self.print(),
        };
        self.pad();
        self.out.push_str("print.stdout[(describe[loan '");
        self.out.push_str(&name);
        self.out.push_str("']) \\n];\n");
    }

    /// `print.stdout[(parts-of[loan 'v3']) \n];` — a struct walked by a
    /// function that has never seen it, one copy of the body per field.
    fn parts_call(&mut self) {
        let name = match self.pick_group(false, true) {
            Some((name, _)) => name,
            None => return self.print(),
        };
        self.pad();
        self.out.push_str("print.stdout[(parts-of[loan '");
        self.out.push_str(&name);
        self.out.push_str("']) \\n];\n");
    }

    /// `var.int64 'v7' = [same['v3']];` — the floor of what a blank can do,
    /// and the one shape where what comes back is the blank as well.
    ///
    /// Sometimes text, which is the same shape with a transfer in it: handing a
    /// `str` to a blank moves it in and moves it out again, so what the name
    /// held is somewhere else afterwards and the copy that does it was written
    /// for a type nobody named.
    fn same_call(&mut self) {
        if self.rng.chance(30) {
            // Only one this scope declared, the same rule the end of a scope
            // follows. A name from further out, moved from inside a loop, is
            // moved once per turn — which is refused where the move is written,
            // and rightly.
            let here: Vec<String> = self
                .scopes
                .last()
                .map(|scope| {
                    scope
                        .iter()
                        .filter(|v| {
                            v.ty == Ty::Str
                                && v.many.is_none()
                                && v.group.is_none()
                                && !v.moved
                                && !v.lent
                        })
                        .map(|v| v.name.clone())
                        .collect()
                })
                .unwrap_or_default();
            let picked = if here.is_empty() {
                None
            } else {
                let at = self.rng.below(here.len() as u32) as usize;
                Some(here[at].clone())
            };
            if let Some(name) = picked {
                let fresh = self.fresh();
                self.pad();
                self.out.push_str("var.str '");
                self.out.push_str(&fresh);
                self.out.push_str("' = [same[move '");
                self.out.push_str(&name);
                self.out.push_str("']];\n");
                for scope in &mut self.scopes {
                    for var in scope {
                        if var.name == name {
                            var.moved = true;
                        }
                    }
                }
                self.declare(Var {
                    name: fresh,
                    ty: Ty::Str,
                    mutable: false,
                    many: None,
                    moved: false,
                    lent: false,
                    group: None, sum: None,
                    parts_moved: Vec::new(),
                    inner: None,
                    grows: false,
                });
                return;
            }
        }
        let ty = match self.whole_in_scope() {
            Some(ty) => ty,
            None => return self.print(),
        };
        let name = match self.pick_name(ty) {
            Some(name) => name,
            None => return self.print(),
        };
        let fresh = self.fresh();
        self.pad();
        self.out.push_str("var.");
        self.out.push_str(ty.written());
        self.out.push_str(" '");
        self.out.push_str(&fresh);
        self.out.push_str("' = [same['");
        self.out.push_str(&name);
        self.out.push_str("']];\n");
        self.declare(Var {
            name: fresh,
            ty,
            mutable: false,
            many: None,
            moved: false,
            lent: false,
            group: None, sum: None,
            parts_moved: Vec::new(),
            inner: None,
            grows: false,
        });
    }

    /// A name holding text that nobody is using for anything else.
    fn lendable(&mut self, writable: bool) -> Option<String> {
        let mut seen: Vec<String> = Vec::new();
        for scope in &self.scopes {
            for var in scope {
                if var.ty == Ty::Str && var.many.is_none() && var.group.is_none() && var.sum.is_none() && !var.moved && !var.lent
                    && (!writable || var.mutable) {
                    seen.push(var.name.clone());
                }
            }
        }
        if seen.is_empty() {
            return None;
        }
        let at = self.rng.below(seen.len() as u32) as usize;
        Some(seen[at].clone())
    }

    fn markLent(&mut self, name: &str, lent: bool) {
        for scope in &mut self.scopes {
            for var in scope {
                if var.name == name {
                    var.lent = lent;
                }
            }
        }
    }

    /// Lending, in the two shapes the language has: for the length of one call,
    /// and held in a name across several statements.
    ///
    /// The loan is closed before anything else happens to what it borrows from,
    /// because a program the region pass rightly refuses is this file's mistake
    /// and not a finding.
    /// Lending a number and reading it through the loan.
    ///
    /// Every lending this wrote used to be text, so no generated program ever
    /// printed a borrowed number — and printing one was wrong in two engines
    /// for as long as printing has existed. Three engines cannot disagree about
    /// a program nobody writes.
    fn lending_number(&mut self) {
        let mut seen: Vec<(String, Ty)> = Vec::new();
        let mut mutable: Vec<bool> = Vec::new();
        for scope in &self.scopes {
            for var in scope {
                // Anything that copies, not only numbers. A `bool` behind a
                // loan was never generated either, and `not` through one asked
                // LLVM to invert a pointer — the compiler fell over rather than
                // the program.
                if (Self::numeric(var.ty) || var.ty == Ty::Bool)
                    && var.many.is_none() && var.group.is_none() && var.sum.is_none()
                    && !var.moved && !var.lent {
                    seen.push((var.name.clone(), var.ty));
                    mutable.push(var.mutable);
                }
            }
        }
        if seen.is_empty() {
            self.print();
            return;
        }
        let at = self.rng.below(seen.len() as u32) as usize;
        let (borrowed, ty) = seen[at].clone();

        // Written through as well as read, when the name allows it. Reading one
        // was wrong in two engines; writing through one was wrong in two
        // engines and differently, and neither had ever been generated.
        let writable = mutable[at] && self.rng.chance(50);
        if writable {
            self.writing_through(&borrowed, ty);
            return;
        }

        let holder = self.fresh();
        self.markLent(&borrowed, true);
        self.pad();
        self.out.push_str("var.loan.");
        self.out.push_str(ty.written());
        self.out.push_str(" '");
        self.out.push_str(&holder);
        self.out.push_str("' = [loan '");
        self.out.push_str(&borrowed);
        self.out.push_str("'];\n");

        let looks = self.rng.below(3) + 1;
        for _ in 0..looks {
            self.pad();
            self.out.push_str("print.stdout['");
            self.out.push_str(&holder);
            self.out.push_str("' \\n];\n");
        }
        // Nothing looks at the holder again, so the loan is done here.
        self.markLent(&borrowed, false);
    }

    /// `var.loanmut.int64 'h' = [loanmut 'v']; set 'h' = ['h' + *3*];`
    ///
    /// Writing a number through a loan. Native stored the value over the loan
    /// itself and then followed it as a pointer; the fast engine answered with
    /// the number the name started as. Both for the same reason: every loan
    /// anybody had ever generated was a loan of text.
    fn writing_through(&mut self, borrowed: &str, ty: Ty) {
        let holder = self.fresh();
        self.markLent(borrowed, true);
        self.pad();
        self.out.push_str("var.loanmut.");
        self.out.push_str(ty.written());
        self.out.push_str(" '");
        self.out.push_str(&holder);
        self.out.push_str("' = [loanmut '");
        self.out.push_str(borrowed);
        self.out.push_str("'];\n");

        let times = self.rng.below(3) + 1;
        for _ in 0..times {
            self.pad();
            self.out.push_str("set '");
            self.out.push_str(&holder);
            self.out.push_str("' = [");
            if ty == Ty::Bool {
                self.out.push_str("not '");
                self.out.push_str(&holder);
                self.out.push('\'');
            } else {
                self.out.push('\'');
                self.out.push_str(&holder);
                self.out.push_str("' + ");
                self.literal(ty);
            }
            self.out.push_str("];\n");
            self.pad();
            self.out.push_str("print.stdout['");
            self.out.push_str(&holder);
            self.out.push_str("' \\n];\n");
        }
        self.markLent(borrowed, false);
        // What it holds now is whatever was written through the loan.
        self.pad();
        self.out.push_str("print.stdout['");
        self.out.push_str(borrowed);
        self.out.push_str("' \\n];\n");
    }

    fn lending(&mut self) {
        let writable = self.rng.chance(40);
        let borrowed = match self.lendable(writable) {
            Some(name) => name,
            None => {
                self.print();
                return;
            }
        };

        if self.rng.chance(45) {
            // For the length of one call, and done with by the semicolon.
            self.pad();
            if writable {
                self.out.push_str("edit[loanmut '");
            } else {
                self.out.push_str("look[loan '");
            }
            self.out.push_str(&borrowed);
            self.out.push_str("'];\n");
            return;
        }

        // Or held in a name, and looked at a few times before it is done.
        let holder = self.fresh();
        self.markLent(&borrowed, true);
        self.pad();
        self.out.push_str(if writable { "var.loanmut.str '" } else { "var.loan.str '" });
        self.out.push_str(&holder);
        self.out.push_str("' = [");
        self.out.push_str(if writable { "loanmut '" } else { "loan '" });
        self.out.push_str(&borrowed);
        self.out.push_str("'];\n");

        let looks = self.rng.below(3) + 1;
        for _ in 0..looks {
            self.pad();
            if writable && self.rng.chance(50) {
                // Written through the loan, which is what being lent for
                // writing is for.
                self.out.push_str("set '");
                self.out.push_str(&holder);
                self.out.push_str("' = ['");
                self.out.push_str(&holder);
                self.out.push_str("' *!*];\n");
            } else if self.rng.chance(50) {
                self.out.push_str("print.stdout['");
                self.out.push_str(&holder);
                self.out.push_str("' \\n];\n");
            } else {
                self.out.push_str("print.stdout[(count['");
                self.out.push_str(&holder);
                self.out.push_str("']) \\n];\n");
            }
        }
        // Nothing looks at the holder again, so the loan is done here.
        self.markLent(&borrowed, false);
    }

    /// A `many` that is still whole: nothing moved out of it, nothing lent.
    fn arrays(&mut self, want_mutable: bool) -> Vec<(String, Ty, u32, Option<u32>)> {
        let mut seen = Vec::new();
        for scope in &self.scopes {
            for var in scope {
                if let Some(length) = var.many {
                    if length > 0 && !var.moved && !var.lent
                        && (!want_mutable || var.mutable) {
                        seen.push((var.name.clone(), var.ty, length, var.inner));
                    }
                }
            }
        }
        seen
    }

    fn pick_array(&mut self, want_mutable: bool) -> Option<(String, Ty, u32, Option<u32>)> {
        let seen = self.arrays(want_mutable);
        if seen.is_empty() {
            return None;
        }
        let at = self.rng.below(seen.len() as u32) as usize;
        Some(seen[at].clone())
    }

    /// `var.many.T 'v' = [… … …];`, or the same length asked for at once with
    /// `fill`, which only a value that copies can answer.
    fn array_declaration(&mut self) {
        let ty = match self.rng.below(10) {
            0..=5 => self.pick_whole(),
            6..=7 => Ty::Bool,
            _ => Ty::Str,
        };
        // One that grows is always writable, because growing is changing and a
        // name that does not change cannot be grown.
        let grows = self.rng.chance(22);
        let mutable = grows || self.rng.chance(60);
        let length = self.rng.below(4) + 1;
        // A `many` of a `many`, sometimes. Every row the same length, so that
        // reaching into one is safe wherever it is reached from. Not both at
        // once, so that what each of the two is doing stays readable in a case
        // kept from a failing run.
        let inner = if !grows && self.rng.chance(25) {
            Some(self.rng.below(3) + 1)
        } else {
            None
        };
        let name = self.fresh();
        self.pad();
        self.out.push_str("var.");
        if mutable {
            self.out.push_str("mut.");
        }
        self.out.push_str(if grows { "many-growing." } else { "many." });
        if inner.is_some() {
            self.out.push_str("many.");
        }
        self.out.push_str(ty.written());
        self.out.push_str(" '");
        self.out.push_str(&name);
        self.out.push_str("' = [");
        if let Some(across) = inner {
            // Brackets where an item goes make a `many`, so a `many` of them is
            // brackets inside brackets.
            for i in 0..length {
                if i > 0 {
                    self.out.push(' ');
                }
                self.out.push('[');
                for j in 0..across {
                    if j > 0 {
                        self.out.push(' ');
                    }
                    self.expr(ty, if ty == Ty::Str { 0 } else { 1 });
                }
                self.out.push(']');
            }
        } else if !grows && ty != Ty::Str && self.rng.chance(30) {
            self.out.push_str("fill[");
            self.expr(ty, 1);
            self.out.push_str(", *");
            let mut digits = length.to_string();
            self.out.push_str(&digits);
            digits.clear();
            self.out.push_str("*]");
        } else {
            for i in 0..length {
                if i > 0 {
                    self.out.push(' ');
                }
                // Every item here is a place of its own, so text is written
                // whole rather than as pieces that would join anywhere else —
                // and a name would be handed over, which takes a word.
                self.expr(ty, if ty == Ty::Str { 0 } else { 1 });
            }
        }
        self.out.push_str("];\n");
        self.declare(Var { name: name.clone(), ty, mutable, many: Some(length), moved: false, lent: false, group: None, sum: None, parts_moved: Vec::new(), inner, grows });
        // The same again: a `many` asked to be writable, and then written.
        if mutable && length > 0 && self.rng.chance(80) {
            self.set_a_place(&name, ty, length, inner);
        }
        // And one asked to grow is grown, so that the room it took to begin
        // with runs out and every place moves.
        if grows {
            for _ in 0..self.rng.below(3) + 1 {
                self.grow_it(&name, ty);
            }
        }
    }

    /// `set 'v'[*i*] = […];` — one place, and the index is written in range so
    /// that the program runs rather than stopping.
    fn array_set(&mut self) {
        let Some((name, ty, length, inner)) = self.pick_array(true) else {
            self.print();
            return;
        };
        self.set_a_place(&name, ty, length, inner);
    }

    /// `if 'v3'.v4 holds 'v9' { … }` — the only way to reach what a field that
    /// may hold nothing is holding. Showing one straight out would not say
    /// which of the two it is, and is refused where it is written.
    fn group_holds(&mut self) {
        let mut seen: Vec<(String, String, Ty)> = Vec::new();
        for scope in &self.scopes {
            for var in scope {
                let Some(which) = var.group else { continue };
                if var.moved || var.lent || !var.parts_moved.is_empty() {
                    continue;
                }
                for (field, held) in &self.shapes[which].fields {
                    if let Held::Maybe(ty) = held {
                        seen.push((var.name.clone(), field.clone(), *ty));
                    }
                }
            }
        }
        if seen.is_empty() {
            self.print();
            return;
        }
        let at = self.rng.below(seen.len() as u32) as usize;
        let (name, field, ty) = seen[at].clone();
        let got = self.fresh();
        self.pad();
        self.out.push_str("if '");
        self.out.push_str(&name);
        self.out.push_str("'.");
        self.out.push_str(&field);
        self.out.push_str(" holds '");
        self.out.push_str(&got);
        self.out.push_str("' {\n");
        self.indent += 1;
        self.pad();
        self.out.push_str("print.stdout['");
        self.out.push_str(&got);
        self.out.push_str("' \\n];\n");
        // What is lent inside an arm is lent for that arm only, so nothing is
        // declared here that the rest of the scope would have to know about.
        let _ = ty;
        self.indent -= 1;
        self.pad();
        self.out.push_str("}\n");
    }

    /// `add 'v3' = […];` — one more place at the end. Nothing counts it toward
    /// the length this file remembers: a growth written inside an arm that does
    /// not run never happens, and the length it was made with is the one an
    /// index can always rely on.
    fn grow_it(&mut self, name: &str, ty: Ty) {
        let name = name.to_string();
        self.pad();
        self.out.push_str("add '");
        self.out.push_str(&name);
        self.out.push_str("' = [");
        self.expr(ty, if ty == Ty::Str { 0 } else { 1 });
        self.out.push_str("];\n");
    }

    /// One that may be grown right now: writable, still here, and not lent —
    /// what is lent stays where it is, and growing may move every place.
    fn growable(&mut self) -> Option<(String, Ty)> {
        let mut seen: Vec<(String, Ty)> = Vec::new();
        for scope in &self.scopes {
            for var in scope {
                if var.grows && var.mutable && !var.moved && !var.lent {
                    seen.push((var.name.clone(), var.ty));
                }
            }
        }
        if seen.is_empty() {
            return None;
        }
        let at = self.rng.below(seen.len() as u32) as usize;
        Some(seen[at].clone())
    }

    fn array_grow(&mut self) {
        match self.growable() {
            Some((name, ty)) => self.grow_it(&name, ty),
            None => self.print(),
        }
    }

    /// `set 'v3'[*2*] = […]` — one place of a `many`, written.
    fn set_a_place(&mut self, name: &str, ty: Ty, length: u32, inner: Option<u32>) {
        let name = name.to_string();
        // Places are counted from one, so the first is 1 and the last is the
        // length. `below` counts from zero, which is a place no `many` has.
        let at = self.rng.below(length) + 1;
        self.pad();
        self.out.push_str("set '");
        self.out.push_str(&name);
        self.out.push_str("'[*");
        self.out.push_str(&at.to_string());
        self.out.push_str("*] = [");
        // One place of a `many` of a `many` holds a whole `many`, so what goes
        // in is several values rather than one — and as many of them as every
        // other row has, so that reaching into this one stays safe.
        match inner {
            Some(across) => {
                for j in 0..across {
                    if j > 0 {
                        self.out.push(' ');
                    }
                    self.expr(ty, if ty == Ty::Str { 0 } else { 1 });
                }
            }
            None => self.expr(ty, 2),
        }
        self.out.push_str("];\n");
    }

    /// Reading one place, and asking how many there are.
    fn array_read(&mut self) {
        let Some((name, _ty, length, inner)) = self.pick_array(false) else {
            self.print();
            return;
        };
        let at = self.rng.below(length) + 1;
        self.pad();
        self.out.push_str("print.stdout['");
        self.out.push_str(&name);
        self.out.push_str("'[*");
        self.out.push_str(&at.to_string());
        self.out.push_str("*]");
        // A place of a `many` of a `many` is itself several, and showing one is
        // refused — so this reaches the rest of the way in.
        if let Some(across) = inner {
            self.out.push_str("[*");
            self.out.push_str(&(self.rng.below(across) + 1).to_string());
            self.out.push_str("*]");
        }
        self.out.push_str(" str:* of * (count[loan '");
        self.out.push_str(&name);
        self.out.push_str("']) \\n];\n");
    }

    fn declaration(&mut self) {
        let ty = match self.rng.below(10) {
            0..=5 => self.pick_whole(),
            6..=7 => Ty::Bool,
            _ => Ty::Str,
        };
        let mutable = self.rng.chance(50);
        let name = self.fresh();
        self.pad();
        self.out.push_str("var.");
        if mutable {
            self.out.push_str("mut.");
        }
        // Everything written here means to wrap: the expected answers are
        // worked out by the engines themselves, which wrap. Saying so keeps the
        // compiler from remarking on it — and a remark would otherwise land on
        // stderr, which is part of what the engines are compared on, while the
        // built program's own output has none of it.
        if Self::numeric(ty) && !matches!(ty, Ty::Real(_) | Ty::Deci(_)) {
            self.out.push_str("wrapping.");
        }
        self.out.push_str(ty.written());
        self.out.push_str(" '");
        self.out.push_str(&name);
        self.out.push_str("' = [");
        self.expr(ty, 2);
        self.out.push_str("];\n");
        self.declare(Var { name: name.clone(), ty, mutable, many: None, moved: false, lent: false, group: None, sum: None, parts_moved: Vec::new(), inner: None, grows: false });

        // `mut` asks for something, and asking without doing it is the whole of
        // what `W0003` is for. Most of the time it is done here, where the name
        // is still the newest thing in scope — which also writes the path where
        // text is set over the top of text and the old value has to go.
        //
        // Not always: a `mut` nothing changes is a real warning worth writing
        // sometimes. Always was drowning every other diagnostic in a dumped
        // case, at a hundred and eighty warnings to twelve programs.
        if mutable && self.rng.chance(80) {
            self.pad();
            self.out.push_str("set '");
            self.out.push_str(&name);
            self.out.push_str("' = [");
            self.expr(ty, 2);
            self.out.push_str("];\n");
        }
    }

    fn assignment(&mut self) {
        // Whatever is set, it is set to something of its own type.
        let mut choices: Vec<(String, Ty)> = Vec::new();
        for scope in &self.scopes {
            for var in scope {
                if Self::numeric(var.ty) && var.many.is_none() && var.group.is_none() && var.sum.is_none() && var.mutable
                    && !var.moved && !var.lent {
                    choices.push((var.name.clone(), var.ty));
                }
            }
        }
        if choices.is_empty() {
            self.print();
            return;
        }
        let at = self.rng.below(choices.len() as u32) as usize;
        let (name, ty) = choices[at].clone();
        self.pad();
        self.out.push_str("set '");
        self.out.push_str(&name);
        self.out.push_str("' = [");
        self.expr(ty, 2);
        self.out.push_str("];\n");
    }

    /// A `many` or a struct with nothing absent anywhere inside it. Showing one
    /// writes every value it holds, which is the only way the oracle sees
    /// inside what these programs build — an element at a time only ever asked
    /// about the element it named.
    fn showable_whole(&mut self) -> Vec<String> {
        let mut seen: Vec<String> =
            self.arrays(false).into_iter().map(|(name, _, _, _)| name).collect();
        for scope in &self.scopes {
            for var in scope {
                let Some(which) = var.group else { continue };
                if var.moved || var.lent || !var.parts_moved.is_empty() {
                    continue;
                }
                if Self::shows_whole(&self.shapes, which, 0) {
                    seen.push(var.name.clone());
                }
            }
        }
        seen
    }

    /// Whether every field of this struct, and of every struct inside it, is
    /// there to be written. An absence is not a value, so showing one is
    /// refused wherever it sits.
    fn shows_whole(shapes: &[Shape], which: usize, depth: u32) -> bool {
        if depth > 8 || which >= shapes.len() {
            return false;
        }
        shapes[which].fields.iter().all(|(_, held)| match held {
            Held::Maybe(_) => false,
            Held::Group(inner) => Self::shows_whole(shapes, *inner, depth + 1),
            _ => true,
        })
    }

    fn print(&mut self) {
        self.pad();
        // Which of the two streams, chosen per statement. The same text on the
        // wrong one is a different answer, and it is an answer only running the
        // program finds — comparing what a program wrote without asking where
        // it wrote it, three engines writing everything to standard output
        // would agree perfectly.
        self.out.push_str(if self.rng.chance(20) {
            "print.stderr["
        } else {
            "print.stdout["
        });
        // The whole of something that holds several values, sometimes: every
        // value it holds, one after another. Both ways of writing it, because
        // `convert-to-str` promises the same characters and the promise is
        // worth asking about.
        if self.rng.chance(30) {
            let seen = self.showable_whole();
            if !seen.is_empty() {
                let at = self.rng.below(seen.len() as u32) as usize;
                let name = seen[at].clone();
                if self.rng.chance(35) {
                    self.out.push_str("(convert-to-str[loan '");
                    self.out.push_str(&name);
                    self.out.push_str("']) ");
                } else {
                    self.out.push('\'');
                    self.out.push_str(&name);
                    self.out.push_str("' ");
                }
                self.out.push_str("\\n];\n");
                return;
            }
        }
        match self.rng.below(3) {
            0 => {
                self.out.push_str("str:*");
                self.word();
                self.out.push_str("* ");
            }
            1 => {
                // A print says nothing about what it is given, so what it is
                // given has to say for itself.
                let ty = self.whole_in_scope().unwrap_or(COUNTED);
                self.out.push('(');
                self.whole_typed(ty, 2);
                self.out.push_str(") ");
            }
            _ => {
                if let Some(name) = self.pick_name(Ty::Str) {
                    self.out.push('\'');
                    self.out.push_str(&name);
                    self.out.push_str("' ");
                } else {
                    let ty = self.whole_in_scope().unwrap_or(COUNTED);
                    self.out.push('(');
                    self.whole_typed(ty, 2);
                    self.out.push_str(") ");
                }
            }
        }
        self.out.push_str("\\n];\n");
    }

    fn branch(&mut self) {
        self.pad();
        self.out.push_str("if ");
        self.condition();
        self.out.push_str(" {\n");
        self.indent += 1;
        self.scopes.push(Vec::new());
        let statements = self.rng.below(2) + 1;
        self.body(statements);
        self.finish_scope();
        self.scopes.pop();
        self.indent -= 1;
        self.pad();
        if self.rng.chance(50) {
            self.out.push_str("} else {\n");
            self.indent += 1;
            self.scopes.push(Vec::new());
            let statements = self.rng.below(2) + 1;
            self.body(statements);
            self.finish_scope();
            self.scopes.pop();
            self.indent -= 1;
            self.pad();
        }
        self.out.push_str("}\n");
    }

    /// A `loop.while`, which nothing wrote until 2026-09-08 — so the oracle had
    /// never compared a single one on any engine, and the compiler running
    /// programs while compiling them had no test at all for the one loop shape
    /// that may never finish.
    ///
    /// Its counter is written and stepped by this, not by the loop, so it is
    /// bounded by construction: a generated program that never ends is not a
    /// disagreement, it is a case nobody can compare.
    fn while_loop(&mut self) {
        let counter = self.fresh();
        let ty = self.pick_whole();
        let rounds = self.rng.between(1, 4);
        self.pad();
        self.out.push_str("var.mut.");
        self.out.push_str(ty.written());
        self.out.push_str(" '");
        self.out.push_str(&counter);
        self.out.push_str("' = [*0*];\n");
        self.declare(Var {
            name: counter.clone(),
            ty,
            mutable: true,
            many: None,
            moved: false,
            lent: false,
            group: None, sum: None,
            parts_moved: Vec::new(),
            inner: None,
            grows: false,
        });

        self.stepped.push(counter.clone());
        self.pad();
        self.out.push_str("loop.while '");
        self.out.push_str(&counter);
        // The end says its own type. A comparison declares nothing, so neither
        // side is told what it is and a written value in one has to say.
        self.out.push_str("' < ");
        self.out.push_str(ty.written());
        self.out.push_str(":*");
        push_number(self.out, rounds);
        self.out.push_str("* {\n");
        self.indent += 1;
        self.scopes.push(Vec::new());
        let statements = self.rng.below(2) + 1;
        self.body(statements);
        // Stepped last, so the body sees every value from zero up and the loop
        // still stops. Written here rather than left to chance: a body that
        // happened not to step it would be a program nothing could compare.
        self.pad();
        self.out.push_str("set '");
        self.out.push_str(&counter);
        self.out.push_str("' = ['");
        self.out.push_str(&counter);
        self.out.push_str("' + *1*];\n");
        self.finish_scope();
        self.scopes.pop();
        self.stepped.retain(|name| name != &counter);
        self.indent -= 1;
        self.pad();
        self.out.push_str("}\n");
    }

    fn counted_loop(&mut self) {
        let counter = self.fresh();
        let ty = self.pick_whole();
        // `perm` keeps the counter, so the name belongs where the loop is.
        let keeps = self.rng.chance(25);
        let first = self.rng.between(1, 3);
        let last = first + self.rng.between(0, 3);
        self.pad();
        self.out.push_str(if keeps { "loop.perm.range." } else { "loop.range." });
        self.out.push_str(ty.written());
        self.out.push_str(" '");
        self.out.push_str(&counter);
        self.out.push_str("' = [*");
        push_number(self.out, first);
        self.out.push_str("*, *");
        push_number(self.out, last);
        self.out.push_str("*] {\n");
        self.indent += 1;
        // Stepped by the loop, the same as a `while`'s own counter: a borrow of
        // one held across a turn points at what the next turn changes.
        self.stepped.push(counter.clone());
        let held = Var { name: counter.clone(), ty, mutable: false, many: None, moved: false, lent: false, group: None, sum: None, parts_moved: Vec::new(), inner: None, grows: false };
        if keeps {
            self.declare(held.clone());
            self.scopes.push(Vec::new());
        } else {
            self.scopes.push(vec![held]);
        }
        let statements = self.rng.below(2) + 1;
        self.body(statements);
        self.finish_scope();
        self.scopes.pop();
        self.stepped.retain(|name| name != &counter);
        self.indent -= 1;
        self.pad();
        self.out.push_str("}\n");
    }

    // ---- expressions

    fn condition(&mut self) {
        if self.rng.chance(20) {
            if let Some(name) = self.pick_name(Ty::Bool) {
                self.out.push('\'');
                self.out.push_str(&name);
                self.out.push('\'');
                return;
            }
        }
        // A comparison takes no type from anywhere, so *both* sides have to say
        // what they are. The right one used to take its type from the left; it
        // does not any more, because the left is the value standing beside it
        // rather than a slot it goes into. And both sides are bracketed, because
        // `mod` beside a comparison has no agreed order and Xag refuses to
        // invent one.
        let ty = self.whole_in_scope().unwrap_or(COUNTED);
        self.out.push('(');
        self.whole_typed(ty, 1);
        self.out.push(')');
        self.out.push(' ');
        let compares = match self.rng.below(6) {
            0 => "<",
            1 => ">",
            2 => "<==",
            3 => ">==",
            4 => "==",
            _ => "!==",
        };
        self.out.push_str(compares);
        self.out.push_str(" (");
        self.whole_typed(ty, 1);
        self.out.push(')');
    }

    /// Something that says what it is without being told: a name, a call, a
    /// count, or a literal with arithmetic done to it.
    fn whole_typed(&mut self, ty: Ty, depth: u32) {
        let number = self.pick_name(ty);
        let text = if ty == COUNTED { self.pick_name(Ty::Str) } else { None };
        let callable = self.funs.iter().any(|f| f.answers == ty && f.name != "consume");
        let _ = &text;
        match self.rng.below(4) {
            0 if number.is_some() => {
                let name = number.unwrap();
                self.out.push('\'');
                self.out.push_str(&name);
                self.out.push('\'');
            }
            1 if text.is_some() => {
                let name = text.unwrap();
                self.out.push_str("count[loan '");
                self.out.push_str(&name);
                self.out.push_str("']");
            }
            2 if callable => self.call(ty, depth),
            _ => {
                // A written value says its own type the way it always could:
                // `str:*hello*` and `int32:*161*` are one notation, not two.
                self.out.push_str(ty.written());
                self.out.push(':');
                self.literal(ty);
            }
        }
    }

    fn expr(&mut self, ty: Ty, depth: u32) {
        match ty {
            Ty::Bool => {
                if depth == 0 || self.rng.chance(40) {
                    match self.pick_name(Ty::Bool) {
                        Some(name) => {
                            self.out.push('\'');
                            self.out.push_str(&name);
                            self.out.push('\'');
                        }
                        None => self.literal(Ty::Bool),
                    }
                } else {
                    self.condition();
                }
            }
            Ty::Str => {
                // Pieces side by side join, and every piece has to be text.
                if depth == 0 || self.rng.chance(50) {
                    self.literal(Ty::Str);
                } else {
                    self.literal(Ty::Str);
                    if let Some(name) = self.pick_name(Ty::Str) {
                        self.out.push_str(" '");
                        self.out.push_str(&name);
                        self.out.push('\'');
                    }
                    self.out.push(' ');
                    self.literal(Ty::Str);
                }
            }
            Ty::Whole(_) | Ty::Real(_) | Ty::Deci(_) => self.number(ty, depth),
        }
    }

    fn number(&mut self, ty: Ty, depth: u32) {
        if depth == 0 {
            match self.pick_name(ty) {
                Some(name) => {
                    self.out.push('\'');
                    self.out.push_str(&name);
                    self.out.push('\'');
                }
                None => self.literal(ty),
            }
            return;
        }
        match self.rng.below(10) {
            0..=2 => self.expr(ty, 0),
            3 => self.literal(ty),
            4..=6 => {
                self.expr(ty, depth - 1);
                self.out.push(' ');
                let operator = match self.rng.below(4) {
                    0 => "+",
                    1 => "-",
                    2 => "x",
                    _ => "^",
                };
                self.out.push_str(operator);
                self.out.push(' ');
                // A power with a large exponent is a long loop, not a bug.
                let exponent = self.rng.between(0, 4);
                self.out.push('*');
                push_number(self.out, exponent);
                self.out.push('*');
                let _ = ty;
            }
            7 => {
                // A divisor is written, and never zero: dividing by zero stops
                // the program, and a stop is not a disagreement. The whole
                // thing is bracketed because nothing settled where `mod` binds.
                // Both sides, not just the whole: `('a' + *4* mod *2*)` still
                // has a `+` and a `mod` side by side inside the brackets.
                self.out.push_str("((");
                self.expr(ty, depth - 1);
                self.out.push_str(") ");
                let operator = if self.rng.chance(50) { "/" } else { "mod" };
                self.out.push_str(operator);
                self.out.push_str(" *");
                let divisor = self.rng.between(1, 9);
                push_number(self.out, divisor);
                self.out.push_str("*)");
            }
            8 => {
                // `count` answers with one type and no other, so it only fits
                // where that type was asked for.
                let text = if ty == COUNTED { self.pick_name(Ty::Str) } else { None };
                match text {
                    Some(name) => {
                        self.out.push_str("count[loan '");
                        self.out.push_str(&name);
                        self.out.push_str("']");
                    }
                    None => self.literal(ty),
                }
            }
            _ => self.call(ty, depth),
        }
    }

    fn call(&mut self, ty: Ty, depth: u32) {
        let callable: Vec<usize> = (0..self.funs.len())
            .filter(|&i| self.funs[i].answers == ty && self.funs[i].name != "consume")
            .collect();
        if callable.is_empty() {
            self.literal(ty);
            return;
        }
        let at = callable[self.rng.below(callable.len() as u32) as usize];
        let name = self.funs[at].name.clone();
        let params = self.funs[at].params.clone();
        let arity = params.len();
        self.out.push_str(&name);
        self.out.push('[');
        for i in 0..arity {
            if i > 0 {
                self.out.push_str(", ");
            }
            self.expr(params[i], depth.saturating_sub(1));
        }
        self.out.push(']');
    }

    fn literal(&mut self, ty: Ty) {
        match ty {
            Ty::Whole(which) => {
                let (low, high) = literal_range(which);
                let value = self.rng.between(low, high);
                self.out.push('*');
                push_number(self.out, value);
                self.out.push('*');
            }
            Ty::Deci(which) => {
                // Small enough for a `deci32`'s seven digits, and written with
                // places often enough that keeping them is worth testing.
                let whole = self.rng.between(if which == 0 { -900 } else { -9000 },
                                             if which == 0 { 900 } else { 9000 });
                self.out.push('*');
                push_number(self.out, whole);
                if self.rng.chance(60) {
                    self.out.push('.');
                    let places = self.rng.between(1, 99);
                    push_number(self.out, places);
                }
                self.out.push('*');
            }
            Ty::Real(which) => {
                // Small enough that a `bin16` holds it, and written with a
                // fraction often enough to be worth having.
                let whole = self.rng.between(if which == 0 { -60 } else { -400 },
                                             if which == 0 { 60 } else { 400 });
                let _ = which;
                self.out.push('*');
                push_number(self.out, whole);
                if self.rng.chance(60) {
                    self.out.push('.');
                    let fraction = self.rng.between(1, 999);
                    push_number(self.out, fraction);
                }
                self.out.push('*');
            }
            Ty::Bool => {
                let truth = if self.rng.chance(50) { "*true*" } else { "*false*" };
                self.out.push_str(truth);
            }
            Ty::Str => {
                self.out.push('*');
                self.word();
                self.out.push('*');
            }
        }
    }

    fn word(&mut self) {
        const WORDS: [&str; 12] = [
            "alpha", "beta", "gamma", "hi", "there", "café", "🧑‍🧑‍🧒‍🧒", "x y",
            "one", "two", "🇹🇭", "…",
        ];
        let at = self.rng.below(WORDS.len() as u32) as usize;
        let word = WORDS[at];
        self.out.push_str(word);
    }
}

fn push_number(out: &mut String, mut value: i64) {
    if value < 0 {
        out.push('-');
        // Written negatively rather than negated: `-` is subtraction here.
        value = -value;
    }
    let mut digits = [0u8; 20];
    let mut at = digits.len();
    if value == 0 {
        out.push('0');
        return;
    }
    while value > 0 {
        at -= 1;
        digits[at] = b'0' + (value % 10) as u8;
        value /= 10;
    }
    out.push_str(std::str::from_utf8(&digits[at..]).unwrap());
}
