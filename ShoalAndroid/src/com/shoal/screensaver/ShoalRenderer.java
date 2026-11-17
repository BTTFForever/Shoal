package com.shoal.screensaver;

import android.content.Context;
import android.app.Activity;
import android.opengl.EGL14;
import android.opengl.GLES20;
import android.opengl.GLES30;
import android.opengl.GLES31;
import android.opengl.GLSurfaceView;
import android.util.Log;
import android.view.Display;
import android.view.WindowManager;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.FloatBuffer;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Random;

import javax.microedition.khronos.egl.EGLConfig;
import javax.microedition.khronos.opengles.GL10;

/**
 * GLES 3.1 port of ShoalLinux/shoal.cpp: world setup (boids), the compute-shader simulation step and the
 * instanced fish draw. The shaders are ShoalCore/shaders converted to GLSL ES by tools/es_shaders.py.
 * The whole screen is one "monitor". Rocks are left out: the desktop rule (screen at least 900 px tall) never
 * places any on the projector's 1024x600 panel. sim.comp still declares the rock buffer, so a 1-entry dummy is bound
 * with rockCount = 0.
 */
final class ShoalRenderer implements GLSurfaceView.Renderer {
    static final String TAG = "Shoal";
    // Fish mesh, drawn indexed (see fish.vert). The segment counts are the lighter Android ones set by
    // tools/es_shaders.py (ANDROID_SEGMENTS: 6 + 2), not fish.vert's desktop 12 + 4.
    private static final int BODY_SEGMENTS = 6, FIN_SEGMENTS = 2;
    private static final int FISH_INDICES = (BODY_SEGMENTS + FIN_SEGMENTS) * 6 + 6;
    private int fishIndexBuffer;

    private static java.nio.ShortBuffer fishIndices() {
        java.nio.ShortBuffer b = ByteBuffer.allocateDirect(FISH_INDICES * 2).order(ByteOrder.nativeOrder()).asShortBuffer();
        int segments = BODY_SEGMENTS + FIN_SEGMENTS;
        for (int n = 0; n < segments; ++n) {
            short a0 = (short) (2 * n), a1 = (short) (a0 + 1), b0 = (short) (a0 + 2), b1 = (short) (a0 + 3);
            b.put(a0).put(b0).put(a1).put(b0).put(b1).put(a1);
        }
        for (int k = 0; k < 6; ++k) b.put((short) (2 * (segments + 1) + k));
        b.flip();
        return b;
    }
    private static final int BOID_FLOATS = 18, OVERLAY_FLOATS = 6, MAX_OVERLAY_VERTICES = 2048;

    private static final class Boid { float x, y, vx, vy, breakaway, turnRate, turnTarget, length, depth; }

    private final Context context;
    private final ShoalSettings s;
    private final Random rng = new Random();

    private final int frameRateMode;   // 0 = V-Sync (panel), 1 = half, 2 = double, 3 = triple, 4 = unlocked
    private int width, height, boidLength;
    private float panelHz = 60.0f;      // the display's refresh rate, read when the surface is created
    private long frameDeadline;         // schedule used by the paced modes (half, double, triple)
    private final List<Boid> boids = new ArrayList<>();

    private boolean gpuOk;
    private int simProgram, fishProgram, overlayProgram, shadowLoc;
    private int boidBuffer, monitorBuffer, rockBuffer, simUbo, renderUbo, speciesUbo, overlayVbo, overlayVao, fishVao, spineBuffer;
    // Spine chain per fish (sim.comp updateSpine, fish.vert): points from nose to tail tip, and the head pivot.
    private static final int SPINE_POINTS = 10;
    private static final float SPINE_PIVOT = 0.3f;
    private final java.nio.IntBuffer speciesData = ByteBuffer.allocateDirect(16 * 4).order(ByteOrder.nativeOrder()).asIntBuffer();
    private final FloatBuffer simConstants = floats(16), renderConstants = floats(52), overlay = floats(MAX_OVERLAY_VERTICES * OVERLAY_FLOATS);
    private float simTime;
    private long lastNanos, fpsNanos, logNanos;
    private int frames, fps, logFrames;

    // Benchmark (ShoalActivity --ez bench true): frame rate unlocked, fish counts stepped up until under BENCH_MIN_FPS.
    private static final int[] BENCH_COUNTS = { 250, 500, 700, 1000, 1500, 2000, 2500, 3000, 4000, 5000, 6000, 8000, 10000 };
    private static final float BENCH_MIN_FPS = 5;
    private final boolean bench;
    private int benchStage = -1, benchPhase, benchFrames;
    private long benchPhaseStart, benchSimNanos, benchDrawNanos;
    private float benchFps;
    private final StringBuilder benchTable = new StringBuilder();

    ShoalRenderer(Context context, ShoalSettings settings, boolean bench) {
        this.context = context; this.s = settings; this.bench = bench; this.frameRateMode = settings.frameRate;
        if (bench) s.showFps = true;
    }

    // ------------------------------------------------------------------ GL callbacks

    @Override public void onSurfaceCreated(GL10 unused, EGLConfig config) {
        width = height = 0; // context is new: everything is rebuilt in onSurfaceChanged
        panelHz = panelRefreshRate();
        // Mode 0 waits for vblank; the other rates free run and are paced in onDrawFrame, because this driver
        // clamps eglSwapInterval to 1 (a swap interval of 2 still gives 60 fps). The benchmark runs unlocked.
        int interval = bench || frameRateMode != 0 ? 0 : 1;
        EGL14.eglSwapInterval(EGL14.eglGetCurrentDisplay(), interval);
        gpuOk = compilePrograms();
        int[] v = new int[1];
        GLES20.glGetIntegerv(GLES31.GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS, v, 0);
        Log.i(TAG, "GL " + GLES20.glGetString(GLES20.GL_VERSION) + " / " + GLES20.glGetString(GLES20.GL_RENDERER)
            + ", vertex SSBOs " + v[0] + ", programs " + (gpuOk ? "ok" : "FAILED") + ", " + panelHz + " Hz panel, "
            + "frame rate mode " + frameRateMode + " (" + targetFps() + ")");
    }

    @Override public void onSurfaceChanged(GL10 unused, int w, int h) {
        if (w == width && h == height) return;
        width = w; height = h;
        GLES20.glViewport(0, 0, width, height);
        boidLength = Math.max(4, Math.round(height * s.sizeTenths / 1000.0f));
        createBoids();
        if (gpuOk) uploadWorld();
        simTime = 0;
        lastNanos = 0;
        frameDeadline = 0;
        Log.i(TAG, "surface " + w + "x" + h + ": " + boids.size() + " fish of length " + boidLength);
    }

    @Override public void onDrawFrame(GL10 unused) {
        long now = System.nanoTime();
        if (!gpuOk) { GLES20.glClearColor(0.3f, 0, 0, 1); GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT); return; }
        float dt = lastNanos == 0 ? 1.0f / 60.0f : (now - lastNanos) / 1e9f;
        lastNanos = now;
        dt = Math.max(0.0001f, Math.min(dt, 0.1f));
        if (bench && benchFrame(now)) {
            long t0 = System.nanoTime();
            step(dt);
            GLES20.glFinish();
            long t1 = System.nanoTime();
            render();
            GLES20.glFinish();
            benchSimNanos += t1 - t0;
            benchDrawNanos += System.nanoTime() - t1;
        } else {
            step(dt);
            render();
            if (!bench && frameRateMode != 0 && frameRateMode != 4) paceFrame(now);
        }
        ++frames; ++logFrames;
        if (fpsNanos == 0) fpsNanos = logNanos = now;
        if (now - fpsNanos >= 1_000_000_000L) { fps = frames; frames = 0; fpsNanos = now; }
        if (now - logNanos >= 10_000_000_000L) {
            Log.i(TAG, String.format(java.util.Locale.ROOT, "%.1f fps, %d fish", logFrames / ((now - logNanos) / 1e9), boids.size()));
            logFrames = 0; logNanos = now;
        }
    }

    /** The frames per second the frame rate mode asks for, for logging. */
    private String targetFps() {
        if (bench || frameRateMode == 4) return "unlocked";
        float f = frameRateMode == 1 ? panelHz * 0.5f : frameRateMode == 2 ? panelHz * 2 : frameRateMode == 3 ? panelHz * 3 : panelHz;
        return Math.round(f) + " fps";
    }

    private float panelRefreshRate() {
        try {
            WindowManager wm = (WindowManager) context.getSystemService(Context.WINDOW_SERVICE);
            Display d = wm != null ? wm.getDefaultDisplay() : null;
            float hz = d != null ? d.getRefreshRate() : 0.0f;
            return hz > 1.0f ? hz : 60.0f;
        } catch (RuntimeException e) {
            return 60.0f;
        }
    }

    /**
     * Holds the frame rate at the mode's multiple of the panel refresh (half, double or triple). The swap does not
     * block in these modes, so the caller waits for a running schedule instead; if a frame overruns, the schedule
     * restarts from it.
     */
    private void paceFrame(long frameStart) {
        float multiple = frameRateMode == 1 ? 0.5f : frameRateMode == 3 ? 3.0f : 2.0f;
        long period = (long) (1e9 / (panelHz * multiple));
        frameDeadline += period;
        if (frameDeadline < frameStart) frameDeadline = frameStart + period;
        while (System.nanoTime() < frameDeadline) Thread.yield();
    }

    // ------------------------------------------------------------------ benchmark

    /**
     * Runs before each benchmark frame. Phases per fish count: 0 = warm-up (1.5 s), 1 = unlocked frame rate (4 s),
     * 2 = GPU cost split, timed with glFinish after the simulation and after the draw (2 s). Each timed phase needs
     * at least 3 frames. Returns true while the caller should time the frame (phase 2).
     */
    private boolean benchFrame(long now) {
        if (benchStage < 0 || benchStage >= BENCH_COUNTS.length) {
            if (benchStage >= 0) return false; // finished, waiting for the activity to close
            benchTable.append(String.format(java.util.Locale.ROOT, "%6s %8s %8s %8s%n", "fish", "fps", "sim ms", "draw ms"));
            startBenchStage(0, now);
            return false;
        }
        double elapsed = (now - benchPhaseStart) / 1e9;
        double[] phaseSeconds = { 1.5, 4.0, 2.0 };
        if (elapsed >= phaseSeconds[benchPhase] && (benchPhase == 0 || benchFrames >= 3)) {
            if (benchPhase == 1) benchFps = (float) (benchFrames / elapsed);
            if (benchPhase == 2) {
                String row = String.format(java.util.Locale.ROOT, "%6d %8.1f %8.2f %8.2f", boids.size(), benchFps,
                    benchSimNanos / 1e6 / benchFrames, benchDrawNanos / 1e6 / benchFrames);
                Log.i(TAG, "BENCH " + row);
                benchTable.append(row).append('\n');
                if (benchFps < BENCH_MIN_FPS || benchStage + 1 >= BENCH_COUNTS.length) {
                    benchStage = BENCH_COUNTS.length;
                    Log.i(TAG, "BENCH done (" + width + "x" + height + ", fish length " + boidLength + ", size variation "
                        + s.sizeVariation + ")\n" + benchTable);
                    if (context instanceof Activity) ((Activity) context).runOnUiThread(((Activity) context)::finish);
                    return false;
                }
                startBenchStage(benchStage + 1, now);
                return false;
            }
            benchPhase++;
            benchPhaseStart = now;
            benchFrames = 0;
            benchSimNanos = benchDrawNanos = 0;
        }
        benchFrames++;
        return benchPhase == 2;
    }

    private void startBenchStage(int stage, long now) {
        benchStage = stage;
        benchPhase = 0;
        benchPhaseStart = now;
        benchFrames = 0;
        s.boidCount = BENCH_COUNTS[stage];
        createBoids();
        uploadWorld();
        simTime = 0;
        lastNanos = 0;
    }

    // ------------------------------------------------------------------ world setup (shoal.cpp CreateBoids)

    private float rand(float lo, float hi) { return lo + (hi - lo) * rng.nextFloat(); }
    private void createBoids() {
        boids.clear();
        int count = s.boidCount;
        for (int i = 0; i < count; ++i) {
            Boid b = new Boid();
            float u = rand(-1, 1);
            if (s.sizeBell) u = (u + rand(-1, 1) + rand(-1, 1)) / 3.0f;
            b.length = Math.max(4.0f, (float) (boidLength * Math.pow(s.sizeVariation / 100.0, u)));
            b.x = rand(b.length, width - b.length);
            b.y = rand(b.length, height - b.length);
            float angle = rand(0, (float) (2 * Math.PI));
            b.vx = (float) Math.cos(angle) * s.speed;
            b.vy = (float) Math.sin(angle) * s.speed;
            b.breakaway = angle;
            b.turnRate = rand(-0.35f, 0.35f);
            b.turnTarget = rand(-0.35f, 0.35f);
            boids.add(b);
        }
        // Depth rank: small fish lower, ties broken randomly.
        Integer[] ranked = new Integer[boids.size()];
        float[] tie = new float[boids.size()];
        for (int i = 0; i < ranked.length; ++i) { ranked[i] = i; tie[i] = rng.nextFloat(); }
        Arrays.sort(ranked, (a, b) -> {
            int c = Float.compare(boids.get(a).length, boids.get(b).length);
            return c != 0 ? c : Float.compare(tie[a], tie[b]);
        });
        for (int r = 0; r < ranked.length; ++r) boids.get(ranked[r]).depth = ranked.length > 1 ? (float) r / (ranked.length - 1) : 0.5f;
    }

    // ------------------------------------------------------------------ GPU

    private static FloatBuffer floats(int n) { return ByteBuffer.allocateDirect(n * 4).order(ByteOrder.nativeOrder()).asFloatBuffer(); }

    private String asset(String name) throws IOException {
        try (InputStream in = context.getAssets().open("shaders/" + name)) {
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] buf = new byte[8192];
            for (int n; (n = in.read(buf)) > 0; ) out.write(buf, 0, n);
            return out.toString("UTF-8");
        }
    }

    private int stage(int type, String name) {
        try {
            int sh = GLES20.glCreateShader(type);
            GLES20.glShaderSource(sh, asset(name));
            GLES20.glCompileShader(sh);
            int[] ok = new int[1];
            GLES20.glGetShaderiv(sh, GLES20.GL_COMPILE_STATUS, ok, 0);
            if (ok[0] == 0) { Log.e(TAG, "shader " + name + " failed:\n" + GLES20.glGetShaderInfoLog(sh)); return 0; }
            return sh;
        } catch (IOException e) { Log.e(TAG, "missing shader " + name, e); return 0; }
    }

    private int program(String name, int... stages) {
        for (int st : stages) if (st == 0) return 0;
        int p = GLES20.glCreateProgram();
        for (int st : stages) GLES20.glAttachShader(p, st);
        GLES20.glLinkProgram(p);
        int[] ok = new int[1];
        GLES20.glGetProgramiv(p, GLES20.GL_LINK_STATUS, ok, 0);
        if (ok[0] == 0) { Log.e(TAG, "program " + name + " failed:\n" + GLES20.glGetProgramInfoLog(p)); return 0; }
        return p;
    }

    private boolean compilePrograms() {
        simProgram = program("sim", stage(GLES31.GL_COMPUTE_SHADER, "sim.comp"));
        fishProgram = program("fish", stage(GLES20.GL_VERTEX_SHADER, "fish.vert"), stage(GLES20.GL_FRAGMENT_SHADER, "fish.frag"));
        overlayProgram = program("overlay", stage(GLES20.GL_VERTEX_SHADER, "overlay.vert"), stage(GLES20.GL_FRAGMENT_SHADER, "overlay.frag"));
        if (simProgram == 0 || fishProgram == 0 || overlayProgram == 0) return false;
        shadowLoc = GLES20.glGetUniformLocation(fishProgram, "uShadow");

        int[] ids = new int[8];
        GLES20.glGenBuffers(8, ids, 0);
        spineBuffer = ids[7];
        boidBuffer = ids[0]; monitorBuffer = ids[1]; rockBuffer = ids[2]; simUbo = ids[3]; renderUbo = ids[4]; overlayVbo = ids[5];
        speciesUbo = ids[6];
        GLES20.glBindBuffer(GLES30.GL_UNIFORM_BUFFER, speciesUbo);
        GLES20.glBufferData(GLES30.GL_UNIFORM_BUFFER, 16 * 4, null, GLES20.GL_DYNAMIC_DRAW);
        GLES20.glBindBuffer(GLES30.GL_UNIFORM_BUFFER, simUbo);
        GLES20.glBufferData(GLES30.GL_UNIFORM_BUFFER, 16 * 4, null, GLES20.GL_DYNAMIC_DRAW);
        GLES20.glBindBuffer(GLES30.GL_UNIFORM_BUFFER, renderUbo);
        GLES20.glBufferData(GLES30.GL_UNIFORM_BUFFER, 52 * 4, null, GLES20.GL_DYNAMIC_DRAW);

        int[] vaos = new int[2];
        GLES30.glGenVertexArrays(2, vaos, 0);
        fishVao = vaos[0]; overlayVao = vaos[1];
        // fish.vert reads the boid SSBO as per-instance attributes (no vertex-stage SSBOs on Mali-G31).
        GLES30.glBindVertexArray(fishVao);
        GLES20.glBindBuffer(GLES20.GL_ARRAY_BUFFER, boidBuffer);
        for (int a = 0; a < 5; ++a) {
            GLES20.glEnableVertexAttribArray(a);
            GLES20.glVertexAttribPointer(a, a == 4 ? 2 : 4, GLES20.GL_FLOAT, false, BOID_FLOATS * 4, a * 16);
            GLES30.glVertexAttribDivisor(a, 1);
        }
        // The spine chain as five more per-instance attributes (locations 5-9, two points each), from the same buffer
        // that sim.comp writes as SSBO 3 (Mali-G31 has no vertex-shader SSBOs).
        GLES20.glBindBuffer(GLES20.GL_ARRAY_BUFFER, spineBuffer);
        for (int a = 0; a < 5; ++a) {
            GLES20.glEnableVertexAttribArray(5 + a);
            GLES20.glVertexAttribPointer(5 + a, 4, GLES20.GL_FLOAT, false, SPINE_POINTS * 2 * 4, a * 16);
            GLES30.glVertexAttribDivisor(5 + a, 1);
        }
        int[] ib = new int[1];
        GLES20.glGenBuffers(1, ib, 0);
        fishIndexBuffer = ib[0];
        GLES20.glBindBuffer(GLES20.GL_ELEMENT_ARRAY_BUFFER, fishIndexBuffer); // recorded in fishVao
        GLES20.glBufferData(GLES20.GL_ELEMENT_ARRAY_BUFFER, FISH_INDICES * 2, fishIndices(), GLES20.GL_STATIC_DRAW);
        GLES30.glBindVertexArray(overlayVao);
        GLES20.glBindBuffer(GLES20.GL_ARRAY_BUFFER, overlayVbo);
        GLES20.glBufferData(GLES20.GL_ARRAY_BUFFER, MAX_OVERLAY_VERTICES * OVERLAY_FLOATS * 4, null, GLES20.GL_DYNAMIC_DRAW);
        GLES20.glEnableVertexAttribArray(0);
        GLES20.glVertexAttribPointer(0, 2, GLES20.GL_FLOAT, false, OVERLAY_FLOATS * 4, 0);
        GLES20.glEnableVertexAttribArray(1);
        GLES20.glVertexAttribPointer(1, 4, GLES20.GL_FLOAT, false, OVERLAY_FLOATS * 4, 8);
        GLES30.glBindVertexArray(0);
        return true;
    }

    private void uploadWorld() {
        FloatBuffer b = floats(boids.size() * BOID_FLOATS);
        for (int i = 0; i < boids.size(); ++i) {
            Boid o = boids.get(i);
            float phase = (float) ((i * 2.39996323) % (2 * Math.PI)), sp = s.speed;
            b.put(new float[] { o.x, o.y, o.vx, o.vy, o.turnRate, o.turnTarget, 0, 0, (float) Math.atan2(o.vy, o.vx), sp, 0,
                o.breakaway, sp, 0, phase, 1, o.length, o.depth });
        }
        b.flip();
        GLES20.glBindBuffer(GLES31.GL_SHADER_STORAGE_BUFFER, boidBuffer);
        GLES20.glBufferData(GLES31.GL_SHADER_STORAGE_BUFFER, b.capacity() * 4, b, GLES20.GL_DYNAMIC_DRAW);

        // Spines start straight behind each fish's heading.
        FloatBuffer sp = floats(Math.max(1, boids.size()) * SPINE_POINTS * 2);
        for (Boid o : boids) {
            float h = (float) Math.atan2(o.vy, o.vx), cx = (float) Math.cos(h), cy = (float) Math.sin(h);
            for (int j = 0; j < SPINE_POINTS; ++j) {
                float back = o.length * ((float) j / (SPINE_POINTS - 1) - SPINE_PIVOT);
                sp.put(o.x - cx * back).put(o.y - cy * back);
            }
        }
        sp.position(0);
        GLES20.glBindBuffer(GLES31.GL_SHADER_STORAGE_BUFFER, spineBuffer);
        GLES20.glBufferData(GLES31.GL_SHADER_STORAGE_BUFFER, sp.capacity() * 4, sp, GLES20.GL_DYNAMIC_DRAW);

        FloatBuffer m = floats(4);
        m.put(new float[] { 0, 0, width, height }).flip();
        GLES20.glBindBuffer(GLES31.GL_SHADER_STORAGE_BUFFER, monitorBuffer);
        GLES20.glBufferData(GLES31.GL_SHADER_STORAGE_BUFFER, 16, m, GLES20.GL_STATIC_DRAW);

        GLES20.glBindBuffer(GLES31.GL_SHADER_STORAGE_BUFFER, rockBuffer); // unused by the shader (rockCount = 0)
        GLES20.glBufferData(GLES31.GL_SHADER_STORAGE_BUFFER, 64, floats(16), GLES20.GL_STATIC_DRAW);
    }

    private void step(float dt) {
        simTime += dt;
        simConstants.clear();
        simConstants.put(new float[] { dt, s.speed, boids.size(), s.randomness, 1, simTime, 0, 0, boidLength,
            s.speedVariability, s.colourCount, s.colourCount > 1 && s.colourSort ? 1 : 0, s.colourSort && s.colourAvoid ? 1 : 0, 0, 0, 0 });
        simConstants.flip();
        GLES20.glBindBuffer(GLES30.GL_UNIFORM_BUFFER, simUbo);
        GLES20.glBufferSubData(GLES30.GL_UNIFORM_BUFFER, 0, 16 * 4, simConstants);
        GLES30.glBindBufferBase(GLES30.GL_UNIFORM_BUFFER, 0, simUbo);
        GLES30.glBindBufferBase(GLES31.GL_SHADER_STORAGE_BUFFER, 0, boidBuffer);
        GLES30.glBindBufferBase(GLES31.GL_SHADER_STORAGE_BUFFER, 1, monitorBuffer);
        GLES30.glBindBufferBase(GLES31.GL_SHADER_STORAGE_BUFFER, 2, rockBuffer);
        GLES30.glBindBufferBase(GLES31.GL_SHADER_STORAGE_BUFFER, 3, spineBuffer);
        int groups = (boids.size() + 63) / 64;
        GLES20.glUseProgram(simProgram);
        GLES31.glDispatchCompute(groups, 1, 1);
        GLES31.glMemoryBarrier(GLES31.GL_SHADER_STORAGE_BARRIER_BIT | GLES31.GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT);
    }

    private void render() {
        boolean depth = s.depthEffect;
        GLES20.glDepthMask(true);
        GLES20.glClearColor(0, 0, 0, 1);
        GLES20.glClearDepthf(1);
        GLES20.glClearStencil(0);
        GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT | GLES20.GL_DEPTH_BUFFER_BIT | GLES20.GL_STENCIL_BUFFER_BIT);

        renderConstants.clear();
        renderConstants.put(new float[] { 0, 0, width, height, boidLength, 1, 0, s.tailPercent / 100.0f, s.colourCount,
            s.tailShade / 100.0f, depth ? 1 : 0, 0 });
        for (int i = 0; i < 10; ++i) {
            int c = s.colours[i];
            renderConstants.put(new float[] { ((c >> 16) & 255) / 255.0f, ((c >> 8) & 255) / 255.0f, (c & 255) / 255.0f, 1 });
        }
        renderConstants.flip();
        GLES20.glBindBuffer(GLES30.GL_UNIFORM_BUFFER, renderUbo);
        GLES20.glBufferSubData(GLES30.GL_UNIFORM_BUFFER, 0, 52 * 4, renderConstants);
        GLES30.glBindBufferBase(GLES30.GL_UNIFORM_BUFFER, 1, renderUbo);
        // SpeciesSet in fish.vert: (count, mode, -, -) then the enabled species numbers, 0-based.
        speciesData.clear();
        int enabled = 0;
        int[] list = new int[12];
        for (int i = 0; i < 10; ++i) if ((s.speciesMask & (1 << i)) != 0) list[enabled++] = i;
        if (enabled == 0) enabled = 1; // nothing chosen: the classic fish (list[0] is 0)
        speciesData.put(new int[] { enabled, s.speciesMode, 0, 0 }).put(list).flip();
        GLES20.glBindBuffer(GLES30.GL_UNIFORM_BUFFER, speciesUbo);
        GLES20.glBufferSubData(GLES30.GL_UNIFORM_BUFFER, 0, 16 * 4, speciesData);
        GLES30.glBindBufferBase(GLES30.GL_UNIFORM_BUFFER, 2, speciesUbo);
        GLES30.glBindBufferBase(GLES31.GL_SHADER_STORAGE_BUFFER, 0, boidBuffer);
        GLES30.glBindBufferBase(GLES31.GL_SHADER_STORAGE_BUFFER, 1, monitorBuffer);

        GLES30.glBindVertexArray(fishVao);
        GLES20.glUseProgram(fishProgram);
        int count = boids.size();
        if (depth) { GLES20.glEnable(GLES20.GL_DEPTH_TEST); GLES20.glDepthFunc(GLES20.GL_LESS); }
        GLES20.glUniform1i(shadowLoc, 0);
        GLES30.glDrawElementsInstanced(GLES20.GL_TRIANGLES, FISH_INDICES, GLES20.GL_UNSIGNED_SHORT, 0, count);
        if (depth) {
            GLES20.glDepthMask(false);
            GLES20.glEnable(GLES20.GL_STENCIL_TEST);
            GLES20.glStencilFunc(GLES20.GL_EQUAL, 0, 0xFF);
            GLES20.glStencilOp(GLES20.GL_KEEP, GLES20.GL_KEEP, GLES20.GL_INCR);
            GLES20.glStencilMask(0xFF);
            GLES20.glEnable(GLES20.GL_BLEND);
            GLES20.glBlendFuncSeparate(GLES20.GL_ZERO, GLES20.GL_SRC_COLOR, GLES20.GL_ZERO, GLES20.GL_ONE);
            GLES20.glUniform1i(shadowLoc, 1);
            GLES30.glDrawElementsInstanced(GLES20.GL_TRIANGLES, FISH_INDICES, GLES20.GL_UNSIGNED_SHORT, 0, count);
            GLES20.glDisable(GLES20.GL_BLEND);
            GLES20.glDisable(GLES20.GL_STENCIL_TEST);
            GLES20.glDisable(GLES20.GL_DEPTH_TEST);
            GLES20.glDepthMask(true);
        }

        int vertices = buildOverlay();
        if (vertices > 0) {
            GLES30.glBindVertexArray(overlayVao);
            GLES20.glBindBuffer(GLES20.GL_ARRAY_BUFFER, overlayVbo);
            GLES20.glBufferSubData(GLES20.GL_ARRAY_BUFFER, 0, vertices * OVERLAY_FLOATS * 4, overlay);
            GLES20.glUseProgram(overlayProgram);
            GLES20.glDrawArrays(GLES20.GL_TRIANGLES, 0, vertices);
        }
        GLES30.glBindVertexArray(0);
    }

    // ------------------------------------------------------------------ overlay: the FPS counter

    private void add(float x, float y, float shade) {
        if (overlay.position() + OVERLAY_FLOATS > overlay.capacity()) return;
        overlay.put(x / width * 2 - 1).put(1 - y / height * 2).put(shade).put(shade).put(shade).put(1);
    }

    private void quad(float x, float y, float w, float h) {
        add(x, y, 1); add(x + w, y, 1); add(x, y + h, 1); add(x + w, y, 1); add(x + w, y + h, 1); add(x, y + h, 1);
    }

    private static final boolean[][] SEGMENTS = {
        { true, true, true, true, true, true, false }, { false, true, true, false, false, false, false },
        { true, true, false, true, true, false, true }, { true, true, true, true, false, false, true },
        { false, true, true, false, false, true, true }, { true, false, true, true, false, true, true },
        { true, false, true, true, true, true, true }, { true, true, true, false, false, false, false },
        { true, true, true, true, true, true, true }, { true, true, true, true, false, true, true } };

    private int buildOverlay() {
        overlay.clear();
        if (s.showFps) {
            String digits = Integer.toString(fps);
            float t = 2, dw = 10, dh = 18, startX = width - 18.0f * digits.length() - 12;
            for (int di = 0; di < digits.length(); ++di) {
                boolean[] seg = SEGMENTS[digits.charAt(di) - '0'];
                float x = startX + di * 18, y = 10;
                if (seg[0]) quad(x + 1, y, dw - 2, t);
                if (seg[1]) quad(x + dw - t, y + 1, t, dh * 0.5f - 1);
                if (seg[2]) quad(x + dw - t, y + dh * 0.5f, t, dh * 0.5f - 1);
                if (seg[3]) quad(x + 1, y + dh - t, dw - 2, t);
                if (seg[4]) quad(x, y + dh * 0.5f, t, dh * 0.5f - 1);
                if (seg[5]) quad(x, y + 1, t, dh * 0.5f - 1);
                if (seg[6]) quad(x + 1, y + dh * 0.5f - t * 0.5f, dw - 2, t);
            }
        }
        int vertices = overlay.position() / OVERLAY_FLOATS / 3 * 3;
        overlay.flip();
        return vertices;
    }
}
