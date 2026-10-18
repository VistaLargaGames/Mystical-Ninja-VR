package com.goemon64.recompiled;

import android.app.Activity;
import android.content.Intent;

// Shows the ROM window every time the game starts. The player can play with the
// ROM already stored, pick their own ROM (any file name) from the Quest's file
// browser, or quit. The native side calls request() on the game's thread before
// the game starts; it shows RomPickerActivity (a 2D panel) and waits.
public final class RomPicker {
    public static final int QUIT = 0;
    public static final int PICKED = 1;
    public static final int PLAY_CURRENT = 2;

    private static final Object lock = new Object();
    private static Integer result;
    static String message = "";
    static String destPath = "";
    // Name of the ROM already stored, or null when there is none to play.
    static String currentRom = null;

    // Returns PICKED once the chosen file has been copied to destPath (its display
    // name goes to destPath + ".name"), PLAY_CURRENT to use the stored ROM, or QUIT.
    public static int request(Activity activity, String message, String destPath, String currentRom) {
        synchronized (lock) {
            result = null;
            RomPicker.message = message;
            RomPicker.destPath = destPath;
            RomPicker.currentRom = (currentRom != null && !currentRom.isEmpty()) ? currentRom : null;
        }

        activity.startActivity(new Intent(activity, RomPickerActivity.class));

        synchronized (lock) {
            while (result == null) {
                try {
                    lock.wait();
                }
                catch (InterruptedException e) {
                    return QUIT;
                }
            }

            return result;
        }
    }

    static void report(int choice) {
        synchronized (lock) {
            if (result == null) {
                result = choice;
                lock.notifyAll();
            }
        }
    }
}
