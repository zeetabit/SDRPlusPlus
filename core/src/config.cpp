#include <config.h>
#include <utils/flog.h>
#include <fstream>
#include <filesystem>

ConfigManager::ConfigManager() {}

ConfigManager::~ConfigManager() {
    disableAutoSave();
}

void ConfigManager::setPath(std::string file) {
    path = std::filesystem::absolute(file).string();
}

void ConfigManager::load(json def, bool lock) {
    std::unique_lock<std::mutex> lck(mtx, std::defer_lock);
    if (lock) { lck.lock(); }

    if (path.empty()) {
        flog::error("Config manager tried to load file with no path specified");
        return;
    }

    if (!std::filesystem::exists(path)) {
        flog::warn("Config file '{0}' does not exist, creating it", path);
        conf = def;
        save(false);
        return;
    }

    if (!std::filesystem::is_regular_file(path)) {
        flog::error("Config file '{0}' isn't a file", path);
        return;
    }

    try {
        std::ifstream file(path);
        file >> conf;
        file.close();
    }
    catch (const std::exception& e) {
        flog::error("Config file '{}' is corrupted, resetting it: {}", path, e.what());
        conf = def;
        save(false);
    }
}

void ConfigManager::save(bool lock) {
    std::unique_lock<std::mutex> lck(mtx, std::defer_lock);
    if (lock) { lck.lock(); }

    std::ofstream file(path);
    if (!file.is_open()) {
        flog::error("Failed to open config file for writing: {0}", path);
        return;
    }
    file << conf.dump(4);
    file.close();

    if (file.fail()) {
        flog::error("Failed to write config file: {0}", path);
    }
}

void ConfigManager::enableAutoSave() {
    bool expected = false;
    if (!autoSaveEnabled.compare_exchange_strong(expected, true)) { return; }
    termFlag = false;
    autoSaveThread = std::thread(&ConfigManager::autoSaveWorker, this);
}

void ConfigManager::disableAutoSave() {
    bool expected = true;
    if (!autoSaveEnabled.compare_exchange_strong(expected, false)) { return; }
    {
        std::lock_guard<std::mutex> lock(termMtx);
        termFlag = true;
    }
    termCond.notify_one();
    if (autoSaveThread.joinable()) { autoSaveThread.join(); }
}

void ConfigManager::acquire() {
    mtx.lock();
}

void ConfigManager::release(bool modified) {
    changed = changed.load() || modified;
    mtx.unlock();
}

void ConfigManager::withConfig(std::function<void(json&)> fn) {
    std::lock_guard<std::mutex> lck(mtx);
    fn(conf);
    changed = true;
}

void ConfigManager::autoSaveWorker() {
    while (autoSaveEnabled) {
        if (!mtx.try_lock()) {
            flog::warn("ConfigManager locked, waiting...");
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            continue;
        }

        if (changed.exchange(false)) {
            save(false);
        }

        mtx.unlock();

        std::unique_lock<std::mutex> lock(termMtx);
        termCond.wait_for(lock, std::chrono::milliseconds(1000), [this]() { return termFlag.load(); });
    }
}
