//! Filesystem transaction primitives for user programs.
use crate::{agent::Versions, disk::{self, ENTRY_LIMIT}, process::{Entry, Volume}};
use alloc::{format, string::String, vec::Vec};

type Result<T> = core::result::Result<T, &'static str>;
use alloc::collections::{BTreeMap, BTreeSet};

type Changes = BTreeMap<String, Option<Option<Vec<u8>>>>;

impl Volume {
    fn apply_changes(&mut self, changes: Changes, moved_versions: Versions, moved_times: disk::Times, touched: BTreeSet<String>) -> Result<()> {
        if changes.is_empty() && touched.is_empty() { return Ok(()); }
        let size = self.files.len()
            + changes.iter().filter(|(path, entry)| entry.is_some() && !self.files.contains_key(*path)).count()
            - changes.iter().filter(|(path, entry)| entry.is_none() && self.files.contains_key(*path)).count();
        if size > ENTRY_LIMIT { return Err("filesystem full"); }
        let mut undo = Vec::with_capacity(changes.len());
        for (path, entry) in changes {
            let old = match entry {
                Some(entry) => self.files.insert(path.clone(), entry),
                None => self.files.remove(&path),
            };
            let previous = self.versions.remove(&path);
            undo.push((path, old, previous));
        }
        self.versions.extend(moved_versions);
        let updates = undo.iter().map(|(path, old, _)| (path.as_str(), old.is_some()))
            .chain(touched.iter().filter(|path| self.files.contains_key(*path)).map(|path| (path.as_str(), true)))
            .collect::<Vec<_>>();
        if let Err(error) = self.save_changes(updates.into_iter(), &moved_times) {
            for (path, old, version) in undo {
                if let Some(entry) = old {
                    self.files.insert(path.clone(), entry);
                } else {
                    self.files.remove(&path);
                }
                self.versions.remove(&path);
                if let Some(version) = version { self.versions.insert(path, version); }
            }
            return Err(error);
        }
        Ok(())
    }
}

fn subtree<'a>(files: &'a disk::Files, root: &str)
    -> impl Iterator<Item = (&'a String, &'a Option<Vec<u8>>)> {
    let prefix = format!("{root}/");
    files.get_key_value(root).into_iter().chain(
        files.range(prefix.clone()..).take_while(move |(path, _)| path.starts_with(&prefix)),
    )
}

fn inside(path: &str, root: &str) -> bool {
    path == root || path.strip_prefix(root).is_some_and(|rest| rest.starts_with('/'))
}


pub(crate) struct Batch {
    generation: u64,
    changes: Changes,
    moved_versions: Versions,
    moved_times: disk::Times,
    touched: BTreeSet<String>,
}

impl Batch {
    pub fn new(volume: &Volume) -> Self {
        Self { generation: volume.disk.revision(), changes: Changes::new(), moved_versions: Versions::new(), moved_times: disk::Times::new(), touched: BTreeSet::new() }
    }

    fn unchanged(&self, volume: &Volume) -> Result<()> {
        if self.generation != volume.disk.revision() {
            return Err("filesystem changed during transaction; retry");
        }
        Ok(())
    }

    fn kind(&self, volume: &Volume, path: &str) -> Option<bool> {
        match self.changes.get(path) {
            Some(entry) => entry.as_ref().map(Option::is_none),
            None => volume.entry(path).map(|entry| matches!(entry, Entry::Directory)),
        }
    }

    pub fn exists(&self, volume: &Volume, path: &str) -> Result<bool> {
        self.unchanged(volume)?;
        Ok(self.kind(volume, path).is_some())
    }

    pub fn edit(&mut self, volume: &Volume, cwd: &str, op: u64, flags: u64, path: &str, other: &str) -> Result<()> {
        self.unchanged(volume)?;
        let files = &volume.files;
        if flags > 1 { return Err("invalid filesystem flags"); }
        if op != 4 { volume.require_writable(path)?; }
        if op >= 4 { volume.require_writable(other)?; }
        if path == "/" {
            return Err(if op == 1 { "already exists" } else { "cannot remove the working directory or root" });
        }
        if op == 3 && self.changes.get(path) == Some(&None) { return Ok(()); }
        let (parent, _) = path.rsplit_once('/').unwrap();
        let parent = if parent.is_empty() { "/" } else { parent };
        if self.kind(volume, parent) != Some(true) { return Err("not a directory"); }
        let kind = self.kind(volume, path);
        match op {
            // Create a directory. The flag permits an existing directory.
            1 if kind == Some(true) && flags == 1 => (),
            1 if kind.is_some() => return Err(if flags == 1 { "not a directory" } else { "already exists" }),
            1 => { self.changes.insert(path.into(), Some(None)); }
            // Create an empty regular file if absent.
            2 if kind.is_none() => { self.changes.insert(path.into(), Some(Some(Vec::new()))); }
            2 => { self.touched.insert(path.into()); }
            // Unlink a single entry, with directory and emptiness checks.
            3 if kind.is_none() => return Err("file not found"),
            3 if kind == Some(false) && flags == 1 => return Err("not a directory"),
            3 if kind == Some(true) && flags == 0 => return Err("is a directory"),
            3 => {
                if kind == Some(true) {
                    if inside(cwd, path) { return Err("cannot remove the working directory or root"); }
                    if disk::children(files, path).any(|(name, _)| {
                        self.changes.get(&format!("{path}/{name}")) != Some(&None)
                    }) {
                        return Err("directory not empty");
                    }
                    if self.changes.range(format!("{path}/")..).take_while(|(p, _)| inside(p, path))
                        .any(|(_, entry)| entry.is_some()) {
                        return Err("directory not empty");
                    }
                }
                self.changes.insert(path.into(), None);
            }
            // Copy one regular file to a destination, with optional no-clobber.
            4 => {
                check_target(volume, self, path, other)?;
                if flags == 1 && self.kind(volume, other).is_some() { return Ok(()); }
                if self.kind(volume, other) == Some(true) { return Err("is a directory"); }
                if self.changes.contains_key(other) { return Err("operands overlap"); }
                self.changes.insert(other.into(), Some(Some(volume.read_file(path)?)));
            }
            // Rename a subtree and its path-keyed version metadata.
            5 => {
                check_target(volume, self, path, other)?;
                if flags == 1 && self.kind(volume, other).is_some() { return Ok(()); }
                let directory = files.get(path).ok_or("file not found")?.is_none();
                let prefix = format!("{path}/");
                if self.changes.contains_key(path)
                    || self.changes.range(prefix.clone()..).next().is_some_and(|(p, _)| p.starts_with(&prefix))
                {
                    return Err("operands overlap");
                }
                if directory && inside(cwd, path) { return Err("cannot move the working directory"); }
                if let Some(existing) = self.kind(volume, other) {
                    if directory || existing { return Err("destination directory already exists or conflicts with a file"); }
                }
                for (source, value) in subtree(files, path) {
                    let dest = format!("{other}{}", &source[path.len()..]);
                    if dest.len() > 512 { return Err("path too long"); }
                    if self.changes.contains_key(source) || self.changes.contains_key(&dest) {
                        return Err("operands overlap");
                    }
                    if let Some(version) = volume.versions.get(source) {
                        if dest.ends_with(".py") { self.moved_versions.insert(dest.clone(), version.clone()); }
                    }
                    self.changes.insert(source.clone(), None);
                    self.moved_times.insert(dest.clone(), volume.disk.times.get(source).copied().unwrap_or_default());
                    self.changes.insert(dest, Some(value.clone()));
                }
            }
            _ => return Err("unknown filesystem operation"),
        }
        if self.changes.len() > ENTRY_LIMIT * 2 { return Err("filesystem full"); }
        Ok(())
    }

    pub fn commit(self, volume: &mut Volume) -> Result<()> {
        self.unchanged(volume)?;
        volume.apply_changes(self.changes, self.moved_versions, self.moved_times, self.touched)
    }
}

fn check_target(volume: &Volume, batch: &Batch, source: &str, target: &str) -> Result<()> {
    if target == "/" || inside(target, source) || inside(source, target) {
        return Err("source and destination overlap");
    }
    let (parent, _) = target.rsplit_once('/').unwrap();
    let parent = if parent.is_empty() { "/" } else { parent };
    if batch.kind(volume, parent) != Some(true) { return Err("not a directory"); }
    Ok(())
}
