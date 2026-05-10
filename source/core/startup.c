#include "nxmt/startup.h"

bool nxmt_startup_file_diagnostics_enabled(void) {
#ifdef NXMT_ENABLE_BOOT_FILE_DIAGNOSTICS
    return true;
#else
    return false;
#endif
}
