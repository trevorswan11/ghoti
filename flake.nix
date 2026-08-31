{
  description = "Ghoti language development.";

  inputs = {
    nixpkgs.url = "github:nixos/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
    zig-flake.url = "github:mitchellh/zig-overlay";
    zls-flake = {
      url = "github:zigtools/zls?ref=0.16.0";
      inputs.nixpkgs.follows = "nixpkgs";
    };
    templ-flake.url = "github:a-h/templ";
    rust-overlay.url = "github:oxalica/rust-overlay";
  };

  outputs =
    {
      nixpkgs,
      flake-utils,
      zig-flake,
      zls-flake,
      templ-flake,
      rust-overlay,
      ...
    }:
    flake-utils.lib.eachDefaultSystem (
      system:
      let
        pkgs = import nixpkgs {
          inherit system;
          templ = templ-flake.packages.${system}.templ;
          overlays = [
            (final: prev: {
              zig = zig-flake.packages.${system}."0.16.0";
              zls = zls-flake.packages.${system}.default;
            })
            rust-overlay.overlays.default
          ];
        };
        rustToolchain = pkgs.rust-bin.stable.latest.minimal.override {
          extensions = [
            "rust-analyzer"
            "clippy"
            "rustfmt"
          ];
          targets = [
            "wasm32-wasip2"
          ];
        };
      in
      with pkgs;
      {
        devShells.default = mkShell {
          buildInputs = [
            zig
            zls
            go
            gopls
            templ
            prettier
            rustToolchain
          ]
          ++ (with llvmPackages_21; [
            clang-tools
            lldb
          ]);

          shellHook = ''
            # Without this, Zig freaks out over unknown flags
            export NIX_CFLAGS_COMPILE=$(echo $NIX_CFLAGS_COMPILE | sed 's/-fmacro-prefix-map=[^ ]*//g')
            export NIX_LDFLAGS=$(echo $NIX_LDFLAGS | sed 's/-fmacro-prefix-map=[^ ]*//g')

            # Required for LLDB on macOS (stinky)
            ${lib.optionalString stdenv.isDarwin ''
              # Tested both paths on my machine and they both work (adds some flexibility)
              if [[ -z "$LLDB_DEBUGSERVER_PATH" ]]; then
                XCODE_PATH="/Applications/Xcode.app/Contents/SharedFrameworks/LLDB.framework/Versions/A/Resources/debugserver"
                CLT_PATH="/Library/Developer/CommandLineTools/Library/PrivateFrameworks/LLDB.framework/Versions/A/Resources/debugserver"

                if [[ -f "$XCODE_PATH" ]]; then
                    export LLDB_DEBUGSERVER_PATH="$XCODE_PATH"
                elif [[ -f "$CLT_PATH" ]]; then
                    export LLDB_DEBUGSERVER_PATH="$CLT_PATH"
                fi
              fi
            ''}
          '';
        };
      }
    );
}
