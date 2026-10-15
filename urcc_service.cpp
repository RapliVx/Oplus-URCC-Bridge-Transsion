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

#include <android/log.h>
#include <android/binder_ibinder.h>
#include <android/binder_status.h>
#include <android/binder_parcel.h>

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

/* Target opCodes used by ColorOS PhoneManager SuperComputing / Performance Center */
#define OPCODE_GPU_LOAD_GENERIC  0x01020220  /* 16908832: Generic MTK / Dimensity */
#define OPCODE_GPU_LOAD_MT6897   0x01020240  /* 16908864: MT6897 / Dimensity 8300 */
#define OPCODE_GPU_LOAD_FALLBACK 0x01020200  /* Generic fallback */

/*
 * Read GPU loading percentage from MediaTek kernel nodes.
 * Priority:
 * 1. /sys/module/ged/parameters/gpu_loading (MediaTek GED module)
 * 2. /sys/kernel/debug/mali0/dvfs_utilization (Mali kernel driver debugfs node)
 */
static int read_gpu_loading(void) {
    int load = -1;

    /* 1. Try reading from MediaTek GED sysfs parameter (Helio G99 / MT6789) */
    FILE *fp = fopen("/sys/module/ged/parameters/gpu_loading", "r");
    if (fp) {
        if (fscanf(fp, "%d", &load) == 1) {
            fclose(fp);
            if (load >= 0 && load <= 100) {
                /* Ensure minimum 1% so PhoneManager does not hide the gauge on idle */
                return (load == 0) ? 1 : load;
            }
        } else {
            fclose(fp);
        }
    }

    /* 2. Fallback: Mali dvfs_utilization (busy_time / total_time) */
    static uint64_t prev_busy = 0;
    static uint64_t prev_idle = 0;

    fp = fopen("/sys/kernel/debug/mali0/dvfs_utilization", "r");
    if (fp) {
        uint64_t busy = 0, idle = 0;
        if (fscanf(fp, "busy_time: %" SCNu64 " idle_time: %" SCNu64, &busy, &idle) == 2) {
            fclose(fp);
            if (prev_busy > 0 && prev_idle > 0 && (busy >= prev_busy) && (idle >= prev_idle)) {
                uint64_t d_busy = busy - prev_busy;
                uint64_t d_idle = idle - prev_idle;
                uint64_t d_total = d_busy + d_idle;
                if (d_total > 0) {
                    load = (int)((d_busy * 100) / d_total);
                }
            }
            prev_busy = busy;
            prev_idle = idle;

            if (load >= 0 && load <= 100) {
                return (load == 0) ? 1 : load;
            }
        } else {
            fclose(fp);
        }
    }

    /* 3. Fallback: Default idle state (1%) */
    return 1;
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
static void write_urcc_request_data(AParcel *out, int32_t res_id, int32_t event_id,
                                    const char *path, const char *value, const char *desc) {
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
    const char *effective_path = (path && path[0] != '\0') ? path : (value ? value : "0");
    AParcel_writeString(out, effective_path, (int32_t)strlen(effective_path));

    const char *effective_value = (value && value[0] != '\0') ? value : "0";
    AParcel_writeString(out, effective_value, (int32_t)strlen(effective_value));

    if (desc) {
        AParcel_writeString(out, desc, (int32_t)strlen(desc));
    } else {
        AParcel_writeString(out, "", 0);
    }

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
            int gpu_load = read_gpu_loading();
            char load_str[16];
            snprintf(load_str, sizeof(load_str), "%d", gpu_load);

            ALOGD("TRANSACTION_urccResStateRequest -> reporting GPU load: %s%%", load_str);

            /* 1. Write OK status header (AStatus) */
            AStatus *status = AStatus_newOk();
            AParcel_writeStatusHeader(out, status);
            AStatus_delete(status);

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
                    AParcel_writeInt32(out, 1); /* Non-null flag required by AParcel_readParcelableArray */
                    write_urcc_request_data(out, requested_ids[i], 0, load_str, load_str, "gpu_loading");
                }
            } else {
                /* Default fallback: supply generic MTK, MT6897, and fallback opcodes */
                AParcel_writeInt32(out, 3);

                AParcel_writeInt32(out, 1);
                write_urcc_request_data(out, OPCODE_GPU_LOAD_GENERIC, 0, load_str, load_str, "gpu_loading");

                AParcel_writeInt32(out, 1);
                write_urcc_request_data(out, OPCODE_GPU_LOAD_MT6897, 0, load_str, load_str, "gpu_loading");

                AParcel_writeInt32(out, 1);
                write_urcc_request_data(out, OPCODE_GPU_LOAD_FALLBACK, 0, load_str, load_str, "gpu_loading");
            }

            return STATUS_OK;
        }

        case TRANSACTION_urccInit: {
            ALOGI("TRANSACTION_urccInit called");
            AStatus *status = AStatus_newOk();
            AParcel_writeStatusHeader(out, status);
            AStatus_delete(status);
            return STATUS_OK;
        }

        case TRANSACTION_urccResCtlRequest: {
            ALOGD("TRANSACTION_urccResCtlRequest called");
            AStatus *status = AStatus_newOk();
            AParcel_writeStatusHeader(out, status);
            AStatus_delete(status);
            AParcel_writeInt32(out, 0); /* 0 = Success */
            return STATUS_OK;
        }

        case TRANSACTION_urccResCtlRelease: {
            ALOGD("TRANSACTION_urccResCtlRelease called");
            AStatus *status = AStatus_newOk();
            AParcel_writeStatusHeader(out, status);
            AStatus_delete(status);
            AParcel_writeInt32(out, 0); /* 0 = Success */
            return STATUS_OK;
        }

        case TRANSACTION_urccPropertyGet: {
            ALOGD("TRANSACTION_urccPropertyGet called");
            AStatus *status = AStatus_newOk();
            AParcel_writeStatusHeader(out, status);
            AStatus_delete(status);
            /* Return numeric "0" to prevent potential std::stoi crash in UAH client */
            AParcel_writeString(out, "0", 1);
            return STATUS_OK;
        }

        case TRANSACTION_urccPropertySet: {
            ALOGD("TRANSACTION_urccPropertySet called");
            AStatus *status = AStatus_newOk();
            AParcel_writeStatusHeader(out, status);
            AStatus_delete(status);
            AParcel_writeInt32(out, 0);
            return STATUS_OK;
        }

        case TRANSACTION_urccResListeningRegister: {
            ALOGD("TRANSACTION_urccResListeningRegister called");
            AStatus *status = AStatus_newOk();
            AParcel_writeStatusHeader(out, status);
            AStatus_delete(status);
            AParcel_writeInt32(out, 0);
            return STATUS_OK;
        }

        case TRANSACTION_urccResListeningUnRegister: {
            ALOGD("TRANSACTION_urccResListeningUnRegister called");
            AStatus *status = AStatus_newOk();
            AParcel_writeStatusHeader(out, status);
            AStatus_delete(status);
            AParcel_writeInt32(out, 0);
            return STATUS_OK;
        }

        case TRANSACTION_urccThermalListeningRegister: {
            ALOGD("TRANSACTION_urccThermalListeningRegister called");
            AStatus *status = AStatus_newOk();
            AParcel_writeStatusHeader(out, status);
            AStatus_delete(status);
            AParcel_writeInt32(out, 0);
            return STATUS_OK;
        }

        case TRANSACTION_urccThermalListeningUnRegister: {
            ALOGD("TRANSACTION_urccThermalListeningUnRegister called");
            AStatus *status = AStatus_newOk();
            AParcel_writeStatusHeader(out, status);
            AStatus_delete(status);
            AParcel_writeInt32(out, 0);
            return STATUS_OK;
        }

        case TRANSACTION_uahNotifyExt: {
            ALOGD("TRANSACTION_uahNotifyExt called");
            AStatus *status = AStatus_newOk();
            AParcel_writeStatusHeader(out, status);
            AStatus_delete(status);
            AParcel_writeInt32(out, 0);
            return STATUS_OK;
        }

        case TRANSACTION_setRelatedSysInfo: {
            ALOGD("TRANSACTION_setRelatedSysInfo called");
            AStatus *status = AStatus_newOk();
            AParcel_writeStatusHeader(out, status);
            AStatus_delete(status);
            AParcel_writeInt32(out, 0);
            return STATUS_OK;
        }

        case TRANSACTION_urccRuleCtl: {
            ALOGD("TRANSACTION_urccRuleCtl called");
            AStatus *status = AStatus_newOk();
            AParcel_writeStatusHeader(out, status);
            AStatus_delete(status);
            AParcel_writeInt32(out, 0);
            return STATUS_OK;
        }

        case TRANSACTION_uahResCtlRequestBypass: {
            ALOGD("TRANSACTION_uahResCtlRequestBypass called");
            AStatus *status = AStatus_newOk();
            AParcel_writeStatusHeader(out, status);
            AStatus_delete(status);
            return STATUS_OK;
        }

        case TRANSACTION_getInterfaceVersion: {
            AStatus *status = AStatus_newOk();
            AParcel_writeStatusHeader(out, status);
            AStatus_delete(status);
            AParcel_writeInt32(out, URCC_INTERFACE_VERSION);
            return STATUS_OK;
        }

        case TRANSACTION_getInterfaceHash: {
            AStatus *status = AStatus_newOk();
            AParcel_writeStatusHeader(out, status);
            AStatus_delete(status);
            AParcel_writeString(out, URCC_INTERFACE_HASH, (int32_t)strlen(URCC_INTERFACE_HASH));
            return STATUS_OK;
        }

        default: {
            ALOGD("Unhandled URCC transaction code: %u, returning OK", code);
            AStatus *status = AStatus_newOk();
            AParcel_writeStatusHeader(out, status);
            AStatus_delete(status);
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

    ALOGI("Starting vendor.oplus.hardware.urcc-service shim daemon for Android 14+...");

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
