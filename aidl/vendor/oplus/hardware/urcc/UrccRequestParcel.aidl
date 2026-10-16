package vendor.oplus.hardware.urcc;

import vendor.oplus.hardware.urcc.UrccRequestData;
import vendor.oplus.hardware.urcc.IUrccCallback;

@VintfStability
parcelable UrccRequestParcel {
    int field1;
    @nullable UrccRequestData[] mUrccRequestData;
    boolean field3;
    int field4;
    int field5;
    int field6;
    long duration;
    @nullable String[] field8;
    int handle;
    @nullable IUrccCallback urccCallback;
    @nullable String desc;
    @nullable String field12;
    @nullable byte[] mParams;
}
