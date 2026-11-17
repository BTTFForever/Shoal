package com.shoal.screensaver;

import android.app.AlertDialog;
import android.content.Context;
import android.graphics.drawable.GradientDrawable;
import android.preference.ListPreference;
import android.util.AttributeSet;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.BaseAdapter;
import android.widget.CheckedTextView;
import android.widget.LinearLayout;

/** List preference whose rows and current value show the palette's colour swatches. */
public final class PalettePreference extends ListPreference {
    public PalettePreference(Context context, AttributeSet attrs) {
        super(context, attrs);
        setEntries(ShoalPalettes.NAMES);
        setEntryValues(ShoalPalettes.IDS);
        setDefaultValue(ShoalPalettes.DEFAULT);
    }

    private int dp(float v) {
        return Math.round(TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v, getContext().getResources().getDisplayMetrics()));
    }

    /** Five fish-shaped (pill) swatches. */
    private LinearLayout swatches(int[] colours) {
        LinearLayout row = new LinearLayout(getContext());
        row.setGravity(Gravity.CENTER_VERTICAL);
        for (int c : colours) {
            View v = new View(getContext());
            GradientDrawable d = new GradientDrawable();
            d.setColor(0xff000000 | c);
            d.setCornerRadius(dp(6));
            v.setBackground(d);
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(dp(26), dp(12));
            lp.leftMargin = dp(6);
            row.addView(v, lp);
        }
        return row;
    }

    @Override protected void onBindView(View view) {
        super.onBindView(view);
        ViewGroup frame = view.findViewById(android.R.id.widget_frame);
        if (frame == null) return;
        frame.removeAllViews();
        int[] colours = ShoalPalettes.colours(getValue());
        if (colours == null) { frame.setVisibility(View.GONE); return; }
        frame.addView(swatches(colours));
        frame.setVisibility(View.VISIBLE);
    }

    @Override public CharSequence getSummary() {
        CharSequence entry = getEntry();
        return entry != null ? entry : "Custom (set over adb)";
    }

    @Override protected void onPrepareDialogBuilder(AlertDialog.Builder builder) {
        int checked = findIndexOfValue(getValue());
        BaseAdapter adapter = new BaseAdapter() {
            @Override public int getCount() { return ShoalPalettes.IDS.length; }
            @Override public Object getItem(int i) { return ShoalPalettes.IDS[i]; }
            @Override public long getItemId(int i) { return i; }
            @Override public View getView(int i, View convert, ViewGroup parent) {
                LinearLayout row = new LinearLayout(getContext());
                row.setGravity(Gravity.CENTER_VERTICAL);
                row.setPadding(dp(20), dp(10), dp(20), dp(10));
                CheckedTextView name = (CheckedTextView) View.inflate(getContext(), android.R.layout.simple_list_item_single_choice, null);
                name.setText(ShoalPalettes.NAMES[i]);
                name.setChecked(i == checked);
                row.addView(name, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
                row.addView(swatches(ShoalPalettes.COLOURS[i]));
                return row;
            }
        };
        builder.setAdapter(adapter, (dialog, which) -> {
            String value = ShoalPalettes.IDS[which];
            if (callChangeListener(value)) { setValue(value); notifyChanged(); }
            dialog.dismiss();
        });
        builder.setPositiveButton(null, null);
    }
}
