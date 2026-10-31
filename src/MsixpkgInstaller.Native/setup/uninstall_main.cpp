#include <Windows.h>

#include "setup_entry.h"

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    return msixpkg::setup::run_setup_process(instance, true);
}
