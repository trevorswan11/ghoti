# Style Guide

Cairn adheres to a somewhat strict style guide to enhance readability.

## Formatting
`clang-format` is used for code formatting all C++ code, while Zig's builtin formatter is used for formatting all Zig code. You must use `clang-format` version 21.1.8 for consistent project-wide formatting

Upon PR creation and workflow approval, GitHub actions will run `zig build fmt-check`, an equally-intensive step that performs a dry run of `clang-format` on the codebase. This can also be invoked on your machine if you are interested in confirming the state of your local version of the codebase.

## Language-Specific Conventions

### C++
#### Naming
- **Files**: `snake_case.cc`, `snake_case.hh`
- **Functions & Variables**: snake_case
- **Types (Classes, Structs, Enums)**: snake_case
- **Type Traits**: snake_case (e.g. stdx::is_box)
- **Concepts & Template Parameters**: PascalCase (e.g., template <typename TValue, Option O, bool Real>)
- **Enumerations, Constants & Macros**: SCREAMING_SNAKE_CASE
- **Private/Internal Members**: Suffix with an underscore (e.g. member_variable_)

#### General
- Use `.hh` for headers and `.cc` for source files
- Avoid `using namespace ...;` in headers
- Prefer enum class over standard enum
- Use the `#pragma once` directive over `#ifdef` include guards
- `#undef` macros in header files after use when possible
- All files should include only what they use and should avoid transitive includes wherever possible
- Almost always use brace initialization unless `=` is required
- Include groups should be newline-separated and should be ordered as:
1. The respective header file for the source file (if in a `.cc` file) in quotes
2. Any standard library includes in angle brackets
3. Any third party library includes in angle brackets
4. Any includes from the current library in quotes
    - This group should include test helpers when applicable
5. Any includes from other internal libraries in angle brackets
    - This group should include config headers when applicable

### Zig
#### Naming
- **Files**: `snake_case.cc`, `snake_case.hh`
- Follow the [Zig Standard Library style](https://ziglang.org/documentation/0.16.0/#Style-Guide)

#### General
- Use PascalCase for functions that return a type
- Use PascalCase for files that should be treated as types

## Commits & PRs
Commits to cairn should use [conventional commits](https://www.conventionalcommits.org/en/v1.0.0/) whenever possible. When possible, try to rebase over merge to prevent messy merge commits from leaking into the commit history.
