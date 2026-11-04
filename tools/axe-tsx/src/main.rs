//! axe-tsx: compiles TSX into fine-grained @axe/ui calls (docs/ui.md, "How TSX compiles").
//!
//!     axe-tsx FILE [-o OUT] [--map]                one file (stdout without -o)
//!     axe-tsx --root DIR --out DIR [--timings] FILE...
//!                                                  each FILE (under the root) into the same
//!                                                  path under --out: .tsx compiled, with an
//!                                                  inline source map; .ts copied
//!
//! The output is still TypeScript (types are kept: esbuild strips them next) with no JSX left.
//! Errors and warnings go to stderr as `file:line:col: error: ...`; any error makes the exit
//! status 1 and leaves that file unwritten.

mod classify;
mod compile;
mod text;

use std::path::{Path, PathBuf};
use std::time::Instant;
use std::{env, fs, process};

use oxc_allocator::Allocator;
use oxc_ast::ast::Statement;
use oxc_ast_visit::Visit;
use oxc_parser::Parser;
use oxc_semantic::SemanticBuilder;
use oxc_span::{SourceType, Span};
use oxc_sourcemap::SourceMapBuilder;

use compile::{Compiler, Diag, Finder, PropsWarnings};
use text::Lines;

struct Output {
    code: String,
    map: String,
    errors: usize,
}

struct Timings {
    parse: u128,
    semantic: u128,
    transform: u128,
}

fn report(file: &str, src: &str, lines: &Lines, diags: &[Diag]) -> usize {
    let mut errors = 0;
    for d in diags {
        let (line, col) = lines.at(src, d.offset);
        let kind = if d.warning { "warning" } else { "error" };
        eprintln!("{file}:{}:{}: {kind}: {}", line + 1, col + 1, d.message);
        errors += usize::from(!d.warning);
    }
    errors
}

fn transform(file: &str, src: &str, t: &mut Timings) -> Output {
    let lines = Lines::new(src);
    let allocator = Allocator::default();
    let start = Instant::now();
    let parsed = Parser::new(&allocator, src, SourceType::tsx()).parse();
    t.parse += start.elapsed().as_micros();
    if !parsed.diagnostics.is_empty() {
        let mut errors = 0;
        for d in parsed.diagnostics.iter() {
            let offset = d.labels.first().map_or(0, |l| l.offset() as u32);
            let (line, col) = lines.at(src, offset);
            eprintln!("{file}:{}:{}: error: {}", line + 1, col + 1, d.message);
            errors += 1;
        }
        return Output { code: String::new(), map: String::new(), errors };
    }
    let start = Instant::now();
    let semantic = SemanticBuilder::new().build(&parsed.program).semantic;
    t.semantic += start.elapsed().as_micros();

    let start = Instant::now();
    let program = &parsed.program;
    let mut c = Compiler::new(src, semantic.scoping(), program);
    let mut finder = Finder { c: &mut c, reps: Vec::new() };
    finder.visit_program(program);
    let mut reps = finder.reps;
    let mut warnings = Vec::new();
    PropsWarnings { diags: &mut warnings }.visit_program(program);
    c.diags.extend(warnings);

    // The runtime import and the templates go after the imports.
    if !c.helpers.is_empty() || !c.templates.is_empty() {
        let at = program
            .body
            .iter()
            .filter_map(|s| if let Statement::ImportDeclaration(d) = s { Some(d.span.end) } else { None })
            .max();
        let names: Vec<String> = c.helpers.iter().map(|h| format!("{h} as _${h}")).collect();
        let mut head = format!("import {{ {} }} from \"@axe/ui/jsx-runtime\";\n", names.join(", "));
        for tpl in &c.templates {
            head.push_str(tpl);
            head.push('\n');
        }
        let (offset, head) = match at {
            Some(end) => (end, format!("\n{}", head.trim_end_matches('\n'))),
            None => (0, head),
        };
        reps.push((Span::new(offset, offset), head));
        reps.sort_by_key(|(s, _)| (s.start, s.end));
    }
    let (code, map) = assemble(file, src, &lines, &reps);
    t.transform += start.elapsed().as_micros();
    let errors = report(file, src, &lines, &c.diags);
    Output { code, map, errors }
}

/// The output and its source map: copied text maps to itself line by line; generated text maps
/// to the start of what it replaced.
fn assemble(file: &str, src: &str, lines: &Lines, reps: &[(Span, String)]) -> (String, String) {
    let mut out = String::with_capacity(src.len() + 1024);
    let mut map = SourceMapBuilder::default();
    let id = map.add_source_and_content(file, src);
    let (mut line, mut col) = (0u32, 0u32);
    let mut at = 0usize;
    let push_copied = |out: &mut String, map: &mut SourceMapBuilder, start: usize, end: usize, line: &mut u32, col: &mut u32| {
        let piece = &src[start..end];
        let (sl, sc) = lines.at(src, start as u32);
        map.add_token(*line, *col, sl, sc, Some(id), None);
        let mut offset = start;
        for (k, part) in piece.split('\n').enumerate() {
            if k > 0 {
                out.push('\n');
                offset += 1;
                *line += 1;
                *col = 0;
                let (sl, sc) = lines.at(src, offset as u32);
                map.add_token(*line, 0, sl, sc, Some(id), None);
            }
            out.push_str(part);
            *col += part.encode_utf16().count() as u32;
            offset += part.len();
        }
    };
    for (span, code) in reps {
        if span.start as usize > at {
            push_copied(&mut out, &mut map, at, span.start as usize, &mut line, &mut col);
        }
        let (sl, sc) = lines.at(src, span.start);
        map.add_token(line, col, sl, sc, Some(id), None);
        for (k, part) in code.split('\n').enumerate() {
            if k > 0 {
                out.push('\n');
                line += 1;
                col = 0;
                map.add_token(line, 0, sl, sc, Some(id), None);
            }
            out.push_str(part);
            col += part.encode_utf16().count() as u32;
        }
        at = at.max(span.end as usize);
    }
    if at < src.len() {
        push_copied(&mut out, &mut map, at, src.len(), &mut line, &mut col);
    }
    (out, map.into_sourcemap().to_json_string())
}

fn base64(bytes: &[u8]) -> String {
    const T: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let mut out = String::with_capacity(bytes.len().div_ceil(3) * 4);
    for chunk in bytes.chunks(3) {
        let n = (u32::from(chunk[0]) << 16) | (u32::from(*chunk.get(1).unwrap_or(&0)) << 8) | u32::from(*chunk.get(2).unwrap_or(&0));
        for i in 0..4 {
            if i <= chunk.len() {
                out.push(T[(n >> (18 - 6 * i)) as usize & 63] as char);
            } else {
                out.push('=');
            }
        }
    }
    out
}

fn with_map(code: &str, map: &str) -> String {
    format!("{code}\n//# sourceMappingURL=data:application/json;base64,{}\n", base64(map.as_bytes()))
}

fn usage() -> ! {
    eprintln!("usage: axe-tsx FILE [-o OUT] [--map]\n       axe-tsx --root DIR --out DIR [--timings] FILE...");
    process::exit(2);
}

fn main() {
    let mut args = env::args().skip(1);
    let (mut root, mut out_dir, mut out_file) = (None::<PathBuf>, None::<PathBuf>, None::<PathBuf>);
    let (mut timings, mut map) = (false, false);
    let mut files = Vec::new();
    while let Some(a) = args.next() {
        match a.as_str() {
            "--root" => root = Some(args.next().unwrap_or_else(|| usage()).into()),
            "--out" => out_dir = Some(args.next().unwrap_or_else(|| usage()).into()),
            "-o" => out_file = Some(args.next().unwrap_or_else(|| usage()).into()),
            "--timings" => timings = true,
            "--map" => map = true,
            "-h" | "--help" => usage(),
            _ if a.starts_with('-') => usage(),
            _ => files.push(PathBuf::from(a)),
        }
    }
    if files.is_empty() {
        usage();
    }
    let mut failed = false;
    let mut total = Timings { parse: 0, semantic: 0, transform: 0 };
    for path in &files {
        let name = path.to_string_lossy().to_string();
        let src = match fs::read_to_string(path) {
            Ok(s) => s,
            Err(e) => {
                eprintln!("{name}: error: {e}");
                failed = true;
                continue;
            }
        };
        let tsx = path.extension().is_some_and(|e| e == "tsx");
        let mut t = Timings { parse: 0, semantic: 0, transform: 0 };
        let code = if tsx {
            let o = transform(&name, &src, &mut t);
            if o.errors > 0 {
                failed = true;
                if let (Some(r), Some(d)) = (&root, &out_dir) {
                    let _ = fs::remove_file(target(r, d, path));
                }
                continue;
            }
            if map || out_dir.is_some() { with_map(&o.code, &o.map) } else { o.code }
        } else {
            src
        };
        if timings && tsx {
            eprintln!(
                "{name}: parse {} us, semantic {} us, transform {} us ({} bytes)",
                t.parse, t.semantic, t.transform, code.len()
            );
            total.parse += t.parse;
            total.semantic += t.semantic;
            total.transform += t.transform;
        }
        let written = match (&root, &out_dir, &out_file) {
            (Some(r), Some(d), _) => {
                let to = target(r, d, path);
                fs::create_dir_all(to.parent().unwrap()).and_then(|_| write_if_changed(&to, &code))
            }
            (_, _, Some(f)) => fs::write(f, &code),
            _ => {
                print!("{code}");
                Ok(())
            }
        };
        if let Err(e) = written {
            eprintln!("{name}: error: {e}");
            failed = true;
        }
    }
    if timings && files.len() > 1 {
        eprintln!("total: parse {} us, semantic {} us, transform {} us", total.parse, total.semantic, total.transform);
    }
    process::exit(i32::from(failed));
}

fn target(root: &Path, out: &Path, file: &Path) -> PathBuf {
    let rel = file.strip_prefix(root).unwrap_or_else(|_| Path::new(file.file_name().unwrap()));
    out.join(rel)
}

// Unchanged files keep their timestamps, so the bundler's dependents don't rebuild.
fn write_if_changed(path: &Path, code: &str) -> std::io::Result<()> {
    if fs::read_to_string(path).is_ok_and(|old| old == code) {
        return Ok(());
    }
    fs::write(path, code)
}
