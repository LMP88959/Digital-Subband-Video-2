const std = @import("std");

fn addDsvSources(bin: *std.Build.Step.Compile) void {
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
}

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{ .preferred_optimize_mode = .ReleaseFast });
    const strip = b.option(bool, "strip", "Whether to strip symbols from the binary, defaults to false") orelse false;
    const linkage = b.option(std.builtin.LinkMode, "linkage", "How to link") orelse null;

    // distribution binaries
    {
        const dist_step = b.step("dist", "Compile binaries for distribution");
        for ([_]std.Target.Query{
            // x86(-64) and arm64, the big ones
            .{ .cpu_arch = .x86_64, .os_tag = .linux, .abi = .musl },
            .{ .cpu_arch = .x86_64, .os_tag = .windows },
            .{ .cpu_arch = .x86_64, .os_tag = .macos },

            .{ .cpu_arch = .aarch64, .os_tag = .windows },
            .{ .cpu_arch = .aarch64, .os_tag = .linux, .abi = .musl },
            .{ .cpu_arch = .aarch64, .os_tag = .macos },

            .{ .cpu_arch = .x86, .os_tag = .windows },
            .{ .cpu_arch = .x86, .os_tag = .linux, .abi = .musl },

            // less common architectures, linux
            .{ .cpu_arch = .arm, .os_tag = .linux, .abi = .musleabi },
            .{ .cpu_arch = .riscv32, .os_tag = .linux, .abi = .musl },
            .{ .cpu_arch = .riscv64, .os_tag = .linux, .abi = .musl },

            // wasm
            .{ .cpu_arch = .wasm32, .os_tag = .wasi },
        }) |query| {
            const dist_target = b.resolveTargetQuery(query);
            const dist_bin = b.addExecutable(.{
                .name = b.fmt("dsv2-{s}", .{query.zigTriple(b.allocator) catch @panic("oom")}),
                .target = dist_target,
                .link_libc = true,
                .optimize = .ReleaseFast,
                .strip = true,
                .linkage = if (query.os_tag == .macos) .dynamic else .static,
            });
            addDsvSources(dist_bin);

            dist_step.dependOn(&b.addInstallArtifact(dist_bin, .{
                .dest_dir = .{ .override = .{ .custom = "dist" } },
            }).step);
        }
    }

    // Create the executable
    const bin = b.addExecutable(.{
        .name = "dsv2",
        .target = target,
        // uses libc
        .link_libc = true,
        .optimize = optimize,
        .strip = strip,
        .linkage = linkage,
    });
    addDsvSources(bin);
    b.installArtifact(bin);
}
