package com.shoal.screensaver;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.ComponentName;
import android.content.Intent;
import android.os.Bundle;
import android.preference.ListPreference;
import android.preference.Preference;
import android.preference.PreferenceFragment;
import android.preference.PreferenceManager;
import android.provider.Settings;
import android.widget.Toast;

/**
 * Settings screen, opened from the launcher tile and from Android's screensaver settings (the dream's
 * settingsActivity). Changes apply the next time the screensaver or the preview starts.
 */
public final class ShoalSettingsActivity extends Activity {
    @Override protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        if (saved == null) getFragmentManager().beginTransaction().replace(android.R.id.content, new Fragment()).commit();
    }

    public static final class Fragment extends PreferenceFragment {
        private static final String DREAM = "com.shoal.screensaver/com.shoal.screensaver.ShoalDream";

        @Override public void onCreate(Bundle saved) {
            super.onCreate(saved);
            getPreferenceManager().setSharedPreferencesName(ShoalSettings.PREFS);
            addPreferencesFromResource(R.xml.preferences);

            findPreference("species").setOnPreferenceChangeListener((p, value) -> {
                @SuppressWarnings("unchecked") java.util.Set<String> set = (java.util.Set<String>) value;
                if (set.isEmpty()) {
                    Toast.makeText(getActivity(), "Choose at least one species", Toast.LENGTH_SHORT).show();
                    return false;
                }
                showSpecies(set);
                return true;
            });
            findPreference("preview").setOnPreferenceClickListener(p -> {
                startActivity(new Intent(getActivity(), ShoalActivity.class));
                return true;
            });
            findPreference("reset").setOnPreferenceClickListener(p -> {
                new AlertDialog.Builder(getActivity()).setTitle("Reset all Shoal settings?")
                    .setPositiveButton("Reset", (d, w) -> {
                        getPreferenceManager().getSharedPreferences().edit().clear().commit();
                        PreferenceManager.setDefaultValues(getActivity(), ShoalSettings.PREFS, MODE_PRIVATE, R.xml.preferences, true);
                        getActivity().recreate();
                    })
                    .setNegativeButton("Cancel", null).show();
                return true;
            });
            findPreference("timeout").setOnPreferenceChangeListener((p, value) -> {
                if (!Settings.System.canWrite(getActivity())) {
                    Toast.makeText(getActivity(), "Not allowed to change this. Run: adb shell appops set "
                        + getActivity().getPackageName() + " WRITE_SETTINGS allow", Toast.LENGTH_LONG).show();
                    return false;
                }
                Settings.System.putInt(getActivity().getContentResolver(), Settings.System.SCREEN_OFF_TIMEOUT,
                    Integer.parseInt((String) value));
                showTimeout();
                return false; // not stored in our prefs; showTimeout() displays the system value
            });
            // This firmware has no screensaver page in Android settings, so the choice is made here.
            // Needs WRITE_SECURE_SETTINGS, granted by build.sh install (pm grant).
            findPreference("status").setOnPreferenceClickListener(p -> {
                boolean selected = isSelected();
                new AlertDialog.Builder(getActivity())
                    .setTitle(selected ? "Switch back to the projector's own screensaver?" : "Use Shoal as the screensaver?")
                    .setPositiveButton(selected ? "Switch back" : "Use Shoal", (d, w) -> {
                        String c = selected ? STOCK : DREAM;
                        try {
                            Settings.Secure.putString(getActivity().getContentResolver(), "screensaver_components", c);
                            Settings.Secure.putString(getActivity().getContentResolver(), "screensaver_default_component", c);
                        } catch (SecurityException e) {
                            Toast.makeText(getActivity(), "Not allowed. Run: adb shell pm grant " + getActivity().getPackageName()
                                + " android.permission.WRITE_SECURE_SETTINGS", Toast.LENGTH_LONG).show();
                        }
                        showStatus();
                    })
                    .setNegativeButton("Cancel", null).show();
                return true;
            });
        }

        private static final String STOCK = "com.htc.screensaver/com.htc.screensaver.ScreenSaverService";

        private boolean isSelected() {
            String current = Settings.Secure.getString(getActivity().getContentResolver(), "screensaver_components");
            ComponentName c = current == null ? null : ComponentName.unflattenFromString(current);
            return c != null && DREAM.equals(c.flattenToString());
        }

        private void showStatus() {
            findPreference("status").setSummary(isSelected() ? "Shoal is the screensaver"
                : "Shoal is not the screensaver. Select to use it.");
        }

        @Override public void onResume() {
            super.onResume();
            showStatus();
            showTimeout();
            showSpecies(((android.preference.MultiSelectListPreference) findPreference("species")).getValues());
        }

        private static final String[] SPECIES = { "Classic", "Sardine", "Goldfish", "Koi", "Tuna", "Trout", "Pike", "Tetra", "Fantail", "Mackerel" };

        private void showSpecies(java.util.Set<String> set) {
            if (set.size() == SPECIES.length) { findPreference("species").setSummary("All ten"); return; }
            StringBuilder b = new StringBuilder();
            for (int i = 1; i <= SPECIES.length; ++i)
                if (set.contains(Integer.toString(i))) b.append(b.length() == 0 ? "" : ", ").append(SPECIES[i - 1]);
            findPreference("species").setSummary(b.toString());
        }

        private void showTimeout() {
            ListPreference p = (ListPreference) findPreference("timeout");
            int ms = Settings.System.getInt(getActivity().getContentResolver(), Settings.System.SCREEN_OFF_TIMEOUT, 1200000);
            p.setValue(Integer.toString(ms));
            int minutes = Math.round(ms / 60000f);
            p.setSummary(minutes >= 60 && minutes % 60 == 0 ? (minutes / 60) + (minutes == 60 ? " hour" : " hours")
                : minutes + (minutes == 1 ? " minute" : " minutes") + " without input");
        }
    }
}
