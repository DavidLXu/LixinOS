use super::{MAX_FILES, Project, Result, visible};
use alloc::{collections::BTreeMap, format, string::{String, ToString}, vec::Vec};
use serde::Serialize;

#[derive(Clone, Copy, PartialEq, Eq, Serialize)]
#[serde(rename_all = "snake_case")]
pub(crate) enum ToolStatus {
    Ok, Failed, RequiresTerminal, TimedOut, Declined, Skipped, Paused,
    NotFound, Staged, NeedsTests, TestPassed, TestFailed, Saved, Verified, Stale,
}

pub(crate) struct ToolReport {
    pub(crate) status: ToolStatus,
    pub(crate) detail: String,
    pub(crate) exit_code: Option<u8>,
    pub(super) path: Option<String>,
    artifact: Option<Receipt>,
}

impl ToolReport {
    pub(crate) fn new(status: ToolStatus, detail: impl Into<String>) -> Self {
        Self { status, detail: detail.into(), exit_code: None, path: None, artifact: None }
    }

    pub(super) fn at(mut self, path: String) -> Self {
        self.path = Some(path);
        self
    }

    pub(super) fn observation(&self, action: &[String], command: bool) -> Result<String> {
        #[derive(Serialize)]
        struct Observation<'a> {
            tool: String,
            status: ToolStatus,
            detail: String,
            #[serde(skip_serializing_if = "Option::is_none")]
            path: Option<&'a str>,
            #[serde(skip_serializing_if = "Option::is_none")]
            command: Option<String>,
            #[serde(skip_serializing_if = "Option::is_none")]
            exit_code: Option<u8>,
            #[serde(skip_serializing_if = "Option::is_none")]
            artifact: Option<&'a Receipt>,
        }
        let mut record = Observation {
            tool: visible(&action[0], 32), status: self.status, detail: visible(&self.detail, 1600),
            path: self.path.as_deref(),
            command: command.then(|| visible(&format!("{:?}", &action[1..]), 384)),
            exit_code: self.exit_code, artifact: self.artifact.as_ref(),
        };
        let encode = |record: &Observation<'_>| serde_json_wasm::to_string(record)
            .map_err(|_| "could not encode chat tool result");
        let encoded = encode(&record)?;
        if encoded.len() <= 3000 { return Ok(encoded); }
        record.detail = visible(&self.detail, 384);
        encode(&record)
    }
}

#[derive(Clone, Copy, Default, Serialize)]
pub(super) struct Checks {
    pub(super) exact_passed: usize,
    pub(super) smoke_passed: usize,
    pub(super) failed: usize,
}

impl Checks {
    pub(super) fn record(&mut self, smoke: bool, passed: bool) {
        if !passed { self.failed += 1; }
        else if smoke { self.smoke_passed += 1; }
        else { self.exact_passed += 1; }
    }
}

#[derive(Serialize)]
pub(super) struct Receipt {
    bytes: usize,
    snapshot: u64,
    checks: Checks,
    #[serde(skip_serializing_if = "Option::is_none")]
    interactive: Option<&'static str>,
    #[serde(skip_serializing_if = "Option::is_none")]
    launch: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    run: Option<Run>,
}

impl Receipt {
    pub(super) fn describe(&self, path: &str) -> String {
        let mut line = format!(
            "Verified {path:?} on disk ({} bytes, snapshot {}). Tests: {} exact passed, {} smoke passed, {} failed.",
            self.bytes, self.snapshot, self.checks.exact_passed, self.checks.smoke_passed, self.checks.failed,
        );
        if let Some(state) = self.interactive {
            line.push_str(if state == "scripted" { " Interactive: scripted input verified." }
                else { " Interactive: awaiting a check." });
        }
        if let Some(run) = &self.run {
            line.push_str(&format!(" Direct run exited with status {}.", run.exit_code));
        }
        if let Some(launch) = &self.launch {
            line.push_str(&format!(" Run: {launch}"));
        }
        line
    }

    pub(super) fn render(&self, path: &str) {
        use super::ui;
        ui::detail("Verified", &format!("{path:?}  |  {} bytes  |  snapshot {}", self.bytes, self.snapshot), ui::POSITIVE);
        let checks = if self.checks.exact_passed == 0 && self.checks.smoke_passed == 0 && self.checks.failed == 0 {
            String::from("No automated checks recorded.")
        } else {
            format!("{} exact passed, {} smoke passed, {} failed",
                self.checks.exact_passed, self.checks.smoke_passed, self.checks.failed)
        };
        ui::detail("Checks", &checks, if self.checks.failed == 0 { super::COMMAND_COLOR } else { ui::ERROR });
        if let Some(state) = self.interactive {
            if state == "scripted" {
                ui::detail("Input", "Scripted input verified.", ui::POSITIVE);
            } else {
                ui::detail("Input", "Awaiting an interactive check.", ui::ATTENTION);
            }
        }
        if let Some(run) = &self.run {
            ui::detail("Exit", &format!("Direct run exited with status {}.", run.exit_code),
                if run.exit_code == 0 { ui::POSITIVE } else { ui::ERROR });
        }
        if let Some(launch) = &self.launch {
            ui::detail("Run", launch, super::COMMAND_COLOR);
        }
    }
}

struct Artifact {
    bytes: Vec<u8>,
    launch: Option<String>,
    interactive: bool,
}

#[derive(Clone, Serialize)]
struct Run {
    command: String,
    exit_code: u8,
}

pub(super) struct RunTarget {
    path: String,
    bytes: Vec<u8>,
    command: String,
    generation: u64,
}

impl RunTarget {
    pub(super) fn before(project: &Project<'_>, argv: &[String]) -> Option<Self> {
        let command = argv.first()?.as_str();
        if !command.contains('/') { return None; }
        let path = project.path(command).ok()?;
        if project.pending.contains_key(&path) { return None; }
        let volume = project.volume.borrow();
        let bytes = volume.read_file(&path).ok()?;
        Some(Self { path, bytes, command: visible(&format!("{argv:?}"), 256),
            generation: volume.disk.revision() })
    }
}

#[derive(Default)]
pub(super) struct Delivery {
    artifacts: BTreeMap<String, Artifact>,
    checks: BTreeMap<String, (Vec<u8>, Checks)>,
    runs: BTreeMap<String, (Vec<u8>, Run)>,
}

impl Delivery {
    pub(super) fn record_run(&mut self, target: RunTarget, exit_code: u8, generation: u64) {
        if target.generation == generation {
            self.runs.insert(target.path, (target.bytes, Run { command: target.command, exit_code }));
        }
    }

    pub(super) fn record_checks(&mut self, path: String, bytes: Vec<u8>, checks: Checks) {
        self.checks.insert(path, (bytes, checks));
    }

    pub(super) fn record_check(&mut self, path: String, bytes: Vec<u8>, smoke: bool, passed: bool) {
        let (previous, checks) = self.checks.entry(path).or_insert_with(|| (Vec::new(), Checks::default()));
        if *previous != bytes {
            *previous = bytes;
            *checks = Checks::default();
        }
        checks.record(smoke, passed);
    }

    fn readback(project: &Project<'_>, path: &str) -> Result<(Vec<u8>, u64)> {
        if project.pending.contains_key(path) {
            return Err("file has staged changes; use apply before verifying");
        }
        let mut volume = project.volume.borrow_mut();
        let volume = &mut *volume;
        let generation = volume.disk.verify_file(&volume.files, path)?;
        Ok((volume.read_file(path)?, generation))
    }

    fn receipt(&self, path: &str, artifact: &Artifact, snapshot: u64) -> Receipt {
        let checks = self.checks.get(path)
            .filter(|(bytes, _)| *bytes == artifact.bytes)
            .map_or(Checks::default(), |(_, checks)| *checks);
        Receipt {
            bytes: artifact.bytes.len(), snapshot, checks, launch: artifact.launch.clone(),
            run: self.runs.get(path).filter(|(bytes, _)| *bytes == artifact.bytes)
                .map(|(_, run)| run.clone()),
            interactive: artifact.interactive.then_some(if checks.smoke_passed > 0 && checks.failed == 0 {
                "scripted"
            } else { "pending" }),
        }
    }

    pub(super) fn verify(
        &mut self, project: &Project<'_>, operand: &str, launch: Option<&str>, interactive: bool,
    ) -> Result<ToolReport> {
        if launch.is_some_and(|launch| launch.trim().is_empty() || launch.len() > 256
            || launch.chars().any(char::is_control))
        {
            return Err("launch command must be one printable line of at most 256 bytes");
        }
        let path = project.path(operand)?;
        if self.artifacts.len() == MAX_FILES && !self.artifacts.contains_key(&path) {
            return Err("verify at most four files per turn");
        }
        let (bytes, generation) = Self::readback(project, &path)?;
        let artifact = Artifact { bytes, launch: launch.map(ToString::to_string), interactive };
        let receipt = self.receipt(&path, &artifact, generation);
        let mut report = ToolReport::new(ToolStatus::Verified, receipt.describe(&path));
        self.artifacts.insert(path.clone(), artifact);
        report.path = Some(path);
        report.artifact = Some(receipt);
        Ok(report)
    }

    pub(super) fn finalize(&self, project: &Project<'_>) -> core::result::Result<Vec<(String, Receipt)>, (String, &'static str)> {
        self.artifacts.iter().map(|(path, artifact)| {
            let (current, generation) = Self::readback(project, path).map_err(|error| (path.clone(), error))?;
            if current != artifact.bytes {
                return Err((path.clone(), "artifact changed after verification"));
            }
            Ok((path.clone(), self.receipt(path, artifact, generation)))
        }).collect()
    }
}
