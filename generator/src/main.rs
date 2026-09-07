//! The oracle: write a program, ask every engine what it says, and report any
//! engine that says something different.
//!
//! A case costs about forty milliseconds and nearly all of it is spent in
//! processes — compiling, linking, running. So the work is spread across every
//! core by an atomic counter rather than a fixed split, because cases differ in
//! cost and a thread that finishes early should take the next one rather than
//! wait for its share.

mod gen;
mod rng;

use std::path::{Path, PathBuf};
use std::io::Read;
use std::process::{Command, Stdio};
use std::sync::atomic::{AtomicU64, AtomicUsize, Ordering};
use std::sync::Mutex;
use std::time::Instant;

struct Settings {
    xagc: PathBuf,
    cases: u64,
    first_seed: u64,
    jobs: usize,
    workspace: PathBuf,
    keep_going: bool,
    shrink: bool,
    size: u32,
}

#[derive(Debug)]
struct Answer {
    said: String,
    status: i32,
}

/// A program the compiler would not get through. Kept whole, because the ones
/// worth catching here have been the ones that only happen under load — and a
/// count of them is not something anybody can look into afterwards.
struct Broke {
    seed: u64,
    step: &'static str,
    program: String,
    why: String,
}

struct Finding {
    seed: u64,
    program: String,
    answers: Vec<(&'static str, Answer)>,
    odd: Option<&'static str>,
}

/// Which engine, if any, is the one out of step.
///
/// Out of step is not the same as wrong, and it is worth not confusing them. On
/// 2026-09-07 two engines shared a mistake about printing a borrowed number —
/// they had shared it for months — and the third was right. A vote between them
/// would have named the correct one as the odd one out, and so would the usual
/// tie-break of believing the test interpreter, which was one of the two.
///
/// So three engines can say *that* they disagree and *which one stands apart*.
/// Which one is right is a question for whoever reads the program.
fn oddOneOut(answers: &[(&'static str, Answer)]) -> Option<&'static str> {
    for i in 0..answers.len() {
        let mut agrees = 0;
        for j in 0..answers.len() {
            if i != j && answers[i].1.said == answers[j].1.said
                && answers[i].1.status == answers[j].1.status
            {
                agrees += 1;
            }
        }
        if agrees == 0 {
            // Everyone else agrees with each other, and not with this one.
            let others: Vec<usize> = (0..answers.len()).filter(|&j| j != i).collect();
            let all = others.windows(2).all(|w| {
                answers[w[0]].1.said == answers[w[1]].1.said
                    && answers[w[0]].1.status == answers[w[1]].1.status
            });
            if all && others.len() >= 2 {
                return Some(answers[i].0);
            }
        }
    }
    None
}

fn main() {
    let settings = match read_settings() {
        Ok(settings) => settings,
        Err(trouble) => {
            eprintln!("xag-oracle: {trouble}");
            eprintln!(
                "\n  xag-oracle --xagc <path> [--cases N] [--seed S] [--jobs J]\n\
                 \x20            [--dir D] [--keep-going] [--no-shrink]\n"
            );
            std::process::exit(2);
        }
    };

    let next = AtomicU64::new(settings.first_seed);
    let end = settings.first_seed + settings.cases;
    let done = AtomicUsize::new(0);
    let rejected = AtomicUsize::new(0);
    let skipped = AtomicUsize::new(0);
    let findings: Mutex<Vec<Finding>> = Mutex::new(Vec::new());
    let broke: Mutex<Vec<Broke>> = Mutex::new(Vec::new());
    let started = Instant::now();

    println!(
        "asking {} case(s) across {} thread(s), seeds {}..{}",
        settings.cases, settings.jobs, settings.first_seed, end
    );

    std::thread::scope(|scope| {
        for worker in 0..settings.jobs {
            let (next, done, rejected, skipped, findings, broke, settings) =
                (&next, &done, &rejected, &skipped, &findings, &broke, &settings);
            scope.spawn(move || {
                // One directory per thread, so no two cases ever contend for a
                // name and nothing has to be locked to write a file.
                let room = settings.workspace.join(format!("worker{worker}"));
                let _ = std::fs::create_dir_all(&room);
                let mut program = String::with_capacity(4096);

                loop {
                    let seed = next.fetch_add(1, Ordering::Relaxed);
                    if seed >= end {
                        break;
                    }
                    gen::generate(seed, settings.size, &mut program);
                    match ask(settings, &room, &program) {
                        Verdict::Agreed => {}
                        Verdict::Skipped => {
                            skipped.fetch_add(1, Ordering::Relaxed);
                        }
                        Verdict::Refused(why) => {
                            rejected.fetch_add(1, Ordering::Relaxed);
                            if rejected.load(Ordering::Relaxed) <= 3 {
                                // With the program, because the reason on its
                                // own says which rule was broken and never
                                // which line of the generator wrote it.
                                eprintln!(
                                    "\nseed {seed}: the generator wrote something the \
                                     compiler would not take —\n{why}\n{program}"
                                );
                            }
                        }
                        Verdict::Broke(step, why) => {
                            broke.lock().unwrap().push(Broke {
                                seed,
                                step,
                                program: program.clone(),
                                why,
                            });
                            if !settings.keep_going {
                                next.store(end, Ordering::Relaxed);
                            }
                        }
                        Verdict::Differed(answers) => {
                            let smaller = if settings.shrink {
                                shrink(settings, &room, &program)
                            } else {
                                program.clone()
                            };
                            let odd = oddOneOut(&answers);
                            findings.lock().unwrap().push(Finding {
                                seed,
                                program: smaller,
                                answers,
                                odd,
                            });
                            if !settings.keep_going {
                                next.store(end, Ordering::Relaxed);
                            }
                        }
                    }
                    let so_far = done.fetch_add(1, Ordering::Relaxed) + 1;
                    if so_far % 200 == 0 {
                        let rate = so_far as f64 / started.elapsed().as_secs_f64();
                        print!("\r{so_far} cases, {rate:.0}/s");
                        use std::io::Write;
                        let _ = std::io::stdout().flush();
                    }
                }
            });
        }
    });

    let elapsed = started.elapsed().as_secs_f64();
    let ran = done.load(Ordering::Relaxed);
    let lines = ran as f64 * settings.size as f64;
    println!(
        "\r{ran} case(s) in {elapsed:.1}s — {:.0}/s, ~{:.0} statements/s{}",
        ran as f64 / elapsed.max(1e-9),
        lines / elapsed.max(1e-9),
        {
            let mut notes = String::new();
            if rejected.load(Ordering::Relaxed) > 0 {
                notes.push_str(&format!(", {} rejected", rejected.load(Ordering::Relaxed)));
            }
            if skipped.load(Ordering::Relaxed) > 0 {
                notes.push_str(&format!(", {} skipped", skipped.load(Ordering::Relaxed)));
            }
            notes
        }
    );

    // A program the compiler would not build is reported whole, and in its own
    // right. It was once only counted, which meant an intermittent failure to
    // build large programs under load could be seen twice and looked into
    // never: by the time anybody read the number, the case was gone.
    let stuck = broke.into_inner().unwrap();
    for one in &stuck {
        println!("\n──────── seed {} ────────", one.seed);
        println!("{}", one.program);
        println!(">>> {} did not finish. What it said:\n{}\n", one.step, one.why);
    }

    let found = findings.into_inner().unwrap();
    if found.is_empty() && stuck.is_empty() {
        println!("every engine agreed.");
        return;
    }
    if found.is_empty() {
        println!(
            "{} case(s) the compiler would not get through; no engine disagreed \
             about the rest.",
            stuck.len()
        );
        std::process::exit(1);
    }
    for finding in &found {
        println!("\n──────── seed {} ────────", finding.seed);
        println!("{}", finding.program);
        match finding.odd {
            Some(name) => println!(
                ">>> {name} is the one out of step; the other two agree.\n\
                 >>> That is not the same as {name} being wrong: a mistake two of\n\
                 >>> them share makes the third the odd one.\n"
            ),
            None => println!(">>> all three say something different.\n"),
        }
        for (name, answer) in &finding.answers {
            println!("{name} (status {}):\n{}", answer.status, answer.said);
        }
    }
    println!("\n{} disagreement(s).", found.len());
    if !stuck.is_empty() {
        println!("{} case(s) the compiler would not get through.", stuck.len());
    }
    std::process::exit(1);
}

enum Verdict {
    Agreed,
    Skipped,
    /// The checker said no. That is the generator's fault — it wrote something
    /// the language does not allow — and it says nothing about the compiler.
    Refused(String),
    /// A step of the compiler did not finish. That is the compiler's fault, and
    /// it is worth as much as a disagreement: the engines cannot be compared on
    /// a program one of them would not build, so a run that reports "every
    /// engine agreed" while quietly counting these has not asked what it says
    /// it asked.
    Broke(&'static str, String),
    Differed(Vec<(&'static str, Answer)>),
}

/// One program, put to every engine.
fn ask(settings: &Settings, room: &Path, program: &str) -> Verdict {
    let source = room.join("case.xag");
    if let Err(why) = std::fs::write(&source, program) {
        return Verdict::Broke("writing the case out", why.to_string());
    }

    let interpreted = run(Command::new(&settings.xagc).arg("run").arg(&source));
    if interpreted.status != 0 && interpreted.said.contains("Rule(s) broken") {
        return Verdict::Refused(interpreted.said);
    }
    // A case that outstays its welcome, or that runs past what an engine will
    // follow, says nothing about whether they agree.
    if interpreted.status == -2 || interpreted.said.contains("longer than this engine") {
        return Verdict::Skipped;
    }

    let quick = run(Command::new(&settings.xagc).arg("fast").arg(&source));
    if quick.status == -2 || quick.said.contains("longer than this engine") {
        return Verdict::Skipped;
    }

    let built = run(Command::new(&settings.xagc).arg("build").arg(&source));
    if built.status != 0 {
        return Verdict::Broke("xagc build", built.said);
    }
    let binary = room.join("case");
    let native = run(&mut Command::new(&binary));
    let _ = std::fs::remove_file(&binary);
    if native.status == -2 {
        return Verdict::Skipped;
    }

    let answers = vec![
        ("test interpreter", interpreted),
        ("fast interpreter", quick),
        ("native", native),
    ];
    let alike = answers
        .windows(2)
        .all(|w| w[0].1.said == w[1].1.said && w[0].1.status == w[1].1.status);
    if alike {
        Verdict::Agreed
    } else {
        Verdict::Differed(answers)
    }
}

/// Take lines away for as long as the engines keep disagreeing. A finding that
/// arrives as forty lines is a finding somebody still has to read.
fn shrink(settings: &Settings, room: &Path, program: &str) -> String {
    let mut best: Vec<String> = program.lines().map(|line| line.to_string()).collect();
    let mut improved = true;
    let mut rounds = 0;
    while improved && rounds < 8 {
        improved = false;
        rounds += 1;
        let mut at = best.len();
        while at > 0 {
            at -= 1;
            let mut tried = best.clone();
            tried.remove(at);
            let text = tried.join("\n");
            if matches!(ask(settings, room, &text), Verdict::Differed(_)) {
                best = tried;
                improved = true;
            }
        }
    }
    best.join("\n")
}

/// A program that will not stop is not a disagreement, it is a case to put down.
/// Status -2 says so, and the caller treats it as neither a finding nor a fault.
const PATIENCE: std::time::Duration = std::time::Duration::from_secs(10);

fn run(command: &mut Command) -> Answer {
    let mut child = match command.stdout(Stdio::piped()).stderr(Stdio::piped()).spawn() {
        Ok(child) => child,
        Err(trouble) => return Answer { said: trouble.to_string(), status: -1 },
    };

    // Read both pipes on their own threads: a child that fills one and is never
    // drained would wait forever, and so would this.
    let mut out = child.stdout.take().unwrap();
    let mut err = child.stderr.take().unwrap();
    let reading = std::thread::spawn(move || {
        let mut said = Vec::new();
        let _ = out.read_to_end(&mut said);
        said
    });
    let reading_err = std::thread::spawn(move || {
        let mut said = Vec::new();
        let _ = err.read_to_end(&mut said);
        said
    });

    let began = std::time::Instant::now();
    let status = loop {
        match child.try_wait() {
            Ok(Some(status)) => break status.code().unwrap_or(-1),
            Ok(None) => {
                if began.elapsed() > PATIENCE {
                    let _ = child.kill();
                    let _ = child.wait();
                    break -2;
                }
                std::thread::sleep(std::time::Duration::from_millis(2));
            }
            Err(_) => break -1,
        }
    };

    let mut said = String::from_utf8_lossy(&reading.join().unwrap_or_default()).into_owned();
    let stderr = reading_err.join().unwrap_or_default();
    if !stderr.is_empty() {
        said.push_str(&String::from_utf8_lossy(&stderr));
    }
    Answer { said, status }
}

fn read_settings() -> Result<Settings, String> {
    let mut xagc: Option<PathBuf> = None;
    let mut cases = 200u64;
    let mut first_seed = 1u64;
    let mut jobs = std::thread::available_parallelism().map(|n| n.get()).unwrap_or(4);
    // One room per run, not one per machine: two oracles asking at once would
    // otherwise write each other's cases and report the tear as a finding.
    let mut workspace = std::env::temp_dir().join(format!("xag-oracle-{}", std::process::id()));
    let mut keep_going = false;
    let mut shrink = true;
    let mut size = 250u32;

    let mut args = std::env::args().skip(1);
    while let Some(arg) = args.next() {
        let mut value = || args.next().ok_or_else(|| format!("{arg} wants a value"));
        match arg.as_str() {
            "--xagc" => xagc = Some(PathBuf::from(value()?)),
            "--cases" => cases = value()?.parse().map_err(|_| "--cases wants a number")?,
            "--seed" => first_seed = value()?.parse().map_err(|_| "--seed wants a number")?,
            "--jobs" => jobs = value()?.parse().map_err(|_| "--jobs wants a number")?,
            "--dir" => workspace = PathBuf::from(value()?),
            "--keep-going" => keep_going = true,
            "--no-shrink" => shrink = false,
            "--size" => size = value()?.parse().map_err(|_| "--size wants a number")?,
            other => return Err(format!("{other} is not something this asks for")),
        }
    }

    let xagc = xagc.ok_or("--xagc is wanted: the compiler to ask")?;
    if !xagc.exists() {
        return Err(format!("{} is not there", xagc.display()));
    }
    std::fs::create_dir_all(&workspace).map_err(|e| e.to_string())?;
    Ok(Settings { xagc, cases, first_seed, jobs: jobs.max(1), workspace, keep_going, shrink, size })
}
