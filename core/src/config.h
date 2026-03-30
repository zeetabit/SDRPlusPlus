#pragma once
#include <json.hpp>
#include <thread>
#include <string>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <functional>

using nlohmann::json;

class ConfigManager {
public:
    ConfigManager();
    ~ConfigManager();
    void setPath(std::string file);
    void load(json def, bool lock = true);
    void save(bool lock = true);
    void enableAutoSave();
    void disableAutoSave();

    // Legacy manual lock API (prefer withConfig/readConfig instead)
    void acquire();
    void release(bool modified = false);

    // RAII config access -- eliminates manual acquire/release.
    // Usage (read-write):
    //   config.withConfig([](json& conf) {
    //       conf["key"] = value;
    //   });
    //
    // Usage (read-only, no auto-save triggered):
    //   std::string val = config.readConfig([](const json& conf) {
    //       return (std::string)conf["key"];
    //   });
    void withConfig(std::function<void(json&)> fn);

    template<typename T>
    T readConfig(std::function<T(const json&)> fn) {
        std::lock_guard<std::mutex> lck(mtx);
        return fn(conf);
    }

    void readConfig(std::function<void(const json&)> fn) {
        std::lock_guard<std::mutex> lck(mtx);
        fn(conf);
    }

    // Public for backward compatibility -- prefer withConfig/readConfig.
    json conf;

private:
    void autoSaveWorker();

    std::string path = "";
    std::atomic<bool> changed{false};
    std::atomic<bool> autoSaveEnabled{false};
    std::thread autoSaveThread;
    std::mutex mtx;

    std::mutex termMtx;
    std::condition_variable termCond;
    std::atomic<bool> termFlag{false};
};
