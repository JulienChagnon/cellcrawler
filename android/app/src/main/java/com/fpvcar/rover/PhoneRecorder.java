package com.fpvcar.rover;

import android.Manifest;
import android.content.ContentResolver;
import android.content.ContentValues;
import android.content.Context;
import android.content.pm.PackageManager;
import android.media.MediaRecorder;
import android.net.Uri;
import android.os.Build;
import android.os.Environment;
import android.os.ParcelFileDescriptor;
import android.provider.MediaStore;
import android.util.Log;

import java.io.File;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;

/**
 * Full-quality recording on the phone, started and stopped from the PC (F key): the same
 * camera as the live stream, at up to 1920x1080 and a high bitrate, with microphone audio,
 * saved to Movies/FPV Rover. The camera feeds the MediaRecorder's Surface as a second output
 * next to the live encoder, so the live stream keeps running.
 */
final class PhoneRecorder {
    private static final String TAG = "rover";

    private final Context ctx;
    private MediaRecorder rec;
    private boolean surfaceSet;
    private Uri uri;
    private ParcelFileDescriptor pfd;
    private String name;

    PhoneRecorder(Context ctx) {
        this.ctx = ctx;
    }

    boolean active() {
        return rec != null;
    }

    /** Starts recording. False (and logged) if it couldn't. */
    boolean start() {
        int[] size = NativeBridge.recordSize();  // {width, height, fps} of the camera in use
        if (size == null || size[0] <= 0) return false;
        boolean audio = ctx.checkSelfPermission(Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED;
        name = "fpv-" + new SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(new Date()) + ".mp4";
        try {
            rec = Build.VERSION.SDK_INT >= 31 ? new MediaRecorder(ctx) : new MediaRecorder();
            if (audio) rec.setAudioSource(MediaRecorder.AudioSource.CAMCORDER);
            rec.setVideoSource(MediaRecorder.VideoSource.SURFACE);
            rec.setOutputFormat(MediaRecorder.OutputFormat.MPEG_4);
            openOutput();
            rec.setVideoEncoder(MediaRecorder.VideoEncoder.H264);
            rec.setVideoSize(size[0], size[1]);
            rec.setVideoFrameRate(size[2]);
            rec.setVideoEncodingBitRate(size[0] * size[1] >= 1920 * 1080 ? 16_000_000 : 10_000_000);
            if (audio) {
                rec.setAudioEncoder(MediaRecorder.AudioEncoder.AAC);
                rec.setAudioSamplingRate(48000);
                rec.setAudioEncodingBitRate(128_000);
                rec.setAudioChannels(1);
            }
            rec.prepare();
            if (!NativeBridge.setRecordSurface(rec.getSurface()))
                throw new IllegalStateException("camera refused the recording output");
            surfaceSet = true;
            rec.start();
            if (!NativeBridge.setRecordingActive(true)) throw new IllegalStateException("camera refused to record");
            NativeBridge.setRecordingState(true);
            Log.i(TAG, "record: phone recording " + size[0] + "x" + size[1] + "@" + size[2] + (audio ? " with audio" : " (no microphone permission)") + " to " + name);
            return true;
        } catch (Exception e) {
            Log.e(TAG, "record: phone recording failed to start", e);
            finish(false);
            return false;
        }
    }

    void stop() {
        if (rec == null) return;
        NativeBridge.setRecordingActive(false);
        boolean ok = true;
        try {
            rec.stop();
        } catch (RuntimeException e) {
            ok = false;  // stopped before any frame arrived: nothing worth keeping
        }
        finish(ok);
        Log.i(TAG, "record: phone recording " + (ok ? "saved: Movies/FPV Rover/" + name : "discarded (no frames)"));
    }

    private void openOutput() throws Exception {
        if (Build.VERSION.SDK_INT >= 29) {
            ContentValues v = new ContentValues();
            v.put(MediaStore.Video.Media.DISPLAY_NAME, name);
            v.put(MediaStore.Video.Media.MIME_TYPE, "video/mp4");
            v.put(MediaStore.Video.Media.RELATIVE_PATH, Environment.DIRECTORY_MOVIES + "/FPV Rover");
            v.put(MediaStore.Video.Media.IS_PENDING, 1);
            ContentResolver cr = ctx.getContentResolver();
            uri = cr.insert(MediaStore.Video.Media.EXTERNAL_CONTENT_URI, v);
            if (uri == null) throw new IllegalStateException("cannot create " + name);
            pfd = cr.openFileDescriptor(uri, "rw");
            rec.setOutputFile(pfd.getFileDescriptor());
        } else {
            File dir = ctx.getExternalFilesDir(Environment.DIRECTORY_MOVIES);
            rec.setOutputFile(new File(dir, name).getAbsolutePath());
        }
    }

    /** Releases everything; keeps the file only if it's a finished recording. */
    private void finish(boolean keep) {
        if (surfaceSet) NativeBridge.setRecordSurface(null);
        surfaceSet = false;
        if (rec != null) rec.release();
        rec = null;
        NativeBridge.setRecordingState(false);
        try {
            if (pfd != null) pfd.close();
        } catch (Exception ignored) {
        }
        pfd = null;
        if (uri != null) {
            ContentResolver cr = ctx.getContentResolver();
            if (keep) {
                ContentValues v = new ContentValues();
                v.put(MediaStore.Video.Media.IS_PENDING, 0);
                cr.update(uri, v, null, null);
            } else {
                cr.delete(uri, null, null);
            }
        }
        uri = null;
    }
}
