/*
 * Copyright (C) 2026 Project EVA / CosFix
 *
 * Universal GPU Telemetry Bridge for Transsion Devices
 * (MediaTek Helio / Dimensity, UNISOC Spreadtrum, Qualcomm Snapdragon)
 *
 * Implements high-performance, zero-allocation hardware telemetry sampling
 * with persistent file descriptors, vDSO coarse timer caching, and zero memory leaks.
 */

use std::fs;
use std::os::unix::ffi::OsStrExt;
use std::sync::atomic::{AtomicI32, AtomicI64, Ordering};
use std::sync::OnceLock;

pub const OPCODE_GPU_LOAD_GENERIC: i32 = 0x01020220;    // Generic MTK / Dimensity
pub const OPCODE_GPU_LOAD_MT6897: i32 = 0x01020240;     // MT6897 / Dimensity 8300
pub const OPCODE_GPU_LOAD_MTK_LEGACY: i32 = 0x01020210; // Older Helio SoCs (G80/G85/G88/P35)
pub const OPCODE_GPU_LOAD_UNISOC: i32 = 0x01020230;     // UNISOC T606/T612/T616/T760
pub const OPCODE_GPU_LOAD_QCOM: i32 = 0x01020100;       // Qualcomm Adreno
pub const OPCODE_GPU_LOAD_FALLBACK: i32 = 0x01020200;   // Generic fallback

pub const DEFAULT_OPCODES: [i32; 6] = [
    OPCODE_GPU_LOAD_GENERIC,
    OPCODE_GPU_LOAD_MT6897,
    OPCODE_GPU_LOAD_MTK_LEGACY,
    OPCODE_GPU_LOAD_UNISOC,
    OPCODE_GPU_LOAD_QCOM,
    OPCODE_GPU_LOAD_FALLBACK,
];

#[repr(i32)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum GpuSourceType {
    Uninitialized = 0,
    PreadDirect = 1,   // Persistent FD: MTK GED, KGSL, SPRD
    PreadIdle = 2,     // Persistent FD: 100 - idle
    DevfreqRatio = 3,  // Devfreq cur/max scaling
    MaliDebugfs = 4,   // Mali debugfs
    StaticFallback = 5 // 1% fallback
}

impl GpuSourceType {
    fn from_i32(val: i32) -> Self {
        match val {
            1 => GpuSourceType::PreadDirect,
            2 => GpuSourceType::PreadIdle,
            3 => GpuSourceType::DevfreqRatio,
            4 => GpuSourceType::MaliDebugfs,
            5 => GpuSourceType::StaticFallback,
            _ => GpuSourceType::Uninitialized,
        }
    }
}

static GPU_SRC_TYPE: AtomicI32 = AtomicI32::new(GpuSourceType::Uninitialized as i32);
static PERSISTENT_GPU_FD: AtomicI32 = AtomicI32::new(-1);

static CACHED_LOAD: AtomicI32 = AtomicI32::new(1);
static LAST_READ_MS: AtomicI64 = AtomicI64::new(0);

struct DevfreqPaths {
    cur: String,
    max: String,
    min: String,
}

static DEVFREQ_PATHS: OnceLock<Option<DevfreqPaths>> = OnceLock::new();

#[inline]
fn parse_ascii_i32(buf: &[u8]) -> Option<i32> {
    let mut val: i32 = 0;
    let mut started = false;
    for &b in buf {
        if b.is_ascii_digit() {
            val = val.saturating_mul(10).saturating_add((b - b'0') as i32);
            started = true;
        } else if started {
            break;
        } else if b == b' ' || b == b'\t' || b == b'\n' || b == b'\r' {
            continue;
        } else {
            return None;
        }
    }
    if started { Some(val) } else { None }
}

#[inline]
fn parse_ascii_i64(buf: &[u8]) -> Option<i64> {
    let mut val: i64 = 0;
    let mut started = false;
    for &b in buf {
        if b.is_ascii_digit() {
            val = val.saturating_mul(10).saturating_add((b - b'0') as i64);
            started = true;
        } else if started {
            break;
        } else if b == b' ' || b == b'\t' || b == b'\n' || b == b'\r' {
            continue;
        } else {
            return None;
        }
    }
    if started { Some(val) } else { None }
}

fn read_i64_from_file(path: &str) -> i64 {
    let mut c_path = [0u8; 256];
    let bytes = path.as_bytes();
    if bytes.len() >= c_path.len() {
        return -1;
    }
    c_path[..bytes.len()].copy_from_slice(bytes);
    c_path[bytes.len()] = 0;

    let fd = unsafe { libc::open(c_path.as_ptr() as *const libc::c_char, libc::O_RDONLY | libc::O_CLOEXEC) };
    if fd < 0 {
        return -1;
    }

    let mut buf = [0u8; 32];
    let n = unsafe { libc::read(fd, buf.as_mut_ptr() as *mut libc::c_void, buf.len() - 1) };
    unsafe { libc::close(fd) };

    if n > 0 {
        parse_ascii_i64(&buf[..n as usize]).unwrap_or(-1)
    } else {
        -1
    }
}

pub fn init_gpu_telemetry_source() {
    // 1. MediaTek GED gpu_loading
    let ged_path = b"/sys/module/ged/parameters/gpu_loading\0";
    let fd = unsafe { libc::open(ged_path.as_ptr() as *const libc::c_char, libc::O_RDONLY | libc::O_CLOEXEC) };
    if fd >= 0 {
        let mut test_buf = [0u8; 16];
        let n = unsafe { libc::read(fd, test_buf.as_mut_ptr() as *mut libc::c_void, test_buf.len() - 1) };
        if n > 0 {
            PERSISTENT_GPU_FD.store(fd, Ordering::SeqCst);
            GPU_SRC_TYPE.store(GpuSourceType::PreadDirect as i32, Ordering::SeqCst);
            tracing::info!("URCC: Using persistent MediaTek GED telemetry (/sys/module/ged/parameters/gpu_loading)");
            return;
        }
        unsafe { libc::close(fd) };
    }

    // 2. Qualcomm Adreno GPU busy percentage
    let kgsl_path = b"/sys/class/kgsl/kgsl-3d0/gpu_busy_percentage\0";
    let fd = unsafe { libc::open(kgsl_path.as_ptr() as *const libc::c_char, libc::O_RDONLY | libc::O_CLOEXEC) };
    if fd >= 0 {
        PERSISTENT_GPU_FD.store(fd, Ordering::SeqCst);
        GPU_SRC_TYPE.store(GpuSourceType::PreadDirect as i32, Ordering::SeqCst);
        tracing::info!("URCC: Using persistent Qualcomm Adreno telemetry (/sys/class/kgsl/kgsl-3d0/gpu_busy_percentage)");
        return;
    }

    // 3. UNISOC Spreadtrum Governor loading
    let sprd_path = b"/sys/class/devfreq/scene-frequency/sprd-governor/gpu_loading\0";
    let fd = unsafe { libc::open(sprd_path.as_ptr() as *const libc::c_char, libc::O_RDONLY | libc::O_CLOEXEC) };
    if fd >= 0 {
        PERSISTENT_GPU_FD.store(fd, Ordering::SeqCst);
        GPU_SRC_TYPE.store(GpuSourceType::PreadDirect as i32, Ordering::SeqCst);
        tracing::info!("URCC: Using persistent UNISOC governor telemetry");
        return;
    }

    // 4. MediaTek GED gpu_idle
    let idle_path = b"/sys/module/ged/parameters/gpu_idle\0";
    let fd = unsafe { libc::open(idle_path.as_ptr() as *const libc::c_char, libc::O_RDONLY | libc::O_CLOEXEC) };
    if fd >= 0 {
        PERSISTENT_GPU_FD.store(fd, Ordering::SeqCst);
        GPU_SRC_TYPE.store(GpuSourceType::PreadIdle as i32, Ordering::SeqCst);
        tracing::info!("URCC: Using persistent MediaTek GED idle telemetry");
        return;
    }

    // 5. Dynamic Linux Devfreq Discovery
    if let Ok(entries) = fs::read_dir("/sys/class/devfreq") {
        for entry in entries.flatten() {
            let name_os = entry.file_name();
            let name_bytes = name_os.as_bytes();
            let name_str = String::from_utf8_lossy(name_bytes).to_lowercase();
            if name_str.contains("mali") || name_str.contains("gpu") || name_str.contains("kgsl") || name_str.contains("sprd") {
                let cur = format!("/sys/class/devfreq/{}/cur_freq", name_str);
                let max = format!("/sys/class/devfreq/{}/max_freq", name_str);
                let min = format!("/sys/class/devfreq/{}/min_freq", name_str);
                DEVFREQ_PATHS.get_or_init(|| Some(DevfreqPaths { cur, max, min }));
                GPU_SRC_TYPE.store(GpuSourceType::DevfreqRatio as i32, Ordering::SeqCst);
                tracing::info!("URCC: Using dynamic Linux Devfreq telemetry (/sys/class/devfreq/{})", name_str);
                return;
            }
        }
    }

    // 6. Legacy Mali debugfs
    let mali0 = b"/sys/kernel/debug/mali0/dvfs_utilization\0";
    let mali = b"/sys/kernel/debug/mali/dvfs_utilization\0";
    if unsafe { libc::access(mali0.as_ptr() as *const libc::c_char, libc::R_OK) == 0 } ||
       unsafe { libc::access(mali.as_ptr() as *const libc::c_char, libc::R_OK) == 0 } {
        GPU_SRC_TYPE.store(GpuSourceType::MaliDebugfs as i32, Ordering::SeqCst);
        tracing::info!("URCC: Using legacy Mali debugfs telemetry");
        return;
    }

    GPU_SRC_TYPE.store(GpuSourceType::StaticFallback as i32, Ordering::SeqCst);
    tracing::info!("URCC: Telemetry sysfs not found, using static 1% idle fallback");
}

fn read_gpu_loading_direct() -> i32 {
    let mut src = GpuSourceType::from_i32(GPU_SRC_TYPE.load(Ordering::Relaxed));
    if src == GpuSourceType::Uninitialized {
        init_gpu_telemetry_source();
        src = GpuSourceType::from_i32(GPU_SRC_TYPE.load(Ordering::Relaxed));
    }

    match src {
        GpuSourceType::PreadDirect => {
            let fd = PERSISTENT_GPU_FD.load(Ordering::Relaxed);
            if fd >= 0 {
                let mut buf = [0u8; 16];
                let n = unsafe {
                    libc::pread(fd, buf.as_mut_ptr() as *mut libc::c_void, buf.len() - 1, 0)
                };
                if n > 0 {
                    if let Some(val) = parse_ascii_i32(&buf[..n as usize]) {
                        if (0..=100).contains(&val) {
                            return if val == 0 { 1 } else { val };
                        }
                    }
                }
            }
        }
        GpuSourceType::PreadIdle => {
            let fd = PERSISTENT_GPU_FD.load(Ordering::Relaxed);
            if fd >= 0 {
                let mut buf = [0u8; 16];
                let n = unsafe {
                    libc::pread(fd, buf.as_mut_ptr() as *mut libc::c_void, buf.len() - 1, 0)
                };
                if n > 0 {
                    if let Some(idle) = parse_ascii_i32(&buf[..n as usize]) {
                        if (0..=100).contains(&idle) {
                            let val = 100 - idle;
                            return if val <= 0 { 1 } else { val };
                        }
                    }
                }
            }
        }
        GpuSourceType::DevfreqRatio => {
            if let Some(Some(paths)) = DEVFREQ_PATHS.get() {
                let cur = read_i64_from_file(&paths.cur);
                let max = read_i64_from_file(&paths.max);
                let min = read_i64_from_file(&paths.min);
                if cur > 0 && max > min && min >= 0 {
                    let load = (((cur - min) * 100) / (max - min)) as i32;
                    if (0..=100).contains(&load) {
                        return if load == 0 { 1 } else { load };
                    }
                } else if cur > 0 && max > 0 {
                    let load = ((cur * 100) / max) as i32;
                    if (0..=100).contains(&load) {
                        return if load == 0 { 1 } else { load };
                    }
                }
            }
        }
        GpuSourceType::MaliDebugfs => {
            // Delta computation with atomic tracking
            static PREV_BUSY: AtomicI64 = AtomicI64::new(0);
            static PREV_IDLE: AtomicI64 = AtomicI64::new(0);

            let path1 = b"/sys/kernel/debug/mali0/dvfs_utilization\0";
            let path2 = b"/sys/kernel/debug/mali/dvfs_utilization\0";
            let mut fd = unsafe { libc::open(path1.as_ptr() as *const libc::c_char, libc::O_RDONLY | libc::O_CLOEXEC) };
            if fd < 0 {
                fd = unsafe { libc::open(path2.as_ptr() as *const libc::c_char, libc::O_RDONLY | libc::O_CLOEXEC) };
            }
            if fd >= 0 {
                let mut buf = [0u8; 128];
                let n = unsafe { libc::read(fd, buf.as_mut_ptr() as *mut libc::c_void, buf.len() - 1) };
                unsafe { libc::close(fd) };
                if n > 0 {
                    if let Ok(s) = std::str::from_utf8(&buf[..n as usize]) {
                        let mut busy: i64 = 0;
                        let mut idlev: i64 = 0;
                        for part in s.split_whitespace() {
                            if part.starts_with("busy_time:") {
                                if let Some(val_str) = part.split(':').nth(1) {
                                    busy = val_str.trim().parse::<i64>().unwrap_or(0);
                                }
                            } else if part.starts_with("idle_time:") {
                                if let Some(val_str) = part.split(':').nth(1) {
                                    idlev = val_str.trim().parse::<i64>().unwrap_or(0);
                                }
                            }
                        }
                        let p_busy = PREV_BUSY.swap(busy, Ordering::Relaxed);
                        let p_idle = PREV_IDLE.swap(idlev, Ordering::Relaxed);
                        if p_busy > 0 && p_idle > 0 && busy >= p_busy && idlev >= p_idle {
                            let d_busy = busy - p_busy;
                            let d_idle = idlev - p_idle;
                            let d_total = d_busy + d_idle;
                            if d_total > 0 {
                                let load = ((d_busy * 100) / d_total) as i32;
                                if (0..=100).contains(&load) {
                                    return if load == 0 { 1 } else { load };
                                }
                            }
                        }
                    }
                }
            }
        }
        GpuSourceType::StaticFallback | GpuSourceType::Uninitialized => {}
    }

    1
}

#[inline]
pub fn get_cached_gpu_loading() -> i32 {
    let mut ts = libc::timespec { tv_sec: 0, tv_nsec: 0 };
    unsafe {
        libc::clock_gettime(libc::CLOCK_MONOTONIC_COARSE, &mut ts);
    }
    let now_ms = (ts.tv_sec as i64) * 1000 + (ts.tv_nsec as i64) / 1_000_000;

    let last_ms = LAST_READ_MS.load(Ordering::Relaxed);
    if now_ms.saturating_sub(last_ms) < 50 && last_ms != 0 {
        return CACHED_LOAD.load(Ordering::Relaxed);
    }

    let load = read_gpu_loading_direct();
    if load > 0 {
        CACHED_LOAD.store(load, Ordering::Relaxed);
    }
    LAST_READ_MS.store(now_ms, Ordering::Relaxed);
    load
}

pub static LOAD_STRINGS: [&str; 101] = [
    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
    "10", "11", "12", "13", "14", "15", "16", "17", "18", "19",
    "20", "21", "22", "23", "24", "25", "26", "27", "28", "29",
    "30", "31", "32", "33", "34", "35", "36", "37", "38", "39",
    "40", "41", "42", "43", "44", "45", "46", "47", "48", "49",
    "50", "51", "52", "53", "54", "55", "56", "57", "58", "59",
    "60", "61", "62", "63", "64", "65", "66", "67", "68", "69",
    "70", "71", "72", "73", "74", "75", "76", "77", "78", "79",
    "80", "81", "82", "83", "84", "85", "86", "87", "88", "89",
    "90", "91", "92", "93", "94", "95", "96", "97", "98", "99",
    "100",
];

#[inline(always)]
pub fn gpu_load_str(val: i32) -> &'static str {
    let idx = val.clamp(0, 100) as usize;
    LOAD_STRINGS[idx]
}
