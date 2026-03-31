#pragma once
#include <gui/interfaces/iconfig_store.h>

class ConfigStoreAdapter : public IConfigStore {
public:
    void readConfig(std::function<void(const json&)> fn) override;
    void withConfig(std::function<void(json&)> fn) override;
};
