#include <module_config.h>

ModuleConfig::ModuleConfig(ConfigManager* mgr, std::string instanceName)
    : mgr_(mgr), instanceName_(std::move(instanceName)) {}

void ModuleConfig::ensureNamespace(json& conf) {
    if (!conf.contains(instanceName_) || !conf[instanceName_].is_object()) {
        conf[instanceName_] = json::object();
    }
}

void ModuleConfig::applyDefaults(const json& defaults) {
    mgr_->withConfig([&](json& conf) {
        ensureNamespace(conf);
        json& section = conf[instanceName_];
        for (auto& [key, value] : defaults.items()) {
            if (!section.contains(key)) {
                section[key] = value;
            }
        }
    });
}

void ModuleConfig::with(std::function<void(json&)> fn) {
    mgr_->withConfig([&](json& conf) {
        ensureNamespace(conf);
        fn(conf[instanceName_]);
    });
}

void ModuleConfig::read(std::function<void(const json&)> fn) {
    mgr_->readConfig([&](const json& conf) {
        if (conf.contains(instanceName_) && conf[instanceName_].is_object()) {
            fn(conf[instanceName_]);
        }
        else {
            json empty = json::object();
            fn(empty);
        }
    });
}
