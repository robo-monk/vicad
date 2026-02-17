#define NOB_IMPLEMENTATION
#include "nob.h"
#include <sys/stat.h>
#include <limits.h>
#include <stdlib.h>

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
    "src/app_kernel.cpp",
    "src/event_router.cpp",
    "src/interaction_state.cpp",
    "src/main.cpp",
    "src/picking.cpp",
    "src/edge_detection.cpp",
    "src/face_detection.cpp",
    "src/input_controller.cpp",
    "src/lod_policy.cpp",
    "src/op_decoder.cpp",
    "src/op_reader.cpp",
    "src/op_trace.cpp",
    "src/render_scene.cpp",
    "src/render_ui.cpp",
    "src/renderer_3d.cpp",
    "src/renderer_overlay.cpp",
    "src/scene_session.cpp",
    "src/script_worker_client.cpp",
    "src/scene_runtime.cpp",
    "src/sketch_semantics.cpp",
    "src/sketch_dimensions.cpp",
    "src/ui_layout.cpp",
    "src/ui_state.cpp",
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
    const char *root;
    const char *include_dir;
    const char *amalgamated_source;
    bool found;
} HarfBuzzInfo;

typedef struct {
    const char *root;
    const char *include_dir;
    const char *common_source;
    const char *platform_source;
    bool found;
} NfdInfo;

typedef struct {
    bool asan;
    bool asan_deps;
    int max_procs;
} BuildOptions;

typedef enum {
    COMPILE_LANG_C,
    COMPILE_LANG_CXX,
    COMPILE_LANG_OBJC,
} CompileLang;

typedef enum {
    COMPILE_GROUP_APP,
    COMPILE_GROUP_MANIFOLD,
    COMPILE_GROUP_CLIPPER,
    COMPILE_GROUP_HARFBUZZ,
    COMPILE_GROUP_FREETYPE_RUNTIME,
    COMPILE_GROUP_NFD,
    COMPILE_GROUP_FONT_BAKER,
} CompileGroup;

typedef struct {
    const char *src_path;
    const char *obj_path;
    CompileLang lang;
    CompileGroup group;
    bool sanitize;
    const char *extra_dependencies[4];
    size_t extra_dependency_count;
} CompileUnit;

typedef struct {
    CompileUnit *items;
    size_t count;
    size_t capacity;
} CompileUnits;

typedef struct {
    CompileUnits units;
    Nob_File_Paths objects;
} BuildPlan;

typedef struct {
    BuildOptions opt;
    size_t max_procs;
    const char *obj_root;
    const char *binary_path;

    bool enable_cross_section;
    bool enable_meshio;
    bool enable_harfbuzz;
    bool enable_nfd;

    const Clipper2Info *clipper2;
    const AssimpInfo *assimp;
    const HarfBuzzInfo *harfbuzz;
    const NfdInfo *nfd;
} BuildContext;

static void append_sanitizer_flags(Nob_Cmd *cmd, BuildOptions opt) {
    if (!opt.asan) return;
    nob_cmd_append(cmd,
                   "-O1",
                   "-g",
                   "-fno-omit-frame-pointer",
                   "-shared-libsan",
                   "-fsanitize=address",
                   "-fsanitize=undefined",
                   "-fno-sanitize-recover=all");
}

static void append_common_cxx_flags(Nob_Cmd *cmd) {
    nob_cmd_append(cmd,
                   "-std=c++20",
                   "-Wall",
                   "-Wextra",
                   "-Wpedantic",
                   "-O2");
}

static void append_common_c_flags(Nob_Cmd *cmd) {
    nob_cmd_append(cmd,
                   "-std=c99",
                   "-Wall",
                   "-Wextra",
                   "-Wpedantic",
                   "-O2");
}

static void append_common_freetype_include_flags(Nob_Cmd *cmd) {
    nob_cmd_append(cmd,
                   "-I.",
                   "-Itools",
                   "-Ifreetype/include",
                   "-Ifreetype/src/base",
                   "-Ifreetype/src/sfnt",
                   "-Ifreetype/src/truetype",
                   "-Ifreetype/src/smooth",
                   "-Ifreetype/src/raster",
                   "-Ifreetype/src/psnames",
                   "-Ifreetype/src/gzip");
}

static void append_feature_includes_defs(const BuildContext *ctx, Nob_Cmd *cmd) {
    if (ctx->enable_cross_section && ctx->clipper2 && ctx->clipper2->include_dir) {
        nob_cmd_append(cmd, nob_temp_sprintf("-I%s", ctx->clipper2->include_dir));
    }
    if (ctx->enable_meshio && ctx->assimp) {
        nob_cmd_append(cmd,
                       nob_temp_sprintf("-I%s", ctx->assimp->include_src_dir),
                       nob_temp_sprintf("-I%s", ctx->assimp->include_build_dir));
    }
    if (ctx->enable_harfbuzz && ctx->harfbuzz && ctx->harfbuzz->include_dir) {
        nob_cmd_append(cmd,
                       "-DVICAD_HAS_HARFBUZZ=1",
                       nob_temp_sprintf("-I%s", ctx->harfbuzz->include_dir),
                       "-Ifreetype/include");
    }
    if (ctx->enable_nfd && ctx->nfd && ctx->nfd->include_dir) {
        nob_cmd_append(cmd,
                       "-DVICAD_HAS_NFD=1",
                       nob_temp_sprintf("-I%s", ctx->nfd->include_dir));
    }
}

static bool read_command_first_line(const char *command, char *out, size_t out_cap) {
    if (!command || !out || out_cap == 0) return false;
    FILE *fp = popen(command, "r");
    if (!fp) return false;
    if (!fgets(out, (int)out_cap, fp)) {
        pclose(fp);
        return false;
    }
    pclose(fp);
    size_t n = strlen(out);
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r')) {
        out[n - 1] = '\0';
        n--;
    }
    return n > 0;
}

static const char *dir_of_path(const char *path) {
    if (!path || path[0] == '\0') return "";
    const char *slash = strrchr(path, '/');
    if (!slash) return ".";
    return nob_temp_sprintf("%.*s", (int)(slash - path), path);
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

static const char *path_stem(const char *path) {
    const char *base = nob_path_name(path);
    const char *dot = strrchr(base, '.');
    int stem_len = dot ? (int)(dot - base) : (int)strlen(base);
    return nob_temp_sprintf("%.*s", stem_len, base);
}

static const char *make_obj_path(const char *obj_root, const char *subdir, const char *src_path) {
    return nob_temp_sprintf("%s/%s/%s.o", obj_root, subdir, path_stem(src_path));
}

static bool append_compile_unit(BuildPlan *plan, CompileUnit unit, bool add_to_link_objects) {
    nob_da_append(&plan->units, unit);
    if (add_to_link_objects) {
        nob_da_append(&plan->objects, unit.obj_path);
    }
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

static NfdInfo detect_nativefiledialog(void) {
    NfdInfo info = {0};
    info.root = getenv("NATIVEFILEDIALOG_DIR");
    if (info.root == NULL || info.root[0] == '\0') {
        info.root = "nativefiledialog";
    }
    info.include_dir = nob_temp_sprintf("%s/src/include", info.root);
    info.common_source = nob_temp_sprintf("%s/src/nfd_common.c", info.root);
#if defined(__APPLE__)
    info.platform_source = nob_temp_sprintf("%s/src/nfd_cocoa.m", info.root);
#elif defined(_WIN32)
    info.platform_source = nob_temp_sprintf("%s/src/nfd_win.cpp", info.root);
#else
    info.platform_source = nob_temp_sprintf("%s/src/nfd_gtk.c", info.root);
#endif

    const char *header = nob_temp_sprintf("%s/nfd.h", info.include_dir);
    info.found = nob_file_exists(header) != 0 &&
                 nob_file_exists(info.common_source) != 0 &&
                 nob_file_exists(info.platform_source) != 0;
    return info;
}

static HarfBuzzInfo detect_harfbuzz(void) {
    HarfBuzzInfo info = {0};
    info.root = getenv("HARFBUZZ_DIR");
    if (info.root == NULL || info.root[0] == '\0') {
        info.root = "harfbuzz";
    }

    info.include_dir = nob_temp_sprintf("%s/src", info.root);
    info.amalgamated_source = nob_temp_sprintf("%s/src/harfbuzz.cc", info.root);
    const char *header = nob_temp_sprintf("%s/hb.h", info.include_dir);
    info.found = nob_file_exists(info.amalgamated_source) != 0 &&
                 nob_file_exists(header) != 0;
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

static int needs_rebuild_unit(const BuildContext *ctx, const CompileUnit *unit) {
    if (unit->group == COMPILE_GROUP_HARFBUZZ && ctx->harfbuzz) {
        long long obj_mtime = 0;
        const bool have_obj = file_mtime_ns(unit->obj_path, &obj_mtime);
        long long newest_hb_src = 0;
        if (!newest_file_mtime_ns_recursive(ctx->harfbuzz->include_dir, &newest_hb_src)) return -1;
        return (have_obj && newest_hb_src <= obj_mtime) ? 0 : 1;
    }

    if (unit->extra_dependency_count > 0) {
        const size_t dep_count = 1 + unit->extra_dependency_count;
        const char *deps[1 + NOB_ARRAY_LEN(unit->extra_dependencies)] = {0};
        deps[0] = unit->src_path;
        for (size_t i = 0; i < unit->extra_dependency_count; ++i) {
            deps[i + 1] = unit->extra_dependencies[i];
        }
        return nob_needs_rebuild(unit->obj_path, deps, dep_count);
    }

    return nob_needs_rebuild1(unit->obj_path, unit->src_path);
}

static void build_compile_cmd(const BuildContext *ctx, const CompileUnit *unit, Nob_Cmd *cmd) {
    const bool is_nfd_group = unit->group == COMPILE_GROUP_NFD;
    const bool is_cc_group = unit->group == COMPILE_GROUP_FREETYPE_RUNTIME || unit->group == COMPILE_GROUP_FONT_BAKER;
    if (is_nfd_group) {
        nob_cmd_append(cmd, "clang");
    } else if (is_cc_group) {
        nob_cmd_append(cmd, "cc");
    } else if (unit->lang == COMPILE_LANG_CXX) {
        nob_cmd_append(cmd, "clang++");
    } else {
        nob_cmd_append(cmd, "cc");
    }

    if (unit->lang == COMPILE_LANG_OBJC) {
        nob_cmd_append(cmd, "-x", "objective-c");
    }

    switch (unit->group) {
    case COMPILE_GROUP_APP:
    case COMPILE_GROUP_MANIFOLD:
    case COMPILE_GROUP_CLIPPER:
        append_common_cxx_flags(cmd);
        nob_cmd_append(cmd,
                       "-I.",
                       "-Ibuild/generated",
                       "-Imanifold/include",
                       "-DMANIFOLD_PAR=-1",
                       ctx->enable_cross_section ? "-DMANIFOLD_CROSS_SECTION=1" : "-DMANIFOLD_CROSS_SECTION=0",
                       "-DMANIFOLD_EXPORT=0");
        append_feature_includes_defs(ctx, cmd);
        break;
    case COMPILE_GROUP_HARFBUZZ:
        nob_cmd_append(cmd,
                       "-std=c++20",
                       "-O2",
                       "-DHAVE_FREETYPE=1",
                       "-I.",
                       "-Ifreetype/include",
                       nob_temp_sprintf("-I%s", ctx->harfbuzz->include_dir));
        break;
    case COMPILE_GROUP_FREETYPE_RUNTIME:
        append_common_c_flags(cmd);
        nob_cmd_append(cmd, "-DFT2_BUILD_LIBRARY");
        append_common_freetype_include_flags(cmd);
        break;
    case COMPILE_GROUP_NFD:
        append_common_c_flags(cmd);
        nob_cmd_append(cmd,
                       "-I.",
                       nob_temp_sprintf("-I%s", ctx->nfd->include_dir));
        break;
    case COMPILE_GROUP_FONT_BAKER:
        append_common_c_flags(cmd);
        nob_cmd_append(cmd, "-DFT2_BUILD_LIBRARY");
        append_common_freetype_include_flags(cmd);
        break;
    }

    nob_cmd_append(cmd,
                   "-c",
                   unit->src_path,
                   "-o",
                   unit->obj_path);

    if (unit->sanitize) {
        append_sanitizer_flags(cmd, ctx->opt);
    }
}

static bool run_compile_units_parallel(const BuildContext *ctx, BuildPlan *plan) {
    size_t dirty_count = 0;
    size_t skipped_count = 0;
    bool success = true;
    Nob_Procs procs = {0};

    for (size_t i = 0; i < plan->units.count; ++i) {
        int rebuild = needs_rebuild_unit(ctx, &plan->units.items[i]);
        if (rebuild < 0) {
            success = false;
            break;
        }
        if (rebuild == 0) {
            skipped_count += 1;
            continue;
        }

        dirty_count += 1;
        Nob_Cmd cmd = {0};
        build_compile_cmd(ctx, &plan->units.items[i], &cmd);

        if (ctx->max_procs > 0) {
            if (!nob_cmd_run(&cmd, .async = &procs, .max_procs = ctx->max_procs)) {
                success = false;
                break;
            }
        } else {
            if (!nob_cmd_run(&cmd, .async = &procs)) {
                success = false;
                break;
            }
        }
    }

    nob_log(NOB_INFO,
            "Compilation plan: total=%zu dirty=%zu skipped=%zu",
            plan->units.count,
            dirty_count,
            skipped_count);

    if (!nob_procs_flush(&procs)) {
        success = false;
    }

    return success;
}

static bool append_app_units(BuildPlan *plan, const BuildContext *ctx, const char *baked_font_header) {
    for (size_t i = 0; i < NOB_ARRAY_LEN(app_sources); ++i) {
        const char *src = app_sources[i];
        const char *obj = make_obj_path(ctx->obj_root, "src", src);
        CompileUnit unit = {0};
        unit.src_path = src;
        unit.obj_path = obj;
        unit.lang = COMPILE_LANG_CXX;
        unit.group = COMPILE_GROUP_APP;
        unit.sanitize = ctx->opt.asan;
        if (strcmp(src, "src/app_kernel.cpp") == 0) {
            unit.extra_dependencies[unit.extra_dependency_count++] = baked_font_header;
        }
        if (!append_compile_unit(plan, unit, true)) return false;
    }
    return true;
}

static bool append_manifold_units(BuildPlan *plan, const BuildContext *ctx) {
    for (size_t i = 0; i < NOB_ARRAY_LEN(manifold_sources); ++i) {
        const char *src = manifold_sources[i];
        const char *obj = make_obj_path(ctx->obj_root, "manifold/src", src);
        CompileUnit unit = {0};
        unit.src_path = src;
        unit.obj_path = obj;
        unit.lang = COMPILE_LANG_CXX;
        unit.group = COMPILE_GROUP_MANIFOLD;
        unit.sanitize = ctx->opt.asan && ctx->opt.asan_deps;
        if (!append_compile_unit(plan, unit, true)) return false;
    }
    return true;
}

static bool append_optional_units(BuildPlan *plan, const BuildContext *ctx) {
    if (ctx->enable_meshio && ctx->assimp) {
        CompileUnit unit = {0};
        unit.src_path = manifold_meshio_source;
        unit.obj_path = make_obj_path(ctx->obj_root, "manifold/src", manifold_meshio_source);
        unit.lang = COMPILE_LANG_CXX;
        unit.group = COMPILE_GROUP_MANIFOLD;
        unit.sanitize = ctx->opt.asan && ctx->opt.asan_deps;
        unit.extra_dependencies[unit.extra_dependency_count++] = nob_temp_sprintf("%s/assimp/config.h", ctx->assimp->include_build_dir);
        if (!append_compile_unit(plan, unit, true)) return false;
    }

    if (ctx->enable_cross_section) {
        CompileUnit cross = {0};
        cross.src_path = manifold_cross_section_source;
        cross.obj_path = make_obj_path(ctx->obj_root, "manifold/src", manifold_cross_section_source);
        cross.lang = COMPILE_LANG_CXX;
        cross.group = COMPILE_GROUP_MANIFOLD;
        cross.sanitize = ctx->opt.asan && ctx->opt.asan_deps;
        if (!append_compile_unit(plan, cross, true)) return false;

        for (size_t i = 0; i < NOB_ARRAY_LEN(ctx->clipper2->sources); ++i) {
            CompileUnit clip = {0};
            clip.src_path = ctx->clipper2->sources[i];
            clip.obj_path = make_obj_path(ctx->obj_root, "clipper2/src", ctx->clipper2->sources[i]);
            clip.lang = COMPILE_LANG_CXX;
            clip.group = COMPILE_GROUP_CLIPPER;
            clip.sanitize = ctx->opt.asan && ctx->opt.asan_deps;
            if (!append_compile_unit(plan, clip, true)) return false;
        }
    }

    if (ctx->enable_harfbuzz && ctx->harfbuzz) {
        CompileUnit hb = {0};
        hb.src_path = ctx->harfbuzz->amalgamated_source;
        hb.obj_path = make_obj_path(ctx->obj_root, "harfbuzz", ctx->harfbuzz->amalgamated_source);
        hb.lang = COMPILE_LANG_CXX;
        hb.group = COMPILE_GROUP_HARFBUZZ;
        hb.sanitize = ctx->opt.asan && ctx->opt.asan_deps;
        if (!append_compile_unit(plan, hb, true)) return false;

        for (size_t i = 0; i < NOB_ARRAY_LEN(freetype_baker_sources); ++i) {
            CompileUnit ft = {0};
            ft.src_path = freetype_baker_sources[i];
            ft.obj_path = make_obj_path(ctx->obj_root, "freetype", freetype_baker_sources[i]);
            ft.lang = COMPILE_LANG_C;
            ft.group = COMPILE_GROUP_FREETYPE_RUNTIME;
            ft.sanitize = ctx->opt.asan && ctx->opt.asan_deps;
            if (!append_compile_unit(plan, ft, true)) return false;
        }
    }

    if (ctx->enable_nfd && ctx->nfd) {
        CompileUnit common = {0};
        common.src_path = ctx->nfd->common_source;
        common.obj_path = make_obj_path(ctx->obj_root, "nativefiledialog", ctx->nfd->common_source);
        common.lang = COMPILE_LANG_C;
        common.group = COMPILE_GROUP_NFD;
        common.sanitize = ctx->opt.asan && ctx->opt.asan_deps;
        if (!append_compile_unit(plan, common, true)) return false;

        CompileUnit platform = {0};
        platform.src_path = ctx->nfd->platform_source;
        platform.obj_path = make_obj_path(ctx->obj_root, "nativefiledialog", ctx->nfd->platform_source);
#if defined(__APPLE__)
        platform.lang = COMPILE_LANG_OBJC;
#else
        platform.lang = COMPILE_LANG_C;
#endif
        platform.group = COMPILE_GROUP_NFD;
        platform.sanitize = ctx->opt.asan && ctx->opt.asan_deps;
        if (!append_compile_unit(plan, platform, true)) return false;
    }

    return true;
}

static bool append_font_baker_units(BuildPlan *plan, const char *obj_root) {
    CompileUnit tool = {0};
    tool.src_path = font_baker_tool_source;
    tool.obj_path = make_obj_path(obj_root, ".", font_baker_tool_source);
    tool.lang = COMPILE_LANG_C;
    tool.group = COMPILE_GROUP_FONT_BAKER;
    tool.sanitize = false;
    tool.extra_dependencies[tool.extra_dependency_count++] = "tools/freetype/config/ftmodule.h";
    if (!append_compile_unit(plan, tool, false)) return false;

    for (size_t i = 0; i < NOB_ARRAY_LEN(freetype_baker_sources); ++i) {
        CompileUnit ft = {0};
        ft.src_path = freetype_baker_sources[i];
        ft.obj_path = make_obj_path(obj_root, ".", freetype_baker_sources[i]);
        ft.lang = COMPILE_LANG_C;
        ft.group = COMPILE_GROUP_FONT_BAKER;
        ft.sanitize = false;
        ft.extra_dependencies[ft.extra_dependency_count++] = "tools/freetype/config/ftmodule.h";
        if (!append_compile_unit(plan, ft, false)) return false;
    }

    return true;
}

static bool build_font_baker(const BuildOptions *opt, const char *baker_bin_path) {
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

    BuildContext ctx = {0};
    ctx.opt = *opt;
    ctx.max_procs = opt->max_procs > 0 ? (size_t)opt->max_procs : 0;

    BuildPlan plan = {0};
    if (!append_font_baker_units(&plan, "build/font_baker")) return false;

    if (!run_compile_units_parallel(&ctx, &plan)) {
        nob_log(NOB_ERROR, "Font baker object compilation failed");
        return false;
    }

    Nob_File_Paths objects = {0};
    for (size_t i = 0; i < plan.units.count; ++i) {
        nob_da_append(&objects, plan.units.items[i].obj_path);
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
    nob_cmd_append(&cmd, baker_bin_path, font_path, header_path, "32");
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
        } else if (strcmp(arg, "--max-procs") == 0) {
            if (argc <= 0) {
                nob_log(NOB_ERROR, "--max-procs expects an integer argument");
                return 1;
            }
            const char *value = nob_shift_args(&argc, &argv);
            opt.max_procs = atoi(value);
        } else if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            nob_log(NOB_INFO, "Usage: ./nob [--asan] [--asan-deps] [--max-procs N]");
            nob_log(NOB_INFO, "  --asan  Build vicad with ASan+UBSan instrumentation.");
            nob_log(NOB_INFO, "  --asan-deps  Also instrument manifold/clipper dependencies (requires --asan).");
            nob_log(NOB_INFO, "  --max-procs N  Limit concurrent compiler processes (N <= 0 uses Nob default).");
            return 0;
        } else {
            nob_log(NOB_ERROR, "Unknown argument: %s", arg);
            nob_log(NOB_INFO, "Usage: ./nob [--asan] [--asan-deps] [--max-procs N]");
            return 1;
        }
    }
    if (opt.asan_deps && !opt.asan) {
        nob_log(NOB_ERROR, "--asan-deps requires --asan");
        return 1;
    }

    const Clipper2Info clipper2 = detect_clipper2();
    const AssimpInfo assimp = detect_assimp();
    const HarfBuzzInfo harfbuzz = detect_harfbuzz();
    const NfdInfo nfd = detect_nativefiledialog();

    const bool enable_cross_section = clipper2.found;
    const bool enable_meshio = assimp.found;
    const bool enable_harfbuzz = harfbuzz.found;
    const bool enable_nfd = nfd.found;

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
    if (enable_harfbuzz) {
        nob_log(NOB_INFO, "HarfBuzz detected at %s; enabling text shaping library build (amalgamated src/harfbuzz.cc)", harfbuzz.root);
    } else {
        nob_log(NOB_WARNING,
                "HarfBuzz not found at %s; shaping library build disabled. "
                "Set HARFBUZZ_DIR to your clone root to enable it.",
                harfbuzz.root);
    }
    if (enable_nfd) {
        nob_log(NOB_INFO, "nativefiledialog detected at %s; enabling native Open dialog", nfd.root);
    } else {
        nob_log(NOB_WARNING,
                "nativefiledialog not found at %s; File/Open dialog will be disabled. "
                "Set NATIVEFILEDIALOG_DIR to your clone root to enable it.",
                nfd.root);
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
    if (!build_font_baker(&opt, font_baker_bin)) return 1;
    if (!bake_funnel_sans_header(font_baker_bin, baked_font_header)) return 1;

    if (!nob_mkdir_if_not_exists(obj_root)) return 1;
    if (!nob_mkdir_if_not_exists(nob_temp_sprintf("%s/src", obj_root))) return 1;
    if (!nob_mkdir_if_not_exists(nob_temp_sprintf("%s/manifold", obj_root))) return 1;
    if (!nob_mkdir_if_not_exists(nob_temp_sprintf("%s/manifold/src", obj_root))) return 1;
    if (enable_cross_section) {
        if (!nob_mkdir_if_not_exists(nob_temp_sprintf("%s/clipper2", obj_root))) return 1;
        if (!nob_mkdir_if_not_exists(nob_temp_sprintf("%s/clipper2/src", obj_root))) return 1;
    }
    if (enable_nfd) {
        if (!nob_mkdir_if_not_exists(nob_temp_sprintf("%s/nativefiledialog", obj_root))) return 1;
    }
    if (enable_harfbuzz) {
        if (!nob_mkdir_if_not_exists(nob_temp_sprintf("%s/harfbuzz", obj_root))) return 1;
        if (!nob_mkdir_if_not_exists(nob_temp_sprintf("%s/freetype", obj_root))) return 1;
    }

    BuildContext ctx = {0};
    ctx.opt = opt;
    ctx.max_procs = opt.max_procs > 0 ? (size_t)opt.max_procs : 0;
    ctx.obj_root = obj_root;
    ctx.binary_path = binary_mode_path;
    ctx.enable_cross_section = enable_cross_section;
    ctx.enable_meshio = enable_meshio;
    ctx.enable_harfbuzz = enable_harfbuzz;
    ctx.enable_nfd = enable_nfd;
    ctx.clipper2 = &clipper2;
    ctx.assimp = &assimp;
    ctx.harfbuzz = &harfbuzz;
    ctx.nfd = &nfd;

    BuildPlan plan = {0};
    if (!append_app_units(&plan, &ctx, baked_font_header)) return 1;
    if (!append_manifold_units(&plan, &ctx)) return 1;
    if (!append_optional_units(&plan, &ctx)) return 1;

    if (!run_compile_units_parallel(&ctx, &plan)) {
        nob_log(NOB_ERROR, "Compilation failed");
        return 1;
    }

    int relink = nob_needs_rebuild(binary_mode_path, plan.objects.items, plan.objects.count);
    if (relink < 0) return 1;
    if (relink == 0) {
        nob_log(NOB_INFO, "%s is up-to-date", binary_mode_path);
    } else {
        Nob_Cmd cmd = {0};
        nob_cmd_append(&cmd, "clang++");
        for (size_t i = 0; i < plan.objects.count; ++i) {
            nob_cmd_append(&cmd, plan.objects.items[i]);
        }
        if (enable_meshio) {
            nob_cmd_append(&cmd, assimp.lib_assimp);
            if (nob_file_exists(assimp.lib_zlib) != 0) {
                nob_cmd_append(&cmd, assimp.lib_zlib);
            }
        }
        append_sanitizer_flags(&cmd, opt);

#ifdef __APPLE__
        if (opt.asan) {
            char asan_dylib[PATH_MAX] = {0};
            char ubsan_dylib[PATH_MAX] = {0};
            if (read_command_first_line("clang++ --print-file-name=libclang_rt.asan_osx_dynamic.dylib",
                                        asan_dylib, sizeof(asan_dylib))) {
                const char *asan_dir = dir_of_path(asan_dylib);
                nob_cmd_append(&cmd, "-Wl,-rpath", nob_temp_sprintf("%s", asan_dir), asan_dylib);
            }
            if (read_command_first_line("clang++ --print-file-name=libclang_rt.ubsan_osx_dynamic.dylib",
                                        ubsan_dylib, sizeof(ubsan_dylib))) {
                nob_cmd_append(&cmd, ubsan_dylib);
            }
        }
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
