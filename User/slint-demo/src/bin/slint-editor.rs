// SPDX-License-Identifier: GPL-3.0-only
use slint::{ComponentHandle, Weak};
use slint_demo::{Editor, run};
use std::{
    fs::{self, File},
    io::{self, Read, Write},
    path::{Path, PathBuf},
    rc::Rc,
};

const LIMIT: usize = 1024 * 1024;

fn load(path: &Path) -> io::Result<String> {
    let file = match File::open(path) {
        Ok(file) => file,
        Err(error) if error.kind() == io::ErrorKind::NotFound => return Ok(String::new()),
        Err(error) => return Err(error),
    };
    let mut text = String::new();
    file.take((LIMIT + 1) as u64).read_to_string(&mut text)?;
    if text.len() > LIMIT || text.contains('\0') {
        return Err(io::Error::new(io::ErrorKind::InvalidData, "expected a text file up to 1 MiB"));
    }
    Ok(text)
}

fn save(path: &Path, document: &str) -> io::Result<()> {
    if document.len() > LIMIT {
        return Err(io::Error::new(io::ErrorKind::InvalidData, "document exceeds 1 MiB"));
    }
    // Save beside the destination so an interrupted write leaves its old contents intact.
    let temporary = path.with_file_name(format!(".slint-save-{}", std::process::id()));
    let mut file = File::options().write(true).create_new(true).open(&temporary)?;
    let result = file.write_all(document.as_bytes()).and_then(|_| file.sync_all());
    drop(file);
    let result = result.and_then(|_| fs::rename(&temporary, path));
    if result.is_err() {
        let _ = fs::remove_file(temporary);
    }
    result
}

fn edit(path: PathBuf) -> Result<(), String> {
    let path = Rc::new(path);
    let text = load(&path).map_err(|error| format!("{}: {error}", path.display()))?;
    run(
        |closed| {
            let editor = Editor::new().map_err(|error| format!("{error:?}"))?;
            let weak: Weak<Editor> = editor.as_weak();
            editor.set_filename(path.display().to_string().into());
            editor.set_document(text.into());
            editor.set_status("Ctrl+S to save".into());
            editor.on_save_requested({
                let path = path.clone();
                let weak = weak.clone();
                move || {
                    let Some(editor) = weak.upgrade() else { return; };
                    match save(&path, &editor.get_document()) {
                        Ok(()) => {
                            editor.set_dirty(false);
                            editor.set_confirm_close(false);
                            editor.set_status("Saved".into());
                        }
                        Err(error) => editor.set_status(format!("Save failed: {error}").into()),
                    }
                }
            });
            editor.on_document_edited({
                let weak = weak.clone();
                move || {
                    if let Some(editor) = weak.upgrade() {
                        editor.set_dirty(true);
                        editor.set_status("Unsaved changes".into());
                    }
                }
            });
            editor.on_discard_requested({
                let closed = closed.clone();
                move || closed.set(true)
            });
            editor.on_close_requested(move || {
                let Some(editor) = weak.upgrade() else { return; };
                if editor.get_dirty() {
                    editor.set_confirm_close(true);
                    editor.set_status("Save or discard changes".into());
                } else {
                    closed.set(true);
                }
            });
            Ok(editor)
        },
        |editor, key| match key {
            19 => { editor.invoke_save_requested(); true }
            27 => { editor.invoke_close_requested(); true }
            _ => false,
        },
        true,
    )?;
    Ok(())
}

fn main() {
    let mut args = std::env::args_os().skip(1);
    let Some(path) = args.next().filter(|_| args.next().is_none()) else {
        eprintln!("usage: slint-editor FILE");
        std::process::exit(2);
    };
    if let Err(error) = edit(path.into()) {
        eprintln!("slint-editor: {error}");
        std::process::exit(1);
    }
}
