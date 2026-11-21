// SPDX-License-Identifier: GPL-3.0-only
use slint_demo::{Settings, run};

fn main() {
    let result = run(
        |closed| {
            let settings = Settings::new().map_err(|error| format!("{error:?}"))?;
            settings.on_close_requested(move || closed.set(true));
            Ok(settings)
        },
        |_, _| false,
        false,
    );
    match result {
        Ok(stats) if std::env::args().any(|arg| arg == "--stats") => println!(
            "slint frames: full={} partial={} largest_partial={}",
            stats.full_frames, stats.partial_frames, stats.largest_partial
        ),
        Ok(_) => (),
        Err(error) => {
            eprintln!("slint-demo: {error}");
            std::process::exit(1);
        }
    }
}
