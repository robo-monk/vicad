#include <cstdio>
#include <string>

#include "app_kernel.h"

int main() {
    AppKernel app = {};
    std::string err;
    if (!AppInit(&app, &err)) {
        if (!err.empty()) std::fprintf(stderr, "[vicad] init failed: %s\n", err.c_str());
        AppShutdown(&app);
        return 1;
    }

    int rc = 0;
    while (true) {
        err.clear();
        if (!AppTick(&app, &err)) {
            if (!err.empty()) {
                std::fprintf(stderr, "[vicad] runtime error: %s\n", err.c_str());
                rc = 1;
            }
            break;
        }
    }

    AppShutdown(&app);
    return rc;
}
