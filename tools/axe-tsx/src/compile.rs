//! TSX to @axe/ui calls (docs/ui.md, "How TSX compiles").
//!
//! The source is kept as it is outside JSX; each outermost JSX expression is replaced by its
//! compiled form, and the templates it needs are declared at module level after the imports.
//!
//! - `<View>` and `<Text>` (imported from @axe/ui) become nodes of a template: their constant
//!   props (module-level values) go into the template, a Text's constant paragraph too. What
//!   isn't constant attaches to the instance's nodes in its setup: bindings for dynamic props
//!   (calls, member accesses), one-off calls for local values, regions for dynamic children.
//! - A Text's children (text, expressions, Spans) are one paragraph: constant, or one binding.
//! - Components become `createComponent(Tag, props)`: dynamic props are getters, and children
//!   a getter.
//! - Text outside a Text, a Span outside a Text, and nodes inside a Text are compile errors.

use std::collections::{BTreeSet, HashMap};

use oxc_ast::ast::*;
use oxc_ast_visit::Visit;
use oxc_semantic::Scoping;
use oxc_span::{GetSpan, Span};
use oxc_syntax::scope::ScopeFlags;

use crate::classify::{Class, Classifier};
use crate::dev::{self, Dev};
use crate::text::{js_string, jsx_text};

pub struct Diag {
    pub offset: u32,
    pub message: String,
    pub warning: bool,
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum Builtin {
    View,
    Text,
    Span,
}

const TEXT_OPTIONS: &[&str] =
    &["families", "size", "weight", "slant", "language", "color", "lineHeight", "align", "direction", "maxLines", "ellipsis"];
const SPAN_OPTIONS: &[&str] = &["families", "size", "weight", "slant", "language", "color"];
/// Nodes per template: past it, a subtree becomes a template of its own.
const MAX_NODES: usize = 256;

struct NodeOut {
    parent: i32,
    text: bool,
    style_bound: bool,
    props: Vec<String>,
}

struct Tpl {
    nodes: Vec<NodeOut>,
    setup: Vec<String>,
}

enum Item {
    Node(usize),
    Hole(String),
}

pub struct Compiler<'s, 'a> {
    pub src: &'a str,
    scoping: &'s Scoping,
    /// Local name -> (module, imported name).
    imports: HashMap<String, (String, String)>,
    pub templates: Vec<String>,
    pub helpers: BTreeSet<&'static str>,
    pub diags: Vec<Diag>,
    next_template: usize,
    next_region: usize,
    /// Inside a Text's content: a Span is a value there.
    in_text: u32,
    /// Spans compiled so far (a Text with none, and no options, is plain text).
    spans: u32,
    /// With --dev.
    pub dev: Option<Dev>,
}

impl<'s, 'a> Compiler<'s, 'a> {
    pub fn new(src: &'a str, scoping: &'s Scoping, program: &Program<'a>) -> Self {
        let mut imports = HashMap::new();
        for stmt in &program.body {
            let Statement::ImportDeclaration(decl) = stmt else { continue };
            let Some(specifiers) = &decl.specifiers else { continue };
            for s in specifiers {
                if let ImportDeclarationSpecifier::ImportSpecifier(s) = s {
                    imports.insert(
                        s.local.name.to_string(),
                        (decl.source.value.as_str().unwrap_or_default().to_string(), s.imported.name().to_string()),
                    );
                }
            }
        }
        Compiler {
            src,
            scoping,
            imports,
            templates: Vec::new(),
            helpers: BTreeSet::new(),
            diags: Vec::new(),
            next_template: 1,
            next_region: 1,
            in_text: 0,
            spans: 0,
            dev: None,
        }
    }

    /// `for await` can't hand the module's owner back between its awaits (--dev).
    pub fn top_level_for_await(&mut self, offset: u32) {
        self.error(offset, "`for await` at a module's top level doesn't work with hot reload: move it into an async function and await that (docs/ui.md, \"Top-level await\")");
    }

    /// What a module-level name was imported as: (module, imported name).
    pub fn import_of(&self, name: &str) -> Option<(&str, &str)> {
        self.imports.get(name).map(|(m, i)| (m.as_str(), i.as_str()))
    }

    fn error(&mut self, offset: u32, message: impl Into<String>) {
        self.diags.push(Diag { offset, message: message.into(), warning: false });
    }

    fn helper(&mut self, name: &'static str) -> String {
        self.helpers.insert(name);
        format!("_${name}")
    }

    fn slice(&self, span: Span) -> &'a str {
        &self.src[span.start as usize..span.end as usize]
    }

    // ---- Expressions ------------------------------------------------------------------------

    /// An expression's source with the JSX in it compiled.
    pub fn expr(&mut self, e: &Expression<'a>) -> String {
        match e.without_parentheses() {
            Expression::JSXElement(el) => return self.element(el),
            Expression::JSXFragment(f) => return self.fragment(f),
            _ => {}
        }
        let span = e.span();
        let mut finder = Finder { c: self, reps: Vec::new() };
        finder.visit_expression(e);
        let reps = finder.reps;
        splice(self.src, span, &reps)
    }

    fn class(&self, e: &Expression<'a>) -> Class {
        Classifier::of(self.scoping, e)
    }

    fn builtin(&self, name: &JSXElementName<'a>) -> Option<Builtin> {
        let JSXElementName::IdentifierReference(id) = name else { return None };
        if !Classifier::module_level(self.scoping, id) {
            return None;
        }
        let (module, imported) = self.imports.get(id.name.as_str())?;
        if module != "@axe/ui" {
            return None;
        }
        match imported.as_str() {
            "View" => Some(Builtin::View),
            "Text" => Some(Builtin::Text),
            "Span" => Some(Builtin::Span),
            _ => None,
        }
    }

    fn tag(&mut self, name: &JSXElementName<'a>) -> String {
        match name {
            JSXElementName::Identifier(id) => {
                self.error(id.span.start, format!(
                    "<{}> isn't an element: Axe has no lowercase elements; use View, Text or a component",
                    id.name.as_str()
                ));
                "undefined".into()
            }
            other => self.slice(other.span()).to_string(),
        }
    }

    // ---- Elements -----------------------------------------------------------------------------

    /// Spreads of module-level values are constant props; any other spread makes the element a
    /// component call instead of a template node.
    fn local_spread(&self, el: &JSXElement<'a>) -> bool {
        el.opening_element.attributes.iter().any(|a| match a {
            JSXAttributeItem::SpreadAttribute(s) => self.class(&s.argument) != Class::Module,
            JSXAttributeItem::Attribute(_) => false,
        })
    }

    pub fn element(&mut self, el: &JSXElement<'a>) -> String {
        let spread = self.local_spread(el);
        match self.builtin(&el.opening_element.name) {
            Some(Builtin::Span) if self.in_text > 0 => {
                let mut class = Class::Module;
                self.span_value(el, &mut class, false)
            }
            Some(Builtin::Span) => {
                self.error(el.span.start, "a Span must be inside <Text> (docs/ui.md, \"Text\")");
                "null".into()
            }
            Some(_) if !spread => self.template_root(el),
            _ => self.component(el),
        }
    }

    pub fn fragment(&mut self, f: &JSXFragment<'a>) -> String {
        let values = self.child_values(&f.children);
        if values.len() == 1 { values.into_iter().next().unwrap() } else { format!("[{}]", values.join(", ")) }
    }

    fn component(&mut self, el: &JSXElement<'a>) -> String {
        let tag = self.tag(&el.opening_element.name);
        let props = self.props_object(&el.opening_element.attributes, &el.children);
        let create = self.helper("createComponent");
        format!("{create}({tag}, {props})")
    }

    fn attr_key(name: &JSXAttributeName<'a>) -> String {
        match name {
            JSXAttributeName::Identifier(id) => id.name.to_string(),
            JSXAttributeName::NamespacedName(n) => format!("{}:{}", n.namespace.name.as_str(), n.name.name.as_str()),
        }
    }

    fn prop_key(key: &str) -> String {
        if key.chars().all(|c| c.is_ascii_alphanumeric() || c == '_' || c == '$')
            && !key.starts_with(|c: char| c.is_ascii_digit())
        {
            key.to_string()
        } else {
            js_string(key)
        }
    }

    /// An attribute's value as code, and what it is.
    fn attr_value(&mut self, value: &Option<JSXAttributeValue<'a>>) -> (String, Class) {
        match value {
            None => ("true".into(), Class::Module),
            Some(JSXAttributeValue::StringLiteral(s)) => (js_string(s.value.as_str().unwrap_or_default()), Class::Module),
            Some(JSXAttributeValue::ExpressionContainer(c)) => match c.expression.as_expression() {
                Some(e) => {
                    let class = self.class(e);
                    (self.expr(e), class)
                }
                None => ("undefined".into(), Class::Module),
            },
            Some(JSXAttributeValue::Element(e)) => (self.element(e), Class::Local),
            Some(JSXAttributeValue::Fragment(f)) => (self.fragment(f), Class::Local),
        }
    }

    fn props_object(&mut self, attrs: &[JSXAttributeItem<'a>], children: &[JSXChild<'a>]) -> String {
        let mut parts: Vec<String> = Vec::new(); // objects and spreads, merged in order
        let mut fields: Vec<String> = Vec::new();
        for a in attrs {
            match a {
                JSXAttributeItem::SpreadAttribute(s) => {
                    if !fields.is_empty() {
                        parts.push(format!("{{ {} }}", fields.join(", ")));
                        fields.clear();
                    }
                    parts.push(self.expr(&s.argument));
                }
                JSXAttributeItem::Attribute(attr) => {
                    let key = Self::prop_key(&Self::attr_key(&attr.name));
                    let (v, class) = self.attr_value(&attr.value);
                    fields.push(if class == Class::Dynamic { format!("get {key}() {{ return {v}; }}") } else { format!("{key}: {v}") });
                }
            }
        }
        if has_content(children) {
            let values = self.child_values(children);
            let v = if values.len() == 1 { values.into_iter().next().unwrap() } else { format!("[{}]", values.join(", ")) };
            fields.push(format!("get children() {{ return {v}; }}"));
        }
        if parts.is_empty() {
            return if fields.is_empty() { "{}".into() } else { format!("{{ {} }}", fields.join(", ")) };
        }
        if !fields.is_empty() {
            parts.push(format!("{{ {} }}", fields.join(", ")));
        }
        let merge = self.helper("mergeProps");
        format!("{merge}({})", parts.join(", "))
    }

    /// Children as values (a component's children, a fragment): text as strings.
    fn child_values(&mut self, children: &[JSXChild<'a>]) -> Vec<String> {
        let mut out = Vec::new();
        for child in children {
            match child {
                JSXChild::Text(t) => {
                    let s = jsx_text(t.value.as_str());
                    if !s.is_empty() {
                        out.push(js_string(&s));
                    }
                }
                JSXChild::Element(el) => out.push(self.element(el)),
                JSXChild::Fragment(f) => out.extend(self.child_values(&f.children)),
                JSXChild::ExpressionContainer(c) => {
                    if let Some(e) = c.expression.as_expression() {
                        out.push(self.expr(e));
                    }
                }
                JSXChild::Spread(s) => out.push(format!("...{}", self.expr(&s.expression))),
            }
        }
        out
    }

    // ---- Templates ----------------------------------------------------------------------------

    fn template_root(&mut self, el: &JSXElement<'a>) -> String {
        let mut t = Tpl { nodes: Vec::new(), setup: Vec::new() };
        self.add_node(&mut t, el, -1);
        let id = self.next_template;
        self.next_template += 1;
        let nodes: Vec<String> = t
            .nodes
            .iter()
            .map(|n| {
                let mut f = vec![format!("p: {}", n.parent)];
                if n.text {
                    f.push("t: 1".into());
                }
                if n.style_bound {
                    f.push("sb: 1".into());
                }
                if !n.props.is_empty() {
                    f.push(format!("props: () => ({{ {} }})", n.props.join(", ")));
                }
                format!("{{ {} }}", f.join(", "))
            })
            .collect();
        let definition = match &mut self.dev {
            Some(dev) => dev::template(&dev.template_key(), &nodes.join(", ")),
            None => format!("/*#__PURE__*/ {}([{}])", self.helper("template"), nodes.join(", ")),
        };
        self.templates.push(format!("const _tmpl${id} = {definition};"));
        let instance = self.helper("instance");
        if t.setup.is_empty() {
            format!("{instance}(_tmpl${id})")
        } else {
            format!("{instance}(_tmpl${id}, (_n) => {{ {} }})", t.setup.join(" "))
        }
    }

    fn add_node(&mut self, t: &mut Tpl, el: &JSXElement<'a>, parent: i32) -> usize {
        let i = t.nodes.len();
        let text = self.builtin(&el.opening_element.name) == Some(Builtin::Text);
        let name = if text { "Text" } else { "View" };
        t.nodes.push(NodeOut { parent, text, style_bound: false, props: Vec::new() });
        let text_arg = if text { ", 1" } else { "" };
        let mut options: Vec<(String, String, Class)> = Vec::new();
        for a in &el.opening_element.attributes {
            let attr = match a {
                JSXAttributeItem::Attribute(attr) => attr,
                // A module-level spread (local ones went the component way): constant props.
                JSXAttributeItem::SpreadAttribute(sp) => {
                    let v = self.expr(&sp.argument);
                    if text {
                        options.push(("...".into(), v, Class::Module));
                    } else {
                        t.nodes[i].props.push(format!("...{v}"));
                    }
                    continue;
                }
            };
            let key = Self::attr_key(&attr.name);
            let (v, class) = self.attr_value(&attr.value);
            match key.as_str() {
                "ref" => {
                    let r = self.helper("ref");
                    t.setup.push(format!("{r}(_n[{i}], {v});"));
                }
                "style" => match class {
                    Class::Module => t.nodes[i].props.push(format!("style: {v}")),
                    Class::Local => {
                        let f = self.helper("applyStyle");
                        t.setup.push(format!("{f}(_n[{i}], {v}{text_arg});"));
                        t.nodes[i].style_bound = true;
                    }
                    Class::Dynamic => {
                        let f = self.helper("bindStyle");
                        t.setup.push(format!("{f}(_n[{i}], () => {v}{text_arg});"));
                        t.nodes[i].style_bound = true;
                    }
                },
                "tokens" | "state" => {
                    if class == Class::Module {
                        t.nodes[i].props.push(format!("{key}: {v}"));
                    } else {
                        let f = self.helper(if key == "tokens" { "bindTokens" } else { "bindState" });
                        t.setup.push(format!("{f}(_n[{i}], () => {v});"));
                    }
                }
                k if text && TEXT_OPTIONS.contains(&k) => options.push((key.clone(), v, class)),
                "children" => self.error(attr.span.start, format!("put {name}'s children between its tags")),
                _ => self.error(attr.span.start, format!("{name} has no prop `{key}`")),
            }
        }
        if text {
            self.text_node(t, i, el, options);
        } else {
            self.view_children(t, i, &el.children);
        }
        i
    }

    fn view_children(&mut self, t: &mut Tpl, i: usize, children: &[JSXChild<'a>]) {
        let mut items = Vec::new();
        self.view_items(t, i, children, &mut items);
        // Regions from the last: each goes before what follows it (a node, or the next region).
        let mut after = "null".to_string();
        let mut names: Vec<Option<String>> = vec![None; items.len()];
        let mut decls = Vec::new();
        for (k, item) in items.iter().enumerate().rev() {
            match item {
                Item::Node(idx) => after = format!("_n[{idx}]"),
                Item::Hole(_) => {
                    let r = format!("_r{}", self.next_region);
                    self.next_region += 1;
                    let region = self.helper("region");
                    decls.push(format!("const {r} = {region}(_n[{i}], {after});"));
                    after = r.clone();
                    names[k] = Some(r);
                }
            }
        }
        t.setup.extend(decls);
        for (k, item) in items.into_iter().enumerate() {
            if let Item::Hole(v) = item {
                let insert = self.helper("insert");
                t.setup.push(format!("{insert}({}, {v});", names[k].as_ref().unwrap()));
            }
        }
    }

    fn view_items(&mut self, t: &mut Tpl, i: usize, children: &[JSXChild<'a>], items: &mut Vec<Item>) {
        for child in children {
            match child {
                JSXChild::Text(text) => {
                    let s = jsx_text(text.value.as_str());
                    if !s.trim().is_empty() {
                        self.leaf_text(text.span.start, &s);
                    }
                }
                JSXChild::Element(el) => {
                    let spread = self.local_spread(el);
                    match self.builtin(&el.opening_element.name) {
                        Some(Builtin::Span) => self.error(el.span.start, "a Span must be inside <Text> (docs/ui.md, \"Text\")"),
                        Some(_) if !spread && t.nodes.len() < MAX_NODES => {
                            let idx = self.add_node(t, el, i as i32);
                            items.push(Item::Node(idx));
                        }
                        _ => {
                            let v = self.element(el);
                            items.push(Item::Hole(v));
                        }
                    }
                }
                JSXChild::Fragment(f) => self.view_items(t, i, &f.children, items),
                JSXChild::ExpressionContainer(c) => {
                    let Some(e) = c.expression.as_expression() else { continue };
                    match e.without_parentheses() {
                        Expression::StringLiteral(s) => {
                            self.leaf_text(c.span.start, s.value.as_str().unwrap_or_default());
                            continue;
                        }
                        Expression::NumericLiteral(n) => {
                            self.leaf_text(c.span.start, &n.value.to_string());
                            continue;
                        }
                        Expression::TemplateLiteral(_) => {
                            self.leaf_text(c.span.start, self.slice(e.span()));
                            continue;
                        }
                        _ => {}
                    }
                    let class = self.class(e);
                    let v = self.expr(e);
                    items.push(Item::Hole(if class == Class::Dynamic { format!("() => {v}") } else { v }));
                }
                JSXChild::Spread(s) => {
                    let v = self.expr(&s.expression);
                    items.push(Item::Hole(v));
                }
            }
        }
    }

    fn leaf_text(&mut self, offset: u32, text: &str) {
        let short: String = text.trim().chars().take(24).collect();
        self.error(offset, format!("text in a View must be inside <Text>: <Text>{short}</Text> (docs/ui.md, \"Text\")"));
    }

    // ---- Text ---------------------------------------------------------------------------------

    fn text_node(&mut self, t: &mut Tpl, i: usize, el: &JSXElement<'a>, options: Vec<(String, String, Class)>) {
        let mut class = options.iter().map(|o| o.2).max().unwrap_or(Class::Module);
        let mut parts = Vec::new();
        let spans_before = self.spans;
        self.in_text += 1;
        for child in &el.children {
            self.text_part(child, &mut parts, &mut class, false);
        }
        self.in_text -= 1;
        // No options and no Span: plain text, its parts joined (a part that turns out to be a
        // Span at run time takes the general path there).
        if options.is_empty() && self.spans == spans_before && class != Class::Module {
            let value = if parts.len() == 1 { parts[0].clone() } else { format!("[{}]", parts.join(", ")) };
            if class == Class::Dynamic {
                let bind = self.helper("bindPlainText");
                t.setup.push(format!("{bind}(_n[{i}], () => {value});"));
            } else {
                let apply = self.helper("applyPlainText");
                t.setup.push(format!("{apply}(_n[{i}], {value});"));
            }
            return;
        }
        let opts: Vec<String> = options
            .iter()
            .map(|(k, v, _)| if k == "..." { format!("...{v}") } else { format!("{}: {v}", Self::prop_key(k)) })
            .collect();
        if class == Class::Module {
            t.nodes[i].props.extend(opts);
            t.nodes[i].props.push(format!("children: [{}]", parts.join(", ")));
        } else {
            let mut fields = opts;
            fields.push(format!("children: [{}]", parts.join(", ")));
            if class == Class::Dynamic {
                let bind = self.helper("bindText");
                t.setup.push(format!("{bind}(_n[{i}], () => ({{ {} }}));", fields.join(", ")));
            } else {
                let apply = self.helper("applyText");
                t.setup.push(format!("{apply}(_n[{i}], {{ {} }});", fields.join(", ")));
            }
        }
    }

    fn text_part(&mut self, child: &JSXChild<'a>, parts: &mut Vec<String>, class: &mut Class, in_span: bool) {
        match child {
            JSXChild::Text(t) => {
                let s = jsx_text(t.value.as_str());
                if !s.is_empty() {
                    parts.push(js_string(&s));
                }
            }
            JSXChild::ExpressionContainer(c) => {
                if let Some(e) = c.expression.as_expression() {
                    *class = (*class).max(self.class(e));
                    parts.push(self.expr(e));
                }
            }
            JSXChild::Element(el) => match self.builtin(&el.opening_element.name) {
                Some(Builtin::Span) if in_span => self.error(el.span.start, "Spans don't nest: close one before opening the next"),
                Some(Builtin::Span) => {
                    let v = self.span_value(el, class, true);
                    parts.push(v);
                }
                _ => self.error(
                    el.span.start,
                    "a Text holds only text and Spans: put other nodes beside it, in a View (docs/ui.md, \"Text\")",
                ),
            },
            JSXChild::Fragment(f) => {
                for c in &f.children {
                    self.text_part(c, parts, class, in_span);
                }
            }
            JSXChild::Spread(s) => {
                *class = (*class).max(self.class(&s.expression));
                parts.push(format!("...{}", self.expr(&s.expression)));
            }
        }
    }

    /// A Span as a value in a paragraph.
    fn span_value(&mut self, el: &JSXElement<'a>, class: &mut Class, _direct: bool) -> String {
        self.spans += 1;
        let mut fields = Vec::new();
        for a in &el.opening_element.attributes {
            match a {
                JSXAttributeItem::Attribute(attr) => {
                    let key = Self::attr_key(&attr.name);
                    if !SPAN_OPTIONS.contains(&key.as_str()) {
                        self.error(attr.span.start, format!("Span has no prop `{key}`"));
                        continue;
                    }
                    let (v, c) = self.attr_value(&attr.value);
                    *class = (*class).max(c);
                    fields.push(format!("{}: {v}", Self::prop_key(&key)));
                }
                JSXAttributeItem::SpreadAttribute(s) => {
                    *class = Class::Dynamic;
                    fields.push(format!("...{}", self.expr(&s.argument)));
                }
            }
        }
        let mut parts = Vec::new();
        self.in_text += 1;
        for child in &el.children {
            self.text_part(child, &mut parts, class, true);
        }
        self.in_text -= 1;
        fields.push(format!("children: [{}]", parts.join(", ")));
        let span = self.helper("Span");
        format!("{span}({{ {} }})", fields.join(", "))
    }
}

fn has_content(children: &[JSXChild]) -> bool {
    children.iter().any(|c| match c {
        JSXChild::Text(t) => !jsx_text(t.value.as_str()).is_empty(),
        JSXChild::ExpressionContainer(c) => c.expression.as_expression().is_some(),
        JSXChild::Fragment(f) => has_content(&f.children),
        _ => true,
    })
}

/// Finds the outermost JSX in what it visits and compiles it.
pub struct Finder<'c, 's, 'a> {
    pub c: &'c mut Compiler<'s, 'a>,
    pub reps: Vec<(Span, String)>,
}

impl<'c, 's, 'a> Finder<'c, 's, 'a> {
    /// What a call is, for --dev: (kind, the callee as written, or a function calling it).
    fn keyed(&self, it: &CallExpression<'a>) -> Option<(&'static str, String)> {
        let imported = |name: &str| {
            self.c.import_of(name).filter(|(module, _)| *module == "@axe/ui" || *module == "@axe/core").map(|(_, i)| i)
        };
        match &it.callee {
            Expression::Identifier(id) => {
                let kind = match imported(id.name.as_str())? {
                    "signal" => "signal",
                    "computed" => "computed",
                    "style" => "style",
                    "token" => "token",
                    "theme" => "theme",
                    "context" => "context",
                    _ => return None,
                };
                Some((kind, id.name.to_string()))
            }
            Expression::StaticMemberExpression(m) => {
                let Expression::Identifier(object) = &m.object else { return None };
                let method = m.property.name.as_str();
                if imported(object.name.as_str())? != "Token" || !matches!(method, "create" | "local") {
                    return None;
                }
                Some(("token", format!("(..._a) => {}.{method}(..._a)", object.name)))
            }
            _ => None,
        }
    }
}

impl<'c, 's, 'a> Visit<'a> for Finder<'c, 's, 'a> {
    fn visit_jsx_element(&mut self, it: &JSXElement<'a>) {
        let code = self.c.element(it);
        self.reps.push((it.span, code));
    }
    fn visit_jsx_fragment(&mut self, it: &JSXFragment<'a>) {
        let code = self.c.fragment(it);
        self.reps.push((it.span, code));
    }
    fn visit_call_expression(&mut self, it: &CallExpression<'a>) {
        if self.c.dev.is_some()
            && let Some((kind, callee)) = self.keyed(it)
        {
            // The callee and the parenthesis are replaced, up to the first argument, or to the
            // closing parenthesis when there is none. Type arguments go with them.
            let open = it.arguments.first().map_or(it.span.end - 1, |a| a.span().start);
            let args = &self.c.src[open as usize..it.span.end as usize - 1];
            let args_hash = dev::hash(args);
            let dev = self.c.dev.as_mut().unwrap();
            let key = dev.key(kind);
            self.reps.push((Span::new(it.span.start, open), dev::keyed_call(kind, &key, &args_hash, &callee)));
            for a in &it.arguments {
                self.visit_argument(a);
            }
            return;
        }
        oxc_ast_visit::walk::walk_call_expression(self, it);
    }
    fn visit_function(&mut self, it: &Function<'a>, flags: ScopeFlags) {
        let scope = self.c.dev.as_ref().filter(|d| d.component.is_none()).and_then(|_| dev::component_function(it));
        if let (Some(name), Some(d)) = (&scope, &mut self.c.dev) {
            d.component = Some(name.clone());
        }
        if let Some(d) = &mut self.c.dev {
            d.functions += 1;
        }
        oxc_ast_visit::walk::walk_function(self, it, flags);
        if let Some(d) = &mut self.c.dev {
            d.functions -= 1;
            if scope.is_some() {
                d.component = None;
            }
        }
    }
    fn visit_arrow_function_expression(&mut self, it: &ArrowFunctionExpression<'a>) {
        if let Some(d) = &mut self.c.dev {
            d.functions += 1;
        }
        oxc_ast_visit::walk::walk_arrow_function_expression(self, it);
        if let Some(d) = &mut self.c.dev {
            d.functions -= 1;
        }
    }
    // A top-level await gives up the module's owner while it waits, and takes it back after.
    fn visit_await_expression(&mut self, it: &AwaitExpression<'a>) {
        if self.c.dev.as_ref().is_none_or(|d| d.functions > 0) {
            oxc_ast_visit::walk::walk_await_expression(self, it);
            return;
        }
        let inner = it.argument.span();
        self.reps.push((Span::new(it.span.start, inner.start), dev::await_start()));
        self.visit_expression(&it.argument);
        self.reps.push((Span::new(inner.end, it.span.end), "))".to_string()));
    }
    fn visit_for_of_statement(&mut self, it: &ForOfStatement<'a>) {
        if it.r#await && self.c.dev.as_ref().is_some_and(|d| d.functions == 0) {
            self.c.top_level_for_await(it.span.start);
        }
        oxc_ast_visit::walk::walk_for_of_statement(self, it);
    }
    fn visit_variable_declarator(&mut self, it: &VariableDeclarator<'a>) {
        let Some(d) = &mut self.c.dev else {
            oxc_ast_visit::walk::walk_variable_declarator(self, it);
            return;
        };
        let name = if let BindingPattern::BindingIdentifier(id) = &it.id { Some(id.name.to_string()) } else { None };
        let pushed = name.is_some();
        if let Some(n) = name {
            d.names.push(n);
        }
        let scope = if d.component.is_none() { dev::component_variable(it) } else { None };
        if scope.is_some() {
            d.component = scope.clone();
        }
        oxc_ast_visit::walk::walk_variable_declarator(self, it);
        let d = self.c.dev.as_mut().unwrap();
        if scope.is_some() {
            d.component = None;
        }
        if pushed {
            d.names.pop();
        }
    }
    fn visit_object_property(&mut self, it: &ObjectProperty<'a>) {
        let Some(d) = &mut self.c.dev else {
            oxc_ast_visit::walk::walk_object_property(self, it);
            return;
        };
        let named = if let PropertyKey::StaticIdentifier(k) = &it.key {
            let name = d.names.last().map_or_else(|| k.name.to_string(), |n| format!("{n}.{}", k.name));
            d.names.push(name);
            true
        } else {
            false
        };
        oxc_ast_visit::walk::walk_object_property(self, it);
        if named {
            self.c.dev.as_mut().unwrap().names.pop();
        }
    }
}

/// `src[span]` with the replacements (in source order, inside the span) applied.
pub fn splice(src: &str, span: Span, reps: &[(Span, String)]) -> String {
    let mut out = String::new();
    let mut at = span.start as usize;
    for (s, code) in reps {
        out.push_str(&src[at..s.start as usize]);
        out.push_str(code);
        at = s.end as usize;
    }
    out.push_str(&src[at..span.end as usize]);
    out
}

// ---- Warnings -------------------------------------------------------------------------------------

/// Warns about components that destructure their props: props that change are getters, and
/// destructuring reads them once.
pub struct PropsWarnings<'d> {
    pub diags: &'d mut Vec<Diag>,
}

pub struct HasJsx(pub bool);
impl<'a> Visit<'a> for HasJsx {
    fn visit_jsx_element(&mut self, _: &JSXElement<'a>) {
        self.0 = true;
    }
    fn visit_jsx_fragment(&mut self, _: &JSXFragment<'a>) {
        self.0 = true;
    }
}

pub fn capitalized(name: &str) -> bool {
    name.starts_with(|c: char| c.is_ascii_uppercase())
}

impl<'d> PropsWarnings<'d> {
    fn check<'a>(&mut self, name: &str, params: &FormalParameters<'a>, has_jsx: bool) {
        let Some(first) = params.items.first() else { return };
        if !matches!(first.pattern, BindingPattern::ObjectPattern(_)) || !has_jsx {
            return;
        }
        self.diags.push(Diag {
            offset: first.span.start,
            message: format!(
                "{name} destructures its props: props that change won't update it; read them as props.x where they're used (docs/ui.md, \"Props\")"
            ),
            warning: true,
        });
    }
}

impl<'a, 'd> Visit<'a> for PropsWarnings<'d> {
    fn visit_function(&mut self, it: &Function<'a>, flags: ScopeFlags) {
        if let (Some(id), Some(body)) = (&it.id, &it.body)
            && capitalized(id.name.as_str())
        {
            let mut jsx = HasJsx(false);
            jsx.visit_function_body(body);
            self.check(id.name.as_str(), &it.params, jsx.0);
        }
        oxc_ast_visit::walk::walk_function(self, it, flags);
    }
    fn visit_variable_declarator(&mut self, it: &VariableDeclarator<'a>) {
        if let (BindingPattern::BindingIdentifier(id), Some(init)) = (&it.id, &it.init)
            && capitalized(id.name.as_str())
        {
            match init.without_parentheses() {
                Expression::ArrowFunctionExpression(f) => {
                    let mut jsx = HasJsx(false);
                    jsx.visit_arrow_function_expression(f);
                    self.check(id.name.as_str(), &f.params, jsx.0);
                }
                Expression::FunctionExpression(f) => {
                    let mut jsx = HasJsx(false);
                    jsx.visit_function(f, ScopeFlags::Function);
                    self.check(id.name.as_str(), &f.params, jsx.0);
                }
                _ => {}
            }
        }
        oxc_ast_visit::walk::walk_variable_declarator(self, it);
    }
}
