#pragma once
#include <string>
#include <map>
#include <json.hpp>
#include <utils/event.h>
#include <api_version.h>
#include <module_manifest.h>

#ifdef _WIN32
#ifdef SDRPP_IS_CORE
#define SDRPP_EXPORT extern "C" __declspec(dllexport)
#else
#define SDRPP_EXPORT extern "C" __declspec(dllimport)
#endif
#else
#define SDRPP_EXPORT extern
#endif

#ifdef _WIN32
#include <Windows.h>
#define MOD_EXPORT           extern "C" __declspec(dllexport)
#define SDRPP_MOD_EXTENTSION ".dll"
#else
#include <dlfcn.h>
#define MOD_EXPORT extern "C"
#ifdef __APPLE__
#define SDRPP_MOD_EXTENTSION ".dylib"
#else
#define SDRPP_MOD_EXTENTSION ".so"
#endif
#endif

class ModuleManager {
public:
    // V1 module info (kept for backward compatibility)
    struct ModuleInfo_t {
        const char* name;
        const char* description;
        const char* author;
        const int versionMajor;
        const int versionMinor;
        const int versionBuild;
        const int maxInstances;
    };

    class Instance {
    public:
        virtual ~Instance() {}
        virtual void postInit() = 0;
        virtual void enable() = 0;
        virtual void disable() = 0;
        virtual bool isEnabled() = 0;
    };

    struct Module_t {
#ifdef _WIN32
        HMODULE handle;
#else
        void* handle;
#endif
        ModuleManager::ModuleInfo_t* info;
        ModuleInfoV2* infoV2;  // Non-null if module exports V2 manifest
        void (*init)();
        ModuleManager::Instance* (*createInstance)(std::string name);
        void (*deleteInstance)(ModuleManager::Instance* instance);
        void (*end)();

        bool isV2() const { return infoV2 != nullptr; }

        int apiVersion() const {
            return isV2() ? infoV2->apiVersion : SDRPP_MAKE_API_VERSION(1, 0, 0);
        }

        int capabilities() const {
            return isV2() ? infoV2->capabilities : 0;
        }

        int dependencyCount() const {
            return isV2() ? infoV2->dependencyCount : 0;
        }

        const ModuleDependency* dependencies() const {
            return isV2() ? infoV2->dependencies : nullptr;
        }

        friend bool operator==(const Module_t& a, const Module_t& b) {
            if (a.handle != b.handle) { return false; }
            if (a.info != b.info) { return false; }
            if (a.init != b.init) { return false; }
            if (a.createInstance != b.createInstance) { return false; }
            if (a.deleteInstance != b.deleteInstance) { return false; }
            if (a.end != b.end) { return false; }
            return true;
        }
    };

    struct Instance_t {
        ModuleManager::Module_t module;
        ModuleManager::Instance* instance;
        bool faulted = false;
        std::string faultError;
    };

    ModuleManager::Module_t loadModule(std::string path);

    int createInstance(std::string name, std::string module);
    int deleteInstance(std::string name);
    int deleteInstance(ModuleManager::Instance* instance);

    int enableInstance(std::string name);
    int disableInstance(std::string name);
    bool instanceEnabled(std::string name);
    void postInit(std::string name);
    std::string getInstanceModuleName(std::string name);

    int countModuleInstances(std::string module);

    bool checkDependencies(const Module_t& mod);

    void doPostInitAll();

    Event<std::string> onInstanceCreated;
    Event<std::string> onInstanceDelete;
    Event<std::string> onInstanceDeleted;

    std::map<std::string, ModuleManager::Module_t> modules;
    std::map<std::string, ModuleManager::Instance_t> instances;
};

// V1 module info macro (still works for all existing modules)
#define SDRPP_MOD_INFO MOD_EXPORT const ModuleManager::ModuleInfo_t _INFO_