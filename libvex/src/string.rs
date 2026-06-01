//! String-Utilities für libvex

/// Vergleicht zwei Byte-Slices
pub fn eq(a: &[u8], b: &[u8]) -> bool {
    if a.len() != b.len() {
        return false;
    }
    for i in 0..a.len() {
        if a[i] != b[i] {
            return false;
        }
    }
    true
}

/// Tokenisiert einen String
pub fn split<'a>(s: &'a [u8], delim: u8) -> impl Iterator<Item = &'a [u8]> {
    Split { s, delim, pos: 0 }
}

struct Split<'a> {
    s: &'a [u8],
    delim: u8,
    pos: usize,
}

impl<'a> Iterator for Split<'a> {
    type Item = &'a [u8];

    fn next(&mut self) -> Option<Self::Item> {
        if self.pos >= self.s.len() {
            return None;
        }

        let start = self.pos;
        while self.pos < self.s.len() && self.s[self.pos] != self.delim {
            self.pos += 1;
        }

        let end = self.pos;
        if self.pos < self.s.len() {
            self.pos += 1; // Skip delimiter
        }

        Some(&self.s[start..end])
    }
}

/// Trimmt Whitespace
pub fn trim(s: &[u8]) -> &[u8] {
    let mut start = 0;
    let mut end = s.len();

    while start < end && (s[start] == b' ' || s[start] == b'\t') {
        start += 1;
    }

    while end > start
        && (s[end - 1] == b' ' || s[end - 1] == b'\t' || s[end - 1] == b'\n' || s[end - 1] == b'\r')
    {
        end -= 1;
    }

    &s[start..end]
}

/// Konvertiert &[u8] zu &str (unchecked)
pub fn to_str(s: &[u8]) -> &str {
    unsafe { core::str::from_utf8_unchecked(s) }
}
