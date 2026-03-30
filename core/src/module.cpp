#include <module.h>
#include <filesystem>
#include <utils/flog.h>
#include <utils/event_bus.h>
#include <utils/events.h>

ModuleManager::Module_t ModuleManager::loadModule(std::string path) {
    Module_t mod;
    mod.infoV2 = nullptr;

    // On android, the path has to be relative, don't make it absolute
#ifndef __ANDROID__
    if (!std::filesystem::exists(path)) {
        flog::error("{0} does not exist", path);
        mod.handle = NULL;
        return mod;
    }
    if (!std::filesystem::is_regular_file(path)) {
        flog::error("{0} isn't a loadable module", path);
        mod.handle = NULL;
        return mod;
    }
#endif
#ifdef _WIN32
    mod.handle = LoadLibraryA(path.c_str());
    if (mod.handle == NULL) {
        flog::error("Couldn't load {0}. Error code: {1}", path, (int)GetLastError());
        mod.handle = NULL;
        return mod;
    }
    mod.info = (ModuleInfo_t*)GetProcAddress(mod.handle, "_INFO_");
    mod.init = (void (*)())GetProcAddress(mod.handle, "_INIT_");
    mod.createInstance = (Instance * (*)(std::string)) GetProcAddress(mod.handle, "_CREATE_INSTANCE_");
    mod.deleteInstance = (void (*)(Instance*))GetProcAddress(mod.handle, "_DELETE_INSTANCE_");
    mod.end = (void (*)())GetProcAddress(mod.handle, "_END_");
    mod.infoV2 = (ModuleInfoV2*)GetProcAddress(mod.handle, SDRPP_HAS_V2_INFO);
#else
    mod.handle = dlopen(path.c_str(), RTLD_LAZY | RTLD_LOCAL);
    if (mod.handle == NULL) {
        flog::error("Couldn't load {0}: {1}", path, dlerror());
        mod.handle = NULL;
        return mod;
    }
    mod.info = (ModuleInfo_t*)dlsym(mod.handle, "_INFO_");
    mod.init = (void (*)())dlsym(mod.handle, "_INIT_");
    mod.createInstance = (Instance * (*)(std::string)) dlsym(mod.handle, "_CREATE_INSTANCE_");
    mod.deleteInstance = (void (*)(Instance*))dlsym(mod.handle, "_DELETE_INSTANCE_");
    mod.end = (void (*)())dlsym(mod.handle, "_END_");
    mod.infoV2 = (ModuleInfoV2*)dlsym(mod.handle, SDRPP_HAS_V2_INFO);
#endif
    if (mod.info == NULL) {
        flog::error("{0} is missing _INFO_ symbol", path);
        mod.handle = NULL;
        return mod;
    }
    if (mod.init == NULL) {
        flog::error("{0} is missing _INIT_ symbol", path);
        mod.handle = NULL;
        return mod;
    }
    if (mod.createInstance == NULL) {
        flog::error("{0} is missing _CREATE_INSTANCE_ symbol", path);
        mod.handle = NULL;
        return mod;
    }
    if (mod.deleteInstance == NULL) {
        flog::error("{0} is missing _DELETE_INSTANCE_ symbol", path);
        mod.handle = NULL;
        return mod;
    }
    if (mod.end == NULL) {
        flog::error("{0} is missing _END_ symbol", path);
        mod.handle = NULL;
        return mod;
    }

    // V2 API version compatibility check
    if (mod.infoV2) {
        if (!sdrppApiCompatible(mod.infoV2->apiVersion)) {
            int modMajor = (mod.infoV2->apiVersion >> 16) & 0xFF;
            int modMinor = (mod.infoV2->apiVersion >> 8) & 0xFF;
            flog::error("{0} requires API v{1}.{2} but core is v{3}.{4} — skipping",
                path, modMajor, modMinor, SDRPP_API_VERSION_MAJOR, SDRPP_API_VERSION_MINOR);
            mod.handle = NULL;
            return mod;
        }
        flog::info("Loaded V2 module '{0}' (API v{1}.{2}, caps=0x{3:X})",
            mod.info->name,
            (mod.infoV2->apiVersion >> 16) & 0xFF,
            (mod.infoV2->apiVersion >> 8) & 0xFF,
            mod.infoV2->capabilities);
    }

    if (modules.find(mod.info->name) != modules.end()) {
        flog::error("{0} has the same name as an already loaded module", path);
        mod.handle = NULL;
        return mod;
    }
    for (auto const& [name, _mod] : modules) {
        if (mod.handle == _mod.handle) {
            return _mod;
        }
    }

    try {
        mod.init();
    }
    catch (const std::exception& e) {
        flog::error("Module '{0}' threw exception during init: {1}", path, e.what());
        mod.handle = NULL;
        return mod;
    }
    catch (...) {
        flog::error("Module '{0}' threw unknown exception during init", path);
        mod.handle = NULL;
        return mod;
    }

    modules[mod.info->name] = mod;
    return mod;
}

bool ModuleManager::checkDependencies(const Module_t& mod) {
    if (!mod.isV2() || mod.dependencyCount() == 0) { return true; }
    const ModuleDependency* deps = mod.dependencies();
    for (int i = 0; i < mod.dependencyCount(); i++) {
        auto it = modules.find(deps[i].moduleName);
        if (it == modules.end()) {
            flog::error("Module '{0}' requires '{1}' which is not loaded",
                mod.info->name, deps[i].moduleName);
            return false;
        }
        if (deps[i].minApiVersion > 0 && it->second.apiVersion() < deps[i].minApiVersion) {
            flog::error("Module '{0}' requires '{1}' with API >= {2} but loaded version is {3}",
                mod.info->name, deps[i].moduleName, deps[i].minApiVersion, it->second.apiVersion());
            return false;
        }
    }
    return true;
}

int ModuleManager::createInstance(std::string name, std::string module) {
    if (modules.find(module) == modules.end()) {
        flog::warn("Module '{0}' not loaded, skipping instance '{1}'", module, name);
        return -1;
    }
    if (instances.find(name) != instances.end()) {
        flog::error("A module instance with the name '{0}' already exists", name);
        return -1;
    }
    int maxCount = modules[module].info->maxInstances;
    if (countModuleInstances(module) >= maxCount && maxCount > 0) {
        flog::error("Maximum number of instances reached for '{0}'", module);
        return -1;
    }

    Instance_t inst;
    inst.module = modules[module];

    try {
        inst.instance = inst.module.createInstance(name);
    }
    catch (const std::exception& e) {
        flog::error("Module '{0}' threw exception creating instance '{1}': {2}", module, name, e.what());
        inst.faulted = true;
        inst.faultError = e.what();
        inst.instance = nullptr;
        instances[name] = inst;
        return -1;
    }
    catch (...) {
        flog::error("Module '{0}' threw unknown exception creating instance '{1}'", module, name);
        inst.faulted = true;
        inst.faultError = "Unknown exception";
        inst.instance = nullptr;
        instances[name] = inst;
        return -1;
    }

    instances[name] = inst;
    onInstanceCreated.emit(name);
    EventBus::get().publish(events::ModuleInstanceCreated{name, module});
    return 0;
}

int ModuleManager::deleteInstance(std::string name) {
    if (instances.find(name) == instances.end()) {
        flog::error("Tried to remove non-existent instance '{0}'", name);
        return -1;
    }
    onInstanceDelete.emit(name);
    Instance_t inst = instances[name];

    if (inst.instance && !inst.faulted) {
        try {
            inst.module.deleteInstance(inst.instance);
        }
        catch (const std::exception& e) {
            flog::error("Module threw exception deleting instance '{0}': {1}", name, e.what());
        }
        catch (...) {
            flog::error("Module threw unknown exception deleting instance '{0}'", name);
        }
    }

    instances.erase(name);
    onInstanceDeleted.emit(name);
    EventBus::get().publish(events::ModuleInstanceDeleted{name});
    return 0;
}

int ModuleManager::deleteInstance(ModuleManager::Instance* instance) {
    flog::error("Delete instance not implemented");
    return -1;
}

int ModuleManager::enableInstance(std::string name) {
    if (instances.find(name) == instances.end()) {
        flog::error("Cannot enable '{0}', instance doesn't exist", name);
        return -1;
    }
    auto& inst = instances[name];
    if (inst.faulted) {
        flog::warn("Cannot enable '{0}', instance is faulted: {1}", name, inst.faultError);
        return -1;
    }

    try {
        inst.instance->enable();
    }
    catch (const std::exception& e) {
        flog::error("Module threw exception enabling '{0}': {1}", name, e.what());
        inst.faulted = true;
        inst.faultError = e.what();
        return -1;
    }
    catch (...) {
        flog::error("Module threw unknown exception enabling '{0}'", name);
        inst.faulted = true;
        inst.faultError = "Unknown exception in enable()";
        return -1;
    }
    return 0;
}

int ModuleManager::disableInstance(std::string name) {
    if (instances.find(name) == instances.end()) {
        flog::error("Cannot disable '{0}', instance doesn't exist", name);
        return -1;
    }
    auto& inst = instances[name];
    if (inst.faulted) { return -1; }

    try {
        inst.instance->disable();
    }
    catch (const std::exception& e) {
        flog::error("Module threw exception disabling '{0}': {1}", name, e.what());
        inst.faulted = true;
        inst.faultError = e.what();
        return -1;
    }
    catch (...) {
        flog::error("Module threw unknown exception disabling '{0}'", name);
        inst.faulted = true;
        inst.faultError = "Unknown exception in disable()";
        return -1;
    }
    return 0;
}

bool ModuleManager::instanceEnabled(std::string name) {
    if (instances.find(name) == instances.end()) {
        flog::error("Cannot check if '{0}' is enabled, instance doesn't exist", name);
        return false;
    }
    if (instances[name].faulted) { return false; }
    return instances[name].instance->isEnabled();
}

void ModuleManager::postInit(std::string name) {
    if (instances.find(name) == instances.end()) {
        flog::error("Cannot post-init '{0}', instance doesn't exist", name);
        return;
    }
    auto& inst = instances[name];
    if (inst.faulted) { return; }

    try {
        inst.instance->postInit();
    }
    catch (const std::exception& e) {
        flog::error("Module threw exception during post-init of '{0}': {1}", name, e.what());
        inst.faulted = true;
        inst.faultError = e.what();
    }
    catch (...) {
        flog::error("Module threw unknown exception during post-init of '{0}'", name);
        inst.faulted = true;
        inst.faultError = "Unknown exception in postInit()";
    }
}

std::string ModuleManager::getInstanceModuleName(std::string name) {
    if (instances.find(name) == instances.end()) {
        flog::error("Cannot get module name of'{0}', instance doesn't exist", name);
        return "";
    }
    return std::string(instances[name].module.info->name);
}

int ModuleManager::countModuleInstances(std::string module) {
    if (modules.find(module) == modules.end()) {
        flog::error("Cannot count instances of '{0}', Module doesn't exist", module);
        return -1;
    }
    ModuleManager::Module_t mod = modules[module];
    int count = 0;
    for (auto const& [name, instance] : instances) {
        if (instance.module == mod) { count++; }
    }
    return count;
}

void ModuleManager::doPostInitAll() {
    for (auto& [name, inst] : instances) {
        if (inst.faulted) {
            flog::warn("Skipping post-init for faulted instance '{0}': {1}", name, inst.faultError);
            continue;
        }
        flog::info("Running post-init for {0}", name);
        try {
            inst.instance->postInit();
        }
        catch (const std::exception& e) {
            flog::error("Module threw exception during post-init of '{0}': {1}", name, e.what());
            inst.faulted = true;
            inst.faultError = e.what();
        }
        catch (...) {
            flog::error("Module threw unknown exception during post-init of '{0}'", name);
            inst.faulted = true;
            inst.faultError = "Unknown exception in postInit()";
        }
    }
}
