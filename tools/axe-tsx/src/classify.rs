//! What an expression in TSX is (docs/ui.md, "How TSX compiles"):
//!
//! - **Module**: literals, module-level bindings and their static members (`card`, `S.card`,
//!   `"#fff"`, `[card, { gap: 4 }]`). The same for every instance: it can go in a template.
//! - **Local**: no call or member access, but a local binding (`style={s}` with `const s` in the
//!   component): evaluated once per instance.
//! - **Dynamic**: a call, a member access off a local (`props.title`), a tagged template:
//!   anything that may read a signal. It becomes a binding (or a getter, as a component prop).
//!
//! Functions are values: what their bodies read doesn't matter here. JSX inside an expression
//! is an element made per instance: local.

use oxc_ast::ast::*;
use oxc_ast_visit::Visit;
use oxc_semantic::Scoping;
use oxc_syntax::scope::ScopeFlags;

#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Debug)]
pub enum Class {
    Module = 0,
    Local = 1,
    Dynamic = 2,
}

pub struct Classifier<'s> {
    pub scoping: &'s Scoping,
    pub class: Class,
}

impl<'s> Classifier<'s> {
    pub fn of<'a>(scoping: &'s Scoping, e: &Expression<'a>) -> Class {
        let mut c = Classifier { scoping, class: Class::Module };
        c.visit_expression(e);
        c.class
    }

    fn raise(&mut self, c: Class) {
        if c > self.class {
            self.class = c;
        }
    }

    /// Whether `id` names a module-level binding (or a global).
    pub fn module_level(scoping: &Scoping, id: &IdentifierReference) -> bool {
        let Some(rid) = id.reference_id.get() else { return true };
        match scoping.get_reference(rid).symbol_id() {
            None => true, // a global (undefined, Math...)
            Some(sym) => scoping.symbol_scope_id(sym) == scoping.root_scope_id(),
        }
    }

    // A member chain rooted at a module-level identifier, with static keys only.
    fn module_member(&self, e: &Expression) -> bool {
        match e.without_parentheses() {
            Expression::Identifier(id) => Self::module_level(self.scoping, id),
            Expression::StaticMemberExpression(m) => self.module_member(&m.object),
            Expression::ComputedMemberExpression(m) => {
                matches!(m.expression.without_parentheses(), Expression::StringLiteral(_) | Expression::NumericLiteral(_))
                    && self.module_member(&m.object)
            }
            _ => false,
        }
    }
}

impl<'a, 's> Visit<'a> for Classifier<'s> {
    fn visit_identifier_reference(&mut self, it: &IdentifierReference<'a>) {
        if !Self::module_level(self.scoping, it) {
            self.raise(Class::Local);
        }
    }
    fn visit_static_member_expression(&mut self, it: &StaticMemberExpression<'a>) {
        if !self.module_member(&it.object) {
            self.raise(Class::Dynamic);
        }
    }
    fn visit_computed_member_expression(&mut self, it: &ComputedMemberExpression<'a>) {
        let key_static = matches!(it.expression.without_parentheses(), Expression::StringLiteral(_) | Expression::NumericLiteral(_));
        if !(key_static && self.module_member(&it.object)) {
            self.raise(Class::Dynamic);
        }
    }
    fn visit_private_field_expression(&mut self, _: &PrivateFieldExpression<'a>) {
        self.raise(Class::Dynamic);
    }
    fn visit_call_expression(&mut self, _: &CallExpression<'a>) {
        self.raise(Class::Dynamic);
    }
    fn visit_new_expression(&mut self, _: &NewExpression<'a>) {
        self.raise(Class::Dynamic);
    }
    fn visit_tagged_template_expression(&mut self, _: &TaggedTemplateExpression<'a>) {
        self.raise(Class::Dynamic);
    }
    fn visit_await_expression(&mut self, _: &AwaitExpression<'a>) {
        self.raise(Class::Dynamic);
    }
    fn visit_yield_expression(&mut self, _: &YieldExpression<'a>) {
        self.raise(Class::Dynamic);
    }
    fn visit_update_expression(&mut self, _: &UpdateExpression<'a>) {
        self.raise(Class::Dynamic);
    }
    fn visit_assignment_expression(&mut self, _: &AssignmentExpression<'a>) {
        self.raise(Class::Dynamic);
    }
    fn visit_import_expression(&mut self, _: &ImportExpression<'a>) {
        self.raise(Class::Dynamic);
    }
    // Values, whatever they read when called.
    fn visit_function(&mut self, _: &Function<'a>, _: ScopeFlags) {
        self.raise(Class::Local);
    }
    fn visit_arrow_function_expression(&mut self, _: &ArrowFunctionExpression<'a>) {
        self.raise(Class::Local);
    }
    fn visit_class(&mut self, _: &Class_<'a>) {
        self.raise(Class::Local);
    }
    // Made per instance.
    fn visit_jsx_element(&mut self, _: &JSXElement<'a>) {
        self.raise(Class::Local);
    }
    fn visit_jsx_fragment(&mut self, _: &JSXFragment<'a>) {
        self.raise(Class::Local);
    }
    // Types say nothing about values.
    fn visit_ts_type_annotation(&mut self, _: &TSTypeAnnotation<'a>) {}
    fn visit_ts_type(&mut self, _: &TSType<'a>) {}
}

type Class_<'a> = oxc_ast::ast::Class<'a>;
