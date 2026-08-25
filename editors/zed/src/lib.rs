use zed_extension_api as zed;

struct GhotiExtension;

impl zed::Extension for GhotiExtension {
    fn new() -> Self {
        GhotiExtension
    }

    fn language_server_command(
        &mut self,
        _language_server_id: &zed::LanguageServerId,
        worktree: &zed::Worktree,
    ) -> zed::Result<zed::Command> {
        let path = worktree.which("ghoti").ok_or_else(|| {
            "ghoti was not found on PATH. Install ghoti and make sure it's available on your \
             PATH, then reload the language server."
                .to_string()
        })?;

        Ok(zed::Command {
            command: path,
            args: vec!["lsp".to_string()],
            env: worktree.shell_env(),
        })
    }
}

zed::register_extension!(GhotiExtension);
