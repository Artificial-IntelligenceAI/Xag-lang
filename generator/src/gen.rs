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

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum Ty {
    I64,
    Bool,
    Str,
}

impl Ty {
    fn written(self) -> &'static str {
        match self {
            Ty::I64 => "i64",
            Ty::Bool => "bool",
            Ty::Str => "str",
        }
    }
}

#[derive(Clone)]
struct Var {
    name: String,
    ty: Ty,
    mutable: bool,
    moved: bool,
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
    consts: Vec<Var>,
    next_name: u32,
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
        consts: Vec::new(),
        next_name: 0,
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
                if var.ty == ty && !var.moved && (!want_mutable || var.mutable) {
                    seen.push(var);
                }
            }
        }
        if !want_mutable {
            for var in &self.consts {
                if var.ty == ty {
                    seen.push(var);
                }
            }
        }
        seen
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
        self.out.push_str("# written by xag-oracle\n\n");

        // Something to hand a `str` to, so that moves and their drop flags get
        // written as well as read.
        self.out
            .push_str("fn.nothing consume [str 't'] {\n    print.stdout['t' \\n];\n}\n\n");
        self.funs.push(Fun {
            name: "consume".to_string(),
            params: vec![Ty::Str],
            answers: Ty::I64, // never called for its answer
        });

        let constants = self.rng.below(3);
        for _ in 0..constants {
            let name = self.fresh();
            let ty = if self.rng.chance(70) { Ty::I64 } else { Ty::Str };
            self.out.push_str("const.");
            self.out.push_str(ty.written());
            self.out.push_str(" '");
            self.out.push_str(&name);
            self.out.push_str("' = [");
            self.literal(ty);
            self.out.push_str("];\n");
            self.consts.push(Var { name, ty, mutable: false, moved: false });
        }
        if constants > 0 {
            self.out.push('\n');
        }

        let functions = self.rng.below(3) + self.size / 12;
        for _ in 0..functions {
            self.function();
        }

        self.out.push_str("START {\n");
        self.scopes.push(Vec::new());
        self.indent = 1;
        let statements = self.rng.below(self.size / 2 + 1) + self.size / 2 + 1;
        self.body(statements);
        self.finish_scope();
        self.scopes.pop();
        self.out.push_str("}\n");
    }

    fn function(&mut self) {
        let name = format!("f{}", self.funs.len());
        let count = self.rng.below(3);
        let mut params = Vec::new();
        self.out.push_str("fn.i64 ");
        self.out.push_str(&name);
        self.out.push_str(" [");
        self.scopes.push(Vec::new());
        for i in 0..count {
            if i > 0 {
                self.out.push_str(", ");
            }
            let param = self.fresh();
            self.out.push_str("i64 '");
            self.out.push_str(&param);
            self.out.push('\'');
            params.push(Ty::I64);
            self.declare(Var { name: param, ty: Ty::I64, mutable: false, moved: false });
        }
        self.out.push_str("] {\n");
        self.indent = 1;
        let statements = self.rng.below(3) + 1;
        self.body(statements);
        self.pad();
        self.out.push_str("give [");
        self.expr(Ty::I64, 2);
        self.out.push_str("];\n}\n\n");
        self.scopes.pop();
        self.indent = 0;
        self.funs.push(Fun { name, params, answers: Ty::I64 });
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
            .filter(|v| v.ty == Ty::Str && !v.moved)
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
        match self.rng.below(10) {
            0..=3 => self.declaration(),
            4 => self.assignment(),
            5..=6 => self.print(),
            7 => self.branch(),
            8 => self.counted_loop(),
            _ => self.print(),
        }
    }

    fn declaration(&mut self) {
        let ty = match self.rng.below(10) {
            0..=5 => Ty::I64,
            6..=7 => Ty::Bool,
            _ => Ty::Str,
        };
        let mutable = ty != Ty::Str && self.rng.chance(50);
        let name = self.fresh();
        self.pad();
        self.out.push_str("var.");
        if mutable {
            self.out.push_str("mut.");
        }
        self.out.push_str(ty.written());
        self.out.push_str(" '");
        self.out.push_str(&name);
        self.out.push_str("' = [");
        self.expr(ty, 2);
        self.out.push_str("];\n");
        self.declare(Var { name, ty, mutable, moved: false });
    }

    fn assignment(&mut self) {
        let choices: Vec<String> = self
            .visible(Ty::I64, true)
            .iter()
            .map(|v| v.name.clone())
            .collect();
        if choices.is_empty() {
            self.print();
            return;
        }
        let at = self.rng.below(choices.len() as u32) as usize;
        self.pad();
        self.out.push_str("set '");
        self.out.push_str(&choices[at]);
        self.out.push_str("' = [");
        self.expr(Ty::I64, 2);
        self.out.push_str("];\n");
    }

    fn print(&mut self) {
        self.pad();
        self.out.push_str("print.stdout[");
        match self.rng.below(3) {
            0 => {
                self.out.push_str("str:*");
                self.word();
                self.out.push_str("* ");
            }
            1 => {
                // A print says nothing about what it is given, so what it is
                // given has to say for itself.
                self.out.push('(');
                self.i64_typed(2);
                self.out.push_str(") ");
            }
            _ => {
                if let Some(name) = self.pick_name(Ty::Str) {
                    self.out.push('\'');
                    self.out.push_str(&name);
                    self.out.push_str("' ");
                } else {
                    self.out.push('(');
                    self.i64_typed(2);
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

    fn counted_loop(&mut self) {
        let counter = self.fresh();
        let first = self.rng.between(1, 3);
        let last = first + self.rng.between(0, 3);
        self.pad();
        self.out.push_str("loop.range.i64 '");
        self.out.push_str(&counter);
        self.out.push_str("' = [*");
        push_number(self.out, first);
        self.out.push_str("*, *");
        push_number(self.out, last);
        self.out.push_str("*] {\n");
        self.indent += 1;
        self.scopes.push(vec![Var {
            name: counter,
            ty: Ty::I64,
            mutable: false,
            moved: false,
        }]);
        let statements = self.rng.below(2) + 1;
        self.body(statements);
        self.finish_scope();
        self.scopes.pop();
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
        // A comparison takes no type from anywhere, so its left side has to
        // say what it is. And both sides are bracketed, because `mod` beside a
        // comparison has no agreed order and Xag refuses to invent one.
        self.out.push('(');
        self.i64_typed(1);
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
        self.expr(Ty::I64, 1);
        self.out.push(')');
    }

    /// Something that says what it is without being told: a name, a call, a
    /// count, or a literal with arithmetic done to it.
    fn i64_typed(&mut self, depth: u32) {
        let number = self.pick_name(Ty::I64);
        let text = self.pick_name(Ty::Str);
        let callable = self.funs.iter().any(|f| f.name != "consume");
        match self.rng.below(4) {
            0 if number.is_some() => {
                let name = number.unwrap();
                self.out.push('\'');
                self.out.push_str(&name);
                self.out.push('\'');
            }
            1 if text.is_some() => {
                let name = text.unwrap();
                self.out.push_str("count[ref '");
                self.out.push_str(&name);
                self.out.push_str("']");
            }
            2 if callable => self.call(depth),
            _ => {
                self.literal(Ty::I64);
                self.out.push_str(" + *0*");
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
            Ty::I64 => self.number(depth),
        }
    }

    fn number(&mut self, depth: u32) {
        if depth == 0 {
            match self.pick_name(Ty::I64) {
                Some(name) => {
                    self.out.push('\'');
                    self.out.push_str(&name);
                    self.out.push('\'');
                }
                None => self.literal(Ty::I64),
            }
            return;
        }
        match self.rng.below(10) {
            0..=2 => self.expr(Ty::I64, 0),
            3 => self.literal(Ty::I64),
            4..=6 => {
                self.expr(Ty::I64, depth - 1);
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
            }
            7 => {
                // A divisor is written, and never zero: dividing by zero stops
                // the program, and a stop is not a disagreement. The whole
                // thing is bracketed because nothing settled where `mod` binds.
                // Both sides, not just the whole: `('a' + *4* mod *2*)` still
                // has a `+` and a `mod` side by side inside the brackets.
                self.out.push_str("((");
                self.expr(Ty::I64, depth - 1);
                self.out.push_str(") ");
                let operator = if self.rng.chance(50) { "/" } else { "mod" };
                self.out.push_str(operator);
                self.out.push_str(" *");
                let divisor = self.rng.between(1, 9);
                push_number(self.out, divisor);
                self.out.push_str("*)");
            }
            8 => {
                if let Some(name) = self.pick_name(Ty::Str) {
                    self.out.push_str("count[ref '");
                    self.out.push_str(&name);
                    self.out.push_str("']");
                } else {
                    self.literal(Ty::I64);
                }
            }
            _ => self.call(depth),
        }
    }

    fn call(&mut self, depth: u32) {
        let callable: Vec<usize> = (0..self.funs.len())
            .filter(|&i| self.funs[i].answers == Ty::I64 && self.funs[i].name != "consume")
            .collect();
        if callable.is_empty() {
            self.literal(Ty::I64);
            return;
        }
        let at = callable[self.rng.below(callable.len() as u32) as usize];
        let name = self.funs[at].name.clone();
        let arity = self.funs[at].params.len();
        self.out.push_str(&name);
        self.out.push('[');
        for i in 0..arity {
            if i > 0 {
                self.out.push_str(", ");
            }
            self.expr(Ty::I64, depth.saturating_sub(1));
        }
        self.out.push(']');
    }

    fn literal(&mut self, ty: Ty) {
        match ty {
            Ty::I64 => {
                let value = self.rng.between(-20, 100);
                self.out.push('*');
                push_number(self.out, value);
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
