package vendor.oplus.hardware.urcc;

import vendor.oplus.hardware.urcc.UrccRequestData;

@VintfStability
interface IUrccCallback {
    void onCallback(in UrccRequestData[] data);
}
