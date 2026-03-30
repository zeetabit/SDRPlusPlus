#pragma once

// ISinkProvider is declared inside sink.h (after SinkManager::Sink and
// SinkManager::Stream are defined) to avoid circular dependency.
// This header exists as a convenience include.
#include <signal_path/sink.h>
