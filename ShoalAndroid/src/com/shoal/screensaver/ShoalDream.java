package com.shoal.screensaver;

import android.content.Context;
import android.service.dreams.DreamService;
import android.util.Log;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.widget.FrameLayout;

/**
 * The screensaver. Android starts it after the idle timeout (screen_off_timeout). Any remote key or touch ends the
 * dream and the screen that was showing before comes back unchanged.
 *
 * Projector firmware note: with persist.sys.screenSaver=1 (the factory value) PowerManagerService switches the lamp
 * off when any dream starts, and PhoneWindowManager then drops every key except POWER. The install step sets it to 0
 * (see README.md). Input is also caught by a focused root layout as well as by DreamService itself, so the dream ends
 * on the first key however the firmware routes it.
 */
public final class ShoalDream extends DreamService {
    private ShoalView view;

    private final class KeyCatcher extends FrameLayout {
        KeyCatcher(Context c) {
            super(c);
            setFocusable(true);
            setFocusableInTouchMode(true);
            setDescendantFocusability(FOCUS_BLOCK_DESCENDANTS);
        }
        @Override public boolean dispatchKeyEvent(KeyEvent e) { return wake("key " + KeyEvent.keyCodeToString(e.getKeyCode())); }
        @Override public boolean dispatchTouchEvent(MotionEvent e) { return wake("touch"); }
        @Override public boolean dispatchGenericMotionEvent(MotionEvent e) { return wake("motion"); }
    }

    private boolean wake(String why) {
        Log.i(ShoalRenderer.TAG, "dream woken by " + why);
        wakeUp();
        return true;
    }

    @Override public boolean dispatchKeyEvent(KeyEvent e) { return wake("key " + KeyEvent.keyCodeToString(e.getKeyCode())); }

    @Override public void onAttachedToWindow() {
        super.onAttachedToWindow();
        setInteractive(true); // we handle input ourselves (see class comment)
        setFullscreen(true);
        setScreenBright(true);
        view = new ShoalView(this);
        KeyCatcher root = new KeyCatcher(this);
        root.addView(view);
        setContentView(root);
        root.requestFocus();
    }

    @Override public void onDreamingStarted() { super.onDreamingStarted(); if (view != null) view.onResume(); }
    @Override public void onDreamingStopped() { if (view != null) view.onPause(); super.onDreamingStopped(); }
    @Override public void onDetachedFromWindow() { view = null; super.onDetachedFromWindow(); }
}
