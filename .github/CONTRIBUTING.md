# Contributing

Thanks for considering to write a Pull Request (PR) for ghoti! Here are a few guidelines to get you started:

Make sure you are comfortable with the license; all contributions are licensed under the original MIT license.

## Prerequisites & Dependencies

The developer experience has been set up in such a way that the only system dependencies are git and [Zig v0.16.0](https://ziglang.org/download/). This standard should be upheld in all PRs, adding all new dependencies to the project `build.zig.zon` and adding any build steps to the `third-party` directory.

## Pull Request Requirements

To facilitate a smooth review process, all PRs should:

- **Target the `main` branch**: Active development occurs here; releases are marked by `vX.Y.Z` tags.
- **Include documentation**: Explain changes in the PR description or link to a relevant issue.
- **Be self-contained**: Do not impact unrelated subsystems.
- **Include tests**: Ensure changes are covered by unit tests in the relevant libraries.
- **Pass static analysis**: Run `zig build check` before submitting.
- **Be stylistically consistent**: All code must adhere to the [style guide](.github/STYLE_GUIDE.md)

It is recommended that you make these changes on a branch off `main` formatted as `fix/XXX` or `feat/XXX` for bugs and features, respectively, where XXX is the relevant issue number or description.

If you're looking to make a major change that isn't listed in the issue tracker, please open one with the relevant tag (i.e. enhancement or proposal) so we can discuss!

## For maintainers: Making a release

- Update changelog if needed
- Make a release's artifacts by pushing a tag formatted as `vX.Y.Z` on the release commit in `main`
- Make a release in the GitHub UI, use a name such as "vX.Y.Z: Title"
    - Note that releases and their tags are immutable, so the tag is the release's permanent reference
- Versioning should conform to semantic versioning

You can test the release build process on your local machine by running `zig build package`.
