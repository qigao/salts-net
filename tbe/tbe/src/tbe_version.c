#include "tbe_version.h"

const char *tbe_version(void) {
    return TBE_VERSION_STRING;
}

void tbe_version_components(int *major, int *minor, int *patch) {
    if (major) *major = TBE_VERSION_MAJOR;
    if (minor) *minor = TBE_VERSION_MINOR;
    if (patch) *patch = TBE_VERSION_PATCH;
}
