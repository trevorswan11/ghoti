//! Runner for `zig build verify-deps`. Copies the deptrack fixture to a scratch directory, then edits
//! one dependency at a time and checks that a rebuild picks the edit up. Each scenario runs with and
//! without the header-digest stamp, so the table shows both the upstream behavior and our fix.
//!
//! Usage: verify-deps <zig-exe> <build-root> <fixture-dir> <scratch-dir>
const std = @import("std");
const Io = std.Io;

const Edit = struct {
    kind: []const u8,
    file: []const u8,
    from: []const u8,
    to: []const u8,
    expected: []const u8,
};

// Order matters: the first edit makes the other objects cache hits, which is what drops their
// headers from Zig's whole-compilation manifest.
const edits = [_]Edit{
    .{ .kind = "source (.cc)", .file = "src/plain.cc", .from = "return 1", .to = "return 2", .expected = "1 1 2" },
    .{ .kind = "sibling .inc", .file = "src/table.inc", .from = "{1}", .to = "{2}", .expected = "1 2 2" },
    .{ .kind = "nested header, 2nd root", .file = "include2/dt/deep.hh", .from = "{1}", .to = "{2}", .expected = "2 2 2" },
};

const Context = struct {
    gpa: std.mem.Allocator,
    io: Io,
    zig_exe: []const u8,
    build_root: []const u8,
    scratch: []const u8,
};

pub fn main(init: std.process.Init) !void {
    const arena = init.arena.allocator();
    const args = try init.minimal.args.toSlice(arena);
    if (args.len != 5) std.process.fatal("usage: verify-deps <zig> <build-root> <fixture> <scratch>", .{});

    const ctx: Context = .{
        .gpa = arena,
        .io = init.io,
        .zig_exe = args[1],
        .build_root = args[2],
        .scratch = args[4],
    };
    const fixture = args[3];

    var stdout_buffer: [4096]u8 = undefined;
    var stdout_writer = Io.File.stdout().writer(ctx.io, &stdout_buffer);
    const out = &stdout_writer.interface;

    var results: [2][edits.len + 1]bool = undefined;
    for ([_]bool{ false, true }, 0..) |stamp, mode| {
        results[mode] = try runScenario(ctx, fixture, stamp);
    }

    try out.print("{s:<26} {s:<10} {s:<10}\n", .{ "dependency kind", "no stamp", "stamp" });
    const kinds = [_][]const u8{"initial build"} ++ kindNames();
    for (kinds, 0..) |kind, i| {
        try out.print("{s:<26} {s:<10} {s:<10}\n", .{ kind, verdict(results[0][i]), verdict(results[1][i]) });
    }

    var stamped_ok = true;
    for (results[1]) |ok| stamped_ok = stamped_ok and ok;
    var unstamped_ok = true;
    for (results[0]) |ok| unstamped_ok = unstamped_ok and ok;
    if (!unstamped_ok) try out.writeAll("\nwithout the stamp, zig drops cache-hit headers from the whole manifest\n");
    try out.flush();

    if (!stamped_ok) std.process.fatal("dependency tracking is broken even with the header stamp", .{});
}

fn kindNames() [edits.len][]const u8 {
    var names: [edits.len][]const u8 = undefined;
    for (edits, 0..) |edit, i| names[i] = edit.kind;
    return names;
}

fn verdict(ok: bool) []const u8 {
    return if (ok) "rebuilt" else "STALE";
}

fn runScenario(ctx: Context, fixture: []const u8, stamp: bool) ![edits.len + 1]bool {
    const root = try std.fs.path.join(ctx.gpa, &.{ ctx.scratch, if (stamp) "stamp" else "nostamp" });
    const cwd = Io.Dir.cwd();
    cwd.deleteTree(ctx.io, root) catch {};
    try copyTree(ctx, fixture, root);

    var outcomes: [edits.len + 1]bool = undefined;
    outcomes[0] = std.mem.eql(u8, try buildAndRun(ctx, root, stamp), "1 1 1");
    for (edits, 1..) |edit, i| {
        try replaceInFile(ctx, root, edit.file, edit.from, edit.to);
        outcomes[i] = std.mem.eql(u8, try buildAndRun(ctx, root, stamp), edit.expected);
    }
    return outcomes;
}

fn copyTree(ctx: Context, from: []const u8, to: []const u8) !void {
    const cwd = Io.Dir.cwd();
    var src = try cwd.openDir(ctx.io, from, .{ .iterate = true });
    defer src.close(ctx.io);
    try cwd.createDirPath(ctx.io, to);
    var dest = try cwd.openDir(ctx.io, to, .{});
    defer dest.close(ctx.io);

    var walker = try src.walk(ctx.gpa);
    defer walker.deinit();
    while (try walker.next(ctx.io)) |entry| {
        switch (entry.kind) {
            .directory => try dest.createDirPath(ctx.io, entry.path),
            .file => try src.copyFile(entry.path, dest, entry.path, ctx.io, .{}),
            else => {},
        }
    }
}

fn replaceInFile(ctx: Context, root: []const u8, sub_path: []const u8, from: []const u8, to: []const u8) !void {
    const path = try std.fs.path.join(ctx.gpa, &.{ root, sub_path });
    const cwd = Io.Dir.cwd();
    const contents = try cwd.readFileAlloc(ctx.io, path, ctx.gpa, .unlimited);
    const replaced = try std.mem.replaceOwned(u8, ctx.gpa, contents, from, to);
    if (std.mem.eql(u8, contents, replaced)) std.process.fatal("'{s}' not found in {s}", .{ from, path });
    try cwd.writeFile(ctx.io, .{ .sub_path = path, .data = replaced });
}

fn buildAndRun(ctx: Context, root: []const u8, stamp: bool) ![]const u8 {
    const prefix = try std.fs.path.join(ctx.gpa, &.{ root, "out" });
    const build = try std.process.run(ctx.gpa, ctx.io, .{
        .argv = &.{
            ctx.zig_exe,
            "build",
            "deptrack-fixture",
            try std.fmt.allocPrint(ctx.gpa, "-Ddeptrack-root={s}", .{root}),
            if (stamp) "-Ddeptrack-stamp=true" else "-Ddeptrack-stamp=false",
            "--prefix",
            prefix,
        },
        .cwd = .{ .path = ctx.build_root },
    });
    if (build.term != .exited or build.term.exited != 0) {
        std.process.fatal("fixture build failed:\n{s}", .{build.stderr});
    }

    const exe_name = if (@import("builtin").os.tag == .windows) "deptrack.exe" else "deptrack";
    const exe = try std.fs.path.join(ctx.gpa, &.{ prefix, "bin", exe_name });
    const run = try std.process.run(ctx.gpa, ctx.io, .{ .argv = &.{exe} });
    if (run.term != .exited or run.term.exited != 0) std.process.fatal("fixture program failed", .{});
    return std.mem.trim(u8, run.stdout, " \r\n");
}
