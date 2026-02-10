#define NOB_IMPLEMENTATION
#include "nob.h"

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

static bool compile_object_if_needed(const char *src_path, const char *obj_path) {
    int rebuild = nob_needs_rebuild1(obj_path, src_path);
    if (rebuild < 0) return false;
    if (rebuild == 0) return true;

    Nob_Cmd cmd = {0};
    nob_cmd_append(&cmd, "clang++");
    nob_cmd_append(&cmd,
                   "-std=c++17",
                   "-Wall",
                   "-Wextra",
                   "-Wpedantic",
                   "-O2",
                   "-I.",
                   "-Imanifold/include",
                   "-DMANIFOLD_PAR=-1",
                   "-DMANIFOLD_CROSS_SECTION=0",
                   "-DMANIFOLD_EXPORT=0",
                   "-c",
                   src_path,
                   "-o",
                   obj_path);
    return nob_cmd_run(&cmd);
}

int main(int argc, char **argv) {
    NOB_GO_REBUILD_URSELF(argc, argv);

    if (!nob_mkdir_if_not_exists("build")) return 1;
    if (!nob_mkdir_if_not_exists("build/obj")) return 1;
    if (!nob_mkdir_if_not_exists("build/obj/src")) return 1;
    if (!nob_mkdir_if_not_exists("build/obj/manifold")) return 1;
    if (!nob_mkdir_if_not_exists("build/obj/manifold/src")) return 1;

    Nob_File_Paths objects = {0};
    const char *main_src = "src/main.cpp";
    const char *main_obj = "build/obj/src/main.o";
    nob_da_append(&objects, main_obj);
    if (!compile_object_if_needed(main_src, main_obj)) return 1;

    for (size_t i = 0; i < NOB_ARRAY_LEN(manifold_sources); ++i) {
        const char *src = manifold_sources[i];
        const char *base = src + sizeof("manifold/src/") - 1;
        const char *obj = nob_temp_sprintf("build/obj/manifold/src/%.*s.o",
                                           (int)(strlen(base) - sizeof(".cpp") + 1),
                                           base);
        nob_da_append(&objects, obj);
        if (!compile_object_if_needed(src, obj)) return 1;
    }

    int relink = nob_needs_rebuild("build/vicad", objects.items, objects.count);
    if (relink < 0) return 1;
    if (relink == 0) {
        nob_log(NOB_INFO, "build/vicad is up-to-date");
        return 0;
    }

    Nob_Cmd cmd = {0};
    nob_cmd_append(&cmd, "clang++");
    for (size_t i = 0; i < objects.count; ++i) {
        nob_cmd_append(&cmd, objects.items[i]);
    }

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
    nob_cmd_append(&cmd, "-o", "build/vicad");
    if (!nob_cmd_run(&cmd)) return 1;

    nob_log(NOB_INFO, "Built build/vicad");
    return 0;
}
