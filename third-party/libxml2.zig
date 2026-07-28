const std = @import("std");

const parent_build = @import("../build.zig");
const Dependency = parent_build.stdx.Dependency;
const Config = Dependency.Config;

const libxml2 = @import("sources/libxml2.zig");

/// Compiles libxml2 from source as a static library
/// https://github.com/allyourcodebase/libxml2
pub fn build(b: *std.Build, config: struct {
    opts: Config,
    zlib: Dependency,
}) Dependency {
    const upstream = b.dependency("libxml2", .{});
    const target = config.opts.target;
    const mod = b.createModule(.{
        .target = target,
        .optimize = config.opts.optimize,
        .link_libc = true,
    });

    const configs = libxml2.configHeaders(
        b,
        .{
            .config = .{ .cmake = upstream.path("config.h.cmake.in") },
            .xmlversion = .{ .autoconf_at = upstream.path("include/libxml/xmlversion.h.in") },
        },
        target,
    );
    mod.addConfigHeader(configs.config);
    mod.addConfigHeader(configs.xmlversion);

    mod.addCMacro("LIBXML_STATIC", "1");
    mod.addCSourceFiles(.{
        .root = upstream.path("."),
        .files = &libxml2.sources,
        .flags = &.{ "-std=c11", "-D_REENTRANT", "-DLIBXML_STATIC" },
    });
    mod.addIncludePath(upstream.path("include"));
    mod.addIncludePath(config.zlib.upstream.path("."));

    mod.linkLibrary(config.zlib.artifact);
    if (target.result.os.tag == .windows) {
        mod.linkSystemLibrary("bcrypt", .{});
    }

    const lib = b.addLibrary(.{
        .name = "xml2",
        .root_module = mod,
    });
    lib.installConfigHeader(configs.xmlversion);
    lib.installHeadersDirectory(upstream.path("include/libxml"), "libxml", .{});

    return .{
        .upstream = upstream,
        .artifact = lib,
    };
}
