//! `--dev`: what hot reload needs on top of the normal output (docs/ui.md, "Hot reload").
//!
//! - Each top-level component is registered with a hash of its source, so an instance re-runs
//!   only when its component changed.
//! - `signal`, `computed`, `style`, `token`, `theme` and `context` calls get a key (their binding's name and
//!   an ordinal) and a hash of their arguments, so a new version of the module finds what the
//!   previous one made.
//! - Templates get a key the same way.
//! - The module brackets its top level with enter and leave, so what it creates has an owner, and
//!   a top-level await gives the owner up while it waits.

use std::collections::HashMap;

use oxc_ast::ast::*;
use oxc_ast_visit::Visit;
use oxc_span::{GetSpan, Span};
use oxc_syntax::scope::ScopeFlags;

use crate::compile::{capitalized, HasJsx};

/// 32-bit FNV-1a: enough to tell a version of a snippet from the previous one.
pub fn hash(text: &str) -> String {
    let mut h: u32 = 0x811c_9dc5;
    for b in text.bytes() {
        h = (h ^ u32::from(b)).wrapping_mul(0x0100_0193);
    }
    format!("{h:08x}")
}

fn quoted(s: &str) -> String {
    format!("\"{}\"", s.replace('\\', "\\\\").replace('"', "\\\""))
}

#[derive(Default)]
pub struct Dev {
    /// The component the code being visited is in.
    pub component: Option<String>,
    /// Binding names around the code being visited: `S`, then `S.card` inside its property.
    pub names: Vec<String>,
    /// How many functions the code being visited is in: 0 at the module's top level.
    pub functions: u32,
    ordinals: HashMap<(String, String), u32>,
}

impl Dev {
    fn ordinal(&mut self, scope: String, name: String) -> u32 {
        let n = self.ordinals.entry((scope, name)).or_insert(0);
        *n += 1;
        *n - 1
    }

    /// The next key for `name` in the current component (or at module level).
    pub fn key(&mut self, fallback: &str) -> String {
        let name = self.names.last().cloned().unwrap_or_else(|| fallback.to_string());
        let scope = self.component.clone().unwrap_or_default();
        let n = self.ordinal(scope, name.clone());
        format!("{name}#{n}")
    }

    /// A template's key: its component and its ordinal there.
    pub fn template_key(&mut self) -> String {
        let scope = self.component.clone().unwrap_or_default();
        let n = self.ordinal(scope.clone(), "<template>".to_string());
        format!("{scope}#{n}")
    }
}

/// The rewritten start of a keyed call: replaces its callee and opening parenthesis.
pub fn keyed_call(kind: &str, key: &str, args_hash: &str, callee: &str) -> String {
    format!("_$hot.keyed(_$m, {}, {}, {}, {callee}, ", quoted(kind), quoted(key), quoted(args_hash))
}

/// What replaces `await ` at the top level; `))` closes it.
pub fn await_start() -> String {
    "_$hot.resume(_$m, await _$hot.pause(_$m, ".to_string()
}

pub fn head(file: &str) -> String {
    format!("import {{ hot as _$hot }} from \"@axe/ui/hot\";\nconst _$m = _$hot.enter({});\n", quoted(file))
}

pub fn tail() -> String {
    "\n_$hot.leave(_$m);\n".to_string()
}

pub fn template(key: &str, nodes: &str) -> String {
    format!("_$hot.template(_$m, {}, [{nodes}])", quoted(key))
}

fn has_jsx_fn<'a>(f: &Function<'a>) -> bool {
    let mut jsx = HasJsx(false);
    if let Some(body) = &f.body {
        jsx.visit_function_body(body);
    }
    jsx.0
}

fn is_component_init(e: &Expression<'_>) -> bool {
    match e.without_parentheses() {
        Expression::ArrowFunctionExpression(f) => {
            let mut jsx = HasJsx(false);
            jsx.visit_arrow_function_expression(f);
            jsx.0
        }
        Expression::FunctionExpression(f) => {
            let mut jsx = HasJsx(false);
            jsx.visit_function(f, ScopeFlags::Function);
            jsx.0
        }
        _ => false,
    }
}

/// Whether a variable holds a component: `const Card = (props) => <View />`.
pub fn component_variable(d: &VariableDeclarator<'_>) -> Option<String> {
    let BindingPattern::BindingIdentifier(id) = &d.id else { return None };
    let init = d.init.as_ref()?;
    (capitalized(id.name.as_str()) && is_component_init(init)).then(|| id.name.to_string())
}

/// Whether a function is a component.
pub fn component_function(f: &Function<'_>) -> Option<String> {
    let id = f.id.as_ref()?;
    (capitalized(id.name.as_str()) && has_jsx_fn(f)).then(|| id.name.to_string())
}

/// The registrations of the module's components: statements for the head (function declarations
/// are hoisted, so they can be registered before anything runs), and wrappers around variables'
/// initializers (which can't).
pub fn registrations(program: &Program<'_>, src: &str) -> (Vec<String>, Vec<(Span, String)>) {
    let mut head = Vec::new();
    let mut reps = Vec::new();
    let text = |s: Span| &src[s.start as usize..s.end as usize];
    for stmt in &program.body {
        let decl: Option<&Declaration<'_>> = match stmt {
            Statement::ExportDeclaration(e) => Some(&e.declaration),
            s => s.as_declaration(),
        };
        if let Some(Declaration::FunctionDeclaration(f)) = decl {
            if let Some(name) = component_function(f) {
                head.push(register(&name, &hash(text(f.span))));
            }
        } else if let Some(Declaration::VariableDeclaration(v)) = decl {
            for d in &v.declarations {
                let (Some(name), Some(init)) = (component_variable(d), d.init.as_ref()) else { continue };
                let s = init.span();
                let prefix = format!("_$hot.component(_$m, {}, {}, ", quoted(&name), quoted(&hash(text(s))));
                reps.push((Span::new(s.start, s.start), prefix));
                reps.push((Span::new(s.end, s.end), ")".to_string()));
            }
        } else if let Statement::ExportDefaultDeclaration(e) = stmt
            && let ExportDefaultDeclarationKind::FunctionDeclaration(f) = &e.declaration
            && let Some(name) = component_function(f.as_ref())
        {
            head.push(register(&name, &hash(text(f.span))));
        }
    }
    (head, reps)
}

fn register(name: &str, source_hash: &str) -> String {
    format!("{name} = _$hot.component(_$m, {}, {}, {name});", quoted(name), quoted(source_hash))
}
