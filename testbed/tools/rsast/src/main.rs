use syn::spanned::Spanned;
fn main() {
    let path = std::env::args().nth(1).unwrap();
    let src = std::fs::read_to_string(&path).unwrap();
    let file = match syn::parse_file(&src) { Ok(f) => f, Err(_) => std::process::exit(1) };
    for item in file.items {
        let id = match &item {
            syn::Item::Fn(f) => &f.sig.ident,
            syn::Item::Struct(s) => &s.ident,
            syn::Item::Enum(e) => &e.ident,
            syn::Item::Trait(t) => &t.ident,
            syn::Item::Mod(m) => &m.ident,
            syn::Item::Type(t) => &t.ident,
            _ => continue,
        };
        println!("{} {} {}", id, id.span().start().line, item.span().end().line);
    }
}
