#![allow(non_snake_case)]

use oplus_urcc_bridge_transsion::telemetry::{
    self, DEFAULT_OPCODES,
};
use oplus_urcc_bridge_transsion::{INTERFACE_HASH, INTERFACE_VERSION, SERVICE_NAME};

include!(concat!(env!("OUT_DIR"), "/urcc.rs"));

use crate::vendor::oplus::hardware::urcc::IUrcc::{BnUrcc, IUrcc};
use crate::vendor::oplus::hardware::urcc::UrccRequestData::UrccRequestData;
use crate::vendor::oplus::hardware::urcc::UrccRequestParcel::UrccRequestParcel;
use crate::vendor::oplus::hardware::urcc::IUrccCallback::IUrccCallback;

type BinderResult<T> = rsbinder::status::Result<T>;

fn init_logging() {
    let _ = tracing_log::LogTracer::init();

    #[cfg(target_os = "android")]
    {
        use tracing_logcat::{LogcatMakeWriter, LogcatTag};
        use tracing_subscriber::fmt::format::Format;

        match LogcatMakeWriter::new(LogcatTag::Fixed("urcc_service".into())) {
            Ok(writer) => {
                let _ = tracing_subscriber::fmt()
                    .event_format(Format::default().without_time())
                    .with_env_filter(
                        tracing_subscriber::EnvFilter::try_from_default_env()
                            .unwrap_or_else(|_| tracing_subscriber::EnvFilter::new("info")),
                    )
                    .with_writer(writer)
                    .with_ansi(false)
                    .try_init();
            }
            Err(_) => {
                let _ = tracing_subscriber::fmt()
                    .with_env_filter(
                        tracing_subscriber::EnvFilter::try_from_default_env()
                            .unwrap_or_else(|_| tracing_subscriber::EnvFilter::new("info")),
                    )
                    .with_ansi(false)
                    .try_init();
            }
        }
    }

    #[cfg(not(target_os = "android"))]
    let _ = tracing_subscriber::fmt()
        .with_env_filter(
            tracing_subscriber::EnvFilter::try_from_default_env()
                .unwrap_or_else(|_| tracing_subscriber::EnvFilter::new("info")),
        )
        .try_init();
}

fn is_debug_logging_enabled() -> bool {
    let mut val = [0u8; 92];
    let key1 = b"persist.sys.urcc.debug\0";
    let len1 = unsafe {
        libc::__system_property_get(key1.as_ptr() as *const libc::c_char, val.as_mut_ptr() as *mut libc::c_char)
    };
    if len1 > 0 && val[0] == b'1' {
        return true;
    }
    let key2 = b"debug.urcc\0";
    let len2 = unsafe {
        libc::__system_property_get(key2.as_ptr() as *const libc::c_char, val.as_mut_ptr() as *mut libc::c_char)
    };
    len2 > 0 && val[0] == b'1'
}

fn get_system_prop(key: &[u8], default: &str) -> String {
    let mut val = [0u8; 92];
    let len = unsafe {
        libc::__system_property_get(key.as_ptr() as *const libc::c_char, val.as_mut_ptr() as *mut libc::c_char)
    };
    if len > 0 {
        String::from_utf8_lossy(&val[..len as usize]).into_owned()
    } else {
        default.to_string()
    }
}

struct UrccService;

impl rsbinder::Interface for UrccService {}

impl IUrcc for UrccService {
    fn r#urccInit(&self) -> BinderResult<()> {
        tracing::info!("TRANSACTION_urccInit called");
        Ok(())
    }

    fn r#urccResCtlRequest(&self, _request: &UrccRequestParcel) -> BinderResult<i32> {
        if is_debug_logging_enabled() {
            tracing::debug!("TRANSACTION_urccResCtlRequest called");
        }
        Ok(0)
    }

    fn r#urccResCtlRelease(&self, _handle: i32) -> BinderResult<i32> {
        if is_debug_logging_enabled() {
            tracing::debug!("TRANSACTION_urccResCtlRelease called");
        }
        Ok(0)
    }

    fn r#urccResStateRequest(&self, request: &UrccRequestParcel) -> BinderResult<Vec<UrccRequestData>> {
        let gpu_load = telemetry::get_cached_gpu_loading();
        let load_str = telemetry::gpu_load_str(gpu_load);

        if is_debug_logging_enabled() {
            tracing::debug!("TRANSACTION_urccResStateRequest -> reporting GPU load: {}%", load_str);
        }

        let mut results;
        if !request.mUrccRequestData.is_empty() {
            results = Vec::with_capacity(request.mUrccRequestData.len());
            for item in &request.mUrccRequestData {
                results.push(UrccRequestData {
                    res_Id: item.res_Id,
                    event_Id: 0,
                    res_Path: load_str.to_string(),
                    res_Value: load_str.to_string(),
                    desc: "gpu_loading".to_string(),
                });
            }
        } else {
            results = Vec::with_capacity(DEFAULT_OPCODES.len());
            for &opcode in &DEFAULT_OPCODES {
                results.push(UrccRequestData {
                    res_Id: opcode,
                    event_Id: 0,
                    res_Path: load_str.to_string(),
                    res_Value: load_str.to_string(),
                    desc: "gpu_loading".to_string(),
                });
            }
        }

        Ok(results)
    }

    fn r#urccResListeningRegister(
        &self,
        _request: &UrccRequestParcel,
        _callback: &rsbinder::Strong<dyn IUrccCallback>,
    ) -> BinderResult<i32> {
        if is_debug_logging_enabled() {
            tracing::debug!("TRANSACTION_urccResListeningRegister called");
        }
        Ok(0)
    }

    fn r#urccResListeningUnRegister(&self, _handle: i32) -> BinderResult<i32> {
        if is_debug_logging_enabled() {
            tracing::debug!("TRANSACTION_urccResListeningUnRegister called");
        }
        Ok(0)
    }

    fn r#urccPropertyGet(&self, _key: &str) -> BinderResult<String> {
        if is_debug_logging_enabled() {
            tracing::debug!("TRANSACTION_urccPropertyGet called");
        }
        // Return numeric "0" to prevent potential std::stoi crash in UAH client
        Ok("0".to_string())
    }

    fn r#urccPropertySet(&self, _key: &str, _val: &str) -> BinderResult<i32> {
        if is_debug_logging_enabled() {
            tracing::debug!("TRANSACTION_urccPropertySet called");
        }
        Ok(0)
    }

    fn r#urccThermalListeningRegister(
        &self,
        _zones: &[i32],
        _callback: &rsbinder::Strong<dyn IUrccCallback>,
    ) -> BinderResult<i32> {
        if is_debug_logging_enabled() {
            tracing::debug!("TRANSACTION_urccThermalListeningRegister called");
        }
        Ok(0)
    }

    fn r#urccThermalListeningUnRegister(&self, _handle: i32) -> BinderResult<i32> {
        if is_debug_logging_enabled() {
            tracing::debug!("TRANSACTION_urccThermalListeningUnRegister called");
        }
        Ok(0)
    }

    fn r#uahNotifyExt(&self, _arg_type: i32, _cmd: i32, _data: &[i32]) -> BinderResult<i32> {
        if is_debug_logging_enabled() {
            tracing::debug!("TRANSACTION_uahNotifyExt called");
        }
        Ok(0)
    }

    fn r#setRelatedSysInfo(&self, _info: i32, _args: &[u8]) -> BinderResult<i32> {
        if is_debug_logging_enabled() {
            tracing::debug!("TRANSACTION_setRelatedSysInfo called");
        }
        Ok(0)
    }

    fn r#urccRuleCtl(&self, _rule_id: i32, _cmd: i32, _rule_data: &[UrccRequestData]) -> BinderResult<i32> {
        if is_debug_logging_enabled() {
            tracing::debug!("TRANSACTION_urccRuleCtl called");
        }
        Ok(0)
    }

    fn r#uahResCtlRequestBypass(&self, _request: &UrccRequestParcel) -> BinderResult<()> {
        if is_debug_logging_enabled() {
            tracing::debug!("TRANSACTION_uahResCtlRequestBypass called");
        }
        Ok(())
    }

    fn r#getInterfaceVersion(&self) -> BinderResult<i32> {
        Ok(INTERFACE_VERSION)
    }

    fn r#getInterfaceHash(&self) -> BinderResult<String> {
        Ok(INTERFACE_HASH.to_string())
    }
}

fn main() -> anyhow::Result<()> {
    unsafe {
        libc::signal(libc::SIGPIPE, libc::SIG_IGN);
    }

    init_logging();

    let brand = get_system_prop(b"ro.product.brand\0", "Transsion");
    let model = get_system_prop(b"ro.product.model\0", "Device");
    let platform = get_system_prop(b"ro.board.platform\0", "Universal");

    tracing::info!(
        "Starting vendor.oplus.hardware.urcc-service for Transsion ({} {}, SoC: {})...",
        brand, model, platform
    );

    telemetry::init_gpu_telemetry_source();

    rsbinder::ProcessState::init_default().map_err(|error| {
        anyhow::anyhow!("failed to initialize Binder process state: {error}")
    })?;
    rsbinder::ProcessState::start_thread_pool();

    let service = UrccService;
    let binder = BnUrcc::new_binder(service);
    rsbinder::hub::add_service(SERVICE_NAME, binder.as_binder())?;

    tracing::info!("Successfully registered {} with ServiceManager! Waiting for transactions...", SERVICE_NAME);
    rsbinder::ProcessState::join_thread_pool()?;
    Ok(())
}
