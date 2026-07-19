#include <catch.hpp>
#include "../../../tests/source_v2_common/v2_contract_helpers.h"
#include <signal_path/source.h>
#include <map>

// SourceManager test-double state, defined in stubs.cpp.
extern std::map<std::string, ISource*> g_registeredSources;
extern std::map<std::string, SourceManager::SourceHandler*> g_registeredHandlers;
extern void clearSourceManagerState();

// Hooks into kcsdr_stub.c — these let us isolate tests (FIRST: Independent)
// and assert exact vendor-call sequences.
extern "C" {
    typedef enum {
        KCSDR_CALL_LIST_DEVICES,
        KCSDR_CALL_OPEN,
        KCSDR_CALL_CLOSE,
        KCSDR_CALL_SET_PORT,
        KCSDR_CALL_SET_FREQUENCY,
        KCSDR_CALL_SET_ATTENUATION,
        KCSDR_CALL_SET_AMP_GAIN,
        KCSDR_CALL_SET_RX_EXT_AMP_GAIN,
        KCSDR_CALL_SET_SAMPLERATE,
        KCSDR_CALL_START,
        KCSDR_CALL_STOP,
        KCSDR_CALL_RX,
        KCSDR_CALL_TX,
    } kcsdr_call_kind_t;
    typedef struct {
        int count;
        kcsdr_call_kind_t kinds[64];
    } kcsdr_call_log_t;
    extern kcsdr_call_log_t g_kcsdrCallLog;
    extern int g_kcsdrFakeDeviceCount;
    extern const char* g_kcsdrFakeSerials[8];
    extern int g_kcsdrRxReturn;
    void kcsdrStubReset(void);
}

// Returns true if the log contains the given call kind at least once.
static bool logContains(kcsdr_call_kind_t kind) {
    for (int i = 0; i < g_kcsdrCallLog.count; i++) {
        if (g_kcsdrCallLog.kinds[i] == kind) return true;
    }
    return false;
}

TEST_CASE("kcsdr exports valid V2 module info", "[kcsdr][v2_contract]") {
    v2contract::requireSourceModuleInfo("kcsdr_source", "kcsdr_config.json");
}

TEST_CASE("kcsdr exports V2 entry points", "[kcsdr][v2_contract]") {
    v2contract::requireV2EntrypointsPresent();
}

TEST_CASE("kcsdr V2 instance is an ISource", "[kcsdr][v2_contract]") {
    ISource* src = v2contract::requireV2InstanceIsSource("kcsdr_test");
    v2contract::deleteV2Instance(src);
}

TEST_CASE("kcsdr constructs cleanly with zero devices", "[kcsdr][lifecycle]") {
    kcsdrStubReset();
    clearSourceManagerState();
    g_kcsdrFakeDeviceCount = 0;

    ISource* src = v2contract::requireV2InstanceIsSource("kcsdr_t4");

    // Constructor must call kcsdr_list_devices (via refresh()) but NEVER
    // open/start/stop/close — no device should mean no I/O side effects.
    REQUIRE(logContains(KCSDR_CALL_LIST_DEVICES));
    REQUIRE_FALSE(logContains(KCSDR_CALL_OPEN));
    REQUIRE_FALSE(logContains(KCSDR_CALL_START));
    REQUIRE_FALSE(logContains(KCSDR_CALL_STOP));
    REQUIRE_FALSE(logContains(KCSDR_CALL_CLOSE));

    v2contract::deleteV2Instance(src);
}

TEST_CASE("kcsdr registers itself as ISource on construction", "[kcsdr][registration]") {
    kcsdrStubReset();
    clearSourceManagerState();

    ISource* src = v2contract::requireV2InstanceIsSource("kcsdr_t5");

    // The registry must contain "KCSDR" pointing at this ISource (not via
    // the legacy SourceHandler path — that's the whole point of V2).
    REQUIRE(g_registeredSources.count("KCSDR") == 1);
    REQUIRE(g_registeredSources["KCSDR"] == src);
    REQUIRE(g_registeredHandlers.count("KCSDR") == 0);

    v2contract::deleteV2Instance(src);
}

TEST_CASE("kcsdr unregisters on destruction", "[kcsdr][registration]") {
    kcsdrStubReset();
    clearSourceManagerState();

    ISource* src = v2contract::requireV2InstanceIsSource("kcsdr_t8");
    REQUIRE(g_registeredSources.count("KCSDR") == 1);

    v2contract::deleteV2Instance(src);

    REQUIRE(g_registeredSources.count("KCSDR") == 0);
}
