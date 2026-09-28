package com.fpvcar.rover;

import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.hardware.usb.UsbConstants;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbDeviceConnection;
import android.hardware.usb.UsbEndpoint;
import android.hardware.usb.UsbInterface;
import android.hardware.usb.UsbManager;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

import java.util.ArrayList;
import java.util.List;

/**
 * Finds the ESP32's USB-serial chip, gets permission, claims its interfaces and hands the
 * file descriptor to C++ (usb_serial.cpp), which does all the actual I/O.
 */
final class UsbLink {
    private static final String TAG = "rover";
    private static final String ACTION_PERMISSION = "com.fpvcar.rover.USB_PERMISSION";

    private final Context ctx;
    private final UsbManager mgr;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private UsbDeviceConnection conn;
    private UsbDevice device;
    private final List<UsbInterface> claimed = new ArrayList<>();
    private boolean permissionPending;
    private boolean started;

    UsbLink(Context ctx) {
        this.ctx = ctx;
        this.mgr = (UsbManager) ctx.getSystemService(Context.USB_SERVICE);
    }

    static boolean isSupported(UsbDevice d) {
        int vid = d.getVendorId();
        return vid == 0x10C4 || vid == 0x1A86 || vid == 0x303A;
    }

    private final BroadcastReceiver receiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context c, Intent intent) {
            UsbDevice d = intent.getParcelableExtra(UsbManager.EXTRA_DEVICE);
            if (UsbManager.ACTION_USB_DEVICE_DETACHED.equals(intent.getAction())) {
                if (device != null && d != null && d.getDeviceName().equals(device.getDeviceName())) {
                    Log.i(TAG, "usb: detached");
                    close();
                }
            } else if (ACTION_PERMISSION.equals(intent.getAction())) {
                permissionPending = false;
                if (d != null && intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)) open(d);
                else Log.w(TAG, "usb: permission denied");
            }
        }
    };

    private final Runnable poll = new Runnable() {
        @Override
        public void run() {
            scan();
            handler.postDelayed(this, 1000);
        }
    };

    void start() {
        if (started) return;
        started = true;
        IntentFilter f = new IntentFilter();
        f.addAction(UsbManager.ACTION_USB_DEVICE_DETACHED);
        f.addAction(ACTION_PERMISSION);
        if (Build.VERSION.SDK_INT >= 33) ctx.registerReceiver(receiver, f, Context.RECEIVER_NOT_EXPORTED);
        else ctx.registerReceiver(receiver, f);
        handler.post(poll);
    }

    void stop() {
        if (!started) return;
        started = false;
        handler.removeCallbacks(poll);
        ctx.unregisterReceiver(receiver);
        close();
    }

    private void scan() {
        if (conn != null) {
            if (NativeBridge.usbIsOpen()) return;
            Log.w(TAG, "usb: native side lost the device, reopening");
            close();
        }
        for (UsbDevice d : mgr.getDeviceList().values()) {
            if (!isSupported(d)) continue;
            if (mgr.hasPermission(d)) {
                open(d);
            } else if (!permissionPending) {
                permissionPending = true;
                int flags = Build.VERSION.SDK_INT >= 31 ? PendingIntent.FLAG_MUTABLE : 0;
                Intent i = new Intent(ACTION_PERMISSION).setPackage(ctx.getPackageName());
                mgr.requestPermission(d, PendingIntent.getBroadcast(ctx, 0, i, flags));
            }
            return;
        }
    }

    private void open(UsbDevice d) {
        if (conn != null) return;
        UsbDeviceConnection c = mgr.openDevice(d);
        if (c == null) {
            Log.w(TAG, "usb: openDevice failed");
            return;
        }
        // Data interface = the one with a bulk IN and bulk OUT endpoint. CDC devices also have a
        // communications interface that receives the line-coding requests.
        UsbInterface data = null, comm = null;
        UsbEndpoint in = null, out = null;
        for (int i = 0; i < d.getInterfaceCount(); i++) {
            UsbInterface it = d.getInterface(i);
            if (it.getInterfaceClass() == UsbConstants.USB_CLASS_COMM && comm == null) comm = it;
            UsbEndpoint bi = null, bo = null;
            for (int e = 0; e < it.getEndpointCount(); e++) {
                UsbEndpoint ep = it.getEndpoint(e);
                if (ep.getType() != UsbConstants.USB_ENDPOINT_XFER_BULK) continue;
                if (ep.getDirection() == UsbConstants.USB_DIR_IN) bi = ep;
                else bo = ep;
            }
            if (bi != null && bo != null && data == null) {
                data = it;
                in = bi;
                out = bo;
            }
        }
        if (data == null) {
            Log.w(TAG, "usb: no bulk interface");
            c.close();
            return;
        }
        claimed.clear();
        if (!c.claimInterface(data, true)) {
            Log.w(TAG, "usb: claimInterface failed");
            c.close();
            return;
        }
        claimed.add(data);
        if (comm != null && comm != data && c.claimInterface(comm, true)) claimed.add(comm);
        int ctrlIface = comm != null ? comm.getId() : data.getId();

        conn = c;
        device = d;
        if (!NativeBridge.usbOpen(c.getFileDescriptor(), d.getVendorId(), d.getProductId(), ctrlIface,
                in.getAddress(), out.getAddress())) {
            Log.w(TAG, "usb: native init failed");
            close();
        }
    }

    void close() {
        NativeBridge.usbClose();  // stops the native reader before the fd goes away
        if (conn != null) {
            for (UsbInterface i : claimed) conn.releaseInterface(i);
            claimed.clear();
            conn.close();
        }
        conn = null;
        device = null;
    }
}
