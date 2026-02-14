#ifndef VICAD_APP_KERNEL_H_
#define VICAD_APP_KERNEL_H_

#include <string>

struct AppKernel {
    void *impl = nullptr;
};

bool AppInit(AppKernel *app, std::string *error);
bool AppTick(AppKernel *app, std::string *error);
void AppShutdown(AppKernel *app);

#endif  // VICAD_APP_KERNEL_H_
