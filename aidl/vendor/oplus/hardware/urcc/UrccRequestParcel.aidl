package vendor.oplus.hardware.urcc;

import vendor.oplus.hardware.urcc.UrccRequestData;
import vendor.oplus.hardware.urcc.IUrccCallback;

@VintfStability
parcelable UrccRequestParcel {
    int field1;
    UrccRequestData[] mUrccRequestData;
    boolean field3;
    int field4;
    int field5;
    int field6;
    long duration;
    String[] field8;
    int handle;
    @nullable IUrccCallback urccCallback;
    String desc;
    String field12;
    byte[] mParams;
}
