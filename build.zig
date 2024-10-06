const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{ .preferred_optimize_mode = .ReleaseFast });
    const strip = b.option(bool, "strip", "Whether to strip symbols from the binary, defaults to false") orelse false;

    // Create the executable
    const bin = b.addExecutable(.{
        .name = "dsv2",
        .target = target,
        // uses libc
        .link_libc = true,
        .optimize = optimize,
        .strip = strip,
    });

    // Add C source files
    bin.addCSourceFiles(.{
        .files = &.{
            "src/bmc.c",
            "src/bs.c",
            "src/dsv.c",
            "src/dsv_decoder.c",
            "src/dsv_encoder.c",
            "src/dsv_main.c",
            "src/frame.c",
            "src/hme.c",
            "src/hzcc.c",
            "src/sbt.c",
            "src/util.c",
        },
        .flags = &.{
            "-std=c89",
            "-Wall",
            "-Wextra",
            "-Wpedantic",
            "-Werror",
        },
    });
    b.installArtifact(bin);
}
