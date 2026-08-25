# ghoti-zed-extension

A [Zed](https://zed.dev) editor extension for the [ghoti](https://github.com/trevorswan11/ghoti) programming language: syntax highlighting (via tree-sitter) plus wiring for `ghoti lsp` (`ghoti` must be in path).

## Building

```sh
rustup target add wasm32-wasip2
cargo build --target wasm32-wasip2 --release
```

## Installing locally

In Zed: `zed: install dev extension` from the command palette, then select this directory (`ghoti-zed-extension/`).
