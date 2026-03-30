#pragma once
#include <functional>
#include <mutex>
#include <vector>
#include <typeindex>
#include <unordered_map>
#include <memory>
#include <algorithm>
#include <atomic>
#include <utils/flog.h>

// Subscription handle returned by EventBus::subscribe(). Call unsubscribe() or
// let it destruct to automatically remove the handler.
class Subscription {
public:
    using UnsubFn = std::function<void()>;

    Subscription() = default;
    explicit Subscription(UnsubFn fn) : unsub(std::move(fn)), active(true) {}
    ~Subscription() { unsubscribe(); }

    Subscription(Subscription&& o) noexcept : unsub(std::move(o.unsub)), active(o.active.load()) {
        o.active = false;
    }
    Subscription& operator=(Subscription&& o) noexcept {
        if (this != &o) {
            unsubscribe();
            unsub = std::move(o.unsub);
            active = o.active.load();
            o.active = false;
        }
        return *this;
    }

    Subscription(const Subscription&) = delete;
    Subscription& operator=(const Subscription&) = delete;

    void unsubscribe() {
        if (active.exchange(false) && unsub) {
            unsub();
            unsub = nullptr;
        }
    }

    bool isActive() const { return active; }

private:
    UnsubFn unsub;
    std::atomic<bool> active{false};
};

// Global typed event bus. Decouples publishers from subscribers.
//
// Usage:
//   // Define event types as plain structs:
//   struct FrequencyChanged { double freq; };
//   struct SourceSelected   { std::string name; };
//
//   // Subscribe (returns RAII handle — destructor auto-unsubscribes):
//   auto sub = EventBus::get().subscribe<FrequencyChanged>([](const FrequencyChanged& e) {
//       flog::info("Freq: {0}", e.freq);
//   });
//
//   // Publish (calls all handlers synchronously on caller's thread):
//   EventBus::get().publish(FrequencyChanged{145.5e6});
//
// Thread safety: publish and subscribe can be called from any thread.
// Handlers are invoked under a shared lock, so avoid long-running work
// in handlers — post to a queue if needed.
class EventBus {
public:
    static EventBus& get() {
        static EventBus instance;
        return instance;
    }

    template <typename E>
    Subscription subscribe(std::function<void(const E&)> handler) {
        auto key = std::type_index(typeid(E));
        auto wrapper = std::make_shared<Handler<E>>(std::move(handler));
        uint64_t id = nextId++;

        {
            std::lock_guard<std::mutex> lock(mtx);
            channels[key].push_back({id, wrapper});
        }

        return Subscription([this, key, id]() {
            std::lock_guard<std::mutex> lock(mtx);
            auto it = channels.find(key);
            if (it == channels.end()) return;
            auto& vec = it->second;
            vec.erase(std::remove_if(vec.begin(), vec.end(),
                [id](const Entry& e) { return e.id == id; }), vec.end());
            if (vec.empty()) { channels.erase(it); }
        });
    }

    template <typename E>
    void publish(const E& event) {
        auto key = std::type_index(typeid(E));
        std::vector<Entry> snapshot;
        {
            std::lock_guard<std::mutex> lock(mtx);
            auto it = channels.find(key);
            if (it == channels.end()) return;
            snapshot = it->second;
        }
        for (auto& entry : snapshot) {
            auto* typed = static_cast<Handler<E>*>(entry.handler.get());
            typed->fn(event);
        }
    }

    template <typename E>
    size_t subscriberCount() const {
        auto key = std::type_index(typeid(E));
        std::lock_guard<std::mutex> lock(mtx);
        auto it = channels.find(key);
        return (it != channels.end()) ? it->second.size() : 0;
    }

private:
    EventBus() = default;

    struct HandlerBase {
        virtual ~HandlerBase() = default;
    };

    template <typename E>
    struct Handler : HandlerBase {
        explicit Handler(std::function<void(const E&)> f) : fn(std::move(f)) {}
        std::function<void(const E&)> fn;
    };

    struct Entry {
        uint64_t id;
        std::shared_ptr<HandlerBase> handler;
    };

    mutable std::mutex mtx;
    std::unordered_map<std::type_index, std::vector<Entry>> channels;
    std::atomic<uint64_t> nextId{1};
};
