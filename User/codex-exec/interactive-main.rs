#![recursion_limit = "256"]

use clap::{CommandFactory, FromArgMatches, Parser, Subcommand};
use codex_arg0::arg0_dispatch_or_else;
use codex_config::LoaderOverrides;
use codex_exec::{Cli, interactive, run_interactive, run_main};
use codex_utils_cli::CliConfigOverrides;

#[derive(Subcommand)]
enum Command {
    /// Run a single non-interactive task.
    Exec(Cli),
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

fn main() -> anyhow::Result<()> {
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
            Some(Command::Exec(mut cli)) => {
                cli.config_overrides
                    .prepend_root_overrides(args.config_overrides);
                return run_main(cli, paths).await;
            }
            Some(Command::Line(cli)) => cli,
        };
        cli.ephemeral = true;
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
