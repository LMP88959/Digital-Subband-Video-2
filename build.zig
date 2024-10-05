const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});

    // Create the executable
    const bin = b.addExecutable(.{
        .name = "dsv2",
        .target = target,
        .optimize = .ReleaseFast,
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
            "-std=c99",
            "-O3",
        },
    });

    // Link libc
    bin.linkLibC();
    b.installArtifact(bin);
}
