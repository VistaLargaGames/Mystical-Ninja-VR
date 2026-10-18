package com.goemon64.recompiled;

import android.app.Activity;
import android.content.ActivityNotFoundException;
import android.content.Intent;
import android.database.Cursor;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.os.Bundle;
import android.provider.OpenableColumns;
import android.util.TypedValue;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;

// The 2D panel shown at every start: play with the stored ROM, choose a ROM file
// (copied to RomPicker.destPath for the native side to check and store), or quit.
public class RomPickerActivity extends Activity {
    private static final int PICK_ROM = 1;
    private boolean reported = false;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        showPrompt();
    }

    private int dp(int value) {
        return (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, value, getResources().getDisplayMetrics());
    }

    // The owner's banner on the left with the controls under it, the menu on the right
    // (side by side so it fits the 1280 x 760 dp panel without scrolling, like the No
    // Mercy start panel the owner approved). Colours from the banner: deep sea blue
    // behind, parchment boards with red frames like its feature signs.
    private static final int SEA = Color.rgb(12, 30, 72);
    private static final int BOARD = Color.rgb(246, 232, 202);
    private static final int FRAME_RED = Color.rgb(190, 32, 38);
    private static final int INK = Color.rgb(48, 30, 16);

    private static final String CONTROLS =
        "Left stick          move (walks where you look)\n"
        + "Left stick click    first person on / off\n"
        + "A / B               jump / attack\n"
        + "X / Y               change weapon / magic\n"
        + "Left grip           minimap\n"
        + "Right grip          change character\n"
        + "Left trigger        camera control\n"
        + "Right trigger       crawl\n"
        + "Right stick         turn, camera distance\n"
        + "Right stick click   camera behind Goemon\n"
        + "Menu button         pause";

    private GradientDrawable board() {
        GradientDrawable board = new GradientDrawable();
        board.setColor(BOARD);
        board.setCornerRadius(dp(10));
        board.setStroke(dp(3), FRAME_RED);
        return board;
    }

    private Button button(String text, int color, float size, View.OnClickListener action) {
        Button button = new Button(this);
        button.setText(text);
        button.setAllCaps(false);
        button.setTextColor(Color.WHITE);
        button.setTextSize(TypedValue.COMPLEX_UNIT_SP, size);
        button.setTypeface(Typeface.DEFAULT_BOLD);
        GradientDrawable background = new GradientDrawable();
        background.setColor(color);
        background.setCornerRadius(dp(10));
        background.setStroke(dp(2), Color.rgb(60, 20, 10));
        button.setBackground(background);
        button.setOnClickListener(action);
        return button;
    }

    private View buildLayout() {
        int pad = dp(20);
        LinearLayout page = new LinearLayout(this);
        page.setOrientation(LinearLayout.HORIZONTAL);
        page.setPadding(pad, pad, pad, pad);
        page.setBackgroundColor(SEA);

        // Left: the banner, and the controls under it.
        LinearLayout left = new LinearLayout(this);
        left.setOrientation(LinearLayout.VERTICAL);
        page.addView(left, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1.25f));

        int bannerId = getResources().getIdentifier("banner", "drawable", getPackageName());
        if (bannerId != 0) {
            ImageView banner = new ImageView(this);
            banner.setImageResource(bannerId);
            banner.setAdjustViewBounds(true);
            banner.setScaleType(ImageView.ScaleType.FIT_CENTER);
            banner.setContentDescription("Mystical Ninja VR starring Goemon, Meta Quest standalone");
            left.addView(banner, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        }

        TextView controls = new TextView(this);
        controls.setText(CONTROLS);
        controls.setTextColor(INK);
        controls.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        controls.setTypeface(Typeface.MONOSPACE);
        controls.setPadding(dp(18), dp(12), dp(14), dp(12));
        controls.setBackground(board());
        LinearLayout.LayoutParams controlsParams = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        controlsParams.topMargin = dp(12);
        left.addView(controls, controlsParams);

        // Right: the menu, as tall as the left side.
        LinearLayout column = new LinearLayout(this);
        column.setOrientation(LinearLayout.VERTICAL);
        column.setPadding(dp(24), dp(20), dp(24), dp(20));
        column.setBackground(board());
        LinearLayout.LayoutParams columnParams = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 1.0f);
        columnParams.leftMargin = dp(20);
        page.addView(column, columnParams);

        TextView title = new TextView(this);
        title.setText("Mystical Ninja VR");
        title.setTextColor(FRAME_RED);
        title.setTextSize(TypedValue.COMPLEX_UNIT_SP, 30);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        column.addView(title);

        TextView subtitle = new TextView(this);
        subtitle.setText("Starring Goemon, on Meta Quest");
        subtitle.setTextColor(INK);
        subtitle.setTextSize(TypedValue.COMPLEX_UNIT_SP, 16);
        subtitle.setPadding(0, 0, 0, dp(16));
        column.addView(subtitle);

        // Owner: a warning in red that the player must own the game.
        TextView warning = new TextView(this);
        warning.setText("You must own the game. This app does not include Mystical Ninja Starring Goemon: "
            + "use a ROM made from your own cartridge.");
        warning.setTextColor(FRAME_RED);
        warning.setTextSize(TypedValue.COMPLEX_UNIT_SP, 16);
        warning.setTypeface(Typeface.DEFAULT_BOLD);
        warning.setPadding(0, 0, 0, dp(14));
        column.addView(warning);

        TextView message = new TextView(this);
        message.setText(RomPicker.message);
        message.setTextColor(INK);
        message.setTextSize(TypedValue.COMPLEX_UNIT_SP, 16);
        message.setPadding(0, 0, 0, dp(18));
        column.addView(message);

        LinearLayout.LayoutParams first = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(72));
        LinearLayout.LayoutParams next = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(56));
        next.topMargin = dp(14);
        LinearLayout.LayoutParams last = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(56));
        last.topMargin = dp(14);
        if (RomPicker.currentRom != null) {
            column.addView(button("Play", FRAME_RED, 24, v -> done(RomPicker.PLAY_CURRENT)), first);
            column.addView(button("Choose another ROM", Color.rgb(122, 72, 36), 18, v -> openPicker()), next);
        }
        else {
            column.addView(button("Choose ROM file", FRAME_RED, 22, v -> openPicker()), first);
        }
        column.addView(button("Quit", Color.rgb(70, 70, 78), 18, v -> done(RomPicker.QUIT)), last);

        // It still scrolls if a panel is ever smaller.
        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(true);
        scroll.setBackgroundColor(SEA);
        scroll.addView(page);
        return scroll;
    }

    private void showPrompt() {
        setContentView(buildLayout());
    }

    private void openPicker() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");
        try {
            startActivityForResult(intent, PICK_ROM);
        }
        catch (ActivityNotFoundException e) {
            Intent fallback = new Intent(Intent.ACTION_GET_CONTENT);
            fallback.addCategory(Intent.CATEGORY_OPENABLE);
            fallback.setType("*/*");
            try {
                startActivityForResult(Intent.createChooser(fallback, "Choose your ROM"), PICK_ROM);
            }
            catch (ActivityNotFoundException e2) {
                RomPicker.message = "This headset has no file browser to pick the ROM with.\n\n"
                    + "Copy the ROM into Android/data/com.goemon64.recompiled/files/Goemon64Recompiled/ "
                    + "with any name, then start the game again.";
                showPrompt();
            }
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != PICK_ROM) {
            return;
        }

        if ((resultCode != RESULT_OK) || (data == null) || (data.getData() == null)) {
            showPrompt();
            return;
        }

        final Uri uri = data.getData();
        new Thread(() -> {
            final boolean copied = copyTo(uri, RomPicker.destPath);
            if (copied) {
                writeName(uri, RomPicker.destPath + ".name");
            }
            runOnUiThread(() -> {
                if (copied) {
                    done(RomPicker.PICKED);
                }
                else {
                    RomPicker.message = "That file couldn't be read. Please choose it again.";
                    showPrompt();
                }
            });
        }).start();
    }

    private boolean copyTo(Uri uri, String path) {
        try (InputStream in = getContentResolver().openInputStream(uri);
             OutputStream out = new FileOutputStream(path)) {
            if (in == null) {
                return false;
            }

            byte[] buffer = new byte[1 << 16];
            int read;
            while ((read = in.read(buffer)) > 0) {
                out.write(buffer, 0, read);
            }

            return true;
        }
        catch (Exception e) {
            return false;
        }
    }

    // Saves the file's own name so the next start can show which ROM is stored.
    private void writeName(Uri uri, String path) {
        String name = null;
        try (Cursor cursor = getContentResolver().query(uri, new String[] { OpenableColumns.DISPLAY_NAME }, null, null, null)) {
            if ((cursor != null) && cursor.moveToFirst()) {
                name = cursor.getString(0);
            }
        }
        catch (Exception e) {
            name = null;
        }

        if (name == null) {
            name = uri.getLastPathSegment();
        }

        try (OutputStream out = new FileOutputStream(path)) {
            out.write((name != null ? name : "").getBytes(StandardCharsets.UTF_8));
        }
        catch (Exception e) {
            // The name is only shown as a hint.
        }
    }

    private void done(int choice) {
        if (!reported) {
            reported = true;
            RomPicker.report(choice);
        }

        finish();
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        if (!reported && isFinishing()) {
            reported = true;
            RomPicker.report(RomPicker.QUIT);
        }
    }
}
