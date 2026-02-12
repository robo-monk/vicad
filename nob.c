#define NOB_IMPLEMENTATION
#include "nob.h"
#include <sys/stat.h>

static const char *manifold_sources[] = {
    "manifold/src/boolean3.cpp",
    "manifold/src/boolean_result.cpp",
    "manifold/src/constructors.cpp",
    "manifold/src/csg_tree.cpp",
    "manifold/src/edge_op.cpp",
    "manifold/src/face_op.cpp",
    "manifold/src/impl.cpp",
    "manifold/src/lazy_collider.cpp",
    "manifold/src/manifold.cpp",
    "manifold/src/minkowski.cpp",
    "manifold/src/polygon.cpp",
    "manifold/src/properties.cpp",
    "manifold/src/quickhull.cpp",
    "manifold/src/sdf.cpp",
    "manifold/src/smoothing.cpp",
    "manifold/src/sort.cpp",
    "manifold/src/subdivision.cpp",
    "manifold/src/tree2d.cpp",
};

static const char *manifold_cross_section_source = "manifold/src/cross_section/cross_section.cpp";
static const char *manifold_meshio_source = "manifold/src/meshIO/meshIO.cpp";
static const char *app_sources[] = {
    "src/app_state.cpp",
    "src/main.cpp",
    "src/edge_detection.cpp",
    "src/face_detection.cpp",
    "src/input_controller.cpp",
    "src/lod_policy.cpp",
    "src/op_decoder.cpp",
    "src/op_reader.cpp",
    "src/op_trace.cpp",
    "src/render_scene.cpp",
    "src/render_ui.cpp",
    "src/script_worker_client.cpp",
    "src/scene_runtime.cpp",
    "src/sketch_semantics.cpp",
    "src/sketch_dimensions.cpp",
};
static const char *font_baker_tool_source = "tools/font_baker.c";
static const char *freetype_baker_sources[] = {
    "freetype/src/base/ftsystem.c",
    "freetype/src/base/ftinit.c",
    "freetype/src/base/ftbase.c",
    "freetype/src/base/ftbitmap.c",
    "freetype/src/base/ftsynth.c",
    "freetype/src/base/ftmm.c",
    "freetype/src/base/ftdebug.c",
    "freetype/src/sfnt/sfnt.c",
    "freetype/src/truetype/truetype.c",
    "freetype/src/smooth/smooth.c",
    "freetype/src/raster/raster.c",
    "freetype/src/psnames/psnames.c",
    "freetype/src/gzip/ftgzip.c",
};

typedef struct {
    const char *root;
    const char *include_dir;
    const char *sources[4];
    bool found;
} Clipper2Info;

typedef struct {
    const char *root;
    const char *build_dir;
    const char *include_src_dir;
    const char *include_build_dir;
    const char *lib_assimp;
    const char *lib_zlib;
    bool found;
} AssimpInfo;

typedef struct {
    bool asan;
    bool asan_deps;
} BuildOptions;

static void append_sanitizer_flags(Nob_Cmd *cmd, BuildOptions opt) {
    if (!opt.asan) return;
    nob_cmd_append(cmd,
                   "-O1",
                   "-g",
                   "-fno-omit-frame-pointer",
                   "-fsanitize=address,undefined",
                   "-fno-sanitize-recover=all");
}

static bool file_mtime_ns(const char *path, long long *out_ns) {
    struct stat st;
    if (stat(path, &st) != 0) return false;
#if defined(__APPLE__)
    *out_ns = (long long)st.st_mtimespec.tv_sec * 1000000000LL + (long long)st.st_mtimespec.tv_nsec;
#else
    *out_ns = (long long)st.st_mtim.tv_sec * 1000000000LL + (long long)st.st_mtim.tv_nsec;
#endif
    return true;
}

static bool newest_file_mtime_ns_recursive(const char *dir_path, long long *newest_ns) {
    Nob_File_Paths children = {0};
    if (!nob_read_entire_dir(dir_path, &children)) return false;
    for (size_t i = 0; i < children.count; ++i) {
        const char *name = children.items[i];
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
        if (strcmp(name, ".git") == 0) continue;
        const char *path = nob_temp_sprintf("%s/%s", dir_path, name);
        Nob_File_Type type = nob_get_file_type(path);
        if (type == NOB_FILE_DIRECTORY) {
            if (!newest_file_mtime_ns_recursive(path, newest_ns)) {
                nob_da_free(children);
                return false;
            }
        } else if (type == NOB_FILE_REGULAR) {
            long long mt = 0;
            if (file_mtime_ns(path, &mt) && mt > *newest_ns) *newest_ns = mt;
        }
    }
    nob_da_free(children);
    return true;
}

static Clipper2Info detect_clipper2(void) {
    Clipper2Info info = {0};
    info.root = getenv("CLIPPER2_DIR");
    if (info.root == NULL || info.root[0] == '\0') {
        info.root = "Clipper2";
    }

    info.include_dir = nob_temp_sprintf("%s/CPP/Clipper2Lib/include", info.root);
    info.sources[0] = nob_temp_sprintf("%s/CPP/Clipper2Lib/src/clipper.engine.cpp", info.root);
    info.sources[1] = nob_temp_sprintf("%s/CPP/Clipper2Lib/src/clipper.offset.cpp", info.root);
    info.sources[2] = nob_temp_sprintf("%s/CPP/Clipper2Lib/src/clipper.rectclip.cpp", info.root);
    info.sources[3] = nob_temp_sprintf("%s/CPP/Clipper2Lib/src/clipper.triangulation.cpp", info.root);

    const char *header = nob_temp_sprintf("%s/clipper2/clipper.h", info.include_dir);
    info.found = nob_file_exists(header) != 0;
    for (size_t i = 0; i < NOB_ARRAY_LEN(info.sources); ++i) {
        info.found = info.found && (nob_file_exists(info.sources[i]) != 0);
    }

    return info;
}

static AssimpInfo detect_assimp(void) {
    AssimpInfo info = {0};
    info.root = getenv("ASSIMP_DIR");
    if (info.root == NULL || info.root[0] == '\0') {
        info.root = "assimp";
    }
    info.build_dir = "build/assimp";
    info.include_src_dir = nob_temp_sprintf("%s/include", info.root);
    info.include_build_dir = nob_temp_sprintf("%s/include", info.build_dir);
    info.lib_assimp = nob_temp_sprintf("%s/lib/libassimp.a", info.build_dir);
    info.lib_zlib = nob_temp_sprintf("%s/contrib/zlib/libzlibstatic.a", info.build_dir);
    const char *cmakelists = nob_temp_sprintf("%s/CMakeLists.txt", info.root);
    info.found = nob_file_exists(cmakelists) != 0;
    return info;
}

static bool build_assimp_if_needed(const AssimpInfo *assimp) {
    if (!assimp || !assimp->found) return false;

    if (!nob_mkdir_if_not_exists("build")) return false;
    if (!nob_mkdir_if_not_exists(assimp->build_dir)) return false;

    const char *cache_path = nob_temp_sprintf("%s/CMakeCache.txt", assimp->build_dir);
    const char *root_cmake = nob_temp_sprintf("%s/CMakeLists.txt", assimp->root);
    const bool have_cache = nob_file_exists(cache_path) != 0;
    bool need_configure = !have_cache;
    if (!need_configure) {
        int cfg_stale = nob_needs_rebuild1(cache_path, root_cmake);
        if (cfg_stale < 0) return false;
        need_configure = cfg_stale != 0;
    }

    if (need_configure) {
        Nob_Cmd cfg = {0};
        nob_cmd_append(&cfg,
                       "cmake",
                       "-S", assimp->root,
                       "-B", assimp->build_dir,
                       "-DCMAKE_BUILD_TYPE=Release",
                       "-DBUILD_SHARED_LIBS=OFF",
                       "-DASSIMP_BUILD_ASSIMP_TOOLS=OFF",
                       "-DASSIMP_BUILD_TESTS=OFF",
                       "-DASSIMP_BUILD_SAMPLES=OFF",
                       "-DASSIMP_INSTALL=OFF",
                       "-DASSIMP_BUILD_ZLIB=ON");
        if (!nob_cmd_run(&cfg)) return false;
    } else {
        nob_log(NOB_INFO, "Assimp configure is up-to-date");
    }

    bool need_build = false;
    if (nob_file_exists(assimp->lib_assimp) == 0 ||
        nob_file_exists(assimp->include_build_dir) == 0) {
        need_build = true;
    } else {
        long long lib_mtime = 0;
        long long newest_src = 0;
        if (!file_mtime_ns(assimp->lib_assimp, &lib_mtime)) {
            need_build = true;
        } else {
            const char *scan_dirs[] = {
                nob_temp_sprintf("%s/code", assimp->root),
                nob_temp_sprintf("%s/include", assimp->root),
                nob_temp_sprintf("%s/contrib", assimp->root),
                nob_temp_sprintf("%s/cmake-modules", assimp->root),
            };
            for (size_t i = 0; i < NOB_ARRAY_LEN(scan_dirs); ++i) {
                if (nob_file_exists(scan_dirs[i]) != 0) {
                    if (!newest_file_mtime_ns_recursive(scan_dirs[i], &newest_src)) return false;
                }
            }
            long long root_cmake_mtime = 0;
            if (file_mtime_ns(root_cmake, &root_cmake_mtime) && root_cmake_mtime > newest_src) {
                newest_src = root_cmake_mtime;
            }
            need_build = newest_src > lib_mtime;
        }
    }

    if (need_build) {
        Nob_Cmd build = {0};
        nob_cmd_append(&build,
                       "cmake",
                       "--build", assimp->build_dir,
                       "--config", "Release",
                       "--target", "assimp");
        if (!nob_cmd_run(&build)) return false;
    } else {
        nob_log(NOB_INFO, "Assimp build is up-to-date");
    }

    if (nob_file_exists(assimp->lib_assimp) == 0) {
        nob_log(NOB_ERROR, "Assimp static library missing: %s", assimp->lib_assimp);
        return false;
    }
    if (nob_file_exists(assimp->include_build_dir) == 0) {
        nob_log(NOB_ERROR, "Assimp generated include dir missing: %s", assimp->include_build_dir);
        return false;
    }
    return true;
}

static bool compile_object_if_needed(const char *src_path,
                                     const char *obj_path,
                                     bool enable_cross_section,
                                     const char *clipper_include_dir,
                                     bool enable_meshio,
                                     const AssimpInfo *assimp,
                                     const char *extra_dependency,
                                     BuildOptions opt,
                                     bool sanitize_this_object) {
    int rebuild = 0;
    if (extra_dependency != NULL && extra_dependency[0] != '\0') {
        const char *deps[2] = {src_path, extra_dependency};
        rebuild = nob_needs_rebuild(obj_path, deps, NOB_ARRAY_LEN(deps));
    } else {
        rebuild = nob_needs_rebuild1(obj_path, src_path);
    }
    if (rebuild < 0) return false;
    if (rebuild == 0) return true;

    Nob_Cmd cmd = {0};
    nob_cmd_append(&cmd, "clang++");
    nob_cmd_append(&cmd,
                   "-std=c++20",
                   "-Wall",
                   "-Wextra",
                   "-Wpedantic",
                   "-O2",
                   "-I.",
                   "-Ibuild/generated",
                   "-Imanifold/include",
                   "-DMANIFOLD_PAR=-1",
                   enable_cross_section ? "-DMANIFOLD_CROSS_SECTION=1" : "-DMANIFOLD_CROSS_SECTION=0",
                   "-DMANIFOLD_EXPORT=0",
                   "-c",
                   src_path,
                   "-o",
                   obj_path);
    if (sanitize_this_object) append_sanitizer_flags(&cmd, opt);
    if (enable_cross_section && clipper_include_dir != NULL) {
        nob_cmd_append(&cmd, nob_temp_sprintf("-I%s", clipper_include_dir));
    }
    if (enable_meshio && assimp != NULL) {
        nob_cmd_append(&cmd,
                       nob_temp_sprintf("-I%s", assimp->include_src_dir),
                       nob_temp_sprintf("-I%s", assimp->include_build_dir));
    }
    return nob_cmd_run(&cmd);
}

static bool compile_font_baker_object_if_needed(const char *src_path, const char *obj_path) {
    const char *deps[] = {
        src_path,
        "tools/freetype/config/ftmodule.h",
    };
    int rebuild = nob_needs_rebuild(obj_path, deps, NOB_ARRAY_LEN(deps));
    if (rebuild < 0) return false;
    if (rebuild == 0) return true;

    Nob_Cmd cmd = {0};
    nob_cmd_append(&cmd, "cc");
    nob_cmd_append(&cmd,
                   "-std=c99",
                   "-Wall",
                   "-Wextra",
                   "-Wpedantic",
                   "-O2",
                   "-DFT2_BUILD_LIBRARY",
                   "-I.",
                   "-Itools",
                   "-Ifreetype/include",
                   "-Ifreetype/src/base",
                   "-Ifreetype/src/sfnt",
                   "-Ifreetype/src/truetype",
                   "-Ifreetype/src/smooth",
                   "-Ifreetype/src/raster",
                   "-Ifreetype/src/psnames",
                   "-Ifreetype/src/gzip",
                   "-c",
                   src_path,
                   "-o",
                   obj_path);
    return nob_cmd_run(&cmd);
}

static bool build_font_baker(const char *baker_bin_path) {
    if (nob_file_exists("tools/freetype/config/ftmodule.h") == 0) {
        nob_log(NOB_ERROR, "Missing tools/freetype/config/ftmodule.h");
        return false;
    }
    if (nob_file_exists("freetype/include/ft2build.h") == 0) {
        nob_log(NOB_ERROR, "Missing freetype/include/ft2build.h");
        return false;
    }
    if (nob_file_exists(font_baker_tool_source) == 0) {
        nob_log(NOB_ERROR, "Missing %s", font_baker_tool_source);
        return false;
    }

    if (!nob_mkdir_if_not_exists("build")) return false;
    if (!nob_mkdir_if_not_exists("build/font_baker")) return false;

    Nob_File_Paths objects = {0};

    const char *tool_obj = "build/font_baker/font_baker_tool.o";
    nob_da_append(&objects, tool_obj);
    if (!compile_font_baker_object_if_needed(font_baker_tool_source, tool_obj)) return false;

    for (size_t i = 0; i < NOB_ARRAY_LEN(freetype_baker_sources); ++i) {
        const char *src = freetype_baker_sources[i];
        const char *base_name = nob_path_name(src);
        const char *obj = nob_temp_sprintf("build/font_baker/%.*s.o",
                                           (int)(strlen(base_name) - sizeof(".c") + 1),
                                           base_name);
        nob_da_append(&objects, obj);
        if (!compile_font_baker_object_if_needed(src, obj)) return false;
    }

    int relink = nob_needs_rebuild(baker_bin_path, objects.items, objects.count);
    if (relink < 0) return false;
    if (relink == 0) return true;

    Nob_Cmd link = {0};
    nob_cmd_append(&link, "cc");
    for (size_t i = 0; i < objects.count; ++i) {
        nob_cmd_append(&link, objects.items[i]);
    }
    nob_cmd_append(&link, "-o", baker_bin_path);
    return nob_cmd_run(&link);
}

static bool bake_funnel_sans_header(const char *baker_bin_path, const char *header_path) {
    const char *font_path = "Funnel_Sans/static/FunnelSans-Regular.ttf";
    if (nob_file_exists(font_path) == 0) {
        nob_log(NOB_ERROR, "Missing font at %s", font_path);
        return false;
    }

    const char *deps[] = {
        baker_bin_path,
        font_path,
    };
    int rebuild = nob_needs_rebuild(header_path, deps, NOB_ARRAY_LEN(deps));
    if (rebuild < 0) return false;
    if (rebuild == 0) return true;

    if (!nob_mkdir_if_not_exists("build/generated")) return false;

    Nob_Cmd cmd = {0};
    nob_cmd_append(&cmd, baker_bin_path, font_path, header_path, "64");
    return nob_cmd_run(&cmd);
}

int main(int argc, char **argv) {
    NOB_GO_REBUILD_URSELF(argc, argv);
    BuildOptions opt = {0};
    argc--;
    argv++;

    while (argc > 0) {
        const char *arg = nob_shift_args(&argc, &argv);
        if (strcmp(arg, "--asan") == 0) {
            opt.asan = true;
        } else if (strcmp(arg, "--asan-deps") == 0) {
            opt.asan_deps = true;
        } else if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            nob_log(NOB_INFO, "Usage: ./nob [--asan] [--asan-deps]");
            nob_log(NOB_INFO, "  --asan  Build vicad with ASan+UBSan instrumentation.");
            nob_log(NOB_INFO, "  --asan-deps  Also instrument manifold/clipper dependencies (requires --asan).");
            return 0;
        } else {
            nob_log(NOB_ERROR, "Unknown argument: %s", arg);
            nob_log(NOB_INFO, "Usage: ./nob [--asan] [--asan-deps]");
            return 1;
        }
    }
    if (opt.asan_deps && !opt.asan) {
        nob_log(NOB_ERROR, "--asan-deps requires --asan");
        return 1;
    }

    const Clipper2Info clipper2 = detect_clipper2();
    const AssimpInfo assimp = detect_assimp();
    const bool enable_cross_section = clipper2.found;
    const bool enable_meshio = assimp.found;
    const char *build_tag = enable_cross_section ? "cross_section" : "base";
    if (opt.asan) {
        build_tag = nob_temp_sprintf("%s_asan", build_tag);
    }
    const char *obj_root = nob_temp_sprintf("build/obj_%s", build_tag);
    const char *binary_mode_path = nob_temp_sprintf("build/vicad_%s", build_tag);
    const char *font_baker_bin = "build/font_baker/font_baker";
    const char *baked_font_header = "build/generated/funnel_sans_baked.h";

    if (enable_cross_section) {
        nob_log(NOB_INFO, "Clipper2 detected at %s; enabling MANIFOLD_CROSS_SECTION", clipper2.root);
    } else {
        nob_log(NOB_WARNING,
                "Clipper2 not found at %s; building without MANIFOLD_CROSS_SECTION. "
                "Set CLIPPER2_DIR to your clone root to enable it.",
                clipper2.root);
    }
    if (enable_meshio) {
        nob_log(NOB_INFO, "Assimp detected at %s; enabling manifold meshIO (.3mf/.glb export)", assimp.root);
    } else {
        nob_log(NOB_WARNING,
                "Assimp not found at %s; building without meshIO export support. "
                "Set ASSIMP_DIR to your clone root to enable it.",
                assimp.root);
    }
    if (opt.asan) {
        nob_log(NOB_INFO, "Sanitizers enabled (ASan+UBSan).");
        if (opt.asan_deps) {
            nob_log(NOB_INFO, "Dependency sanitizers enabled.");
        } else {
            nob_log(NOB_INFO, "Dependency sanitizers disabled; pass --asan-deps to enable.");
        }
    }

    if (!nob_mkdir_if_not_exists("build")) return 1;
    if (enable_meshio) {
        if (!build_assimp_if_needed(&assimp)) return 1;
    }
    if (!build_font_baker(font_baker_bin)) return 1;
    if (!bake_funnel_sans_header(font_baker_bin, baked_font_header)) return 1;

    if (!nob_mkdir_if_not_exists(obj_root)) return 1;
    if (!nob_mkdir_if_not_exists(nob_temp_sprintf("%s/src", obj_root))) return 1;
    if (!nob_mkdir_if_not_exists(nob_temp_sprintf("%s/manifold", obj_root))) return 1;
    if (!nob_mkdir_if_not_exists(nob_temp_sprintf("%s/manifold/src", obj_root))) return 1;
    if (enable_cross_section) {
        if (!nob_mkdir_if_not_exists(nob_temp_sprintf("%s/clipper2", obj_root))) return 1;
        if (!nob_mkdir_if_not_exists(nob_temp_sprintf("%s/clipper2/src", obj_root))) return 1;
    }

    Nob_File_Paths objects = {0};
    for (size_t i = 0; i < NOB_ARRAY_LEN(app_sources); ++i) {
        const char *src = app_sources[i];
        const char *base = src + sizeof("src/") - 1;
        const char *obj = nob_temp_sprintf("%s/src/%.*s.o",
                                           obj_root,
                                           (int)(strlen(base) - sizeof(".cpp") + 1),
                                           base);
        nob_da_append(&objects, obj);
        const char *extra_dep = strcmp(src, "src/main.cpp") == 0 ? baked_font_header : NULL;
        if (!compile_object_if_needed(src, obj, enable_cross_section, clipper2.include_dir,
                                      enable_meshio, &assimp, extra_dep, opt, opt.asan)) return 1;
    }

    for (size_t i = 0; i < NOB_ARRAY_LEN(manifold_sources); ++i) {
        const char *src = manifold_sources[i];
        const char *base = src + sizeof("manifold/src/") - 1;
        const char *obj = nob_temp_sprintf("%s/manifold/src/%.*s.o",
                                           obj_root,
                                           (int)(strlen(base) - sizeof(".cpp") + 1),
                                           base);
        nob_da_append(&objects, obj);
        if (!compile_object_if_needed(src, obj, enable_cross_section, clipper2.include_dir,
                                      enable_meshio, &assimp, NULL, opt, opt.asan && opt.asan_deps)) return 1;
    }

    if (enable_meshio) {
        const char *obj = nob_temp_sprintf("%s/manifold/src/meshIO.o", obj_root);
        nob_da_append(&objects, obj);
        const char *meshio_dep = nob_temp_sprintf("%s/assimp/config.h", assimp.include_build_dir);
        if (!compile_object_if_needed(manifold_meshio_source, obj, enable_cross_section, clipper2.include_dir,
                                      true, &assimp, meshio_dep, opt, opt.asan && opt.asan_deps)) return 1;
    }

    if (enable_cross_section) {
        const char *obj = nob_temp_sprintf("%s/manifold/src/cross_section.o", obj_root);
        nob_da_append(&objects, obj);
        if (!compile_object_if_needed(manifold_cross_section_source, obj, true, clipper2.include_dir,
                                      enable_meshio, &assimp, NULL, opt, opt.asan && opt.asan_deps)) return 1;

        for (size_t i = 0; i < NOB_ARRAY_LEN(clipper2.sources); ++i) {
            const char *src = clipper2.sources[i];
            const char *base_name = nob_path_name(src);
            const char *obj2 = nob_temp_sprintf("%s/clipper2/src/%.*s.o",
                                                obj_root,
                                                (int)(strlen(base_name) - sizeof(".cpp") + 1),
                                                base_name);
            nob_da_append(&objects, obj2);
            if (!compile_object_if_needed(src, obj2, true, clipper2.include_dir,
                                          enable_meshio, &assimp, NULL, opt, opt.asan && opt.asan_deps)) return 1;
        }
    }

    int relink = nob_needs_rebuild(binary_mode_path, objects.items, objects.count);
    if (relink < 0) return 1;
    if (relink == 0) {
        nob_log(NOB_INFO, "%s is up-to-date", binary_mode_path);
    } else {
        Nob_Cmd cmd = {0};
        nob_cmd_append(&cmd, "clang++");
        for (size_t i = 0; i < objects.count; ++i) {
            nob_cmd_append(&cmd, objects.items[i]);
        }
        if (enable_meshio) {
            nob_cmd_append(&cmd, assimp.lib_assimp);
            if (nob_file_exists(assimp.lib_zlib) != 0) {
                nob_cmd_append(&cmd, assimp.lib_zlib);
            }
        }
        append_sanitizer_flags(&cmd, opt);

#ifdef __APPLE__
        nob_cmd_append(&cmd,
                       "-framework", "Cocoa",
                       "-framework", "OpenGL",
                       "-framework", "IOKit",
                       "-framework", "CoreVideo");
#elif defined(__linux__)
        nob_cmd_append(&cmd, "-lX11", "-lXrandr", "-lGL", "-ldl", "-lm", "-lpthread");
#elif defined(_WIN32)
        nob_cmd_append(&cmd, "-lopengl32", "-lgdi32", "-luser32", "-lshell32");
#endif
        nob_cmd_append(&cmd, "-o", binary_mode_path);
        if (!nob_cmd_run(&cmd)) return 1;

        nob_log(NOB_INFO, "Built %s", binary_mode_path);
    }

    int copy_main = nob_needs_rebuild1("build/vicad", binary_mode_path);
    if (copy_main < 0) return 1;
    if (copy_main != 0) {
        if (!nob_copy_file(binary_mode_path, "build/vicad")) return 1;
    }

    nob_log(NOB_INFO, "Ready build/vicad (%s mode)", build_tag);
    if (opt.asan) {
        nob_log(NOB_INFO,
                "Run leak checks with: ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 ./build/vicad");
    }
    return 0;
}
