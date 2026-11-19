// Compiled inside the upstream cgen module to share its type declarations,
// glue functions and slot numbering with the multi-unit backend.
pub fn standalone(p: &Program, fns: &[FnOut], glue: &GlueSet, entry: u32, path: &str, export: Option<&str>) -> Result<String, String> {
    let runtime = include_str!("runtime.c");
    let mut c = format!("#define LUST_IMPL 1\n{runtime}\n");
    let mut types = HashSet::new();
    for f in fns {
        types.extend(f.types.iter().cloned());
    }
    for key in glue.protos.keys() {
        collect_types(&key.1, &mut types);
    }
    type_decls(p, &mut c, &types);
    for proto in glue.protos.values() { c.push_str(proto); }
    c.push_str(&glue.defs);
    for f in fns { c.push_str(&f.code); }
    c.push_str("void lust_register_u0(void) {\n");
    for (i, f) in fns.iter().enumerate() {
        if !f.code.is_empty() { let _ = writeln!(c, "  lust_fns[{i}] = (lust_fnptr)f{i};"); }
    }
    c.push_str("}\n");
    c.push_str(&table_unit(fns.len(), &[0])[runtime.len()..]);
    if let Some(name) = export {
        let id = FnId(entry);
        let info = p.fn_info(id);
        if !info.params.iter().chain(core::iter::once(&info.ret))
            .all(|&t| matches!(cty(&p.tys, t), CTy::Int(_))) {
            return Err("lust: C exports require integer parameters and result\n".into());
        }
        let signature = fn_sig(p, id, entry).replacen(&format!(" f{entry}("), &format!(" lxrs_{name}("), 1);
        let args: Vec<_> = (1..=info.params.len()).map(|i| format!("p{i}")).collect();
        let lines = vec!["1"; fns.len()].join(",");
        let _ = writeln!(c, "{signature} {{ static const uint32_t lines[] = {{{lines}}};");
        let _ = writeln!(c, "lust_fn_line = lines; lust_file = {}; lust_base_init(); return f{entry}({}); }}",
                         c_string_lit(path.as_bytes()), args.join(","));
    } else {
        c.push_str(&main_unit(entry, path, &vec![1; fns.len()], &[])[runtime.len()..]);
    }
    Ok(c)
}
