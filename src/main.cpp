#include <string>

#include "app_kernel.h"
#include "log.h"

int main() {
    AppKernel app = {};
    std::string err;
    if (!AppInit(&app, &err)) {
        if (!err.empty()) vicad::log_event("INIT_FAILED", 0, err.c_str());
        AppShutdown(&app);
        return 1;
    }

    int rc = 0;
    while (true) {
        err.clear();
        if (!AppTick(&app, &err)) {
            if (!err.empty()) {
                vicad::log_event("RUNTIME_ERROR", 0, err.c_str());
                rc = 1;
            }
            break;
        }
    }

    AppShutdown(&app);
    return rc;
}
