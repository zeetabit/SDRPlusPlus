#include <gui/adapters/config_store_adapter.h>
#include <config.h>
#include <core.h>

void ConfigStoreAdapter::readConfig(std::function<void(const json&)> fn) {
    core::configManager.readConfig(fn);
}
void ConfigStoreAdapter::withConfig(std::function<void(json&)> fn) {
    core::configManager.withConfig(fn);
}
