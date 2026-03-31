#pragma once
#include <json.hpp>
#include <functional>

using json = nlohmann::json;

class IConfigStore {
public:
    virtual ~IConfigStore() = default;

    virtual void readConfig(std::function<void(const json&)> fn) = 0;
    virtual void withConfig(std::function<void(json&)> fn) = 0;
};
