//! JSX text and string literals.

/// JSX text as React and Solid read it: lines are trimmed (leading whitespace except on the first
/// line, trailing except on the last), empty lines dropped, and the rest joined with one space.
/// Entities are already decoded by the parser.
pub fn jsx_text(raw: &str) -> String {
    let lines: Vec<&str> = raw.split(['\n', '\r']).collect();
    let last_non_empty = lines.iter().rposition(|l| l.chars().any(|c| c != ' ' && c != '\t'));
    let mut out = String::new();
    for (i, line) in lines.iter().enumerate() {
        let first = i == 0;
        let last = i == lines.len() - 1;
        let mut s: &str = line;
        if !first {
            s = s.trim_start_matches([' ', '\t']);
        }
        if !last {
            s = s.trim_end_matches([' ', '\t']);
        }
        if s.is_empty() {
            continue;
        }
        out.push_str(s);
        if Some(i) != last_non_empty {
            out.push(' ');
        }
    }
    out
}

/// A JS string literal (JSON's escaping is valid JS, plus U+2028/9 escaped).
pub fn js_string(s: &str) -> String {
    let mut out = String::with_capacity(s.len() + 2);
    out.push('"');
    for c in s.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            '\u{2028}' => out.push_str("\\u2028"),
            '\u{2029}' => out.push_str("\\u2029"),
            c if (c as u32) < 0x20 => out.push_str(&format!("\\u{:04x}", c as u32)),
            c => out.push(c),
        }
    }
    out.push('"');
    out
}

/// Line starts of `src`, for line and column numbers.
pub struct Lines {
    starts: Vec<u32>,
}

impl Lines {
    pub fn new(src: &str) -> Self {
        let mut starts = vec![0];
        for (i, b) in src.bytes().enumerate() {
            if b == b'\n' {
                starts.push(i as u32 + 1);
            }
        }
        Lines { starts }
    }

    /// 0-based line, and column in UTF-16 units.
    pub fn at(&self, src: &str, offset: u32) -> (u32, u32) {
        let line = match self.starts.binary_search(&offset) {
            Ok(i) => i,
            Err(i) => i - 1,
        };
        let start = self.starts[line] as usize;
        let col = src[start..offset as usize].encode_utf16().count();
        (line as u32, col as u32)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn jsx_whitespace() {
        assert_eq!(jsx_text("Hello"), "Hello");
        assert_eq!(jsx_text("  Hello  "), "  Hello  ");
        assert_eq!(jsx_text("\n    Hello\n    world\n  "), "Hello world");
        assert_eq!(jsx_text("\n   \n  "), "");
        assert_eq!(jsx_text("You have "), "You have ");
        assert_eq!(jsx_text(" messages\n  "), " messages");
    }
}
