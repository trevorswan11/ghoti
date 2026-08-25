const std = @import("std");
const builtin = @import("builtin");

pub fn main(init: std.process.Init) !void {
    const templ = init.environ_map.get("GHOTI_TEMPL_ARTIFACT") orelse return error.TemplNotFound;
    var templ_proc = try std.process.spawn(init.io, .{
        .argv = &.{ templ, "generate" },
        .create_no_window = true,
    });

    switch (try templ_proc.wait(init.io)) {
        .exited => |code| if (code != 0) std.process.exit(code),
        else => std.process.exit(1),
    }

    const go = init.environ_map.get("GHOTI_GO_PATH") orelse return error.GoNotFound;
    const output = init.environ_map.get("GHOTI_SITE_OUTPUT") orelse return error.OutputNotFound;
    var go_proc = try std.process.spawn(init.io, .{
        .argv = &.{ go, "build", "-o", output, "./cmd/server" },
        .create_no_window = true,
    });

    switch (try go_proc.wait(init.io)) {
        .exited => |code| if (code != 0) std.process.exit(code),
        else => std.process.exit(1),
    }
}
