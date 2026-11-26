#![recursion_limit = "256"]

use clap::{CommandFactory, FromArgMatches, Parser, Subcommand};
use codex_arg0::arg0_dispatch_or_else;
use codex_config::LoaderOverrides;
use codex_exec::{Cli, interactive, run_interactive, run_main};
use codex_utils_cli::CliConfigOverrides;

#[derive(Parser)]
struct ResumeArgs {
    /// Session id (UUID) or session name to resume.
    #[arg(value_name = "SESSION_ID")]
    session_id: Option<String>,
    /// Continue the most recent session without showing the picker.
    #[arg(long = "last", default_value_t = false)]
    last: bool,
    /// Show all sessions across working directories.
    #[arg(long = "all", default_value_t = false)]
    all: bool,
    /// Include non-interactive sessions in resume listings.
    #[arg(long = "include-non-interactive", default_value_t = false)]
    include_non_interactive: bool,
    #[command(flatten)]
    tui: codex_tui::Cli,
}

#[derive(Parser)]
struct ForkArgs {
    /// Session id (UUID) to fork.
    #[arg(value_name = "SESSION_ID")]
    session_id: Option<String>,
    /// Fork the most recent session without showing the picker.
    #[arg(long = "last", default_value_t = false)]
    last: bool,
    /// Show all sessions across working directories.
    #[arg(long = "all", default_value_t = false)]
    all: bool,
    #[command(flatten)]
    tui: codex_tui::Cli,
}

#[derive(Subcommand)]
enum Command {
    /// Run a single non-interactive task.
    Exec(Cli),
    /// Resume a previous interactive session.
    Resume(ResumeArgs),
    /// Fork a previous interactive session.
    Fork(ForkArgs),
    /// Chat using a simple line prompt.
    Line(Cli),
}

#[derive(Parser)]
#[command(
    name = "codex",
    version,
    about = "Chat with Codex in the LixinOS terminal"
)]
struct CommandLine {
    #[command(flatten)]
    config_overrides: CliConfigOverrides,
    #[command(subcommand)]
    command: Option<Command>,
    #[command(flatten)]
    tui: codex_tui::Cli,
}

fn ensure_guest_defaults() {
    // Dynamic PIE binaries run directly via ld-musl without loader.c defaults.
    // Set defaults single-threaded before Tokio spawns worker threads.
    for (key, value) in [
        ("HOME", "/home/lixin"),
        ("PATH", "/bin"),
        ("SHELL", "/bin/bash"),
        ("TMPDIR", "/tmp"),
        ("SSL_CERT_FILE", "/lib/cpython-3.14.5/roots.pem"),
    ] {
        if std::env::var_os(key).is_none() {
            unsafe { std::env::set_var(key, value) };
        }
    }
}

fn main() -> anyhow::Result<()> {
    ensure_guest_defaults();
    // Fast-path --version / -V before initializing Tokio worker threads.
    let raw_args: Vec<String> = std::env::args().collect();
    if raw_args.len() == 2 && matches!(raw_args[1].as_str(), "--version" | "-V") {
        print!("{}", CommandLine::command().render_version());
        return Ok(());
    }
    arg0_dispatch_or_else(|paths| async move {
        let command = CommandLine::command();
        let matches = command.clone().get_matches();
        if matches.subcommand_name().is_some() {
            for option in command.get_arguments().filter(|arg| !arg.is_global_set()) {
                let id = option.get_id().as_str();
                anyhow::ensure!(
                    matches.value_source(id) != Some(clap::parser::ValueSource::CommandLine),
                    "put the {id} option after the subcommand"
                );
            }
        }
        let args = CommandLine::from_arg_matches(&matches)?;
        let mut cli = match args.command {
            None => {
                let mut tui = args.tui;
                tui.config_overrides = args.config_overrides;
                tui.no_daemon = true;
                let exit =
                    codex_tui::run_main(tui, paths, LoaderOverrides::default(), None).await?;
                if let codex_tui::ExitReason::Fatal(message) = exit.exit_reason {
                    anyhow::bail!("{message}");
                }
                return Ok(());
            }
            Some(Command::Resume(resume)) => {
                let mut tui = resume.tui;
                tui.config_overrides = args.config_overrides;
                tui.no_daemon = true;
                tui.resume_last = resume.last;
                tui.resume_session_id = resume.session_id;
                tui.resume_show_all = resume.all;
                tui.resume_include_non_interactive = resume.include_non_interactive;
                tui.resume_picker = !tui.resume_last && tui.resume_session_id.is_none();
                let exit =
                    codex_tui::run_main(tui, paths, LoaderOverrides::default(), None).await?;
                if let codex_tui::ExitReason::Fatal(message) = exit.exit_reason {
                    anyhow::bail!("{message}");
                }
                return Ok(());
            }
            Some(Command::Fork(fork)) => {
                let mut tui = fork.tui;
                tui.config_overrides = args.config_overrides;
                tui.no_daemon = true;
                tui.fork_last = fork.last;
                tui.fork_session_id = fork.session_id;
                tui.fork_show_all = fork.all;
                tui.fork_picker = !tui.fork_last && tui.fork_session_id.is_none();
                let exit =
                    codex_tui::run_main(tui, paths, LoaderOverrides::default(), None).await?;
                if let codex_tui::ExitReason::Fatal(message) = exit.exit_reason {
                    anyhow::bail!("{message}");
                }
                return Ok(());
            }
            Some(Command::Exec(mut cli)) => {
                cli.config_overrides
                    .prepend_root_overrides(args.config_overrides);
                return run_main(cli, paths).await;
            }
            Some(Command::Line(cli)) => cli,
        };
        cli.skip_git_repo_check = true;
        cli.config_overrides
            .prepend_root_overrides(args.config_overrides);
        let mut ui = interactive::Console::open()?;
        cli.prompt = match cli.prompt.take() {
            Some(text) if !text.trim().is_empty() => Some(text),
            _ => ui.prompt().await?,
        };
        if cli.prompt.is_some() {
            run_interactive(cli, paths, ui).await?;
        }
        Ok(())
    })
}
