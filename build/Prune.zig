//! Runner for `zig build prune`. Reclaims `.zig-cache` and `zig-out` space without ever forcing an
//! LLVM rebuild.
//!
//! Every `o/<digest>` a build can still hit is named by some `h/<name>.txt` manifest: the digest is
//! `SipHash128(init, name ++ each file digest in the manifest)`, the same computation as
//! `std.Build.Cache.Manifest.final`. So an `o/` entry no manifest names is a superseded generation
//! that nothing can reach again, and a referenced entry is deleted together with its manifest.
//!
//! Usage: prune <cache-root> <install-prefix> <keep-list> <project-root> <llvm-root> [options]
//!   --dry-run          print what would be deleted
//!   --keep N           referenced generations kept per linked artifact name (default 2)
//!   --age-days N       age for tmp/, cppcheck/ and args/ entries (default 14)
//!   --prune-llvm       apply the generation limit to LLVM/LLD/Clang entries too
const std = @import("std");
const Io = std.Io;
const Cache = std.Build.Cache;

pub const Options = struct {
    dry_run: bool = false,
    keep: usize = 2,
    age_days: u32 = 14,
    prune_llvm: bool = false,
    // In-flight builds create `o/` before writing the manifest that names it
    grace_seconds: i64 = 60 * 60,
};

pub const Kind = enum {
    linked,
    object,
    other,

    fn label(kind: Kind) []const u8 {
        return switch (kind) {
            .linked => "linked artifacts",
            .object => "C/C++ objects",
            .other => "generated files & other",
        };
    }
};

pub const Entry = struct {
    digest: []const u8,
    size: u64,
    mtime: i64,
    kind: Kind,
    // The main output's file name for linked artifacts and objects
    name: []const u8,
    manifests: std.ArrayList([]const u8) = .empty,
    protected: bool = false,
};

pub const Manifest = struct {
    name: []const u8,
    digest: [32]u8,
    mtime: i64,
    protected: bool,
    // `o/` entries this manifest reads, which inherit its protection (zlib, tblgen output, ...)
    inputs: []const [32]u8 = &.{},
};

pub const Reason = enum {
    superseded,
    old_generation,
    orphan_manifest,
    aged_out,
    not_installed,
    stale_cdb_fragment,

    fn label(reason: Reason) []const u8 {
        return switch (reason) {
            .superseded => "superseded generations (no manifest)",
            .old_generation => "older generations of linked artifacts",
            .orphan_manifest => "manifests without output",
            .aged_out => "aged-out tmp/cppcheck/args entries",
            .not_installed => "zig-out files outside the install graph",
            .stale_cdb_fragment => "cdb fragments for deleted sources",
        };
    }
};

pub const Deletion = struct {
    // Relative to the cache root, or to the install prefix for `not_installed`
    path: []const u8,
    bytes: u64,
    reason: Reason,
    is_dir: bool,
    // Manifests that must be removed first so the build can't hit a missing output
    manifests: []const []const u8 = &.{},
    // What the entry held, for the listing
    label: []const u8 = "",
};

/// Computes the `o/` digest a manifest resolves to on a cache hit
pub fn outputDigest(manifest_name: []const u8, contents: []const u8) ?[32]u8 {
    if (manifest_name.len != 32) return null;
    var name_bin: [16]u8 = undefined;
    _ = std.fmt.hexToBytes(&name_bin, manifest_name) catch return null;

    var hasher = Cache.hasher_init;
    hasher.update(&name_bin);
    var lines = std.mem.splitScalar(u8, contents, '\n');
    _ = lines.next() orelse return null;
    while (lines.next()) |line| {
        const fields = parseLine(line) orelse continue;
        var file_bin: [16]u8 = undefined;
        _ = std.fmt.hexToBytes(&file_bin, fields.digest) catch return null;
        hasher.update(&file_bin);
    }
    var out: [16]u8 = undefined;
    hasher.final(&out);
    return std.fmt.bytesToHex(out, .lower);
}

const Line = struct { digest: []const u8, path: []const u8 };

fn parseLine(line: []const u8) ?Line {
    // size inode mtime digest prefix path, where the path may contain spaces
    var rest = std.mem.trimEnd(u8, line, "\r");
    var fields: [5][]const u8 = undefined;
    for (&fields) |*field| {
        const space = std.mem.indexOfScalar(u8, rest, ' ') orelse return null;
        field.* = rest[0..space];
        rest = rest[space + 1 ..];
    }
    if (fields[3].len != 32) return null;
    return .{ .digest = fields[3], .path = rest };
}

/// LLVM-only inputs mark an entry as expensive to rebuild. Our own objects include LLVM headers,
/// so anything with a project input is never protected.
pub fn isProtected(contents: []const u8, project_root: []const u8, llvm_root: []const u8) bool {
    var saw_llvm = false;
    var lines = std.mem.splitScalar(u8, contents, '\n');
    _ = lines.next();
    while (lines.next()) |line| {
        const fields = parseLine(line) orelse continue;
        if (pathWithin(fields.path, project_root) and !pathWithin(fields.path, llvm_root)) return false;
        if (pathWithin(fields.path, llvm_root)) saw_llvm = true;
    }
    return saw_llvm;
}

/// The `o/<digest>` directories a manifest's inputs live in
pub fn cacheInputs(gpa: std.mem.Allocator, contents: []const u8) ![]const [32]u8 {
    var inputs: std.ArrayList([32]u8) = .empty;
    var lines = std.mem.splitScalar(u8, contents, '\n');
    _ = lines.next();
    while (lines.next()) |line| {
        const fields = parseLine(line) orelse continue;
        if (cacheDigestIn(fields.path)) |digest| try inputs.append(gpa, digest);
    }
    return inputs.items;
}

fn cacheDigestIn(path: []const u8) ?[32]u8 {
    const len = 32;
    var i: usize = 0;
    while (i + 2 + len <= path.len) : (i += 1) {
        const at_segment = i == 0 or isSeparator(path[i - 1]);
        if (!at_segment or path[i] != 'o' or !isSeparator(path[i + 1])) continue;
        const digest = path[i + 2 ..][0..len];
        const ends = i + 2 + len == path.len or isSeparator(path[i + 2 + len]);
        const hex = for (digest) |c| {
            if (!std.ascii.isHex(c)) break false;
        } else true;
        if (ends and hex) return digest.*;
    }
    return null;
}

fn isSeparator(c: u8) bool {
    return c == '/' or c == '\\';
}

fn pathWithin(path: []const u8, root: []const u8) bool {
    if (root.len == 0 or path.len < root.len) return false;
    for (path[0..root.len], root) |p, r| {
        if (normalize(p) != normalize(r)) return false;
    }
    return path.len == root.len or root[root.len - 1] == '/' or root[root.len - 1] == '\\' or
        path[root.len] == '/' or path[root.len] == '\\';
}

fn normalize(c: u8) u8 {
    return if (c == '\\') '/' else std.ascii.toLower(c);
}

pub fn classify(file_names: []const []const u8) struct { kind: Kind, name: []const u8 } {
    const linked_primary = [_][]const u8{ ".exe", ".dll", ".so", ".dylib", ".wasm" };
    const linked_secondary = [_][]const u8{ ".lib", ".a" };
    for (file_names) |name| for (linked_primary) |ext| {
        if (std.mem.endsWith(u8, name, ext)) return .{ .kind = .linked, .name = name };
    };
    for (file_names) |name| for (linked_secondary) |ext| {
        if (std.mem.endsWith(u8, name, ext)) return .{ .kind = .linked, .name = name };
    };
    if (file_names.len >= 1) {
        for (file_names) |name| {
            if (std.mem.endsWith(u8, name, ".o") or std.mem.endsWith(u8, name, ".obj")) {
                return .{ .kind = .object, .name = name };
            }
        }
    }
    return .{ .kind = .other, .name = if (file_names.len > 0) file_names[0] else "" };
}

/// Decides which cache entries go. Pure so the policy is testable on synthetic data.
pub fn planCache(
    gpa: std.mem.Allocator,
    entries: []Entry,
    manifests: []const Manifest,
    now: i64,
    options: Options,
) ![]Deletion {
    var deletions: std.ArrayList(Deletion) = .empty;

    var by_digest: std.StringHashMapUnmanaged(*Entry) = .empty;
    for (entries) |*entry| try by_digest.put(gpa, entry.digest, entry);
    for (manifests) |*manifest| {
        if (by_digest.get(&manifest.digest)) |entry| {
            try entry.manifests.append(gpa, manifest.name);
            entry.protected = entry.protected or manifest.protected;
            if (manifest.protected) for (manifest.inputs) |*input| {
                if (by_digest.get(input)) |used| used.protected = true;
            };
        } else if (now - manifest.mtime > options.grace_seconds) {
            try deletions.append(gpa, .{
                .path = try std.fmt.allocPrint(gpa, "h/{s}.txt", .{manifest.name}),
                .bytes = 0,
                .reason = .orphan_manifest,
                .is_dir = false,
            });
        }
    }

    // Referenced linked artifacts, grouped by name, newest first
    var groups: std.StringArrayHashMapUnmanaged(std.ArrayList(*Entry)) = .empty;
    for (entries) |*entry| {
        const recent = now - entry.mtime <= options.grace_seconds;
        if (entry.manifests.items.len == 0) {
            if (!recent) try deletions.append(gpa, .{
                .path = try std.fmt.allocPrint(gpa, "o/{s}", .{entry.digest}),
                .bytes = entry.size,
                .reason = .superseded,
                .is_dir = true,
            });
            continue;
        }
        if (entry.kind != .linked or (entry.protected and !options.prune_llvm)) continue;
        const group = try groups.getOrPut(gpa, entry.name);
        if (!group.found_existing) group.value_ptr.* = .empty;
        try group.value_ptr.append(gpa, entry);
    }

    for (groups.values()) |*group| {
        std.mem.sort(*Entry, group.items, {}, newerFirst);
        if (group.items.len <= options.keep) continue;
        for (group.items[options.keep..]) |entry| {
            if (now - entry.mtime <= options.grace_seconds) continue;
            try deletions.append(gpa, .{
                .path = try std.fmt.allocPrint(gpa, "o/{s}", .{entry.digest}),
                .bytes = entry.size,
                .reason = .old_generation,
                .is_dir = true,
                .manifests = entry.manifests.items,
                .label = entry.name,
            });
        }
    }
    return deletions.items;
}

fn newerFirst(_: void, lhs: *Entry, rhs: *Entry) bool {
    return lhs.mtime > rhs.mtime;
}

const Context = struct {
    gpa: std.mem.Allocator,
    io: Io,
    cache: Io.Dir,
    now: i64,
    options: Options,
    out: *Io.Writer,
};

pub fn main(init: std.process.Init) !void {
    const gpa = init.arena.allocator();
    const io = init.io;
    const args = try init.minimal.args.toSlice(gpa);
    if (args.len < 6) {
        std.process.fatal("usage: prune <cache-root> <install-prefix> <keep-list> <project-root> <llvm-root> [options]", .{});
    }

    var options: Options = .{};
    var i: usize = 6;
    while (i < args.len) : (i += 1) {
        const arg = args[i];
        if (std.mem.eql(u8, arg, "--dry-run")) {
            options.dry_run = true;
        } else if (std.mem.eql(u8, arg, "--prune-llvm")) {
            options.prune_llvm = true;
        } else if (std.mem.eql(u8, arg, "--keep") and i + 1 < args.len) {
            i += 1;
            options.keep = try std.fmt.parseInt(usize, args[i], 10);
        } else if (std.mem.eql(u8, arg, "--age-days") and i + 1 < args.len) {
            i += 1;
            options.age_days = try std.fmt.parseInt(u32, args[i], 10);
        } else {
            std.process.fatal("unknown argument '{s}'", .{arg});
        }
    }

    var stdout_buffer: [8192]u8 = undefined;
    var stdout_writer = Io.File.stdout().writer(io, &stdout_buffer);
    const cwd = Io.Dir.cwd();
    var cache = try cwd.openDir(io, args[1], .{ .iterate = true });
    defer cache.close(io);

    const ctx: Context = .{
        .gpa = gpa,
        .io = io,
        .cache = cache,
        .now = Io.Clock.real.now(io).toSeconds(),
        .options = options,
        .out = &stdout_writer.interface,
    };

    const manifests = try loadManifests(ctx, args[4], args[5]);
    const entries = try loadEntries(ctx);
    try report(ctx, entries, manifests);

    var deletions: std.ArrayList(Deletion) = .empty;
    try deletions.appendSlice(gpa, try planCache(gpa, entries, manifests, ctx.now, options));
    try planAged(ctx, &deletions, "tmp");
    try planAged(ctx, &deletions, "cppcheck");
    try planAged(ctx, &deletions, "args");
    try planCdbFragments(ctx, &deletions);
    const install_deletions = try planInstall(ctx, args[2], args[3]);

    try execute(ctx, cache, deletions.items);
    var prefix = cwd.openDir(io, args[2], .{}) catch null;
    if (prefix) |*dir| {
        defer dir.close(io);
        try execute(ctx, dir.*, install_deletions);
    }

    try summarize(ctx, deletions.items, install_deletions);
    try ctx.out.flush();
}

fn loadManifests(ctx: Context, project_root: []const u8, llvm_root: []const u8) ![]Manifest {
    var manifests: std.ArrayList(Manifest) = .empty;
    var dir = ctx.cache.openDir(ctx.io, "h", .{ .iterate = true }) catch return manifests.items;
    defer dir.close(ctx.io);
    var it = dir.iterate();
    while (try it.next(ctx.io)) |entry| {
        if (entry.kind != .file or !std.mem.endsWith(u8, entry.name, ".txt")) continue;
        const name = entry.name[0 .. entry.name.len - ".txt".len];
        const contents = dir.readFileAlloc(ctx.io, entry.name, ctx.gpa, .unlimited) catch continue;
        const digest = outputDigest(name, contents) orelse continue;
        const stat = try dir.statFile(ctx.io, entry.name, .{});
        try manifests.append(ctx.gpa, .{
            .name = try ctx.gpa.dupe(u8, name),
            .digest = digest,
            .mtime = stat.mtime.toSeconds(),
            .protected = isProtected(contents, project_root, llvm_root),
            .inputs = try cacheInputs(ctx.gpa, contents),
        });
        ctx.gpa.free(contents);
    }
    return manifests.items;
}

fn loadEntries(ctx: Context) ![]Entry {
    var entries: std.ArrayList(Entry) = .empty;
    var dir = ctx.cache.openDir(ctx.io, "o", .{ .iterate = true }) catch return entries.items;
    defer dir.close(ctx.io);
    var it = dir.iterate();
    while (try it.next(ctx.io)) |entry| {
        if (entry.kind != .directory) continue;
        var sub = try dir.openDir(ctx.io, entry.name, .{ .iterate = true });
        defer sub.close(ctx.io);

        var top_names: std.ArrayList([]const u8) = .empty;
        var top = sub.iterate();
        while (try top.next(ctx.io)) |child| {
            if (child.kind == .file) try top_names.append(ctx.gpa, try ctx.gpa.dupe(u8, child.name));
        }

        var size: u64 = 0;
        var mtime: i64 = (try dir.statFile(ctx.io, entry.name, .{})).mtime.toSeconds();
        var walker = try sub.walk(ctx.gpa);
        defer walker.deinit();
        while (try walker.next(ctx.io)) |child| {
            if (child.kind != .file) continue;
            const stat = sub.statFile(ctx.io, child.path, .{}) catch continue;
            size += stat.size;
            mtime = @max(mtime, stat.mtime.toSeconds());
        }

        const class = classify(top_names.items);
        try entries.append(ctx.gpa, .{
            .digest = try ctx.gpa.dupe(u8, entry.name),
            .size = size,
            .mtime = mtime,
            .kind = class.kind,
            .name = class.name,
        });
    }
    return entries.items;
}

fn planAged(ctx: Context, deletions: *std.ArrayList(Deletion), sub_path: []const u8) !void {
    var dir = ctx.cache.openDir(ctx.io, sub_path, .{ .iterate = true }) catch return;
    defer dir.close(ctx.io);
    const cutoff = @as(i64, ctx.options.age_days) * std.time.s_per_day;
    var it = dir.iterate();
    while (try it.next(ctx.io)) |entry| {
        const stat = dir.statFile(ctx.io, entry.name, .{}) catch continue;
        if (ctx.now - stat.mtime.toSeconds() <= cutoff) continue;
        try deletions.append(ctx.gpa, .{
            .path = try std.fs.path.join(ctx.gpa, &.{ sub_path, entry.name }),
            .bytes = if (entry.kind == .directory) try treeSize(ctx, dir, entry.name) else stat.size,
            .reason = .aged_out,
            .is_dir = entry.kind == .directory,
        });
    }
}

fn treeSize(ctx: Context, parent: Io.Dir, sub_path: []const u8) !u64 {
    var dir = try parent.openDir(ctx.io, sub_path, .{ .iterate = true });
    defer dir.close(ctx.io);
    var walker = try dir.walk(ctx.gpa);
    defer walker.deinit();
    var size: u64 = 0;
    while (try walker.next(ctx.io)) |child| {
        if (child.kind != .file) continue;
        const stat = dir.statFile(ctx.io, child.path, .{}) catch continue;
        size += stat.size;
    }
    return size;
}

fn planCdbFragments(ctx: Context, deletions: *std.ArrayList(Deletion)) !void {
    var dir = ctx.cache.openDir(ctx.io, "cdb-frags", .{ .iterate = true }) catch return;
    defer dir.close(ctx.io);
    var it = dir.iterate();
    while (try it.next(ctx.io)) |entry| {
        if (entry.kind != .file) continue;
        const contents = dir.readFileAlloc(ctx.io, entry.name, ctx.gpa, .unlimited) catch continue;
        const source = fragmentSource(ctx.gpa, contents) orelse continue;
        if (Io.Dir.cwd().access(ctx.io, source, .{})) |_| continue else |_| {}
        try deletions.append(ctx.gpa, .{
            .path = try std.fs.path.join(ctx.gpa, &.{ "cdb-frags", entry.name }),
            .bytes = contents.len,
            .reason = .stale_cdb_fragment,
            .is_dir = false,
        });
    }
}

fn fragmentSource(gpa: std.mem.Allocator, contents: []const u8) ?[]const u8 {
    // A fragment is one compile_commands.json object, with a trailing comma
    const trimmed = std.mem.trimEnd(u8, std.mem.trim(u8, contents, " \r\n\t"), ",");
    const parsed = std.json.parseFromSliceLeaky(struct {
        directory: []const u8 = "",
        file: []const u8,
    }, gpa, trimmed, .{ .ignore_unknown_fields = true }) catch return null;
    if (std.fs.path.isAbsolute(parsed.file)) return parsed.file;
    return std.fs.path.join(gpa, &.{ parsed.directory, parsed.file }) catch null;
}

/// The keep-list has one rule per line: `F <path>` keeps a file, `D <dir>` keeps a subtree, and
/// `S <dir>|<stem>` keeps files in `dir` named `<stem>.<ext>` (PDBs and import libraries).
fn planInstall(ctx: Context, prefix: []const u8, keep_list: []const u8) ![]Deletion {
    var deletions: std.ArrayList(Deletion) = .empty;
    const rules = Io.Dir.cwd().readFileAlloc(ctx.io, keep_list, ctx.gpa, .unlimited) catch return deletions.items;
    var dir = Io.Dir.cwd().openDir(ctx.io, prefix, .{ .iterate = true }) catch return deletions.items;
    defer dir.close(ctx.io);

    var walker = try dir.walk(ctx.gpa);
    defer walker.deinit();
    while (try walker.next(ctx.io)) |entry| {
        if (entry.kind != .file) continue;
        const absolute = try std.fs.path.join(ctx.gpa, &.{ prefix, entry.path });
        if (isInstalled(rules, absolute)) continue;
        const stat = dir.statFile(ctx.io, entry.path, .{}) catch continue;
        try deletions.append(ctx.gpa, .{
            .path = try ctx.gpa.dupe(u8, entry.path),
            .bytes = stat.size,
            .reason = .not_installed,
            .is_dir = false,
        });
    }
    return deletions.items;
}

pub fn isInstalled(rules: []const u8, path: []const u8) bool {
    var lines = std.mem.splitScalar(u8, rules, '\n');
    while (lines.next()) |raw| {
        const line = std.mem.trimEnd(u8, raw, "\r");
        if (line.len < 3) continue;
        const value = line[2..];
        switch (line[0]) {
            'F' => if (samePath(path, value)) return true,
            'D' => if (pathWithin(path, value)) return true,
            'S' => {
                const bar = std.mem.indexOfScalar(u8, value, '|') orelse continue;
                const dir = value[0..bar];
                const stem = value[bar + 1 ..];
                const parent = std.fs.path.dirname(path) orelse continue;
                const base = std.fs.path.basename(path);
                if (samePath(parent, dir) and std.mem.startsWith(u8, base, stem) and
                    base.len > stem.len and base[stem.len] == '.') return true;
            },
            else => {},
        }
    }
    return false;
}

fn samePath(lhs: []const u8, rhs: []const u8) bool {
    const a = std.mem.trimEnd(u8, lhs, "/\\");
    const b = std.mem.trimEnd(u8, rhs, "/\\");
    if (a.len != b.len) return false;
    for (a, b) |x, y| if (normalize(x) != normalize(y)) return false;
    return true;
}

fn execute(ctx: Context, root: Io.Dir, deletions: []const Deletion) !void {
    if (ctx.options.dry_run) return;
    for (deletions) |deletion| {
        // A manifest another build holds can't be exclusively locked; leave its output alone
        var locked: std.ArrayList(Io.File) = .empty;
        defer for (locked.items) |file| file.close(ctx.io);
        const available = for (deletion.manifests) |name| {
            const path = try std.fmt.allocPrint(ctx.gpa, "h/{s}.txt", .{name});
            const file = root.openFile(ctx.io, path, .{
                .mode = .read_write,
                .lock = .exclusive,
                .lock_nonblocking = true,
            }) catch break false;
            try locked.append(ctx.gpa, file);
        } else true;
        if (!available) {
            try ctx.out.print("skipped (in use): {s}\n", .{deletion.path});
            continue;
        }
        for (locked.items) |file| file.close(ctx.io);
        locked.clearRetainingCapacity();

        for (deletion.manifests) |name| {
            const path = try std.fmt.allocPrint(ctx.gpa, "h/{s}.txt", .{name});
            root.deleteFile(ctx.io, path) catch |err| {
                try ctx.out.print("failed to delete {s}: {t}\n", .{ path, err });
            };
        }
        const result = if (deletion.is_dir) root.deleteTree(ctx.io, deletion.path) else root.deleteFile(ctx.io, deletion.path);
        result catch |err| try ctx.out.print("failed to delete {s}: {t}\n", .{ deletion.path, err });
    }
}

const age_buckets = [_]struct { label: []const u8, max_days: i64 }{
    .{ .label = "<1d", .max_days = 1 },
    .{ .label = "<7d", .max_days = 7 },
    .{ .label = "<30d", .max_days = 30 },
    .{ .label = "older", .max_days = std.math.maxInt(i64) },
};

fn report(ctx: Context, entries: []Entry, manifests: []const Manifest) !void {
    var referenced: std.StringHashMapUnmanaged(bool) = .empty;
    for (manifests) |*manifest| {
        const gop = try referenced.getOrPut(ctx.gpa, &manifest.digest);
        gop.value_ptr.* = (gop.found_existing and gop.value_ptr.*) or manifest.protected;
    }

    const Row = struct { count: usize = 0, bytes: u64 = 0, ages: [age_buckets.len]u64 = @splat(0) };
    const row_labels = [_][]const u8{ "LLVM/LLD/Clang (protected)", Kind.linked.label(), Kind.object.label(), Kind.other.label(), "superseded (no manifest)" };
    var rows: [row_labels.len]Row = @splat(.{});
    for (entries) |entry| {
        const row: usize = if (referenced.get(entry.digest)) |protected|
            (if (protected) 0 else 1 + @as(usize, @intFromEnum(entry.kind)))
        else
            4;
        rows[row].count += 1;
        rows[row].bytes += entry.size;
        const age_days = @divFloor(ctx.now - entry.mtime, std.time.s_per_day);
        for (age_buckets, 0..) |bucket, i| if (age_days < bucket.max_days) {
            rows[row].ages[i] += entry.size;
            break;
        };
    }

    try ctx.out.print("{s:<30} {s:>7} {s:>10}", .{ ".zig-cache/o by class", "entries", "size" });
    for (age_buckets) |bucket| try ctx.out.print(" {s:>10}", .{bucket.label});
    try ctx.out.writeAll("\n");
    for (row_labels, rows) |label, row| {
        try ctx.out.print("{s:<30} {d:>7} {Bi:>10.1}", .{ label, row.count, row.bytes });
        for (row.ages) |bytes| try ctx.out.print(" {Bi:>10.1}", .{bytes});
        try ctx.out.writeAll("\n");
    }
    try ctx.out.print("{d} manifests\n\n", .{manifests.len});
}

fn summarize(ctx: Context, cache: []const Deletion, install: []const Deletion) !void {
    var totals: [@typeInfo(Reason).@"enum".fields.len]struct { count: usize = 0, bytes: u64 = 0 } = @splat(.{});
    for ([_][]const Deletion{ cache, install }) |list| for (list) |deletion| {
        const total = &totals[@intFromEnum(deletion.reason)];
        total.count += 1;
        total.bytes += deletion.bytes;
    };

    try ctx.out.print("{s}:\n", .{if (ctx.options.dry_run) "would delete" else "deleted"});
    var bytes: u64 = 0;
    for (totals, 0..) |total, i| {
        if (total.count == 0) continue;
        const reason: Reason = @enumFromInt(i);
        try ctx.out.print("  {s:<42} {d:>6} {Bi:>10.1}\n", .{ reason.label(), total.count, total.bytes });
        bytes += total.bytes;
    }
    try ctx.out.print("  {s:<42} {s:>6} {Bi:>10.1}\n", .{ "total", "", bytes });
    for (cache) |deletion| if (deletion.reason == .old_generation) {
        try ctx.out.print("  {s}  {s:<24} {Bi:>10.1}\n", .{ deletion.path, deletion.label, deletion.bytes });
    };
    for (install) |deletion| try ctx.out.print("  zig-out: {s}\n", .{deletion.path});
}

const testing = std.testing;

fn fakeManifest(gpa: std.mem.Allocator, paths: []const []const u8) ![]u8 {
    var text: std.ArrayList(u8) = .empty;
    try text.appendSlice(gpa, "0\n");
    for (paths, 0..) |path, i| {
        const digest: [16]u8 = @splat(@intCast(i + 1));
        try text.print(gpa, "10 1 1 {x} 0 {s}\n", .{ &digest, path });
    }
    return text.items;
}

test "output digest matches a manifest written by std.Build.Cache" {
    const io = testing.io;
    const gpa = testing.allocator;
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();

    const cwd = try std.process.currentPathAlloc(io, gpa);
    defer gpa.free(cwd);
    try tmp.dir.writeFile(io, .{ .sub_path = "source.cc", .data = "int x;\n" });
    try tmp.dir.writeFile(io, .{ .sub_path = "header.inc", .data = "#define X 1\n" });

    var cache: Cache = .{
        .io = io,
        .gpa = gpa,
        .manifest_dir = try tmp.dir.createDirPathOpen(io, "h", .{}),
        .cwd = cwd,
    };
    cache.addPrefix(.{ .path = null, .handle = tmp.dir });
    defer cache.manifest_dir.close(io);

    var man = cache.obtain();
    defer man.deinit();
    man.hash.addBytes("-O2 -std=c++23");
    _ = try man.addFile("source.cc", null);
    try testing.expect(!try man.hit());
    // Depfile entries arrive after the miss, like a C object's headers
    try man.addFilePost("header.inc");
    const expected = man.final();
    try man.writeManifest();

    const manifest_name = man.hex_digest;
    const contents = try cache.manifest_dir.readFileAlloc(io, &(manifest_name ++ ".txt".*), gpa, .unlimited);
    defer gpa.free(contents);
    try testing.expectEqualStrings(&expected, &outputDigest(&manifest_name, contents).?);
}

test "protection needs llvm inputs and no project inputs" {
    const gpa = testing.allocator;
    var arena: std.heap.ArenaAllocator = .init(gpa);
    defer arena.deinit();
    const a = arena.allocator();

    const project = "C:\\p";
    const llvm = "C:\\p\\zig-pkg\\llvm-pkg";
    const llvm_only = try fakeManifest(a, &.{ "C:\\p\\zig-pkg\\llvm-pkg\\llvm\\lib\\Support\\APInt.cpp", "C:\\zig\\lib\\libcxx\\include\\vector" });
    const ours = try fakeManifest(a, &.{ "C:/p/lib/compiler/src/codegen/linker.cc", "C:\\p\\zig-pkg\\llvm-pkg\\llvm\\include\\llvm\\IR\\Module.h" });
    const neither = try fakeManifest(a, &.{"C:\\elsewhere\\replxx.cpp"});
    try testing.expect(isProtected(llvm_only, project, llvm));
    try testing.expect(!isProtected(ours, project, llvm));
    try testing.expect(!isProtected(neither, project, llvm));
}

test "plan keeps protected and newest generations, drops superseded ones" {
    const gpa = testing.allocator;
    var arena: std.heap.ArenaAllocator = .init(gpa);
    defer arena.deinit();
    const a = arena.allocator();

    const day = std.time.s_per_day;
    const now: i64 = 100 * day;
    const digest = struct {
        fn of(c: u8) [32]u8 {
            return @splat(c);
        }
    }.of;

    var entries = [_]Entry{
        // Three generations of the test binary, each still named by a manifest
        .{ .digest = &comptime digest('1'), .size = 900, .mtime = now - 1 * day, .kind = .linked, .name = "compiler.exe" },
        .{ .digest = &comptime digest('2'), .size = 900, .mtime = now - 2 * day, .kind = .linked, .name = "compiler.exe" },
        .{ .digest = &comptime digest('3'), .size = 900, .mtime = now - 3 * day, .kind = .linked, .name = "compiler.exe" },
        // Protected LLVM archives, older than everything
        .{ .digest = &comptime digest('4'), .size = 500, .mtime = now - 50 * day, .kind = .linked, .name = "LLVMSupport.lib" },
        .{ .digest = &comptime digest('5'), .size = 500, .mtime = now - 60 * day, .kind = .linked, .name = "LLVMSupport.lib" },
        .{ .digest = &comptime digest('6'), .size = 500, .mtime = now - 70 * day, .kind = .linked, .name = "LLVMSupport.lib" },
        // A referenced object and a superseded one
        .{ .digest = &comptime digest('7'), .size = 10, .mtime = now - 40 * day, .kind = .object, .name = "context.obj" },
        .{ .digest = &comptime digest('8'), .size = 10, .mtime = now - 40 * day, .kind = .object, .name = "context.obj" },
        // Superseded, but young enough that a running build may be about to name it
        .{ .digest = &comptime digest('9'), .size = 10, .mtime = now - 60, .kind = .object, .name = "type.obj" },
    };
    const manifests = [_]Manifest{
        .{ .name = "m1", .digest = digest('1'), .mtime = now - day, .protected = false },
        .{ .name = "m2", .digest = digest('2'), .mtime = now - day, .protected = false },
        .{ .name = "m3", .digest = digest('3'), .mtime = now - day, .protected = false },
        .{ .name = "m4", .digest = digest('4'), .mtime = now - day, .protected = true },
        .{ .name = "m5", .digest = digest('5'), .mtime = now - day, .protected = true },
        .{ .name = "m6", .digest = digest('6'), .mtime = now - day, .protected = true },
        .{ .name = "m7", .digest = digest('7'), .mtime = now - day, .protected = false },
        .{ .name = "orphan", .digest = digest('x'), .mtime = now - day, .protected = false },
    };

    const plan = try planCache(a, &entries, &manifests, now, .{});
    var deleted: std.StringHashMapUnmanaged(Deletion) = .empty;
    for (plan) |deletion| try deleted.put(a, deletion.path, deletion);

    try testing.expectEqual(@as(usize, 3), plan.len);
    const old = deleted.get("o/" ++ comptime digest('3')).?;
    try testing.expectEqual(Reason.old_generation, old.reason);
    try testing.expectEqualStrings("m3", old.manifests[0]);
    try testing.expectEqual(Reason.superseded, deleted.get("o/" ++ comptime digest('8')).?.reason);
    try testing.expectEqual(Reason.orphan_manifest, deleted.get("h/orphan.txt").?.reason);

    // With --prune-llvm the protected archives follow the same generation limit
    for (&entries) |*entry| entry.manifests = .empty;
    const with_llvm = try planCache(a, &entries, &manifests, now, .{ .prune_llvm = true });
    try testing.expectEqual(@as(usize, 4), with_llvm.len);
}

test "entries an LLVM manifest reads inherit its protection" {
    const gpa = testing.allocator;
    var arena: std.heap.ArenaAllocator = .init(gpa);
    defer arena.deinit();
    const a = arena.allocator();

    const zlib_old: [32]u8 = @splat('a');
    const contents = try fakeManifest(a, &.{
        "C:\\p\\zig-pkg\\llvm-pkg\\llvm\\utils\\TableGen\\Main.cpp",
        "o\\" ++ zlib_old ++ "\\zlib.lib",
        "C:\\p\\.zig-cache\\o\\not-a-digest\\x.h",
    });
    const inputs = try cacheInputs(a, contents);
    try testing.expectEqual(@as(usize, 1), inputs.len);
    try testing.expectEqualStrings(&zlib_old, &inputs[0]);

    const now: i64 = 100 * std.time.s_per_day;
    const tblgen: [32]u8 = @splat('t');
    var entries = [_]Entry{
        .{ .digest = &comptime @as([32]u8, @splat('1')), .size = 1, .mtime = now - 10, .kind = .linked, .name = "zlib.lib" },
        .{ .digest = &comptime @as([32]u8, @splat('2')), .size = 1, .mtime = now - 20, .kind = .linked, .name = "zlib.lib" },
        .{ .digest = &zlib_old, .size = 1, .mtime = now - 30 * std.time.s_per_day, .kind = .linked, .name = "zlib.lib" },
        .{ .digest = &tblgen, .size = 1, .mtime = now - 30 * std.time.s_per_day, .kind = .linked, .name = "llvm-tblgen.exe" },
    };
    const manifests = [_]Manifest{
        .{ .name = "z1", .digest = @splat('1'), .mtime = now, .protected = false },
        .{ .name = "z2", .digest = @splat('2'), .mtime = now, .protected = false },
        .{ .name = "z3", .digest = zlib_old, .mtime = now, .protected = false },
        .{ .name = "tg", .digest = tblgen, .mtime = now, .protected = true, .inputs = inputs },
    };
    const plan = try planCache(a, &entries, &manifests, now, .{ .grace_seconds = 0 });
    try testing.expectEqual(@as(usize, 0), plan.len);
}

test "install keep-list rules" {
    const rules =
        \\F C:\p\zig-out\bin\ghoti.exe
        \\S C:\p\zig-out\bin|ghoti
        \\D C:\p\zig-out\lib\std
    ;
    try testing.expect(isInstalled(rules, "C:\\p\\zig-out\\bin\\ghoti.exe"));
    try testing.expect(isInstalled(rules, "C:/p/zig-out/bin/ghoti.pdb"));
    try testing.expect(isInstalled(rules, "C:\\p\\zig-out\\lib\\std\\io\\io.gh"));
    try testing.expect(!isInstalled(rules, "C:\\p\\zig-out\\bin\\ghoti-old.exe"));
    try testing.expect(!isInstalled(rules, "C:\\p\\zig-out\\tests\\parser.exe"));
}
