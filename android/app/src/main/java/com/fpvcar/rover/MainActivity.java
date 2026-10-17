package com.fpvcar.rover;

import android.Manifest;
import android.app.Activity;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.graphics.Typeface;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.text.InputType;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import java.util.ArrayList;
import java.util.List;

/** Settings + status screen. Start/Stop runs RoverService; the screen stays on and dimmed while running. */
public class MainActivity extends Activity {
    private final Handler handler = new Handler(Looper.getMainLooper());
    private EditText host, port, key, width, height, fps, kbps, cameraId;
    private CheckBox forceCellular;
    private Button toggle;
    private TextView status;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        int pad = (int) (16 * getResources().getDisplayMetrics().density);
        root.setPadding(pad, pad, pad, pad);

        host = field(root, "PC address (IP or DNS name)", InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_URI);
        port = field(root, "UDP port", InputType.TYPE_CLASS_NUMBER);
        // visible-password: no autocorrect, auto-capitals or learning, but still readable
        key = field(root, "Passphrase (same as fpv.ini, case-sensitive)", InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD);
        width = field(root, "Video width", InputType.TYPE_CLASS_NUMBER);
        height = field(root, "Video height", InputType.TYPE_CLASS_NUMBER);
        fps = field(root, "FPS", InputType.TYPE_CLASS_NUMBER);
        kbps = field(root, "Max bitrate (kbps)", InputType.TYPE_CLASS_NUMBER);
        cameraId = field(root, "Camera id (blank = widest back camera, the 0.6x one if the phone allows)", InputType.TYPE_CLASS_TEXT);
        forceCellular = new CheckBox(this);
        forceCellular.setText("Force cellular (use LTE even when WiFi is on)");
        root.addView(forceCellular);

        toggle = new Button(this);
        toggle.setOnClickListener(v -> onToggle());
        root.addView(toggle);

        status = new TextView(this);
        status.setTypeface(Typeface.MONOSPACE);
        status.setTextSize(12);
        root.addView(status);

        TextView cams = new TextView(this);
        cams.setTypeface(Typeface.MONOSPACE);
        cams.setTextSize(11);
        cams.setText("\nCameras:\n" + NativeBridge.cameraList());
        root.addView(cams);

        ScrollView sv = new ScrollView(this);
        sv.addView(root);
        setContentView(sv);

        Settings s = Settings.load(this);
        host.setText(s.host);
        port.setText(String.valueOf(s.port));
        key.setText(s.key);
        width.setText(String.valueOf(s.width));
        height.setText(String.valueOf(s.height));
        fps.setText(String.valueOf(s.fps));
        kbps.setText(String.valueOf(s.kbps));
        cameraId.setText(s.cameraId);
        forceCellular.setChecked(s.forceCellular);

        requestPermissionsIfNeeded();
    }

    private EditText field(LinearLayout root, String label, int type) {
        TextView l = new TextView(this);
        l.setText(label);
        root.addView(l);
        EditText e = new EditText(this);
        e.setInputType(type);
        e.setSingleLine(true);
        root.addView(e);
        return e;
    }

    private void requestPermissionsIfNeeded() {
        List<String> need = new ArrayList<>();
        if (checkSelfPermission(Manifest.permission.CAMERA) != PackageManager.PERMISSION_GRANTED)
            need.add(Manifest.permission.CAMERA);
        // Sound for the phone's own recordings (F on the PC). Optional: without it they're silent.
        if (checkSelfPermission(Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED)
            need.add(Manifest.permission.RECORD_AUDIO);
        if (Build.VERSION.SDK_INT >= 33 && checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED)
            need.add(Manifest.permission.POST_NOTIFICATIONS);
        if (!need.isEmpty()) requestPermissions(need.toArray(new String[0]), 1);
    }

    private static int num(EditText e, int def) {
        try {
            return Integer.parseInt(e.getText().toString().trim());
        } catch (NumberFormatException ex) {
            return def;
        }
    }

    private void onToggle() {
        if (RoverService.running) {
            stopService(new Intent(this, RoverService.class));
        } else {
            if (checkSelfPermission(Manifest.permission.CAMERA) != PackageManager.PERMISSION_GRANTED) {
                Toast.makeText(this, "Camera permission is required", Toast.LENGTH_LONG).show();
                requestPermissionsIfNeeded();
                return;
            }
            Settings s = new Settings();
            s.host = host.getText().toString().trim();
            s.port = num(port, 47000);
            s.key = key.getText().toString().trim();
            s.width = num(width, 1280);
            s.height = num(height, 720);
            s.fps = num(fps, 60);
            s.kbps = num(kbps, 4000);
            s.cameraId = cameraId.getText().toString().trim();
            s.forceCellular = forceCellular.isChecked();
            s.save(this);
            startForegroundService(new Intent(this, RoverService.class));
        }
        handler.postDelayed(this::refresh, 300);
    }

    private final Runnable tick = new Runnable() {
        @Override
        public void run() {
            refresh();
            handler.postDelayed(this, 500);
        }
    };

    private void refresh() {
        boolean on = RoverService.running;
        toggle.setText(on ? "STOP" : "START");
        status.setText(NativeBridge.status());
        WindowManager.LayoutParams lp = getWindow().getAttributes();
        if (on) {
            // Screen stays on (keeps the camera pipeline at full priority) but dimmed to save power.
            getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
            lp.screenBrightness = 0.02f;
        } else {
            getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
            lp.screenBrightness = WindowManager.LayoutParams.BRIGHTNESS_OVERRIDE_NONE;
        }
        getWindow().setAttributes(lp);
    }

    @Override
    protected void onResume() {
        super.onResume();
        handler.post(tick);
    }

    @Override
    protected void onPause() {
        super.onPause();
        handler.removeCallbacks(tick);
    }
}
