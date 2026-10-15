/*
 * Copyright (C) 2026 Project EVA / CosFix
 *
 * Light-weight URCC (Unified Resource Control Center) AIDL HAL Daemon
 * for OPlus / ColorOS ported devices (MediaTek Helio G99 / MT6789).
 *
 * Implements: vendor.oplus.hardware.urcc.IUrcc/default
 * Target: Android 14+ (API 34+)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <dirent.h>
#include <time.h>

#include <android/log.h>
#include <android/binder_ibinder.h>
#include <android/binder_status.h>
#include <android/binder_parcel.h>
#include <sys/system_properties.h>

#ifdef __cplusplus
extern "C" {
#endif

/* From <android/binder_manager.h> */
binder_status_t AServiceManager_addService(AIBinder* binder, const char* instance);
AIBinder* AServiceManager_checkService(const char* instance);
AIBinder* AServiceManager_waitForService(const char* instance);
bool AServiceManager_isDeclared(const char* instance);

/* From <android/binder_ibinder.h> */
void AIBinder_markVintfStability(AIBinder* binder);

/* From <android/binder_process.h> */
void ABinderProcess_startThreadPool(void);
void ABinderProcess_joinThreadPool(void);
bool ABinderProcess_setThreadPoolMaxThreadCount(uint32_t numThreads);

#ifdef __cplusplus
}
#endif

#define LOG_TAG "urcc_service"
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define ALOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define ALOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

/* Hot-path debug logging: throttled unless persist.sys.urcc.debug or debug.urcc is enabled */
static inline bool is_debug_logging_enabled(void) {
    char val[PROP_VALUE_MAX] = {0};
    if (__system_property_get("persist.sys.urcc.debug", val) > 0 && val[0] == '1') {
        return true;
    }
    if (__system_property_get("debug.urcc", val) > 0 && val[0] == '1') {
        return true;
    }
    return false;
}
#define ALOGD_URCC(...) do { if (__builtin_expect(is_debug_logging_enabled(), 0)) { ALOGD(__VA_ARGS__); } } while (0)

#define URCC_INTERFACE_DESCRIPTOR "vendor.oplus.hardware.urcc.IUrcc"
#define URCC_INSTANCE_NAME        "vendor.oplus.hardware.urcc.IUrcc/default"
#define URCC_INTERFACE_HASH       "cae43655eaf0af39cbb6d273c5e2206eebe33a10"
#define URCC_INTERFACE_VERSION    1

/* Standard AIDL transaction codes (FIRST_CALL_TRANSACTION = 1) */
enum {
    TRANSACTION_urccInit                        = 1,
    TRANSACTION_urccResCtlRequest               = 2,
    TRANSACTION_urccResCtlRelease               = 3,
    TRANSACTION_urccResStateRequest             = 4,
    TRANSACTION_urccResListeningRegister        = 5,
    TRANSACTION_urccResListeningUnRegister      = 6,
    TRANSACTION_urccPropertyGet                 = 7,
    TRANSACTION_urccPropertySet                 = 8,
    TRANSACTION_urccThermalListeningRegister    = 9,
    TRANSACTION_urccThermalListeningUnRegister  = 10,
    TRANSACTION_uahNotifyExt                    = 11,
    TRANSACTION_setRelatedSysInfo               = 12,
    TRANSACTION_urccRuleCtl                     = 13,
    TRANSACTION_uahResCtlRequestBypass          = 14,
    TRANSACTION_getInterfaceVersion             = 0x00fffffe,
    TRANSACTION_getInterfaceHash                = 0x00ffffff,
};

/* Target opCodes used by ColorOS PhoneManager SuperComputing / Performance Center across SoCs */
#define OPCODE_GPU_LOAD_GENERIC     0x01020220  /* 16908832: Generic MTK / Dimensity */
#define OPCODE_GPU_LOAD_MT6897      0x01020240  /* 16908864: MT6897 / Dimensity 8300 */
#define OPCODE_GPU_LOAD_MTK_LEGACY  0x01020210  /* 16908816: Older Helio SoCs (G80/G85/G88/P35) */
#define OPCODE_GPU_LOAD_UNISOC      0x01020230  /* 16908848: UNISOC T606/T612/T616/T760 */
#define OPCODE_GPU_LOAD_QCOM        0x01020100  /* 16908544: Qualcomm Adreno */
#define OPCODE_GPU_LOAD_FALLBACK    0x01020200  /* Generic fallback */

/* Global immutable OK status header (allocated once in main, zero heap churn in Binder calls) */
static AStatus *g_ok_status = NULL;

/* Ultra-fast branch-minimized integer-to-string for 1..100 (avoids snprintf overhead) */
static inline int fast_itoa_100(int val, char *out) {
    if (__builtin_expect(val < 10, 0)) {
        out[0] = (char)('0' + val);
        out[1] = '\0';
        return 1;
    } else if (__builtin_expect(val < 100, 1)) {
        out[0] = (char)('0' + (val / 10));
        out[1] = (char)('0' + (val % 10));
        out[2] = '\0';
        return 2;
    } else {
        out[0] = '1';
        out[1] = '0';
        out[2] = '0';
        out[3] = '\0';
        return 3;
    }
}

/* Fast POSIX long integer reader for frequency sysfs nodes */
static inline long read_long_from_file(const char *path) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    char buf[32];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = '\0';
    return atol(buf);
}

/* Telemetry source types */
enum GpuSourceType {
    GPU_SRC_UNINITIALIZED = 0,
    GPU_SRC_PREAD_DIRECT,    /* Single pread on persistent FD: GED, KGSL, SPRD */
    GPU_SRC_PREAD_IDLE,      /* Single pread on persistent FD: 100 - idle */
    GPU_SRC_DEVFREQ_RATIO,   /* Devfreq cur/max scaling */
    GPU_SRC_MALI_DEBUGFS,    /* Legacy Mali debugfs */
    GPU_SRC_STATIC_FALLBACK  /* Guaranteed safe fallback */
};

static enum GpuSourceType s_gpu_src_type = GPU_SRC_UNINITIALIZED;
static int s_persistent_gpu_fd = -1;
static char s_devfreq_path_cur[160] = {0};
static char s_devfreq_path_max[160] = {0};
static char s_devfreq_path_min[160] = {0};

/*
 * One-time probe of active telemetry source on device boot/first call.
 * Avoids repeated failed open() attempts on nonexistent paths every frame.
 */
static void init_gpu_telemetry_source(void) {
    /* 1. MediaTek GED gpu_loading */
    int fd = open("/sys/module/ged/parameters/gpu_loading", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        char test_buf[16];
        if (read(fd, test_buf, sizeof(test_buf) - 1) > 0) {
            s_persistent_gpu_fd = fd;
            s_gpu_src_type = GPU_SRC_PREAD_DIRECT;
            ALOGI("URCC: Using persistent MediaTek GED telemetry (/sys/module/ged/parameters/gpu_loading)");
            return;
        }
        close(fd);
    }

    /* 2. Qualcomm Adreno GPU busy percentage */
    fd = open("/sys/class/kgsl/kgsl-3d0/gpu_busy_percentage", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        s_persistent_gpu_fd = fd;
        s_gpu_src_type = GPU_SRC_PREAD_DIRECT;
        ALOGI("URCC: Using persistent Qualcomm Adreno telemetry (/sys/class/kgsl/kgsl-3d0/gpu_busy_percentage)");
        return;
    }

    /* 3. UNISOC Spreadtrum Governor loading */
    fd = open("/sys/class/devfreq/scene-frequency/sprd-governor/gpu_loading", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        s_persistent_gpu_fd = fd;
        s_gpu_src_type = GPU_SRC_PREAD_DIRECT;
        ALOGI("URCC: Using persistent UNISOC governor telemetry");
        return;
    }

    /* 4. MediaTek GED gpu_idle */
    fd = open("/sys/module/ged/parameters/gpu_idle", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        s_persistent_gpu_fd = fd;
        s_gpu_src_type = GPU_SRC_PREAD_IDLE;
        ALOGI("URCC: Using persistent MediaTek GED idle telemetry");
        return;
    }

    /* 5. Dynamic Devfreq Discovery (Mali / GPU / KGSL / SPRD) */
    DIR *dir = opendir("/sys/class/devfreq");
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            if (entry->d_name[0] == '.') continue;
            if (strstr(entry->d_name, "mali") ||
                strstr(entry->d_name, "gpu") ||
                strstr(entry->d_name, "kgsl") ||
                strstr(entry->d_name, "sprd")) {
                snprintf(s_devfreq_path_cur, sizeof(s_devfreq_path_cur), "/sys/class/devfreq/%s/cur_freq", entry->d_name);
                snprintf(s_devfreq_path_max, sizeof(s_devfreq_path_max), "/sys/class/devfreq/%s/max_freq", entry->d_name);
                snprintf(s_devfreq_path_min, sizeof(s_devfreq_path_min), "/sys/class/devfreq/%s/min_freq", entry->d_name);
                s_gpu_src_type = GPU_SRC_DEVFREQ_RATIO;
                ALOGI("URCC: Using dynamic Linux Devfreq telemetry (/sys/class/devfreq/%s)", entry->d_name);
                break;
            }
        }
        closedir(dir);
        if (s_gpu_src_type == GPU_SRC_DEVFREQ_RATIO) return;
    }

    /* 6. Legacy Mali debugfs */
    if (access("/sys/kernel/debug/mali0/dvfs_utilization", R_OK) == 0 ||
        access("/sys/kernel/debug/mali/dvfs_utilization", R_OK) == 0) {
        s_gpu_src_type = GPU_SRC_MALI_DEBUGFS;
        ALOGI("URCC: Using legacy Mali debugfs telemetry");
        return;
    }

    s_gpu_src_type = GPU_SRC_STATIC_FALLBACK;
    ALOGI("URCC: Telemetry sysfs not found, using static 1%% idle fallback");
}

/* Direct low-level read from the active telemetry source */
static int read_gpu_loading_direct(void) {
    if (__builtin_expect(s_gpu_src_type == GPU_SRC_UNINITIALIZED, 0)) {
        init_gpu_telemetry_source();
    }

    if (s_gpu_src_type == GPU_SRC_PREAD_DIRECT) {
        char buf[16];
        ssize_t n = pread(s_persistent_gpu_fd, buf, sizeof(buf) - 1, 0);
        if (__builtin_expect(n > 0, 1)) {
            buf[n] = '\0';
            int val = atoi(buf);
            if (val >= 0 && val <= 100) return (val == 0) ? 1 : val;
        }
    } else if (s_gpu_src_type == GPU_SRC_PREAD_IDLE) {
        char buf[16];
        ssize_t n = pread(s_persistent_gpu_fd, buf, sizeof(buf) - 1, 0);
        if (__builtin_expect(n > 0, 1)) {
            buf[n] = '\0';
            int idle = atoi(buf);
            if (idle >= 0 && idle <= 100) {
                int val = 100 - idle;
                return (val <= 0) ? 1 : val;
            }
        }
    } else if (s_gpu_src_type == GPU_SRC_DEVFREQ_RATIO) {
        long cur = read_long_from_file(s_devfreq_path_cur);
        long max = read_long_from_file(s_devfreq_path_max);
        long min = read_long_from_file(s_devfreq_path_min);
        if (cur > 0 && max > min && min >= 0) {
            int load = (int)(((cur - min) * 100L) / (max - min));
            if (load >= 0 && load <= 100) return load;
        } else if (cur > 0 && max > 0) {
            int load = (int)((cur * 100L) / max);
            if (load >= 0 && load <= 100) return load;
        }
    } else if (s_gpu_src_type == GPU_SRC_MALI_DEBUGFS) {
        static uint64_t prev_busy = 0;
        static uint64_t prev_idle = 0;
        FILE *fp = fopen("/sys/kernel/debug/mali0/dvfs_utilization", "r");
        if (!fp) fp = fopen("/sys/kernel/debug/mali/dvfs_utilization", "r");
        if (fp) {
            uint64_t busy = 0, idlev = 0;
            if (fscanf(fp, "busy_time: %" SCNu64 " idle_time: %" SCNu64, &busy, &idlev) == 2) {
                fclose(fp);
                if (prev_busy > 0 && prev_idle > 0 && (busy >= prev_busy) && (idlev >= prev_idle)) {
                    uint64_t d_busy = busy - prev_busy;
                    uint64_t d_idle = idlev - prev_idle;
                    uint64_t d_total = d_busy + d_idle;
                    if (d_total > 0) {
                        int load = (int)((d_busy * 100) / d_total);
                        if (load >= 0 && load <= 100) return (load == 0) ? 1 : load;
                    }
                }
                prev_busy = busy;
                prev_idle = idlev;
            } else {
                fclose(fp);
            }
        }
    }

    return 1;
}

/*
 * High-performance vDSO-cached telemetry getter.
 * Coarse monotonic time check avoids redundant I/O within 50ms window.
 */
static inline int get_cached_gpu_loading(void) {
    static int s_cached_load = 1;
    static int64_t s_last_read_ms = 0;

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_COARSE, &ts);
    int64_t now_ms = (int64_t)ts.tv_sec * 1000 + (ts.tv_nsec / 1000000);

    if (__builtin_expect(now_ms - s_last_read_ms < 50 && s_last_read_ms != 0, 1)) {
        return s_cached_load;
    }

    int load = read_gpu_loading_direct();
    if (__builtin_expect(load > 0, 1)) {
        s_cached_load = load;
    }
    s_last_read_ms = now_ms;
    return s_cached_load;
}

/*
 * Helper to write an individual UrccRequestData element into the output AParcel.
 * Matches exact AIDL wire format expected by ndk::AParcel_readParcelableArray:
 * [int32: length_of_parcelable (including this header)]
 * [int32: res_Id]
 * [int32: event_Id]
 * [string: res_Path]
 * [string: res_Value]
 * [string: desc]
 */
static inline void write_urcc_request_data(AParcel *out, int32_t res_id, int32_t event_id,
                                           const char *val_str, int32_t val_len) {
    int32_t start_pos = AParcel_getDataPosition(out);

    /* Placeholder for parcelable byte length */
    AParcel_writeInt32(out, 0);

    /* Fields */
    AParcel_writeInt32(out, res_id);
    AParcel_writeInt32(out, event_id);

    /*
     * ColorOS liboplus-uah-client.so UahGetFeatureStatus explicitly parses
     * res_Path via std::stoi(res_Path). Passing an empty string causes an
     * uncaught std::invalid_argument exception which crashes system_server!
     * Therefore, ensure res_Path is always a valid numeric string.
     */
    AParcel_writeString(out, val_str, val_len);
    AParcel_writeString(out, val_str, val_len);
    AParcel_writeString(out, "gpu_loading", 11);

    /* Update parcelable length */
    int32_t end_pos = AParcel_getDataPosition(out);
    AParcel_setDataPosition(out, start_pos);
    AParcel_writeInt32(out, end_pos - start_pos);
    AParcel_setDataPosition(out, end_pos);
}

/*
 * Binder onTransact handler for vendor.oplus.hardware.urcc.IUrcc
 */
static binder_status_t urcc_onTransact(AIBinder *binder, transaction_code_t code,
                                       const AParcel *in, AParcel *out) {
    (void)binder;

    switch (code) {
        case TRANSACTION_urccResStateRequest: {
            int gpu_load = get_cached_gpu_loading();
            char load_str[4];
            int load_len = fast_itoa_100(gpu_load, load_str);

            ALOGD_URCC("TRANSACTION_urccResStateRequest -> reporting GPU load: %s%%", load_str);

            /* 1. Write OK status header (0 heap allocation) */
            AParcel_writeStatusHeader(out, g_ok_status);

            /* Check if client requested specific res_Ids in the 'in' parcel */
            int32_t requested_ids[16];
            int requested_count = 0;

            if (in) {
                int32_t in_pos = AParcel_getDataPosition(in);
                int32_t req_non_null = 0;
                if (AParcel_readInt32(in, &req_non_null) == STATUS_OK && req_non_null != 0) {
                    int32_t parcelable_size = 0;
                    if (AParcel_readInt32(in, &parcelable_size) == STATUS_OK && parcelable_size >= 4) {
                        int32_t field0 = 0;
                        int32_t array_len = 0;
                        AParcel_readInt32(in, &field0);
                        if (AParcel_readInt32(in, &array_len) == STATUS_OK && array_len > 0 && array_len < 16) {
                            for (int i = 0; i < array_len; i++) {
                                int32_t elem_non_null = 0;
                                int32_t elem_size = 0;
                                int32_t res_id = 0;
                                if (AParcel_readInt32(in, &elem_non_null) == STATUS_OK && elem_non_null != 0 &&
                                    AParcel_readInt32(in, &elem_size) == STATUS_OK &&
                                    AParcel_readInt32(in, &res_id) == STATUS_OK) {
                                    requested_ids[requested_count++] = res_id;
                                    int32_t cur_pos = AParcel_getDataPosition(in);
                                    if (elem_size > 8) {
                                        AParcel_setDataPosition(in, cur_pos + (elem_size - 8));
                                    }
                                }
                            }
                        }
                    }
                }
                AParcel_setDataPosition(in, in_pos);
            }

            /* 2. Write UrccRequestData array to 'out' */
            if (requested_count > 0) {
                AParcel_writeInt32(out, requested_count);
                for (int i = 0; i < requested_count; i++) {
                    AParcel_writeInt32(out, 1); /* Non-null flag */
                    write_urcc_request_data(out, requested_ids[i], 0, load_str, load_len);
                }
            } else {
                /* Default fallback: supply all Transsion hardware platform opcodes */
                AParcel_writeInt32(out, 6);

                AParcel_writeInt32(out, 1);
                write_urcc_request_data(out, OPCODE_GPU_LOAD_GENERIC, 0, load_str, load_len);

                AParcel_writeInt32(out, 1);
                write_urcc_request_data(out, OPCODE_GPU_LOAD_MT6897, 0, load_str, load_len);

                AParcel_writeInt32(out, 1);
                write_urcc_request_data(out, OPCODE_GPU_LOAD_MTK_LEGACY, 0, load_str, load_len);

                AParcel_writeInt32(out, 1);
                write_urcc_request_data(out, OPCODE_GPU_LOAD_UNISOC, 0, load_str, load_len);

                AParcel_writeInt32(out, 1);
                write_urcc_request_data(out, OPCODE_GPU_LOAD_QCOM, 0, load_str, load_len);

                AParcel_writeInt32(out, 1);
                write_urcc_request_data(out, OPCODE_GPU_LOAD_FALLBACK, 0, load_str, load_len);
            }

            return STATUS_OK;
        }

        case TRANSACTION_urccInit: {
            ALOGI("TRANSACTION_urccInit called");
            AParcel_writeStatusHeader(out, g_ok_status);
            return STATUS_OK;
        }

        case TRANSACTION_urccResCtlRequest: {
            ALOGD_URCC("TRANSACTION_urccResCtlRequest called");
            AParcel_writeStatusHeader(out, g_ok_status);
            AParcel_writeInt32(out, 0); /* 0 = Success */
            return STATUS_OK;
        }

        case TRANSACTION_urccResCtlRelease: {
            ALOGD_URCC("TRANSACTION_urccResCtlRelease called");
            AParcel_writeStatusHeader(out, g_ok_status);
            AParcel_writeInt32(out, 0); /* 0 = Success */
            return STATUS_OK;
        }

        case TRANSACTION_urccPropertyGet: {
            ALOGD_URCC("TRANSACTION_urccPropertyGet called");
            AParcel_writeStatusHeader(out, g_ok_status);
            /* Return numeric "0" to prevent potential std::stoi crash in UAH client */
            AParcel_writeString(out, "0", 1);
            return STATUS_OK;
        }

        case TRANSACTION_urccPropertySet: {
            ALOGD_URCC("TRANSACTION_urccPropertySet called");
            AParcel_writeStatusHeader(out, g_ok_status);
            AParcel_writeInt32(out, 0);
            return STATUS_OK;
        }

        case TRANSACTION_urccResListeningRegister: {
            ALOGD_URCC("TRANSACTION_urccResListeningRegister called");
            AParcel_writeStatusHeader(out, g_ok_status);
            AParcel_writeInt32(out, 0);
            return STATUS_OK;
        }

        case TRANSACTION_urccResListeningUnRegister: {
            ALOGD_URCC("TRANSACTION_urccResListeningUnRegister called");
            AParcel_writeStatusHeader(out, g_ok_status);
            AParcel_writeInt32(out, 0);
            return STATUS_OK;
        }

        case TRANSACTION_urccThermalListeningRegister: {
            ALOGD_URCC("TRANSACTION_urccThermalListeningRegister called");
            AParcel_writeStatusHeader(out, g_ok_status);
            AParcel_writeInt32(out, 0);
            return STATUS_OK;
        }

        case TRANSACTION_urccThermalListeningUnRegister: {
            ALOGD_URCC("TRANSACTION_urccThermalListeningUnRegister called");
            AParcel_writeStatusHeader(out, g_ok_status);
            AParcel_writeInt32(out, 0);
            return STATUS_OK;
        }

        case TRANSACTION_uahNotifyExt: {
            ALOGD_URCC("TRANSACTION_uahNotifyExt called");
            AParcel_writeStatusHeader(out, g_ok_status);
            AParcel_writeInt32(out, 0);
            return STATUS_OK;
        }

        case TRANSACTION_setRelatedSysInfo: {
            ALOGD_URCC("TRANSACTION_setRelatedSysInfo called");
            AParcel_writeStatusHeader(out, g_ok_status);
            AParcel_writeInt32(out, 0);
            return STATUS_OK;
        }

        case TRANSACTION_urccRuleCtl: {
            ALOGD_URCC("TRANSACTION_urccRuleCtl called");
            AParcel_writeStatusHeader(out, g_ok_status);
            AParcel_writeInt32(out, 0);
            return STATUS_OK;
        }

        case TRANSACTION_uahResCtlRequestBypass: {
            ALOGD_URCC("TRANSACTION_uahResCtlRequestBypass called");
            AParcel_writeStatusHeader(out, g_ok_status);
            return STATUS_OK;
        }

        case TRANSACTION_getInterfaceVersion: {
            AParcel_writeStatusHeader(out, g_ok_status);
            AParcel_writeInt32(out, URCC_INTERFACE_VERSION);
            return STATUS_OK;
        }

        case TRANSACTION_getInterfaceHash: {
            AParcel_writeStatusHeader(out, g_ok_status);
            AParcel_writeString(out, URCC_INTERFACE_HASH, 40);
            return STATUS_OK;
        }

        default: {
            ALOGD_URCC("Unhandled URCC transaction code: %u, returning OK", code);
            AParcel_writeStatusHeader(out, g_ok_status);
            return STATUS_OK;
        }
    }
}

static void *urcc_onCreate(void *args) {
    return args;
}

static void urcc_onDestroy(void *userData) {
    (void)userData;
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    signal(SIGPIPE, SIG_IGN);

    char brand[PROP_VALUE_MAX] = {0};
    char model[PROP_VALUE_MAX] = {0};
    char platform[PROP_VALUE_MAX] = {0};
    __system_property_get("ro.product.brand", brand);
    __system_property_get("ro.product.model", model);
    __system_property_get("ro.board.platform", platform);

    ALOGI("Starting vendor.oplus.hardware.urcc-service for Transsion (%s %s, SoC: %s)...",
          brand[0] ? brand : "Transsion",
          model[0] ? model : "Device",
          platform[0] ? platform : "Universal");

    /* Initialize global immutable status header once (reusable across all Binder threads, zero heap churn) */
    g_ok_status = AStatus_newOk();
    init_gpu_telemetry_source();

    /* 1. Setup multi-threaded binder threadpool */
    ABinderProcess_setThreadPoolMaxThreadCount(4);
    ABinderProcess_startThreadPool();

    /* 2. Define the AIDL interface class */
    AIBinder_Class *binder_class = AIBinder_Class_define(
        URCC_INTERFACE_DESCRIPTOR,
        urcc_onCreate,
        urcc_onDestroy,
        urcc_onTransact
    );

    if (!binder_class) {
        ALOGE("Failed to define AIBinder_Class for %s", URCC_INTERFACE_DESCRIPTOR);
        return 1;
    }

    /* 3. Instantiate the binder object */
    AIBinder *service_binder = AIBinder_new(binder_class, NULL);
    if (!service_binder) {
        ALOGE("Failed to allocate AIBinder instance");
        return 1;
    }

    /* 4. Mark VINTF stability (required for AIDL HALs on Android 14+) */
    AIBinder_markVintfStability(service_binder);

    /* 5. Register with ServiceManager */
    binder_status_t status = AServiceManager_addService(service_binder, URCC_INSTANCE_NAME);
    if (status != STATUS_OK) {
        ALOGE("Failed to register %s with ServiceManager, status: %d", URCC_INSTANCE_NAME, status);
        return 1;
    }

    ALOGI("Successfully registered %s with ServiceManager! Waiting for transactions...", URCC_INSTANCE_NAME);

    /* 6. Join threadpool and handle incoming calls indefinitely */
    ABinderProcess_joinThreadPool();

    return 0;
}
