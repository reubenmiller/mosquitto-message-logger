const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    // Get mosquitto dependency for headers
    const mosquitto_dep = b.dependency("mosquitto", .{
        .target = target,
        .optimize = optimize,
    });

    // Build the plugin as a shared library.
    // C sources, include paths and libc linking are configured on the module
    // (as required by the Zig 0.16 build API).
    const mod = b.createModule(.{
        .target = target,
        .optimize = optimize,
        .link_libc = true,
    });

    // Add C source file directly to the module
    mod.addCSourceFile(.{
        .file = b.path("mosquitto_message_logger.c"),
        .flags = &[_][]const u8{
            "-Wall",
            "-Werror",
            "-O2",
            "-fPIC",
            "-D_GNU_SOURCE",
        },
    });

    // Add include paths from mosquitto dependency
    mod.addIncludePath(mosquitto_dep.path("include"));
    mod.addIncludePath(mosquitto_dep.path("src"));

    const lib = b.addLibrary(.{
        .name = "mosquitto_message_logger",
        .root_module = mod,
        .linkage = .dynamic,
    });

    // For plugins, allow undefined symbols (resolved at runtime by mosquitto)
    if (target.result.os.tag == .linux or target.result.os.tag == .macos) {
        lib.linker_allow_shlib_undefined = true;
    }

    // Install the library
    b.installArtifact(lib);

    // Add a step to build for all targets
    const build_all_step = b.step("all", "Build for all targets (glibc + musl variants)");

    // Set glibc version for maximum compatibility
    // 2.17 = RHEL 7 / CentOS 7 (2013) - needed for aarch64 minimum
    // Older architectures (x86_64, x86, arm) will use even older versions (2.4) automatically
    // You can adjust this to:
    //   - 2.19 for Ubuntu 14.04+ compatibility
    //   - 2.27 for Ubuntu 18.04+ / Debian 10+ compatibility  
    //   - 2.31 for Ubuntu 20.04+ / Debian 11+ compatibility
    const glibc_version = std.SemanticVersion{ .major = 2, .minor = 17, .patch = 0 };
    
    // RISC-V requires glibc 2.27+ (added in 2018)
    const glibc_version_riscv = std.SemanticVersion{ .major = 2, .minor = 27, .patch = 0 };

    const linux_targets = [_]std.Target.Query{
        // glibc targets (Debian, Ubuntu, RHEL, etc.)
        .{ .cpu_arch = .x86_64, .os_tag = .linux, .abi = .gnu, .glibc_version = glibc_version },
        .{ .cpu_arch = .x86, .os_tag = .linux, .abi = .gnu, .glibc_version = glibc_version },
        .{ .cpu_arch = .aarch64, .os_tag = .linux, .abi = .gnu, .glibc_version = glibc_version },
        .{ .cpu_arch = .riscv64, .os_tag = .linux, .abi = .gnu, .glibc_version = glibc_version_riscv },
        .{ .cpu_arch = .arm, .os_tag = .linux, .abi = .gnueabihf, .glibc_version = glibc_version, .cpu_model = .{ .explicit = &std.Target.arm.cpu.arm1176jzf_s } },
        .{ .cpu_arch = .arm, .os_tag = .linux, .abi = .gnueabihf, .glibc_version = glibc_version },
        
        // musl targets (Alpine, embedded systems, etc.)
        .{ .cpu_arch = .x86_64, .os_tag = .linux, .abi = .musl },
        .{ .cpu_arch = .x86, .os_tag = .linux, .abi = .musl },
        .{ .cpu_arch = .aarch64, .os_tag = .linux, .abi = .musl },
        .{ .cpu_arch = .riscv64, .os_tag = .linux, .abi = .musl },
        .{ .cpu_arch = .arm, .os_tag = .linux, .abi = .musleabihf, .cpu_model = .{ .explicit = &std.Target.arm.cpu.arm1176jzf_s } },
        .{ .cpu_arch = .arm, .os_tag = .linux, .abi = .musleabihf },
        
        // macOS
        .{ .cpu_arch = .aarch64, .os_tag = .macos },
    };

    const target_names = [_][]const u8{
        // glibc
        "x86_64",
        "x86",
        "aarch64",
        "riscv64",
        "armv6",
        "armv7",
        
        // musl
        "x86_64-musl",
        "x86-musl",
        "aarch64-musl",
        "riscv64-musl",
        "armv6-musl",
        "armv7-musl",
        
        // macOS
        "macos-aarch64",
    };

    for (linux_targets, target_names) |linux_target, target_name| {
        const resolved_target = b.resolveTargetQuery(linux_target);

        const target_mod = b.createModule(.{
            .target = resolved_target,
            .optimize = optimize,
            .link_libc = true,
        });

        target_mod.addCSourceFile(.{
            .file = b.path("mosquitto_message_logger.c"),
            .flags = &[_][]const u8{
                "-Wall",
                "-Werror",
                "-O2",
                "-fPIC",
                "-D_GNU_SOURCE",
            },
        });

        target_mod.addIncludePath(mosquitto_dep.path("include"));
        target_mod.addIncludePath(mosquitto_dep.path("src"));

        const target_lib = b.addLibrary(.{
            .name = "mosquitto_message_logger",
            .root_module = target_mod,
            .linkage = .dynamic,
        });

        // Allow undefined symbols for plugins (resolved at runtime by mosquitto)
        if (resolved_target.result.os.tag == .linux or resolved_target.result.os.tag == .macos) {
            target_lib.linker_allow_shlib_undefined = true;
        }

        // Install with architecture-specific name
        const extension = if (resolved_target.result.os.tag == .macos) "dylib" else "so";
        const install_step = b.addInstallFile(
            target_lib.getEmittedBin(),
            b.fmt("dist/libmosquitto_message_logger-{s}.{s}", .{target_name, extension}),
        );

        build_all_step.dependOn(&install_step.step);
    }
}
