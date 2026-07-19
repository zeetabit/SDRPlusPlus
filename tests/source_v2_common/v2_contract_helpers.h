#pragma once

#include <catch.hpp>
#include <module_manifest.h>
#include <module.h>
#include <signal_path/isource.h>
#include <string>

// Module main.cpp under test exports these extern "C" symbols
// (via SDRPP_MOD_INFO_V2 / SDRPP_MOD_CONFIG / SDRPP_CREATE_INSTANCE_V2 etc.).
// Each source-module test exe links one module's main.cpp, so these
// resolve at link time without any dlopen.
extern "C" {
    extern const ModuleInfoV2 _INFO_V2_;
    extern const ModuleManager::ModuleInfo_t _INFO_;
    extern ConfigManager* _CONFIG_;
    extern ModuleManager::Instance* _CREATE_INSTANCE_(std::string name);
    extern ModuleManager::Instance* _CREATE_INSTANCE_V2_(std::string name, ModuleConfig* config);
    extern void _DELETE_INSTANCE_(ModuleManager::Instance* instance);
    extern void _INIT_();
    extern void _END_();
}

namespace v2contract {

    inline void requireSourceModuleInfo(const char* expectedName, const char* expectedConfigFile) {
        REQUIRE(_INFO_V2_.name != nullptr);
        REQUIRE(std::string(_INFO_V2_.name) == expectedName);
        REQUIRE(_INFO_V2_.apiVersion == SDRPP_API_VERSION);
        REQUIRE((_INFO_V2_.capabilities & MOD_CAP_SOURCE) != 0);
        REQUIRE(_INFO_V2_.configFileName != nullptr);
        REQUIRE(std::string(_INFO_V2_.configFileName) == expectedConfigFile);
    }

    inline void requireV2EntrypointsPresent() {
        REQUIRE(reinterpret_cast<void*>(&_CREATE_INSTANCE_V2_) != nullptr);
        REQUIRE(reinterpret_cast<void*>(&_DELETE_INSTANCE_) != nullptr);
        REQUIRE(reinterpret_cast<void*>(&_INIT_) != nullptr);
        REQUIRE(reinterpret_cast<void*>(&_END_) != nullptr);
    }

    // T3: the instance created via the V2 factory must be an ISource.
    // Returns the ISource* so the caller can chain further assertions, or
    // pass to a deleter (use deleteV2Instance below) to avoid leaks.
    inline ISource* requireV2InstanceIsSource(const std::string& name = "test") {
        ModuleManager::Instance* inst = _CREATE_INSTANCE_V2_(name, nullptr);
        REQUIRE(inst != nullptr);
        ISource* src = dynamic_cast<ISource*>(inst);
        REQUIRE(src != nullptr);
        return src;
    }

    inline void deleteV2Instance(ISource* src) {
        // Safe: ModuleManager::Instance is the primary base via single-inheritance
        // chain in every existing module. Cast back through it for _DELETE_INSTANCE_.
        _DELETE_INSTANCE_(dynamic_cast<ModuleManager::Instance*>(src));
    }
}
