#pragma once

// SDR++ Module API version.
// Bump MAJOR when breaking binary compatibility (struct layout, removed symbols).
// Bump MINOR when adding new optional features modules can use.
// Bump PATCH for non-breaking fixes in the core API headers.
#define SDRPP_API_VERSION_MAJOR 2
#define SDRPP_API_VERSION_MINOR 0
#define SDRPP_API_VERSION_PATCH 0

#define SDRPP_API_VERSION ((SDRPP_API_VERSION_MAJOR << 16) | (SDRPP_API_VERSION_MINOR << 8) | SDRPP_API_VERSION_PATCH)

#define SDRPP_MAKE_API_VERSION(major, minor, patch) (((major) << 16) | ((minor) << 8) | (patch))

// Check that a module's required API version is compatible with the running core.
// Compatible means: same major, module minor <= core minor.
inline bool sdrppApiCompatible(int moduleApiVersion) {
    int modMajor = (moduleApiVersion >> 16) & 0xFF;
    int modMinor = (moduleApiVersion >> 8) & 0xFF;
    return (modMajor == SDRPP_API_VERSION_MAJOR) && (modMinor <= SDRPP_API_VERSION_MINOR);
}
