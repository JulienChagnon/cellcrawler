package com.fpvcar.rover;

import android.content.Context;
import android.content.SharedPreferences;

/** User settings, persisted in SharedPreferences. */
final class Settings {
    String host = "";
    int port = 47000;
    String key = "";
    int width = 1280;
    int height = 720;
    int fps = 60;
    int kbps = 4000;
    String cameraId = "";
    boolean forceCellular = false;

    static Settings load(Context ctx) {
        SharedPreferences p = ctx.getSharedPreferences("rover", Context.MODE_PRIVATE);
        Settings s = new Settings();
        s.host = p.getString("host", s.host);
        s.port = p.getInt("port", s.port);
        s.key = p.getString("key", s.key);
        s.width = p.getInt("width", s.width);
        s.height = p.getInt("height", s.height);
        s.fps = p.getInt("fps", s.fps);
        s.kbps = p.getInt("kbps", s.kbps);
        s.cameraId = p.getString("cameraId", s.cameraId);
        s.forceCellular = p.getBoolean("forceCellular", s.forceCellular);
        return s;
    }

    void save(Context ctx) {
        ctx.getSharedPreferences("rover", Context.MODE_PRIVATE).edit()
                .putString("host", host)
                .putInt("port", port)
                .putString("key", key)
                .putInt("width", width)
                .putInt("height", height)
                .putInt("fps", fps)
                .putInt("kbps", kbps)
                .putString("cameraId", cameraId)
                .putBoolean("forceCellular", forceCellular)
                .apply();
    }
}
