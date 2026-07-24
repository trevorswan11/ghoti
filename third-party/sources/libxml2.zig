//! https://github.com/allyourcodebase/libxml2
const std = @import("std");

const version: std.SemanticVersion = .{
    .major = 2,
    .minor = 15,
    .patch = 1,
};
const version_str = std.fmt.comptimePrint("{f}", .{version});
const version_num = 21501;

pub fn configHeaders(
    b: *std.Build,
    styles: struct {
        config: std.Build.Step.ConfigHeader.Style,
        xmlversion: std.Build.Step.ConfigHeader.Style,
    },
    target: std.Build.ResolvedTarget,
) struct {
    config: *std.Build.Step.ConfigHeader,
    xmlversion: *std.Build.Step.ConfigHeader,
} {
    const config = b.addConfigHeader(.{ .style = styles.config }, .{
        .HAVE_STDLIB_H = 1,
        .HAVE_STDINT_H = 1,
        .HAVE_STAT = 1,
        .HAVE_FSTAT = 1,
        .HAVE_FUNC_ATTRIBUTE_DESTRUCTOR = 1,
        .HAVE_LIBHISTORY = 0,
        .HAVE_LIBREADLINE = 0,
        .XML_SYSCONFDIR = 0,
        .HAVE_DLOPEN = @intFromBool(target.result.os.tag != .windows),
        .XML_THREAD_LOCAL = switch (target.result.os.tag) {
            .windows => "__declspec(thread)",
            else => "_Thread_local",
        },
    });

    const xmlversion = b.addConfigHeader(.{
        .style = styles.xmlversion,
        .include_path = "libxml/xmlversion.h",
    }, .{
        .VERSION = version_str,
        .LIBXML_VERSION_NUMBER = version_num,
        .LIBXML_VERSION_EXTRA = "-ghoti",
        .WITH_THREADS = 1,
        .WITH_THREAD_ALLOC = 1,
        .WITH_OUTPUT = 1,
        .WITH_PUSH = 1,
        .WITH_READER = 1,
        .WITH_PATTERN = 1,
        .WITH_WRITER = 1,
        .WITH_SAX1 = 1,
        .WITH_HTTP = 0,
        .WITH_VALID = 1,
        .WITH_HTML = 1,
        .WITH_C14N = 0,
        .WITH_CATALOG = 0,
        .WITH_XPATH = 1,
        .WITH_XPTR = 1,
        .WITH_XINCLUDE = 1,
        .WITH_ICONV = 0,
        .WITH_ICU = 0,
        .WITH_ISO8859X = 1,
        .WITH_DEBUG = 1,
        .WITH_REGEXPS = 1,
        .WITH_RELAXNG = 0,
        .WITH_SCHEMAS = 0,
        .WITH_SCHEMATRON = 0,
        .WITH_MODULES = 0,
        .WITH_ZLIB = 1,
        .MODULE_EXTENSION = target.result.dynamicLibSuffix(),
    });

    return .{ .config = config, .xmlversion = xmlversion };
}

pub const sources = [_][]const u8{
    "HTMLparser.c",
    "HTMLtree.c",
    "SAX2.c",
    "buf.c",
    "c14n.c",
    "catalog.c",
    "chvalid.c",
    "debugXML.c",
    "dict.c",
    "encoding.c",
    "entities.c",
    "error.c",
    "globals.c",
    "hash.c",
    "list.c",
    "nanohttp.c",
    "parser.c",
    "parserInternals.c",
    "pattern.c",
    "relaxng.c",
    "schematron.c",
    "threads.c",
    "tree.c",
    "uri.c",
    "valid.c",
    "xinclude.c",
    "xlink.c",
    "xmlIO.c",
    "xmlmemory.c",
    "xmlmodule.c",
    "xmlreader.c",
    "xmlregexp.c",
    "xmlsave.c",
    "xmlschemas.c",
    "xmlschemastypes.c",
    "xmlstring.c",
    "xmlwriter.c",
    "xpath.c",
    "xpointer.c",
};
