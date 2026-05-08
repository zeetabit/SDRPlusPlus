#include <gui/menus/sink.h>
#include <signal_path/signal_path.h>
#include <core.h>

namespace sinkmenu {
    void init() {
        // Sink config loading is now handled by SinkManager's AllModulesReady subscription.
        // All providers are guaranteed to be registered by then.
    }

    void draw(void* ctx) {
        sigpath::sinkManager.showMenu();
    }
};