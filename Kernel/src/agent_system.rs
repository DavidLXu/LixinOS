//! Staged edits to the guest volume and a durable undo record for the last batch.
use crate::{agent::Version, disk, print, process::{Entry as VolumeEntry, Volume}, user};
use alloc::{borrow::Cow, collections::BTreeMap, format, string::String, vec::Vec};
use core::fmt::Write;
use serde::{Deserialize, Serialize};

type Result<T> = core::result::Result<T, &'static str>;
type Entry = Option<Option<disk::FileData>>;
type Edits = BTreeMap<String, Entry>;
const ARCHIVE: &str = "/.agent-system-undo";
const MANIFEST: &str = "/.agent-system-undo/manifest";
const MAX_EDITS: usize = 16;

#[derive(Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
enum State {
    Missing,
    Directory,
    File { bytes: usize, checksum: u32 },
}

impl State {
    fn of(entry: Option<&Option<disk::FileData>>) -> Self {
        match entry {
            None => Self::Missing,
            Some(None) => Self::Directory,
            Some(Some(file)) => {
                let (bytes, checksum) = file.sig();
                Self::File { bytes, checksum }
            }
        }
    }

    fn label(self) -> String {
        match self {
            Self::Missing => "missing".into(),
            Self::Directory => "directory".into(),
            Self::File { bytes, checksum } => {
                format!("file ({bytes} bytes, checksum {checksum:08x})")
            }
        }
    }
}

#[derive(Serialize, Deserialize)]
struct UndoEntry {
    path: String,
    before: State,
    after: State,
    version: Option<Version>,
}

#[derive(Default)]
pub(crate) struct Plan {
    edits: Edits,
}

fn reserved(path: &str) -> bool {
    path == ARCHIVE || path.starts_with("/.agent-system-undo/")
}

fn resolve(operand: &str) -> Result<String> {
    if operand.is_empty() || operand.chars().any(char::is_control) {
        return Err("invalid guest path");
    }
    let mut normalized = String::new();
    for part in operand.split('/') {
        match part {
            "" | "." => (),
            ".." => normalized.truncate(normalized.rfind('/').unwrap_or(0)),
            name => {
                normalized.push('/');
                normalized.push_str(name);
            }
        }
    }
    if normalized.is_empty() {
        normalized.push('/');
    }
    if normalized.len() > 512 {
        return Err("path too long");
    }
    if reserved(&normalized) {
        return Err("path is reserved for system rollback");
    }
    Ok(normalized)
}

fn parent(path: &str) -> &str {
    let (directory, _) = path.rsplit_once('/').unwrap();
    if directory.is_empty() { "/" } else { directory }
}

fn effective<'a>(
    files: &'a disk::Files,
    edits: &'a Edits,
    path: &str,
) -> Option<&'a Option<disk::FileData>> {
    match edits.get(path) {
        Some(change) => change.as_ref(),
        None => files.get(path),
    }
}

fn has_children(files: &disk::Files, edits: &Edits, directory: &str) -> bool {
    let prefix = directory.trim_end_matches('/');
    disk::children(files, directory).any(|(name, _)| {
        let path = format!("{prefix}/{name}");
        effective(files, edits, &path).is_some()
    }) || edits
        .iter()
        .any(|(path, entry)| entry.is_some() && parent(path) == directory)
}

fn parse_hex(text: &str) -> Result<Vec<u8>> {
    if text.len() % 2 != 0 {
        return Err("hex data must have two digits per byte");
    }
    let digit = |byte: u8| {
        char::from(byte)
            .to_digit(16)
            .map(|n| n as u8)
            .ok_or("invalid hex data")
    };
    text.as_bytes()
        .chunks_exact(2)
        .map(|pair| Ok(digit(pair[0])? * 16 + digit(pair[1])?))
        .collect()
}

impl Plan {
    fn bytes<'a>(&'a self, volume: &'a Volume, path: &str) -> Result<Cow<'a, [u8]>> {
        if volume.is_virtual_proc(path) {
            return volume.read_file(path).map(Cow::Owned);
        }
        effective(&volume.files, &self.edits, path)
            .and_then(Option::as_deref)
            .map(Cow::Borrowed)
            .ok_or("guest file not found")
    }

    fn stage(&mut self, volume: &Volume, path: String, entry: Entry) -> Result<()> {
        if path == "/" {
            return Err("the filesystem root must remain a directory");
        }
        if effective(&volume.files, &self.edits, parent(&path)) != Some(&None) {
            return Err("parent directory does not exist");
        }
        if entry != Some(None) && has_children(&volume.files, &self.edits, &path) {
            return Err("remove the directory's children first");
        }
        if entry
            .as_ref()
            .and_then(Option::as_ref)
            .is_some_and(|data| data.len() > disk::FILE_LIMIT)
        {
            return Err("file too large");
        }
        if volume.files.get(&path) == entry.as_ref() {
            self.edits.remove(&path);
        } else {
            if self.edits.len() == MAX_EDITS && !self.edits.contains_key(&path) {
                return Err("system change limit reached");
            }
            self.edits.insert(path, entry);
        }
        Ok(())
    }

    pub(crate) fn tool(&mut self, volume: &Volume, action: &[String]) -> Result<String> {
        let [tool, operand, args @ ..] = action else {
            return Err("expected a system tool and path");
        };
        let path = resolve(operand)?;
        if !matches!(tool.as_str(), "list" | "read" | "readhex") {
            volume.require_writable(&path)?;
        }
        match (tool.as_str(), args) {
            ("list", []) => return self.list(volume, &path),
            ("read", [] | [_]) => {
                let bytes = self.bytes(volume, &path)?;
                let offset = args.first().map_or(Ok(0), |value| {
                    value.parse().map_err(|_| "invalid byte offset")
                })?;
                if bytes.contains(&0) {
                    return Err("binary guest file; use readhex");
                }
                let text =
                    core::str::from_utf8(&bytes).map_err(|_| "binary guest file; use readhex")?;
                return text
                    .get(offset..)
                    .map(String::from)
                    .ok_or("offset is outside text or splits a UTF-8 character");
            }
            ("readhex", [offset]) => {
                let bytes = self.bytes(volume, &path)?;
                let offset = offset.parse::<usize>().map_err(|_| "invalid byte offset")?;
                let chunk = bytes.get(offset..).ok_or("offset is outside file")?;
                let mut output = format!("byte {offset} of {}\n", bytes.len());
                for byte in chunk.iter().take(512) {
                    write!(output, "{byte:02x}").unwrap();
                }
                return Ok(output);
            }
            ("mkdir", []) => {
                if effective(&volume.files, &self.edits, &path).is_some() {
                    return Err("entry already exists");
                }
                self.stage(volume, path.clone(), Some(None))?;
            }
            ("remove", []) => {
                if effective(&volume.files, &self.edits, &path).is_none() {
                    return Err("entry not found");
                }
                self.stage(volume, path.clone(), None)?;
            }
            ("write" | "append" | "writehex" | "appendhex", [contents]) => {
                let mut bytes = if tool.starts_with("append") {
                    self.bytes(volume, &path)?.to_vec()
                } else {
                    Vec::new()
                };
                if tool.ends_with("hex") {
                    bytes.extend(parse_hex(contents)?);
                } else {
                    bytes.extend_from_slice(contents.as_bytes());
                }
                self.stage(volume, path.clone(), Some(Some(disk::FileData::from_vec(bytes))))?;
            }
            ("replace", [old, new]) => {
                let bytes = self.bytes(volume, &path)?;
                let text = core::str::from_utf8(&bytes)
                    .map_err(|_| "binary guest file")?;
                if old.is_empty() || text.matches(old).count() != 1 {
                    return Err("old text must occur exactly once");
                }
                let bytes = text.replacen(old, new, 1).into_bytes();
                self.stage(volume, path.clone(), Some(Some(disk::FileData::from_vec(bytes))))?;
            }
            ("copy" | "compile", [source]) => {
                let source = resolve(source)?;
                if source == path {
                    return Err("source and destination must differ");
                }
                let bytes = self.bytes(volume, &source)?;
                let candidate = if tool == "compile" {
                    if !source.ends_with(".c") {
                        return Err("expected C source");
                    }
                    let text = core::str::from_utf8(&bytes).map_err(|_| "C source must be UTF-8")?;
                    user::compile(&source, text)?
                } else {
                    bytes.into_owned()
                };
                self.stage(volume, path.clone(), Some(Some(disk::FileData::from_vec(candidate))))?;
                return Ok(format!("staged {path:?} from {source:?} using {tool}"));
            }
            _ => return Err("unsupported system tool or invalid arguments"),
        }
        Ok(format!("staged {tool} {path:?}"))
    }

    fn list(&self, volume: &Volume, directory: &str) -> Result<String> {
        let is_dir = match self.edits.get(directory) {
            Some(entry) => entry == &Some(None),
            None => matches!(volume.entry(directory), Some(VolumeEntry::Directory)),
        };
        if !is_dir {
            return Err("guest directory not found");
        }
        let prefix = directory.trim_end_matches('/');
        let mut output = String::new();
        for (name, entry) in volume.children(directory, "", None) {
            let path = format!("{prefix}/{name}");
            if reserved(&path) || self.edits.contains_key(&path) {
                continue;
            }
            let kind = if matches!(entry, VolumeEntry::Directory) { "directory" } else { "file" };
            writeln!(output, "{kind} {name:?}").unwrap();
            if output.len() >= 3000 {
                output.push_str("[directory listing truncated]\n");
                break;
            }
        }
        for (path, entry) in &self.edits {
            if parent(path) == directory {
                let kind = State::of(entry.as_ref()).label();
                writeln!(output, "staged {kind} {path:?}").unwrap();
            }
        }
        Ok(output)
    }

    pub(crate) fn is_empty(&self) -> bool {
        self.edits.is_empty()
    }

    pub(crate) fn summary(&self) -> String {
        let mut output = String::new();
        for (path, entry) in &self.edits {
            writeln!(output, "{path:?}: {}", State::of(entry.as_ref()).label()).unwrap();
        }
        output
    }

    pub(crate) fn preview(&self, volume: &Volume, visible: impl Fn(&str, usize) -> String) {
        fn text(entry: Option<&Option<disk::FileData>>) -> &str {
            entry
                .and_then(Option::as_deref)
                .and_then(|bytes| core::str::from_utf8(bytes).ok())
                .filter(|text| !text.contains('\0'))
                .unwrap_or("")
        }
        for (path, next) in &self.edits {
            let old = volume.files.get(path);
            print(&format!(
                "--- current {path:?}: {}\n",
                State::of(old).label()
            ));
            print(&format!(
                "+++ proposed {path:?}: {}\n",
                State::of(next.as_ref()).label()
            ));
            let before = text(old);
            let after = text(next.as_ref());
            let common = before
                .chars()
                .zip(after.chars())
                .take_while(|(a, b)| a == b)
                .map(|(c, _)| c.len_utf8())
                .sum::<usize>();
            let start = before[..common].rfind('\n').map_or(0, |index| index + 1);
            if !before.is_empty() {
                print(&format!("- {}\n", visible(&before[start..], 1024)));
            }
            if !after.is_empty() {
                print(&format!("+ {}\n", visible(&after[start..], 1024)));
            }
        }
    }

    pub(crate) fn apply(&mut self, volume: &mut Volume) -> Result<()> {
        if volume.files.contains_key(ARCHIVE)
            && !volume.files.get(MANIFEST).is_some_and(Option::is_some)
        {
            return Err("system rollback directory is already occupied");
        }
        let mut changes = core::mem::take(&mut self.edits);
        clear_archive(volume, &mut changes);
        changes.insert(ARCHIVE.into(), Some(None));
        let mut manifest = Vec::new();
        for (path, after) in changes.iter().filter(|(path, _)| !reserved(path)) {
            let old = volume.files.get(path);
            manifest.push(UndoEntry {
                path: path.clone(),
                before: State::of(old),
                after: State::of(after.as_ref()),
                version: volume.versions.get(path).cloned(),
            });
        }
        for (index, entry) in manifest.iter().enumerate() {
            if let Some(Some(bytes)) = volume.files.get(&entry.path) {
                changes.insert(format!("{ARCHIVE}/{index}"), Some(Some(bytes.clone())));
            }
        }
        let serialized = serde_json_wasm::to_vec(&manifest)
            .map_err(|_| "could not encode system rollback data")?;
        changes.insert(MANIFEST.into(), Some(Some(disk::FileData::from_vec(serialized))));
        commit(volume, changes, BTreeMap::new())
    }
}

fn clear_archive(volume: &Volume, edits: &mut Edits) {
    let prefix = format!("{ARCHIVE}/");
    for (path, _) in volume
        .files
        .range(prefix.clone()..)
        .take_while(|(path, _)| path.starts_with(&prefix))
    {
        edits.insert(path.clone(), None);
    }
    edits.insert(ARCHIVE.into(), None);
}

fn commit(
    volume: &mut Volume,
    changes: Edits,
    mut versions: BTreeMap<String, Version>,
) -> Result<()> {
    let mut count = volume.files.len();
    for (key, entry) in &changes {
        volume.require_writable(key)?;
        let existed = volume.files.contains_key(key);
        if existed && entry.is_none() {
            count -= 1;
        }
        if !existed && entry.is_some() {
            count += 1;
        }
        if entry.is_some() && effective(&volume.files, &changes, parent(key)) != Some(&None) {
            return Err("changes would leave an entry without a parent directory");
        }
        if entry != &Some(None) && has_children(&volume.files, &changes, key) {
            return Err("changes would leave orphaned directory entries");
        }
    }
    if count > disk::ENTRY_LIMIT {
        return Err("filesystem full");
    }
    let mut undo = Vec::new();
    for (key, entry) in changes {
        let old = match entry {
            Some(data) => volume.files.insert(key.clone(), data),
            None => volume.files.remove(&key),
        };
        let old_version = volume.versions.remove(&key);
        if let Some(version) = versions.remove(&key) {
            volume.versions.insert(key.clone(), version);
        }
        undo.push((key, old, old_version));
    }
    if let Err(error) = volume.save_changes(undo.iter().map(|(path, old, _)| (path.as_str(), old.is_some())), &disk::Times::new()) {
        for (key, old, version) in undo {
            if let Some(data) = old {
                volume.files.insert(key.clone(), data);
            } else {
                volume.files.remove(&key);
            }
            if let Some(version) = version {
                volume.versions.insert(key, version);
            } else {
                volume.versions.remove(&key);
            }
        }
        return Err(error);
    }
    Ok(())
}

pub(crate) fn rollback(volume: &mut Volume) -> Result<()> {
    let encoded = volume
        .files
        .get(MANIFEST)
        .and_then(Option::as_deref)
        .ok_or("no saved system change")?;
    let manifest: Vec<UndoEntry> =
        serde_json_wasm::from_slice(encoded).map_err(|_| "invalid system rollback manifest")?;
    if manifest.is_empty() || manifest.len() > MAX_EDITS {
        return Err("invalid system rollback manifest");
    }
    let mut changes = BTreeMap::new();
    let mut versions = BTreeMap::new();
    for (index, entry) in manifest.into_iter().enumerate() {
        if entry.path == "/"
            || resolve(&entry.path)? != entry.path
            || changes.contains_key(&entry.path)
        {
            return Err("invalid path in system rollback manifest");
        }
        if State::of(volume.files.get(&entry.path)) != entry.after
            || volume.versions.contains_key(&entry.path)
        {
            return Err("a file changed after the system update");
        }
        let before = match entry.before {
            State::Missing => None,
            State::Directory => Some(None),
            State::File { .. } => {
                let value = volume.files.get(&format!("{ARCHIVE}/{index}"));
                if State::of(value) != entry.before {
                    return Err("system rollback file is missing or damaged");
                }
                value.cloned()
            }
        };
        if let Some(version) = entry.version {
            if !entry.path.ends_with(".py") || !matches!(before, Some(Some(_))) || !version.valid()
            {
                return Err("invalid script version in system rollback");
            }
            versions.insert(entry.path.clone(), version);
        }
        changes.insert(entry.path, before);
    }
    clear_archive(volume, &mut changes);
    commit(volume, changes, versions)
}
