Fixture for `zig build verify-deps`. The runner copies this directory to a scratch location, edits
one dependency at a time, rebuilds, and checks that the program's output follows the edit.
