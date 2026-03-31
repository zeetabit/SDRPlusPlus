#pragma once
#include <api_version.h>

// Module capability flags — declares what kind of module this is.
enum ModuleCapability {
    MOD_CAP_SOURCE   = (1 << 0),
    MOD_CAP_SINK     = (1 << 1),
    MOD_CAP_DECODER  = (1 << 2),
    MOD_CAP_MISC     = (1 << 3),
};

// Dependency entry: a module this module requires to function.
struct ModuleDependency {
    const char* moduleName;
    int minApiVersion;  // 0 = any version
};

// Extended module info (V2). Includes API version, capabilities, and dependencies.
// The first 7 fields match ModuleInfo_t layout so the loader can detect V1 vs V2
// by checking the apiVersion field (V1 modules will have garbage there since
// they only define up to maxInstances).
struct ModuleInfoV2 {
    // --- V1-compatible fields ---
    const char* name;
    const char* description;
    const char* author;
    const int versionMajor;
    const int versionMinor;
    const int versionBuild;
    const int maxInstances;

    // --- V2 extensions ---
    const int apiVersion;           // SDRPP_API_VERSION at compile time
    const int capabilities;         // Bitfield of ModuleCapability
    const int dependencyCount;
    const ModuleDependency* dependencies;

    // --- V2.1 config extensions ---
    const char* configDefaults;     // JSON string of default config for new instances, or nullptr
    const char* configFileName;     // Config file name (e.g. "recorder_config.json"), or nullptr for auto
};

// Macro to declare a V2 module info block.
// Usage:
//   static const ModuleDependency deps[] = { {"radio", 0} };
//   SDRPP_MOD_INFO_V2 {
//       "my_module", "My Module", "Author", 1, 0, 0, -1,
//       SDRPP_API_VERSION, MOD_CAP_DECODER, 1, deps
//   };
//
// For modules with no dependencies:
//   SDRPP_MOD_INFO_V2 {
//       "my_module", "My Module", "Author", 1, 0, 0, -1,
//       SDRPP_API_VERSION, MOD_CAP_SOURCE, 0, nullptr
//   };
#define SDRPP_MOD_INFO_V2 MOD_EXPORT const ModuleInfoV2 _INFO_V2_

// Sentinel value: V2 modules export _INFO_V2_ in addition to _INFO_.
// The loader checks for _INFO_V2_ first; if found, it uses the extended struct.
// If not found, it falls back to _INFO_ (V1 behavior).
#define SDRPP_HAS_V2_INFO "_INFO_V2_"
