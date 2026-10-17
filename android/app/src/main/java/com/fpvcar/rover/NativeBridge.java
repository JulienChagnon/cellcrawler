package com.fpvcar.rover;

/** Entry points into librover.so (src/main/cpp/jni.cpp). All real work happens in C++. */
final class NativeBridge {
    static {
        System.loadLibrary("rover");
    }

    private NativeBridge() {}

    /** Starts the engine. Returns null on success, otherwise an error message. */
    static native String start(String host, int port, String key, int width, int height, int fps, int kbps,
                               String cameraId, long networkHandle);

    static native void stop();

    static native boolean usbOpen(int fd, int vid, int pid, int ctrlInterface, int endpointIn, int endpointOut);

    static native void usbClose();

    static native boolean usbIsOpen();

    static native void setNetwork(long networkHandle);

    static native void setPhoneStats(int batteryPct, int tempDeciC, boolean charging, int rsrpDbm, int netType);

    static native String status();

    static native String cameraList();

    // Phone recording (PhoneRecorder), driven by the PC's record flag.
    static native boolean wantRecording();

    /** {width, height, fps} the recorder must use for the camera that's running. */
    static native int[] recordSize();

    /** Adds the recorder's Surface as a second camera output (null removes it). */
    static native boolean setRecordSurface(android.view.Surface surface);

    static native boolean setRecordingActive(boolean on);

    /** Reported to the PC so its HUD can show the phone is recording. */
    static native void setRecordingState(boolean on);
}
