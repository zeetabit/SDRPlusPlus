// Test stub: replaces vendor kcsdr.c in source-module test builds.
// Records every call into the in-process g_kcsdrCallLog so tests can assert
// hardware-call sequences without any device present.
//
// Tests reset the log via kcsdrStubReset() and inspect via the extern arrays.

#include "../src/kcsdr.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define MAX_CALLS 64

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
    kcsdr_call_kind_t kinds[MAX_CALLS];
} kcsdr_call_log_t;

// Exposed to tests
kcsdr_call_log_t g_kcsdrCallLog = {0};
int g_kcsdrFakeDeviceCount = 0;        // tests set this to control device list
const char* g_kcsdrFakeSerials[8] = {0}; // up to 8 fake serials
int g_kcsdrRxReturn = 0;               // value returned by kcsdr_rx (0 ends worker)

void kcsdrStubReset(void) {
    g_kcsdrCallLog.count = 0;
    g_kcsdrFakeDeviceCount = 0;
    g_kcsdrRxReturn = 0;
    memset(g_kcsdrFakeSerials, 0, sizeof(g_kcsdrFakeSerials));
}

static void recordCall(kcsdr_call_kind_t kind) {
    if (g_kcsdrCallLog.count < MAX_CALLS) {
        g_kcsdrCallLog.kinds[g_kcsdrCallLog.count++] = kind;
    }
}

// A non-null sentinel returned by kcsdr_open
static int g_fakeDeviceHandle = 0;

int kcsdr_list_devices(kcsdr_info_t** devices) {
    recordCall(KCSDR_CALL_LIST_DEVICES);
    if (g_kcsdrFakeDeviceCount <= 0) {
        *devices = NULL;
        return 0;
    }
    kcsdr_info_t* list = (kcsdr_info_t*)calloc(g_kcsdrFakeDeviceCount, sizeof(kcsdr_info_t));
    for (int i = 0; i < g_kcsdrFakeDeviceCount; i++) {
        const char* s = g_kcsdrFakeSerials[i] ? g_kcsdrFakeSerials[i] : "FAKE0000";
        strncpy(list[i].serial, s, KCSDR_SERIAL_LEN);
        list[i].serial[KCSDR_SERIAL_LEN] = '\0';
    }
    *devices = list;
    return g_kcsdrFakeDeviceCount;
}

void kcsdr_free_device_list(kcsdr_info_t* devices) {
    free(devices);
}

int kcsdr_open(kcsdr_t** dev, const char* serial) {
    (void)serial;
    recordCall(KCSDR_CALL_OPEN);
    *dev = (kcsdr_t*)&g_fakeDeviceHandle;
    return 0;
}

void kcsdr_close(kcsdr_t* dev) {
    (void)dev;
    recordCall(KCSDR_CALL_CLOSE);
}

int kcsdr_set_port(kcsdr_t* dev, kcsdr_direction_t dir, uint8_t port) {
    (void)dev; (void)dir; (void)port;
    recordCall(KCSDR_CALL_SET_PORT);
    return 0;
}

int kcsdr_set_frequency(kcsdr_t* dev, kcsdr_direction_t dir, uint64_t freq) {
    (void)dev; (void)dir; (void)freq;
    recordCall(KCSDR_CALL_SET_FREQUENCY);
    return 0;
}

int kcsdr_set_attenuation(kcsdr_t* dev, kcsdr_direction_t dir, uint8_t att) {
    (void)dev; (void)dir; (void)att;
    recordCall(KCSDR_CALL_SET_ATTENUATION);
    return 0;
}

int kcsdr_set_amp_gain(kcsdr_t* dev, kcsdr_direction_t dir, uint8_t gain) {
    (void)dev; (void)dir; (void)gain;
    recordCall(KCSDR_CALL_SET_AMP_GAIN);
    return 0;
}

int kcsdr_set_rx_ext_amp_gain(kcsdr_t* dev, uint8_t gain) {
    (void)dev; (void)gain;
    recordCall(KCSDR_CALL_SET_RX_EXT_AMP_GAIN);
    return 0;
}

int kcsdr_set_samplerate(kcsdr_t* dev, kcsdr_direction_t dir, uint32_t samplerate) {
    (void)dev; (void)dir; (void)samplerate;
    recordCall(KCSDR_CALL_SET_SAMPLERATE);
    return 0;
}

int kcsdr_start(kcsdr_t* dev, kcsdr_direction_t dir) {
    (void)dev; (void)dir;
    recordCall(KCSDR_CALL_START);
    return 0;
}

int kcsdr_stop(kcsdr_t* dev, kcsdr_direction_t dir) {
    (void)dev; (void)dir;
    recordCall(KCSDR_CALL_STOP);
    return 0;
}

int kcsdr_rx(kcsdr_t* dev, int16_t* samples, int count) {
    (void)dev; (void)samples; (void)count;
    recordCall(KCSDR_CALL_RX);
    return g_kcsdrRxReturn;
}

int kcsdr_tx(kcsdr_t* dev, const int16_t* samples, int count) {
    (void)dev; (void)samples; (void)count;
    recordCall(KCSDR_CALL_TX);
    return 0;
}
