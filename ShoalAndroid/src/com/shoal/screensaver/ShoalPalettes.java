package com.shoal.screensaver;

/** Colour palettes for the settings picker. Each has 5 colours; "Number of colours" uses the first N. */
final class ShoalPalettes {
    static final String DEFAULT = "ocean", CUSTOM = "custom"; // Ocean: the palette chosen on the TCL TV

    static final String[] IDS =   { "shoal", "reef", "tropical", "ocean", "goldfish", "koi", "sunset", "neon", "pastel", "mono" };
    static final String[] NAMES = { "Shoal", "Reef", "Tropical", "Ocean", "Goldfish", "Koi", "Sunset", "Neon", "Pastel", "Silver" };
    static final int[][] COLOURS = {
        { 0xdce6f0, 0xffcd28, 0xff781e, 0x286ee6, 0x28c8be }, // the desktop defaults
        { 0xff6f91, 0xffc75f, 0x00c9a7, 0x845ec2, 0xf9f871 },
        { 0xff4e50, 0xf9d62e, 0x1fda9a, 0x3d9df3, 0xfc913a },
        { 0x7fd1f7, 0x2f9fe0, 0x1a5fb4, 0x5fe0d0, 0xe0f7ff },
        { 0xff8c00, 0xffd700, 0xff4500, 0xffa94d, 0xfff1d6 },
        { 0xfafafa, 0xff5a1f, 0xd62828, 0xffb347, 0x2b2b2b },
        { 0xffd166, 0xff9f43, 0xef476f, 0xb5179e, 0x7209b7 },
        { 0x39ff14, 0xff073a, 0x00f0ff, 0xfff01f, 0xff00ff },
        { 0xffd1dc, 0xb5ead7, 0xc7ceea, 0xffdac1, 0xe2f0cb },
        { 0xffffff, 0xc8d2dc, 0x96a0aa, 0xe6e6e6, 0x7d8791 },
    };

    /** The palette's colours, or null for "custom" (colour0..9 set over adb) and unknown ids. */
    static int[] colours(String id) {
        for (int i = 0; i < IDS.length; ++i) if (IDS[i].equals(id)) return COLOURS[i];
        return null;
    }

    private ShoalPalettes() { }
}
