package com.goemon64.recompiled;

import android.content.Context;
import android.database.ContentObserver;
import android.net.Uri;
import android.os.Handler;
import android.os.HandlerThread;
import android.provider.MediaStore;
import android.util.Log;

import java.util.Collection;

// Tells the native side whenever the shared video list changes. The Quest's
// recorder adds its video there when a recording starts and updates it when the
// recording stops. While the Quest records, RT64's fast shaders hang the GPU, so
// the game falls back to the slower ubershader; this lets it switch back as soon
// as the recording is done instead of waiting for a timer.
public final class RecordingWatcher {
    private static ContentObserver observer;

    // flags are ContentResolver.NOTIFY_* (INSERT = 4, UPDATE = 8, DELETE = 16),
    // or 0 when Android doesn't say what changed.
    private static native void nativeVideosChanged(int flags);

    public static synchronized void start(Context context) {
        if (observer != null) {
            return;
        }

        HandlerThread thread = new HandlerThread("RecordingWatcher");
        thread.start();
        observer = new ContentObserver(new Handler(thread.getLooper())) {
            @Override
            public void onChange(boolean selfChange) {
                nativeVideosChanged(0);
            }

            @Override
            public void onChange(boolean selfChange, Uri uri) {
                nativeVideosChanged(0);
            }

            // Android 11+: says whether a video was added (a recording started)
            // or updated (a recording finished).
            @Override
            public void onChange(boolean selfChange, Collection<Uri> uris, int flags) {
                nativeVideosChanged(flags);
            }
        };

        try {
            context.getContentResolver().registerContentObserver(MediaStore.Video.Media.EXTERNAL_CONTENT_URI, true, observer);
            Log.i("OpenXR", "recording watcher started");
        }
        catch (Exception e) {
            Log.i("OpenXR", "recording watcher could not start: " + e);
        }
    }
}
