package com.shoal.screensaver;

import android.content.Context;
import android.content.SharedPreferences;
import android.os.Bundle;

/**
 * ShoalLinux's options, minus rocks. The fish count is set directly (at most MAX_FISH) instead of as screen
 * coverage. Defaults are the desktop ones except fish count, size, size variation, speed, speed variation, depth,
 * number of colours, palette and the frame rate, which follow the values chosen on the TCL G10 TV (2026-11-15).
 * Stored in SharedPreferences "shoal".
 * Keep in step with res/xml/preferences.xml.
 */
final class ShoalSettings {
    int speed = 42;
    static final int MAX_FISH = 1000; // 60 fps with headroom on the Mali-G31 (95 fps unlocked at 1000, see README)
    int boidCount = 100;
    int randomness = 0;
    int sizeTenths = 45;         // fish length in tenths of a percent of screen height
    int speedVariability = 10;
    int tailPercent = 33;
    int colourCount = 3;
    int[] colours = { 0x7fd1f7, 0x2f9fe0, 0x1a5fb4, 0x5fe0d0, 0xe0f7ff, 0xffffff, 0xffffff, 0xffffff, 0xffffff, 0xffffff };
    int tailShade = -40;
    boolean colourSort = true;
    boolean colourAvoid = false;
    int sizeVariation = 150;
    boolean sizeBell = true;
    boolean depthEffect = false;
    boolean showFps = true;
    int speciesMask = 0x3FF;     // bit n = species n+1 (shapes in fish.vert)
    int speciesMode = 0;         // 0 = random mix, 1 = one species per colour
    int frameRate = 0;           // 0 = V-Sync (panel rate), 1 = half, 2 = double, 3 = triple, 4 = unlocked

    static final String PREFS = "shoal";
    private static final java.util.Set<String> BOOL_KEYS =
        new java.util.HashSet<>(java.util.Arrays.asList("coloursort", "colouravoid", "sizebell", "depth", "showfps"));

    /** Reads a number stored as a string (settings screen, extras) or as an int (older builds). */
    private static int getInt(SharedPreferences p, String key, int def) {
        Object v = p.getAll().get(key);
        if (v instanceof Integer) return (Integer) v;
        if (v instanceof String) {
            String s = ((String) v).trim();
            try { return s.startsWith("#") ? Integer.parseInt(s.substring(1), 16) : Integer.parseInt(s); }
            catch (NumberFormatException ignored) { }
        }
        return def;
    }

    static ShoalSettings load(Context context) {
        SharedPreferences p = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        ShoalSettings s = new ShoalSettings();
        s.speed = clamp(getInt(p, "speed", s.speed), 1, 1000);
        s.boidCount = clamp(getInt(p, "boids", s.boidCount), 2, MAX_FISH);
        s.randomness = clamp(getInt(p, "randomness", s.randomness), 0, 100);
        s.sizeTenths = clamp(getInt(p, "size", s.sizeTenths), 10, 100);
        s.speedVariability = clamp(getInt(p, "speedvar", s.speedVariability), 0, 10);
        s.tailPercent = clamp(getInt(p, "tail", s.tailPercent), 10, 100);
        s.tailShade = clamp(getInt(p, "shade", s.tailShade), -100, 100);
        s.colourCount = clamp(getInt(p, "colourcount", s.colourCount), 1, 10);
        s.sizeVariation = clamp(getInt(p, "sizevar", s.sizeVariation), 100, 300);
        s.colourSort = p.getBoolean("coloursort", s.colourSort);
        s.colourAvoid = p.getBoolean("colouravoid", s.colourAvoid);
        s.sizeBell = p.getBoolean("sizebell", s.sizeBell);
        s.depthEffect = p.getBoolean("depth", s.depthEffect);
        s.showFps = p.getBoolean("showfps", s.showFps);
        java.util.Set<String> species = p.getStringSet("species", null);
        if (species != null) {
            int mask = 0;
            for (String n : species) {
                try { int i = Integer.parseInt(n.trim()); if (i >= 1 && i <= 10) mask |= 1 << (i - 1); } catch (NumberFormatException ignored) { }
            }
            if (mask != 0) s.speciesMask = mask;
        }
        s.speciesMode = clamp(getInt(p, "speciesmode", s.speciesMode), 0, 1);
        s.frameRate = clamp(getInt(p, "framerate", s.frameRate), 0, 4);
        // Older builds stored a boolean "vsync"; keep their choice when the new key is missing.
        if (!p.contains("framerate") && p.contains("vsync")) s.frameRate = p.getBoolean("vsync", true) ? 0 : 4;
        String palette = p.getString("palette", ShoalPalettes.DEFAULT);
        int[] chosen = ShoalPalettes.colours(palette);
        if (chosen != null) System.arraycopy(chosen, 0, s.colours, 0, chosen.length);
        else for (int i = 0; i < 10; ++i) s.colours[i] = getInt(p, "colour" + i, s.colours[i]) & 0xffffff; // "custom"
        return s;
    }

    /**
     * Saves options passed as intent extras, e.g.
     * {@code am start -n com.shoal.screensaver/.ShoalActivity --ei speed 60 --ez showfps true --es colour0 '#ff0000'}.
     * Ints may also be given as strings. {@code --ez reset true} restores the defaults first.
     */
    static void saveExtras(Context context, Bundle extras) {
        if (extras == null || extras.isEmpty()) return;
        SharedPreferences.Editor e = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit();
        if (extras.getBoolean("reset", false)) e.clear();
        for (String key : extras.keySet()) {
            String k = key.toLowerCase(java.util.Locale.ROOT);
            if (k.equals("reset")) continue;
            Object v = extras.get(key);
            String str = String.valueOf(v).trim();
            try {
                if (k.equals("species")) {
                    // "1,3,4" -> the multi-select preference's string set
                    java.util.Set<String> set = new java.util.HashSet<>();
                    for (String n : str.split("[, ]+")) if (n.matches("\\d+")) set.add(n);
                    if (!set.isEmpty()) e.putStringSet(k, set);
                } else if (BOOL_KEYS.contains(k)) {
                    e.putBoolean(k, v instanceof Boolean ? (Boolean) v : str.equalsIgnoreCase("true") || (!str.equalsIgnoreCase("false") && Integer.parseInt(str) != 0));
                } else if (k.startsWith("colour") && k.length() == 7) {
                    int c = v instanceof Integer ? (Integer) v : Integer.parseInt(str.replace("#", ""), 16);
                    e.putString(k, String.format("#%06x", c & 0xffffff));
                    e.putString("palette", ShoalPalettes.CUSTOM);
                } else if (k.equals("palette")) {
                    e.putString(k, str.toLowerCase(java.util.Locale.ROOT));
                } else {
                    e.putString(k, Integer.toString(Integer.parseInt(str)));
                }
            } catch (NumberFormatException ignored) { }
        }
        e.apply();
    }

    private static int clamp(int v, int lo, int hi) { return Math.max(lo, Math.min(hi, v)); }
}
