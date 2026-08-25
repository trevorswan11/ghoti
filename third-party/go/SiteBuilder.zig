const std = @import("std");

const parent_build = @import("../../build.zig");
const stdx = parent_build.stdx;

const ProjectPaths = parent_build.ProjectPaths;
const GoDependency = struct {
    dep: *std.Build.Dependency,
    artifact_path: std.Build.LazyPath = undefined,
    builder: *std.Build.Step.Run = undefined,
    install: *std.Build.Step.InstallFile = undefined,
};

const Self = @This();

const output_path = "site/";
const output_dev_path = output_path ++ "dev/";
const go_work = "go.work";
const webserver_exe = "webserver";
const devserver_exe = "devserver";
const templ_exe = "templ";
const air_exe = "air";

b: *std.Build,
optimize: std.builtin.OptimizeMode,
site_path: std.Build.LazyPath,

go_exe_path: []const u8,
go_fmt_path: ?[]const u8,
templ: GoDependency,
air: GoDependency,
main_builder: *std.Build.Step.Run = undefined,
rebuild: *std.Build.Step.Compile = undefined,
work_update_src: *std.Build.Step.UpdateSourceFiles = undefined,

pub fn init(b: *std.Build, optimize: std.builtin.OptimizeMode) !?*Self {
    const go_path = b.findProgram(&.{"go"}, &.{}) catch return null;
    const templ_dep = b.lazyDependency("templ", .{});
    const air_dep = b.lazyDependency("air", .{});
    if (templ_dep == null or air_dep == null) return null;

    const self = try b.allocator.create(Self);
    self.* = .{
        .b = b,
        .optimize = optimize,
        .site_path = b.path(ProjectPaths.site),
        .go_exe_path = go_path,
        .go_fmt_path = b.findProgram(&.{"gofmt"}, &.{}) catch null,
        .templ = .{ .dep = templ_dep.? },
        .air = .{ .dep = air_dep.? },
    };
    return self;
}

/// This is fragile but I don't care
pub fn build(self: *Self) !void {
    try self.buildTempl();
    self.buildAir();
    self.buildRebuild();
    self.buildOneShots();
    self.buildWatch();
}

fn buildTempl(self: *Self) !void {
    const builder = self.addRunGoBuild(.ReleaseFast);
    builder.setCwd(self.templ.dep.path("cmd/templ"));
    self.templ.artifact_path = builder.addOutputFileArg(templ_exe);
    self.templ.install = self.addInstallFile(self.templ.artifact_path, templ_exe, output_dev_path);
    self.work_update_src = try self.addGoWork();
    self.templ.builder = builder;
}

fn buildAir(self: *Self) void {
    const builder = self.addRunGoBuild(.ReleaseFast);
    builder.setCwd(self.air.dep.path("."));
    self.air.artifact_path = builder.addOutputFileArg(air_exe);
    self.air.install = self.addInstallFile(self.air.artifact_path, air_exe, output_dev_path);
    self.air.builder = builder;
}

fn buildRebuild(self: *Self) void {
    const b = self.b;
    self.rebuild = self.b.addExecutable(.{
        .name = "rebuild",
        .root_module = b.addModule("rebuild", .{
            .root_source_file = b.path("site/rebuild.zig"),
            .target = b.graph.host,
        }),
    });
    self.rebuild.step.dependOn(&self.templ.install.step);
}

fn buildOneShots(self: *Self) void {
    const b = self.b;
    const generator = self.addRunTempl();
    generator.setCwd(self.site_path);
    generator.addArg("generate");
    generator.step.dependOn(&self.work_update_src.step);
    generator.has_side_effects = true;

    const builder = self.addRunGoBuild(self.optimize);
    builder.setCwd(self.site_path.path(b, "cmd/server"));
    builder.step.dependOn(&generator.step);
    builder.has_side_effects = true;
    self.main_builder = builder;

    const server_path = builder.addOutputFileArg(webserver_exe);
    const server_install = self.addInstallFile(server_path, webserver_exe, output_path);
    const build_site = b.step("site", "Build and install the project's website");
    build_site.dependOn(&server_install.step);

    const run_site_run: *std.Build.Step.Run = .create(b, "run site");
    run_site_run.addFileArg(server_path);
    const run_site = b.step("run-site", "Build, install, and run the project's website");
    run_site.dependOn(&run_site_run.step);
}

fn buildWatch(self: *Self) void {
    const b = self.b;
    const watch_site_run: *std.Build.Step.Run = .create(b, "run air");
    watch_site_run.step.dependOn(&self.air.install.step);
    const abs_dev_path = b.pathJoin(&.{ b.install_prefix, output_dev_path });
    watch_site_run.setEnvironmentVariable("GHOTI_TEMPL_ARTIFACT", b.pathJoin(&.{
        abs_dev_path,
        self.templ.install.dest_rel_path,
    }));
    watch_site_run.setEnvironmentVariable("GHOTI_GO_PATH", self.go_exe_path);
    const output = stdx.utils.tryAppendExe(b.allocator, b.graph.host, b.pathJoin(&.{ abs_dev_path, devserver_exe }));
    watch_site_run.setEnvironmentVariable("GHOTI_SITE_OUTPUT", output);

    watch_site_run.addFileArg(self.air.artifact_path);
    watch_site_run.addArg("--build.cmd");
    watch_site_run.addArtifactArg(self.rebuild);
    watch_site_run.addArgs(&.{
        "--build.entrypoint",
        output,
        "--build.include_ext",
        "go,templ",
        "--build.exclude_regex",
        ".*_templ.go,.*_test.go",
        "-tmp_dir",
        abs_dev_path,
    });
    watch_site_run.setCwd(self.site_path);

    const watch_site = b.step("watch-site", "Kick off live reloading of the project's website");
    watch_site.dependOn(&watch_site_run.step);
}

fn addGoWork(self: *const Self) !*std.Build.Step.UpdateSourceFiles {
    const b = self.b;
    const templ_path = try self.templ.dep.path(".").getPath4(b, null);
    const go_work_content = b.fmt(
        \\go 1.26.3
        \\
        \\use (
        \\  .
        \\  {s}
        \\)
    , .{try templ_path.toString(b.allocator)});
    const write_work = b.addWriteFile(go_work, go_work_content);
    const gen_work = write_work.getDirectory().path(b, go_work);
    const update = b.addUpdateSourceFiles();
    update.addCopyFileToSource(gen_work, ProjectPaths.site ++ go_work);
    return update;
}

fn addRunGoBuild(self: *const Self, optimize: std.builtin.OptimizeMode) *std.Build.Step.Run {
    const run = self.b.addSystemCommand(&.{self.go_exe_path});
    run.addArg("build");
    run.addArgs(getGoOptimizeFlags(optimize));
    run.addArg("-o");
    return run;
}

fn addInstallFile(
    self: *const Self,
    path: std.Build.LazyPath,
    executable: []const u8,
    prefix_path: []const u8,
) *std.Build.Step.InstallFile {
    const b = self.b;
    return b.addInstallFileWithDir(
        path,
        .{ .custom = prefix_path },
        stdx.utils.tryAppendExe(b.allocator, b.graph.host, executable),
    );
}

pub fn addRunTempl(self: *const Self) *std.Build.Step.Run {
    const run: *std.Build.Step.Run = .create(self.b, "run templ");
    run.addFileArg(self.templ.artifact_path);
    return run;
}

fn getGoOptimizeFlags(optimize: std.builtin.OptimizeMode) []const []const u8 {
    return switch (optimize) {
        .Debug => return &.{ "-gcflags=all=-N -l" },
        .ReleaseSmall => return &.{ "-ldflags=-s -w", "-trimpath" },
        .ReleaseFast, .ReleaseSafe => return &.{"-ldflags=-s -w"},
    };
}
