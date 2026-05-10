#include "nxmt/startup.h"

int main(void) {
    return nxmt_startup_file_diagnostics_enabled() ? 1 : 0;
}
