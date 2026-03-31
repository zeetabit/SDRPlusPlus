#pragma once
#include <config.h>
#include <core.h>
#include <string>
#include <functional>
#include <memory>

class ModuleConfig {
public:
    ModuleConfig(ConfigManager* mgr, std::string instanceName);

    template<typename T>
    T get(const std::string& key, const T& defaultVal) {
        T result = defaultVal;
        mgr_->withConfig([&](json& conf) {
            ensureNamespace(conf);
            json& section = conf[instanceName_];
            if (section.contains(key)) {
                result = section[key].get<T>();
            }
            else {
                section[key] = defaultVal;
            }
        });
        return result;
    }

    template<typename T>
    void set(const std::string& key, const T& value) {
        mgr_->withConfig([&](json& conf) {
            ensureNamespace(conf);
            conf[instanceName_][key] = value;
        });
    }

    void with(std::function<void(json&)> fn);
    void read(std::function<void(const json&)> fn);

    void applyDefaults(const json& defaults);

    const std::string& name() const { return instanceName_; }
    ConfigManager* manager() { return mgr_; }

private:
    ConfigManager* mgr_;
    std::string instanceName_;

    void ensureNamespace(json& conf);
};

// Helper for _INIT_ — replaces the 3-line setPath/load/enableAutoSave boilerplate.
inline void sdrppInitModuleConfig(ConfigManager& config, const char* fileName) {
    std::string root = (std::string)core::args["root"];
    config.setPath(root + "/" + fileName);
    config.load(json({}));
    config.enableAutoSave();
}
