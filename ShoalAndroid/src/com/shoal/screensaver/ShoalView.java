package com.shoal.screensaver;

import android.content.Context;
import android.opengl.GLSurfaceView;

/** GLES 3 surface with depth + stencil (the shadow pass needs both), rendering continuously at the frame rate setting. */
final class ShoalView extends GLSurfaceView {
    ShoalView(Context context) { this(context, false); }

    /** @param bench run the benchmark instead of the normal settings (frame rate unlocked, see ShoalRenderer). */
    ShoalView(Context context, boolean bench) {
        super(context);
        setEGLContextClientVersion(3);
        setEGLConfigChooser(8, 8, 8, 8, 24, 8);
        setPreserveEGLContextOnPause(true);
        setRenderer(new ShoalRenderer(context, ShoalSettings.load(context), bench));
        setRenderMode(RENDERMODE_CONTINUOUSLY);
        setFocusable(false);
    }
}
