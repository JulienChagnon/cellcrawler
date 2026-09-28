package com.fpvcar.rover;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.ServiceInfo;
import android.net.ConnectivityManager;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.net.NetworkRequest;
import android.os.BatteryManager;
import android.os.Build;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.PowerManager;
import android.telephony.CellSignalStrength;
import android.telephony.CellSignalStrengthLte;
import android.telephony.CellSignalStrengthNr;
import android.telephony.SignalStrength;
import android.telephony.TelephonyManager;
import android.util.Log;
import android.widget.Toast;

/**
 * Foreground service that keeps the rover running with the screen off: holds a wake lock,
 * owns the USB link, optionally pins traffic to the cellular network, and feeds phone
 * stats (battery, temperature, LTE signal) to the native engine once a second.
 */
public class RoverService extends Service {
    private static final String TAG = "rover";
    private static final String CHANNEL = "rover";
    static volatile boolean running;

    private final Handler handler = new Handler(Looper.getMainLooper());
    private PowerManager.WakeLock wakeLock;
    private UsbLink usb;
    private ConnectivityManager.NetworkCallback cellCallback;
    private Settings settings;

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        if (running) return START_NOT_STICKY;
        startInForeground();
        settings = Settings.load(this);

        PowerManager pm = (PowerManager) getSystemService(Context.POWER_SERVICE);
        wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "rover:engine");
        wakeLock.acquire();

        String err = NativeBridge.start(settings.host, settings.port, settings.key, settings.width, settings.height,
                settings.fps, settings.kbps, settings.cameraId, 0);
        if (err != null) {
            Toast.makeText(this, err, Toast.LENGTH_LONG).show();
            stopSelf();
            return START_NOT_STICKY;
        }
        running = true;
        if (settings.forceCellular) requestCellular();
        usb = new UsbLink(this);
        usb.start();
        handler.post(stats);
        return START_NOT_STICKY;
    }

    @Override
    public void onDestroy() {
        running = false;
        handler.removeCallbacks(stats);
        if (usb != null) usb.stop();
        NativeBridge.stop();
        if (cellCallback != null) {
            ((ConnectivityManager) getSystemService(Context.CONNECTIVITY_SERVICE)).unregisterNetworkCallback(cellCallback);
            cellCallback = null;
        }
        if (wakeLock != null && wakeLock.isHeld()) wakeLock.release();
        super.onDestroy();
    }

    private void startInForeground() {
        NotificationManager nm = getSystemService(NotificationManager.class);
        nm.createNotificationChannel(new NotificationChannel(CHANNEL, "Rover", NotificationManager.IMPORTANCE_LOW));
        PendingIntent open = PendingIntent.getActivity(this, 0, new Intent(this, MainActivity.class),
                PendingIntent.FLAG_IMMUTABLE);
        Notification n = new Notification.Builder(this, CHANNEL)
                .setContentTitle("FPV Rover running")
                .setContentText("Streaming video and relaying controls")
                .setSmallIcon(android.R.drawable.presence_video_online)
                .setContentIntent(open)
                .setOngoing(true)
                .build();
        if (Build.VERSION.SDK_INT >= 29) startForeground(1, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_CAMERA);
        else startForeground(1, n);
    }

    /** Pins the engine's socket to LTE even when WiFi is connected. */
    private void requestCellular() {
        ConnectivityManager cm = (ConnectivityManager) getSystemService(Context.CONNECTIVITY_SERVICE);
        NetworkRequest req = new NetworkRequest.Builder()
                .addTransportType(NetworkCapabilities.TRANSPORT_CELLULAR)
                .addCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
                .build();
        cellCallback = new ConnectivityManager.NetworkCallback() {
            @Override
            public void onAvailable(Network network) {
                Log.i(TAG, "cellular network available");
                NativeBridge.setNetwork(network.getNetworkHandle());
            }

            @Override
            public void onLost(Network network) {
                Log.w(TAG, "cellular network lost");
                NativeBridge.setNetwork(0);
            }
        };
        cm.requestNetwork(req, cellCallback);
    }

    private final Runnable stats = new Runnable() {
        @Override
        public void run() {
            pushStats();
            handler.postDelayed(this, 1000);
        }
    };

    private void pushStats() {
        int pct = 0, temp = 0;
        boolean charging = false;
        Intent b = registerReceiver(null, new IntentFilter(Intent.ACTION_BATTERY_CHANGED));
        if (b != null) {
            int level = b.getIntExtra(BatteryManager.EXTRA_LEVEL, 0);
            int scale = b.getIntExtra(BatteryManager.EXTRA_SCALE, 100);
            pct = scale > 0 ? level * 100 / scale : 0;
            temp = b.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, 0);  // tenths of a degree C
            charging = b.getIntExtra(BatteryManager.EXTRA_PLUGGED, 0) != 0;
        }

        int rsrp = 0, netType = 0;
        ConnectivityManager cm = (ConnectivityManager) getSystemService(Context.CONNECTIVITY_SERVICE);
        NetworkCapabilities nc = cm.getNetworkCapabilities(cm.getActiveNetwork());
        boolean wifi = nc != null && nc.hasTransport(NetworkCapabilities.TRANSPORT_WIFI) && !settings.forceCellular;
        if (wifi) netType = 1;
        TelephonyManager tm = (TelephonyManager) getSystemService(Context.TELEPHONY_SERVICE);
        if (Build.VERSION.SDK_INT >= 29 && tm != null) {
            SignalStrength ss = tm.getSignalStrength();
            if (ss != null) {
                for (CellSignalStrength c : ss.getCellSignalStrengths()) {
                    if (c instanceof CellSignalStrengthNr) {
                        int v = ((CellSignalStrengthNr) c).getSsRsrp();
                        if (v != Integer.MAX_VALUE) { rsrp = v; if (!wifi) netType = 3; }
                    } else if (c instanceof CellSignalStrengthLte && rsrp == 0) {
                        int v = ((CellSignalStrengthLte) c).getRsrp();
                        if (v != Integer.MAX_VALUE) { rsrp = v; if (!wifi) netType = 2; }
                    }
                }
            }
        }
        if (netType == 0 && nc != null && nc.hasTransport(NetworkCapabilities.TRANSPORT_CELLULAR)) netType = 4;
        NativeBridge.setPhoneStats(pct, temp, charging, rsrp, netType);
    }
}
