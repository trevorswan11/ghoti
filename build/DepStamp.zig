//! Zig 0.16 only records a C object's depfile entries in the whole-compilation manifest when that
//! object is recompiled. After an update where an object was a cache hit, its headers are missing
//! from the manifest, so editing only those headers hits the whole cache and nothing rebuilds.
//!
//! The stamp is a generated, empty C++ source whose contents hash every header-like file an
//! artifact can see. Any header edit changes the stamp, which misses the whole manifest, and the
//! per-object manifests (which do record every header) then pick exactly the objects to rebuild.
const std = @import("std");

const header_extensions = [_][]const u8{ ".hh", ".h", ".hpp", ".inc" };

// Every artifact of every packaged target shares these directories, so each is walked once
var dir_digests: std.StringHashMapUnmanaged(u64) = .empty;

/// Hashes the header-like files under each directory, which may be build-root-relative or absolute
pub fn digest(b: *std.Build, dirs: []const []const u8) !u64 {
    var hasher: std.hash.Wyhash = .init(0);
    for (dirs) |dir_path| {
        const entry = try dir_digests.getOrPut(b.allocator, dir_path);
        if (!entry.found_existing) entry.value_ptr.* = try digestDir(b, dir_path);
        hasher.update(dir_path);
        hasher.update(&std.mem.toBytes(entry.value_ptr.*));
    }
    return hasher.final();
}

fn digestDir(b: *std.Build, dir_path: []const u8) !u64 {
    const io = b.graph.io;
    const root = if (std.fs.path.isAbsolute(dir_path)) std.Io.Dir.cwd() else b.build_root.handle;
    var dir = root.openDir(io, dir_path, .{ .iterate = true }) catch |err| switch (err) {
        error.FileNotFound => return 0,
        else => return err,
    };
    defer dir.close(io);

    var paths: std.ArrayList([]const u8) = .empty;
    var walker = try dir.walk(b.allocator);
    defer walker.deinit();
    while (try walker.next(io)) |entry| {
        if (entry.kind != .file or !isHeader(entry.basename)) continue;
        try paths.append(b.allocator, b.dupe(entry.path));
    }

    // Walk order is filesystem-dependent, so sort for a stable digest
    std.mem.sort([]const u8, paths.items, {}, lessThan);

    var hasher: std.hash.Wyhash = .init(0);
    for (paths.items) |path| {
        const contents = try dir.readFileAlloc(io, path, b.allocator, .unlimited);
        defer b.allocator.free(contents);
        hasher.update(path);
        hasher.update(&std.mem.toBytes(contents.len));
        hasher.update(contents);
    }
    return hasher.final();
}

/// Adds the stamp source for `dirs` to the artifact's root module
pub fn add(b: *std.Build, artifact: *std.Build.Step.Compile, dirs: []const []const u8) !void {
    const hash = try digest(b, dirs);
    const files = b.addWriteFiles();
    const stamp = files.add("dep_stamp.cc", b.fmt("// header digest {x:0>16}\n", .{hash}));
    artifact.root_module.addCSourceFile(.{ .file = stamp, .language = .cpp });
}

fn isHeader(basename: []const u8) bool {
    for (header_extensions) |ext| {
        if (std.mem.endsWith(u8, basename, ext)) return true;
    }
    return false;
}

fn lessThan(_: void, lhs: []const u8, rhs: []const u8) bool {
    return std.mem.lessThan(u8, lhs, rhs);
}
