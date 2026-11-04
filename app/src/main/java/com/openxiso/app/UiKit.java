package com.openxiso.app;

/*
 * UiKit.java — фабрика элементов интерфейса на платформенных Android View.
 *
 * Никаких внешних библиотек: все формы, скругления, обводки и ripple создаются
 * программно (GradientDrawable / RippleDrawable), чтобы цвета в точности
 * совпадали с палитрой #1A73E8 / #FFFFFF / #F1F3F4 / #DADCE0 / #202124 / #5F6368.
 * Плитки квадратные 120 x 120 dp с радиусом 16 dp и плоской рамкой 1 dp.
 */

import android.content.Context;
import android.content.res.ColorStateList;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.RippleDrawable;
import android.text.TextUtils;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.widget.FrameLayout;
import android.widget.GridLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.Space;
import android.widget.TextView;

final class UiKit {

    private UiKit() {
        // Только статические методы.
    }

    static int dp(Context context, float value) {
        return Math.round(TypedValue.applyDimension(
                TypedValue.COMPLEX_UNIT_DIP, value, context.getResources().getDisplayMetrics()));
    }

    static int color(Context context, int resourceId) {
        return context.getColor(resourceId);
    }

    static GradientDrawable roundedRect(Context context, int fillColor, int strokeColor,
                                        float radiusDp, int strokeWidthPx) {
        final GradientDrawable drawable = new GradientDrawable();
        drawable.setShape(GradientDrawable.RECTANGLE);
        drawable.setColor(fillColor);
        drawable.setCornerRadius(dp(context, radiusDp));
        drawable.setStroke(strokeWidthPx, strokeColor);
        return drawable;
    }

    /** Материальная рябь, ограниченная формой элемента. */
    static RippleDrawable ripple(int rippleColor, Drawable content) {
        return new RippleDrawable(ColorStateList.valueOf(rippleColor), content, null);
    }

    private static GridLayout.LayoutParams tileParams(Context context) {
        final GridLayout.LayoutParams params = new GridLayout.LayoutParams();
        params.width = dp(context, 120);
        params.height = dp(context, 120);
        final int gap = dp(context, 8);
        params.setMargins(gap, gap, gap, gap);
        return params;
    }

    /** Плитка «плюс»: белый фон, рамка 1 dp, синий векторный «плюс», ripple. */
    static FrameLayout createPlusTile(Context context, View.OnClickListener listener) {
        final FrameLayout tile = new FrameLayout(context);
        tile.setLayoutParams(tileParams(context));

        final GradientDrawable box = roundedRect(context,
                color(context, R.color.surface),
                color(context, R.color.outline),
                16f,
                dp(context, 1));
        tile.setBackground(ripple(Color.argb(41, 26, 115, 232), box));
        tile.setClickable(true);
        tile.setFocusable(true);
        tile.setContentDescription(context.getString(R.string.action_create_configuration));

        final ImageView icon = new ImageView(context);
        icon.setImageResource(R.drawable.ic_plus);
        tile.addView(icon, new FrameLayout.LayoutParams(
                dp(context, 36), dp(context, 36), Gravity.CENTER));
        tile.setOnClickListener(listener);
        return tile;
    }

    /** Плитка созданной конфигурации: нажатие запускает настоящий эмулятор. */
    static FrameLayout createConfigurationTile(Context context, String osName,
                                               String fileName,
                                               View.OnClickListener listener) {
        final FrameLayout tile = new FrameLayout(context);
        tile.setLayoutParams(tileParams(context));
        tile.setBackground(ripple(Color.argb(41, 26, 115, 232), roundedRect(context,
                color(context, R.color.secondary),
                color(context, R.color.outline),
                16f,
                dp(context, 1))));
        tile.setClickable(true);
        tile.setFocusable(true);
        tile.setContentDescription(context.getString(R.string.emulator_tile_hint, osName));
        tile.setOnClickListener(listener);

        final LinearLayout column = new LinearLayout(context);
        column.setOrientation(LinearLayout.VERTICAL);
        final int padding = dp(context, 12);
        column.setPadding(padding, padding, padding, padding);
        tile.addView(column, new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.MATCH_PARENT));

        final ImageView icon = new ImageView(context);
        icon.setImageResource(R.drawable.ic_iso_document);
        column.addView(icon, new LinearLayout.LayoutParams(dp(context, 24), dp(context, 24)));

        // Распорка прижимает подписи к нижнему краю плитки.
        column.addView(new Space(context), new LinearLayout.LayoutParams(0, 0, 1f));

        final TextView title = new TextView(context);
        title.setText(osName);
        title.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14f);
        title.setTypeface(Typeface.create("sans-serif-medium", Typeface.NORMAL));
        title.setTextColor(color(context, R.color.text_primary));
        title.setMaxLines(2);
        title.setEllipsize(TextUtils.TruncateAt.END);
        column.addView(title, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT));

        final TextView subtitle = new TextView(context);
        subtitle.setText(fileName);
        subtitle.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f);
        subtitle.setTextColor(color(context, R.color.text_secondary));
        subtitle.setMaxLines(1);
        subtitle.setEllipsize(TextUtils.TruncateAt.END);
        final LinearLayout.LayoutParams subtitleParams = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT);
        subtitleParams.topMargin = dp(context, 4);
        column.addView(subtitle, subtitleParams);
        return tile;
    }
}
