<h1 align="center">ghoti</h1>

<p align="center">
<img src="https://img.shields.io/badge/C%2B%2B-23-blue?logo=c%2B%2B&logoColor=white" alt="C++23" /> <img src="https://img.shields.io/badge/Zig-0.16.0-orange?logo=zig" alt="Zig 0.16.0" /> <a href="LICENSE"><img src="https://img.shields.io/github/license/trevorswan11/ghoti" alt="License" /></a>
</p>

<p align="center">
A compiled systems language combining modern, dual-mode polymorphism with transparent low-level control
<br/>
<a href="https://github.com/trevorswan11/ghoti/issues/new?labels=bug&template=bug-report.md">Report Bug</a>
&middot;
<a href="https://github.com/trevorswan11/ghoti/issues/new?labels=enhancement&template=feature-request.md">Request Feature</a>
</p>

## About the Project

Ghoti is a compiled systems language powered by LLVM, C++, and Zig. It attempts to combine select features from its more popular predecessors (i.e. Zig, Rust, C, C++) into a performant low-level language.

### Why Ghoti?

- Low-level with transparent memory layout
- Manually manged memory
- Immutability by default
- Compile-time evaluation: Sophisticated `constexpr` evaluation with first-class types
- Clear Semantics: No overloading, variable shadowing, or macros
- Flexible polymorphism: Choose between zero-cost static dispatch via `impl` monomorphization or explicit dynamic dispatch using fat-pointer `dyn`

### What's With the Name?

The name 'ghoti' was largely inspired by VSauce's short about [forbidden spellings](https://youtube.com/shorts/3ipFdRfFvK4?si=0cdgxtmpbaFZtFHM). With the help of some abuse of the English language, **'ghoti'** is pronounced **'fish'**:
- gh pronounced as the /f/ in enough
- o pronounced as the /ɪ/ in women
- ti pronounced as the /ʃ/ in nation

### Built With Zig!

Zig is used as the primary orchestrator for all things ghoti. Ghoti uses Zig's `build.zig` to provide a hermetic build. Necessary dependencies are automatically fetched and all required dependencies are built from source. This unified build system manages LLVM compilation (including tools like clang-format), kcov coverage reporting (on supported platforms), and core maintainer tools such as a custom archiver for releases. Ghoti aims to be reproducible anywhere that has a valid and correctly versioned Zig. **No manual linking or hoop-jumping is required to build ghoti, ever, on any platform**.

<details>
<summary><b>Full dependency breakdown</b></summary>

The following are "standalone" dependencies, required and manually fetched by ghoti's build system.
1. [stdx](https://github.com/trevorswan11/stdx.git) is a C++ standard library and zig build system extension library that drives multiple dependencies. A full breakdown of dependencies can be found at the library's github repository. All dependencies transitively brought in by this library are open source and those and linked to `ghoti` artifacts  are permissively licensed 
2. [CLI11](https://github.com/CLIUtils/CLI11) is a command line parser for C++ that provides a rich feature set with a simple and intuitive interface. Is is licensed under the permissive 3-Clause BSD License.
3. [replxx](https://github.com/AmokHuginnsson/replxx) is a read evaluate print loop (REPL) library that provides a cross platform interactive shell that powers terminal debugging support. Is is licensed under the permissive BSD License.
4. [LLVM 21.1.8](https://releases.llvm.org/21.1.0/docs/ReleaseNotes.html) is used as ghoti's compilation backend. It is manually compiled and statically linked against ghoti through the build system. It is licensed under the permissive Apache License 2.0, and has the following dependencies:
    - [libxml2](https://gitlab.gnome.org/GNOME/libxml2), licensed under the MIT License
    - [zlib](https://github.com/madler/zlib), licensed under the MIT License
    - [zstd](https://github.com/facebook/zstd), licensed under the BSD License

Many build functions heavily reference [allyourcodebase](https://github.com/allyourcodebase)'s implementations. Links to specific repositories can be found as a documentation comment above respective `build` functions.

</details>

### Core Principles
- Learn for the sake of learning
- Experiment freely
- KISS & DRY

### Hello, World!
```ghoti
import std;

pub const main := fn(args: [][:0]u8): void {
    const message := "Hello, world!";
    _ = std::io::println(message);
};
```

## Getting Started
### For Nix Users
This is by far the easiest way to get started with development. Just run `nix develop` to get started and automatically get the correct Zig and Go versions as well as some other important development tools. Note that this provides optional preconfigured tools such as LLDB, Clangd, and ZLS to further enhance the developer experience.

### For Others
All you need to get started with ghoti development is git and a valid 0.16.0 Zig installation, which can be found [here](https://ziglang.org/download/).

In either case, assuming you have the git and Zig prerequisites on your system, building ghoti is as easy as running:
```sh
git clone https://github.com/trevorswan11/ghoti
cd ghoti
zig build --release
```

## Language Website
The language's website is written with [Go](https://go.dev/), [HTMX](https://htmx.org/), and [templ](https://github.com/a-h/templ). To build the website, you'll need [go1.26.3](https://go.dev/dl/) on top of the aforementioned Zig version. Once these dependencies are installed, you simply have to run `zig build site`, producing a binary in the `zig-out` directory. 

### Development
[air](https://github.com/air-verse/air) is used for live reloading of the site during development. It and the aforementioned `templ` dependency are build from source, though they both bring in some transitive dependencies that are managed by Go. It is not expected that you have either of these tools installed to work on this project, though you may find it useful to provide `templ` to your editor in such a way that you can take advantage of its bundled LSP.

## Roadmap

- [x] Lexical analysis
- [x] Pratt parsing
    - [x] Syntax documentation
- [x] Multi-pass Semantic Analysis (to support order independent declarations)
    - [x] Symbol registration pass
    - [x] Type resolution pass
    - [x] GIR Emission & Constant Evaluation
    - [x] Type checking pass
- [ ] LLVM Integration
    - [ ] Build system integration
        - [x] Compilation rules for Clang, LLD, and LLVM
        - [x] In-house clang-format
        - [x] Kaleidoscope examples
        - [ ] Test parity through the build system
    - [x] Compiler backend integration
- [x] Tooling (available through subcommands)
    - [x] LSP
    - [x] Formatter
- [ ] Standard library
    - [ ] Cross-platform support w/o forcing libc
    - [ ] Generic data structures
    - [ ] Generic algorithms

See the [open issues](https://github.com/trevorswan11/ghoti/issues) for a full list of proposed features (and known issues).

## Contributing

Contributions are what make the open source community such an amazing place to learn, inspire, and create. Any contributions you make are greatly appreciated.

If you have a suggestion that would make this better, please fork the repo and create a pull request. You can also simply open an issue with the tag "enhancement". Don't forget to give the project a star! Thanks again!

1. Fork the Project
2. Create your Feature Branch (`git checkout -b feat/AmazingFeature`)
3. Commit your Changes (`git commit -m '[feat]: Add some AmazingFeature'`)
4. Push to the Branch (`git push origin feat/AmazingFeature`)
5. Open a Pull Request against `dev`

### Top contributors:

<a href="https://github.com/trevorswan11/ghoti/graphs/contributors">
  <img src="https://contrib.rocks/image?repo=trevorswan11/ghoti" alt="contrib.rocks image" />
</a>

## License

Distributed under the MIT License. See `LICENSE` for more information.

## Contact

[![LinkedIn](https://img.shields.io/badge/linkedin-%230077B5.svg?style=for-the-badge&logo=linkedin&logoColor=white)](https://www.linkedin.com/in/trevorswan11/) [![Gmail](https://img.shields.io/badge/Gmail-D14836?style=for-the-badge&logo=gmail&logoColor=white)](mailto:trevor.swan@case.edu)

Project Link: [https://github.com/trevorswan11/ghoti](https://github.com/trevorswan11/ghoti)

## Acknowledgments

- [Zig](https://ziglang.org/)'s community, language features, and compiler source code
- [Rust](https://rust-lang.org/)'s language features and philosophy
- [cppreference](https://www.cppreference.com/)'s extensive C++ language documentation
- [Thorsten Ball](https://thorstenball.com/)'s "Writing an Interpreter/Compiler in Go" two-book series
