#pragma once
#include <string>
#include <mutex>
#include <unordered_map>
#include <typeindex>
#include <memory>
#include <functional>
#include <vector>
#include <utils/flog.h>

// Typed service registry. Modules register service implementations by name,
// and other modules query them by interface type + name.
//
// Usage:
//
//   // 1. Define a service interface (pure abstract class):
//   class IRadioControl {
//   public:
//       virtual ~IRadioControl() = default;
//       virtual int getMode() = 0;
//       virtual void setMode(int mode) = 0;
//       virtual void setBandwidth(double bw) = 0;
//   };
//
//   // 2. Module provides the service:
//   ServiceRegistry::get().provide<IRadioControl>("Radio 1", this);
//
//   // 3. Another module queries it:
//   auto* radio = ServiceRegistry::get().query<IRadioControl>("Radio 1");
//   if (radio) { radio->setMode(1); }
//
//   // 4. Module removes service on shutdown:
//   ServiceRegistry::get().remove<IRadioControl>("Radio 1");
//
// Thread safety: all methods are mutex-protected.

class ServiceRegistry {
public:
    static ServiceRegistry& get() {
        static ServiceRegistry instance;
        return instance;
    }

    // Register a service instance under a name. Returns false if already registered.
    template <typename T>
    bool provide(const std::string& name, T* service) {
        auto key = makeKey<T>(name);
        std::lock_guard<std::recursive_mutex> lock(mtx);
        if (services.count(key)) {
            flog::error("Service '{}' already registered for type {}", name, typeid(T).name());
            return false;
        }
        services[key] = static_cast<void*>(service);
        return true;
    }

    // Look up a service by type and name. Returns nullptr if not found.
    template <typename T>
    T* query(const std::string& name) {
        auto key = makeKey<T>(name);
        std::lock_guard<std::recursive_mutex> lock(mtx);
        auto it = services.find(key);
        if (it == services.end()) return nullptr;
        return static_cast<T*>(it->second);
    }

    // Check if a service exists.
    template <typename T>
    bool exists(const std::string& name) {
        auto key = makeKey<T>(name);
        std::lock_guard<std::recursive_mutex> lock(mtx);
        return services.count(key) > 0;
    }

    // Remove a service registration.
    template <typename T>
    bool remove(const std::string& name) {
        auto key = makeKey<T>(name);
        std::lock_guard<std::recursive_mutex> lock(mtx);
        return services.erase(key) > 0;
    }

    // Get all service names registered for a given type.
    template <typename T>
    std::vector<std::string> listNames() {
        auto typeKey = std::type_index(typeid(T));
        std::lock_guard<std::recursive_mutex> lock(mtx);
        std::vector<std::string> result;
        for (auto& [key, _] : services) {
            if (key.first == typeKey) {
                result.push_back(key.second);
            }
        }
        return result;
    }

    // Remove all services (called during shutdown).
    void clear() {
        std::lock_guard<std::recursive_mutex> lock(mtx);
        services.clear();
    }

private:
    ServiceRegistry() = default;

    using Key = std::pair<std::type_index, std::string>;

    struct KeyHash {
        size_t operator()(const Key& k) const {
            auto h1 = k.first.hash_code();
            auto h2 = std::hash<std::string>{}(k.second);
            return h1 ^ (h2 << 1);
        }
    };

    template <typename T>
    static Key makeKey(const std::string& name) {
        return {std::type_index(typeid(T)), name};
    }

    std::recursive_mutex mtx;
    std::unordered_map<Key, void*, KeyHash> services;
};
