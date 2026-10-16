package vendor.oplus.hardware.urcc;

import vendor.oplus.hardware.urcc.UrccRequestData;
import vendor.oplus.hardware.urcc.UrccRequestParcel;
import vendor.oplus.hardware.urcc.IUrccCallback;

@VintfStability
interface IUrcc {
    void urccInit();
    int urccResCtlRequest(in UrccRequestParcel request);
    int urccResCtlRelease(int handle);
    UrccRequestData[] urccResStateRequest(in UrccRequestParcel request);
    int urccResListeningRegister(in UrccRequestParcel request, in IUrccCallback callback);
    int urccResListeningUnRegister(int handle);
    String urccPropertyGet(String key);
    int urccPropertySet(String key, String val);
    int urccThermalListeningRegister(in int[] zones, in IUrccCallback callback);
    int urccThermalListeningUnRegister(int handle);
    int uahNotifyExt(int type, int cmd, in int[] data);
    int setRelatedSysInfo(int info, in byte[] args);
    int urccRuleCtl(int ruleId, int cmd, in UrccRequestData[] ruleData);
    void uahResCtlRequestBypass(in UrccRequestParcel request);

    int getInterfaceVersion();
    String getInterfaceHash();
}
