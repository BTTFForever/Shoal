package com.shoal.screensaver;

import android.app.Activity;
import android.os.Bundle;
import android.os.SystemClock;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowManager;

/**
 * Runs Shoal as a normal full-screen app (launcher tile). Any key closes it after a 1 s grace period, like the desktop
 * build. Intent extras are saved as settings and also used by the screensaver, see {@link ShoalSettings#saveExtras}.
 */
public final class ShoalActivity extends Activity {
    private ShoalView view;
    private long startedAt;

    @Override protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        boolean bench = getIntent().getBooleanExtra("bench", false);
        if (!bench) ShoalSettings.saveExtras(this, getIntent().getExtras());
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        view = new ShoalView(this, bench);
        setContentView(view);
        hideSystemUi();
    }

    private void hideSystemUi() {
        getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_FULLSCREEN | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
            | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY | View.SYSTEM_UI_FLAG_LAYOUT_STABLE
            | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION);
    }

    @Override protected void onResume() { super.onResume(); startedAt = SystemClock.uptimeMillis(); view.onResume(); hideSystemUi(); }
    @Override protected void onPause() { view.onPause(); super.onPause(); }

    private boolean ready() { return SystemClock.uptimeMillis() - startedAt > 1000; }

    @Override public boolean dispatchKeyEvent(KeyEvent e) {
        if (e.getKeyCode() == KeyEvent.KEYCODE_HOME || e.getKeyCode() == KeyEvent.KEYCODE_POWER) return super.dispatchKeyEvent(e);
        if (e.getAction() == KeyEvent.ACTION_UP && ready()) finish();
        return true;
    }

    @Override public boolean dispatchTouchEvent(MotionEvent e) {
        if (e.getAction() == MotionEvent.ACTION_UP && ready()) finish();
        return true;
    }
}
