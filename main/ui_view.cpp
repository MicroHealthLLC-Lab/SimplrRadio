/*
 * UI View Implementation (ui_view.hpp). Rows, marks and the notice card follow SimplrSSH's
 * ui_view.cpp (github.com/MicroHealthLLC/SimplrSSH, MIT).
 */

#include "ui_view.hpp"
#include "board.h"
#include "keys.h"
#include "spectrum.h"
#include "mbedtls/platform_util.h"
#include <cmath>
#include <cstdint>
#include <cstring>

static lv_style_t s_row;          // A menu row; highlighted (CHECKED) and touched (PRESSED)
static lv_style_t s_row_selected;
static lv_style_t s_row_pressed;
static lv_style_t s_mark;         // Checkbox / radio / on-off marks at a row's end
static lv_style_t s_button;       // Now Playing controls
static lv_style_t s_button_pressed;
static bool s_styles_ready = false;

static const uint32_t SELECTED_TEXT = 0xFFFFFF;
static const uint32_t SELECTED_DIM = 0xD0DEF5;

// A row's mark label points its user data here (one entry per ui::Mark value)
static const uint8_t MARK_TAGS[] = {0, 1, 2, 3, 4, 5, 6, 7};

// Spectrum shown (LVGL task only)
static float s_bands[SPECTRUM_BANDS];
static float s_peaks[SPECTRUM_BANDS];

static void init_styles()
{
    if (s_styles_ready) {
        return;
    }
    s_styles_ready = true;
    const board_ui_t* ui = board_ui();

    lv_style_init(&s_row);
    lv_style_set_width(&s_row, lv_pct(100));
    lv_style_set_height(&s_row, LV_SIZE_CONTENT);
    lv_style_set_min_height(&s_row, ui->row_h);
    lv_style_set_pad_hor(&s_row, ui->gap * 2);
    lv_style_set_pad_ver(&s_row, ui->gap);
    lv_style_set_pad_column(&s_row, ui->gap * 2);
    lv_style_set_pad_row(&s_row, 0);
    lv_style_set_border_side(&s_row, LV_BORDER_SIDE_BOTTOM);
    lv_style_set_border_width(&s_row, 1);
    lv_style_set_border_color(&s_row, lv_color_hex(theme::DIVIDER));
    lv_style_set_layout(&s_row, LV_LAYOUT_FLEX);
    lv_style_set_flex_flow(&s_row, LV_FLEX_FLOW_ROW_WRAP);
    lv_style_set_flex_cross_place(&s_row, LV_FLEX_ALIGN_CENTER);
    lv_style_set_flex_track_place(&s_row, LV_FLEX_ALIGN_CENTER);

    lv_style_init(&s_row_selected);
    lv_style_set_bg_color(&s_row_selected, lv_color_hex(theme::ACCENT));
    lv_style_set_bg_opa(&s_row_selected, LV_OPA_COVER);
    lv_style_set_radius(&s_row_selected, ui->gap * 3 / 2);
    lv_style_set_border_color(&s_row_selected, lv_color_hex(theme::ACCENT));   // Same height as unselected

    lv_style_init(&s_row_pressed);
    lv_style_set_bg_color(&s_row_pressed, lv_color_hex(theme::DIVIDER));
    lv_style_set_bg_opa(&s_row_pressed, LV_OPA_COVER);

    lv_style_init(&s_mark);
    lv_style_set_border_width(&s_mark, 2);
    lv_style_set_text_align(&s_mark, LV_TEXT_ALIGN_CENTER);
    lv_style_set_text_font(&s_mark, ui->small);
    lv_style_set_bg_opa(&s_mark, LV_OPA_COVER);

    lv_style_init(&s_button);
    lv_style_set_bg_color(&s_button, lv_color_hex(theme::SURFACE));
    lv_style_set_bg_opa(&s_button, LV_OPA_COVER);
    lv_style_set_radius(&s_button, ui->gap * 2);
    lv_style_set_border_width(&s_button, 1);
    lv_style_set_border_color(&s_button, lv_color_hex(theme::DIVIDER));
    lv_style_set_shadow_width(&s_button, 0);
    lv_style_set_text_color(&s_button, lv_color_hex(theme::TEXT));
    lv_style_set_text_font(&s_button, ui->title);

    lv_style_init(&s_button_pressed);
    lv_style_set_bg_color(&s_button_pressed, lv_color_hex(theme::ACCENT));
}

static lv_obj_t* plain_obj(lv_obj_t* parent)
{
    lv_obj_t* obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_scrollable(obj, false);
    return obj;
}

static lv_obj_t* text_label(lv_obj_t* parent, const lv_font_t* font, uint32_t color, const char* text)
{
    lv_obj_t* label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_label_set_text(label, text);
    return label;
}

static lv_obj_t* wrapped_label(lv_obj_t* parent, const lv_font_t* font, uint32_t color, const char* text)
{
    lv_obj_t* label = text_label(parent, font, color, text);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(label, lv_pct(100));
    lv_obj_set_style_pad_hor(label, board_ui()->gap, 0);
    return label;
}

static int32_t text_width(const char* text, const lv_font_t* font)
{
    lv_point_t size;
    lv_text_get_size(&size, text, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return size.x;
}

static void set_hidden(lv_obj_t* obj, bool hidden)
{
    if (obj && hidden != lv_obj_is_hidden(obj)) {   // Unchanged: no needless redraw
        lv_obj_set_hidden(obj, hidden);
    }
}

static void set_text(lv_obj_t* label, const char* text)
{
    if (label && strcmp(lv_label_get_text(label), text) != 0) {
        lv_label_set_text(label, text);
    }
}

// ---------------------------------------------------------------------------
// Title bar
// ---------------------------------------------------------------------------

void UiView::create(lv_obj_t* scr)
{
    init_styles();
    const board_ui_t* ui = board_ui();
    screen = scr;
    lv_obj_set_style_bg_color(screen, lv_color_hex(theme::BG), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(screen, false);

    bar = plain_obj(screen);
    lv_obj_set_size(bar, lv_pct(100), ui->bar_h);
    lv_obj_align(bar, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(theme::SURFACE), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(bar, ui->gap * 2, 0);
    lv_obj_set_style_pad_column(bar, ui->gap * 3, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    back_label = text_label(bar, ui->title, theme::TEXT, LV_SYMBOL_LEFT);
    lv_obj_set_clickable(back_label, true);
    lv_obj_set_ext_click_area(back_label, ui->bar_h / 2);
    lv_obj_add_event_cb(back_label, back_clicked_cb, LV_EVENT_CLICKED, this);
    lv_obj_set_hidden(back_label, true);

    title_label = text_label(bar, ui->title, theme::TEXT, "");
    lv_label_set_long_mode(title_label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_flex_grow(title_label, 1);

    volume_label = text_label(bar, ui->text, theme::DIM, "");
    wifi_label = text_label(bar, ui->text, theme::DIM, LV_SYMBOL_WIFI);

    page = plain_obj(screen);
    lv_obj_set_scrollable(page, true);
    lv_obj_set_hidden(page, true);
    lv_obj_set_scroll_dir(page, LV_DIR_VER);
    lv_obj_set_scroll_elastic(page, false);
    lv_obj_set_scrollbar_mode(page, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_style_bg_color(page, lv_color_hex(theme::DIM), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(page, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(page, lv_color_hex(theme::BG), 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(page, ui->gap, 0);
    lv_obj_set_style_pad_row(page, ui->gap, 0);
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);

    // The on-screen keyboard: made once, docked at the bottom while a prompt wants it
    keyboard = lv_keyboard_create(screen);
    lv_obj_set_size(keyboard, lv_pct(100), ui->keyboard_h);
    lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_text_font(keyboard, ui->text, 0);
    lv_obj_set_style_bg_color(keyboard, lv_color_hex(theme::SURFACE), 0);
    // OK reaches the field as READY too (see show_page), so only the close key is handled here
    lv_obj_add_event_cb(keyboard, keyboard_cb, LV_EVENT_CANCEL, this);
    lv_obj_set_hidden(keyboard, true);

    create_now_playing();
}

void UiView::set_title(const std::string& title)
{
    set_text(title_label, title.c_str());
}

void UiView::set_back(bool shown)
{
    set_hidden(back_label, !shown);
}

void UiView::set_status(bool wifi, int rssi, int volume, bool muted)
{
    if (!bar) {
        return;
    }
    uint32_t color = !wifi ? theme::DANGER : rssi >= -67 ? theme::TEXT : rssi >= -75 ? theme::WARN : theme::DANGER;
    lv_obj_set_style_text_color(wifi_label, lv_color_hex(color), 0);
    char text[24];
    if (muted) {
        snprintf(text, sizeof(text), LV_SYMBOL_MUTE " Muted");
    } else {
        snprintf(text, sizeof(text), "%s %d", volume == 0 ? LV_SYMBOL_MUTE : volume < 50 ? LV_SYMBOL_VOLUME_MID
                                                                                        : LV_SYMBOL_VOLUME_MAX,
                 volume);
    }
    set_text(volume_label, text);
}

// ---------------------------------------------------------------------------
// Pages
// ---------------------------------------------------------------------------

// The page fills the space between the title bar and the screen's bottom (or the keyboard)
void UiView::layout_page()
{
    const board_ui_t* ui = board_ui();
    int32_t bottom = keyboard_shown() ? ui->keyboard_h : 0;
    lv_obj_set_size(page, lv_obj_get_width(screen), lv_obj_get_height(screen) - ui->bar_h - bottom);
    lv_obj_align(page, LV_ALIGN_TOP_LEFT, 0, ui->bar_h);
    if (field) {
        lv_obj_scroll_to_view(field, LV_ANIM_OFF);
    }
}

// The mark at a row's end: a checkbox, a radio button or an on/off pill, as one styled label
static void paint_mark(lv_obj_t* mark_label, ui::Mark mark, bool selected)
{
    bool on = mark == ui::Mark::Checked || mark == ui::Mark::Chosen || mark == ui::Mark::On;
    uint32_t fill = selected ? SELECTED_TEXT : theme::ACCENT;
    uint32_t edge = on ? fill : (selected ? SELECTED_DIM : theme::DIM);
    lv_obj_set_style_border_color(mark_label, lv_color_hex(edge), 0);
    if (mark == ui::Mark::On || mark == ui::Mark::Off) {
        lv_obj_set_style_bg_color(mark_label, lv_color_hex(on ? fill : theme::DIVIDER), 0);
        lv_obj_set_style_text_color(mark_label, lv_color_hex(on ? (selected ? theme::ACCENT : SELECTED_TEXT) : theme::DIM), 0);
    } else {
        lv_obj_set_style_bg_color(mark_label, lv_color_hex(on ? fill : theme::BG), 0);
        lv_obj_set_style_bg_opa(mark_label, on || !selected ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(mark_label, lv_color_hex(selected ? theme::ACCENT : SELECTED_TEXT), 0);
    }
}

static lv_obj_t* create_mark(lv_obj_t* row, ui::Mark mark)
{
    const board_ui_t* ui = board_ui();
    lv_obj_t* label = lv_label_create(row);
    lv_obj_add_style(label, &s_mark, 0);
    int32_t line_h = lv_font_get_line_height(ui->small);
    if (mark == ui::Mark::On || mark == ui::Mark::Off) {
        lv_label_set_text(label, mark == ui::Mark::On ? "ON" : "OFF");
        lv_obj_set_style_radius(label, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_pad_hor(label, ui->gap * 2, 0);
        lv_obj_set_style_pad_ver(label, 1, 0);
    } else {
        int32_t side = line_h + 4;
        lv_label_set_text(label, mark == ui::Mark::Checked ? LV_SYMBOL_OK : "");
        lv_obj_set_size(label, side, side);
        lv_obj_set_style_radius(label, mark == ui::Mark::Chosen || mark == ui::Mark::NotChosen ? LV_RADIUS_CIRCLE : 3, 0);
    }
    lv_obj_set_user_data(label, (void*)&MARK_TAGS[(int)mark]);   // Tells paint_row() it's the mark
    return label;
}

void UiView::show_page(const ui::Page& p, const std::string& notice, int selected)
{
    if (!page) {
        return;
    }
    const board_ui_t* ui = board_ui();
    set_hidden(np, true);
    set_title(p.title);
    if (field) {
        prompt_wipe();
    }
    lv_keyboard_set_textarea(keyboard, NULL);   // Before its field is deleted
    set_hidden(keyboard, true);
    lv_obj_clean(page);
    notice_label = NULL;
    field = NULL;
    reveal_button = NULL;
    first_row = NULL;
    row_count = 0;
    selected_row = -1;

    if (!p.info.empty()) {
        wrapped_label(page, ui->text, theme::DIM, p.info.c_str());
    }
    if (p.busy) {
        lv_obj_t* spinner = lv_spinner_create(page);
        int32_t side = ui->row_h;
        lv_obj_set_size(spinner, side, side);
        lv_obj_set_style_arc_width(spinner, ui->gap, LV_PART_MAIN);
        lv_obj_set_style_arc_width(spinner, ui->gap, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(spinner, lv_color_hex(theme::DIVIDER), LV_PART_MAIN);
        lv_obj_set_style_arc_color(spinner, lv_color_hex(theme::ACCENT), LV_PART_INDICATOR);
        lv_obj_set_style_margin_left(spinner, ui->gap, 0);
    }

    // Messages since the last answer: a card with an accent edge
    notice_label = wrapped_label(page, ui->text, theme::TEXT, "");
    lv_obj_set_style_bg_color(notice_label, lv_color_hex(theme::SURFACE), 0);
    lv_obj_set_style_bg_opa(notice_label, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(notice_label, ui->gap, 0);
    lv_obj_set_style_pad_all(notice_label, ui->gap, 0);
    lv_obj_set_style_pad_left(notice_label, ui->gap * 2, 0);
    lv_obj_set_style_border_side(notice_label, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_width(notice_label, 3, 0);
    lv_obj_set_style_border_color(notice_label, lv_color_hex(theme::ACCENT), 0);
    set_notice(notice);

    if (!p.question.empty()) {
        lv_obj_t* question = wrapped_label(page, ui->title, theme::TEXT, p.question.c_str());
        lv_obj_set_style_pad_top(question, ui->gap, 0);

        // The field, and for passwords a Show button beside it
        lv_obj_t* line = plain_obj(page);
        lv_obj_set_size(line, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(line, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(line, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(line, ui->gap, 0);
        field = lv_textarea_create(line);
        lv_textarea_set_one_line(field, true);
        lv_obj_set_flex_grow(field, 1);
        lv_obj_set_style_text_font(field, ui->title, 0);
        lv_obj_set_style_bg_color(field, lv_color_hex(theme::SURFACE), 0);
        lv_obj_set_style_text_color(field, lv_color_hex(theme::TEXT), 0);
        lv_obj_set_style_border_color(field, lv_color_hex(theme::ACCENT), 0);
        lv_obj_set_style_border_width(field, 2, 0);
        lv_obj_set_style_radius(field, ui->gap, 0);
        if (p.max_len) {
            lv_textarea_set_max_length(field, p.max_len);
        }
        if (p.secret) {
            lv_textarea_set_password_mode(field, true);
            lv_textarea_set_password_show_time(field, 800);   // The last character, briefly
            reveal_button = lv_button_create(line);
            lv_obj_add_style(reveal_button, &s_button, 0);
            lv_obj_add_style(reveal_button, &s_button_pressed, LV_STATE_PRESSED);
            lv_obj_set_height(reveal_button, LV_SIZE_CONTENT);
            lv_obj_add_event_cb(reveal_button, reveal_cb, LV_EVENT_CLICKED, this);
            lv_obj_t* l = text_label(reveal_button, ui->text, theme::TEXT, LV_SYMBOL_EYE_OPEN " Show");
            lv_obj_center(l);
        }
        lv_textarea_set_text(field, p.value.c_str());
        lv_obj_add_state(field, LV_STATE_FOCUSED);   // Cursor shown
        lv_obj_add_event_cb(field, field_clicked_cb, LV_EVENT_CLICKED, this);
        // The on-screen keyboard's OK and Enter both send READY to the field
        lv_obj_add_event_cb(field, keyboard_cb, LV_EVENT_READY, this);
        lv_keyboard_set_textarea(keyboard, field);
        lv_keyboard_set_mode(keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
    }
    if (!p.hint.empty()) {
        wrapped_label(page, ui->text, theme::DIM, p.hint.c_str());
    }

    // Rows: key, icon + label, detail (under the label when it doesn't fit beside it), mark
    layout_page();
    lv_obj_update_layout(page);
    int32_t row_w = lv_obj_get_content_width(page) - ui->gap * 4;
    int32_t key_w = text_width("00", ui->text);
    for (size_t i = 0; i < p.rows.size(); i++) {
        const ui::Row& r = p.rows[i];
        lv_obj_t* row = plain_obj(page);
        lv_obj_add_style(row, &s_row, 0);
        lv_obj_add_style(row, &s_row_selected, LV_STATE_CHECKED);
        lv_obj_add_style(row, &s_row_pressed, LV_STATE_PRESSED);
        lv_obj_set_clickable(row, true);
        lv_obj_set_user_data(row, (void*)(intptr_t)i);
        lv_obj_add_event_cb(row, row_clicked_cb, LV_EVENT_CLICKED, this);
        if (i == 0) {
            first_row = row;
        }

        // Numbers are shortcuts worth showing; letter answers (y/n) are just noise
        bool number = !r.key.empty() && r.key.find_first_not_of("0123456789") == std::string::npos;
        lv_obj_t* key = text_label(row, ui->text, theme::DIM, number ? r.key.c_str() : "");
        lv_obj_set_width(key, key_w);

        std::string text = r.icon ? std::string(r.icon) + "  " + r.label : r.label;
        uint32_t color = r.tone == ui::Tone::Danger ? theme::DANGER : r.tone == ui::Tone::Back ? theme::DIM : theme::TEXT;
        lv_obj_t* label = text_label(row, ui->list, color, text.c_str());
        lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_flex_grow(label, 1);
        lv_obj_set_user_data(label, (void*)(uintptr_t)color);

        std::string detail = r.detail;
        if (r.mark == ui::Mark::More) {
            detail += detail.empty() ? LV_SYMBOL_RIGHT : "  " LV_SYMBOL_RIGHT;
        }
        // The mark stays on the label's line; the detail goes beside it if it fits, else under it
        bool marked = r.mark != ui::Mark::None && r.mark != ui::Mark::More;
        if (marked) {
            create_mark(row, r.mark);
        }
        if (!detail.empty()) {
            int32_t mark_w = marked ? lv_font_get_line_height(ui->small) + ui->gap * 6 : 0;
            int32_t beside = row_w - key_w - text_width(text.c_str(), ui->list) - mark_w - ui->gap * 6;
            lv_obj_t* d = text_label(row, ui->text, theme::DIM, detail.c_str());
            if (text_width(detail.c_str(), ui->text) > beside) {
                // Its own line under the label, lined up with it
                lv_label_set_long_mode(d, LV_LABEL_LONG_MODE_WRAP);
                lv_obj_set_flex_in_new_track(d, true);
                lv_obj_set_width(d, lv_pct(100));
                lv_obj_set_style_pad_left(d, key_w + ui->gap * 2, 0);
            } else if (marked) {
                lv_obj_move_to_index(d, 2);   // Key, label, detail, then the mark at the row's end
            }
        }
        row_count++;
        paint_row(i, false);
    }

    set_hidden(page, false);
    lv_obj_move_foreground(keyboard);
    // The first highlight doesn't scroll: the page opens at its top, so its text is read first
    lv_obj_scroll_to_y(page, 0, LV_ANIM_OFF);
    selected_row = row_obj(selected) ? selected : -1;
    paint_row(selected_row, true);
    if (field) {
        lv_obj_scroll_to_view(field, LV_ANIM_OFF);
    }
}

void UiView::set_notice(const std::string& notice)
{
    if (!notice_label) {
        return;
    }
    set_hidden(notice_label, notice.empty());
    if (!notice.empty()) {
        set_text(notice_label, notice.c_str());
    }
}

lv_obj_t* UiView::row_obj(int row) const
{
    if (!first_row || row < 0 || row >= row_count) {
        return NULL;
    }
    return lv_obj_get_child(page, lv_obj_get_index(first_row) + row);
}

// Colours of a row's labels and mark: white on the accent bar while highlighted
void UiView::paint_row(int row, bool selected)
{
    lv_obj_t* obj = row_obj(row);
    if (!obj) {
        return;
    }
    if (selected) {
        lv_obj_add_state(obj, LV_STATE_CHECKED);
    } else {
        lv_obj_remove_state(obj, LV_STATE_CHECKED);
    }
    uint32_t count = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < count; i++) {
        lv_obj_t* child = lv_obj_get_child(obj, i);
        const uint8_t* tag = (const uint8_t*)lv_obj_get_user_data(child);
        if (tag >= MARK_TAGS && tag < MARK_TAGS + sizeof(MARK_TAGS)) {
            paint_mark(child, (ui::Mark)*tag, selected);
        } else if (i == 1) {
            uint32_t color = (uint32_t)(uintptr_t)lv_obj_get_user_data(child);
            lv_obj_set_style_text_color(child, lv_color_hex(selected ? SELECTED_TEXT : color), 0);
        } else {
            lv_obj_set_style_text_color(child, lv_color_hex(selected ? SELECTED_DIM : theme::DIM), 0);
        }
    }
}

void UiView::select(int row)
{
    if (row == selected_row && row_obj(row)) {
        return;
    }
    paint_row(selected_row, false);
    selected_row = row_obj(row) ? row : -1;
    lv_obj_t* obj = row_obj(selected_row);
    if (obj) {
        paint_row(selected_row, true);
        lv_obj_scroll_to_view(obj, LV_ANIM_OFF);
    }
}

bool UiView::scroll(int direction)
{
    if (!page_shown() || direction == 0) {
        return false;
    }
    int32_t before = lv_obj_get_scroll_y(page);
    int32_t step = lv_obj_get_height(page) * 3 / 4;
    lv_obj_scroll_by_bounded(page, 0, direction > 0 ? step : -step, LV_ANIM_OFF);
    return lv_obj_get_scroll_y(page) != before;
}

// ---------------------------------------------------------------------------
// Prompt field and keyboard
// ---------------------------------------------------------------------------

void UiView::prompt_key(uint32_t key)
{
    if (!field) {
        return;
    }
    switch (key) {
        case KEY_BACKSPACE: lv_textarea_delete_char(field); break;
        case KEY_DELETE: lv_textarea_delete_char_forward(field); break;
        case KEY_LEFT: lv_textarea_cursor_left(field); break;
        case KEY_RIGHT: lv_textarea_cursor_right(field); break;
        case KEY_HOME: lv_textarea_set_cursor_pos(field, 0); break;
        case KEY_END: lv_textarea_set_cursor_pos(field, LV_TEXTAREA_CURSOR_LAST); break;
        default:
            if (key_is_char(key)) {
                lv_textarea_add_char(field, key);
            }
            break;
    }
}

std::string UiView::prompt_text() const
{
    return field ? std::string(lv_textarea_get_text(field)) : std::string();
}

void UiView::prompt_wipe()
{
    if (!field) {
        return;
    }
    // The text (for passwords, LVGL's plain copy) is overwritten before it is freed
    char* text = (char*)lv_textarea_get_text(field);
    if (text) {
        mbedtls_platform_zeroize(text, strlen(text));
    }
    lv_textarea_set_text(field, "");
}

void UiView::toggle_reveal()
{
    if (!field || !reveal_button) {
        return;
    }
    bool hidden = lv_textarea_get_password_mode(field);
    lv_textarea_set_password_mode(field, !hidden);
    lv_obj_t* label = lv_obj_get_child(reveal_button, 0);
    lv_label_set_text(label, hidden ? LV_SYMBOL_EYE_CLOSE " Hide" : LV_SYMBOL_EYE_OPEN " Show");
}

void UiView::show_keyboard(bool shown)
{
    shown = shown && field;
    if (shown == keyboard_shown()) {
        return;
    }
    set_hidden(keyboard, !shown);
    if (shown) {
        lv_obj_move_foreground(keyboard);
    }
    if (page_shown()) {
        layout_page();
    }
}

bool UiView::keyboard_shown() const
{
    return keyboard && !lv_obj_is_hidden(keyboard);
}

// ---------------------------------------------------------------------------
// Now Playing
// ---------------------------------------------------------------------------

static const char* const BUTTON_ICONS[NP_BUTTONS] = {
    LV_SYMBOL_SETTINGS, LV_SYMBOL_PREV, LV_SYMBOL_VOLUME_MAX, LV_SYMBOL_NEXT,
    LV_SYMBOL_MINUS, LV_SYMBOL_PLUS, LV_SYMBOL_LIST,
};
static const char* const BUTTON_WORDS[NP_BUTTONS] = {
    "Menu", "", "", "", "", "", "Stations",
};

void UiView::create_now_playing()
{
    const board_ui_t* ui = board_ui();
    int32_t w = lv_obj_get_width(screen);
    bool wide = w >= 1000;

    np = plain_obj(screen);
    lv_obj_set_size(np, w, lv_obj_get_height(screen) - ui->bar_h);
    lv_obj_align(np, LV_ALIGN_TOP_LEFT, 0, ui->bar_h);
    lv_obj_set_style_pad_all(np, ui->gap * 2, 0);
    lv_obj_set_style_pad_row(np, ui->gap, 0);
    lv_obj_set_flex_flow(np, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_event_cb(np, gesture_cb, LV_EVENT_GESTURE, this);
    lv_obj_set_hidden(np, true);

    np_info = text_label(np, ui->text, theme::DIM, "");
    lv_label_set_long_mode(np_info, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(np_info, lv_pct(100));

    np_station = text_label(np, ui->big, theme::TEXT, "");
    lv_label_set_long_mode(np_station, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(np_station, lv_pct(100));

    np_song = text_label(np, ui->title, theme::SONG, "");
    lv_label_set_long_mode(np_song, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    lv_obj_set_width(np_song, lv_pct(100));

    np_status = text_label(np, ui->text, theme::DIM, "");
    lv_obj_set_width(np_status, lv_pct(100));

    np_spectrum = plain_obj(np);
    lv_obj_set_width(np_spectrum, lv_pct(100));
    lv_obj_set_flex_grow(np_spectrum, 1);
    lv_obj_add_event_cb(np_spectrum, spectrum_draw_cb, LV_EVENT_DRAW_MAIN, this);

    lv_obj_t* buttons = plain_obj(np);
    lv_obj_set_size(buttons, lv_pct(100), ui->button_h);
    lv_obj_set_flex_flow(buttons, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(buttons, ui->gap, 0);
    for (int i = 0; i < NP_BUTTONS; i++) {
        lv_obj_t* b = lv_button_create(buttons);
        lv_obj_add_style(b, &s_button, 0);
        lv_obj_add_style(b, &s_button_pressed, LV_STATE_PRESSED);
        lv_obj_set_height(b, lv_pct(100));
        lv_obj_set_flex_grow(b, 1);
        lv_obj_set_user_data(b, (void*)(intptr_t)i);
        lv_obj_add_event_cb(b, button_cb, LV_EVENT_CLICKED, this);
        std::string text = BUTTON_ICONS[i];
        if (wide && BUTTON_WORDS[i][0]) {
            text += "  ";
            text += BUTTON_WORDS[i];
        }
        lv_obj_t* l = lv_label_create(b);
        lv_label_set_text(l, text.c_str());
        lv_obj_center(l);
        if (i == NP_MUTE) {
            np_mute_label = l;
        }
    }

    // ~30 frames a second; only the spectrum's area is redrawn, and only while it moves
    spectrum_timer = lv_timer_create(spectrum_timer_cb, 33, this);
}

void UiView::show_now_playing()
{
    if (field) {
        prompt_wipe();
    }
    show_keyboard(false);
    lv_keyboard_set_textarea(keyboard, NULL);   // Before its field is deleted
    set_hidden(page, true);
    if (page) {
        lv_obj_clean(page);   // Frees its objects until the next page
    }
    notice_label = NULL;
    field = NULL;
    reveal_button = NULL;
    first_row = NULL;
    row_count = 0;
    selected_row = -1;
    set_back(false);
    set_hidden(np, false);
}

void UiView::update_now_playing(const NowPlaying& s)
{
    set_text(np_info, s.info.c_str());
    set_text(np_station, s.station.empty() ? "No station yet" : s.station.c_str());
    set_text(np_song, s.song.c_str());
    set_text(np_status, s.status.c_str());
    lv_obj_set_style_text_color(np_status, lv_color_hex(s.status_warn ? theme::WARN : theme::DIM), 0);
    set_text(np_mute_label, s.muted ? LV_SYMBOL_MUTE : LV_SYMBOL_VOLUME_MAX);
}

void UiView::spectrum_timer_cb(lv_timer_t* t)
{
    UiView* view = (UiView*)lv_timer_get_user_data(t);
    if (!view->now_playing_shown()) {
        return;
    }
    float bands[SPECTRUM_BANDS], peaks[SPECTRUM_BANDS];
    spectrum_snapshot(bands, peaks);
    bool moved = false;
    for (int b = 0; b < SPECTRUM_BANDS; b++) {
        // Redraw only when a bar would visibly change
        if (fabsf(bands[b] - s_bands[b]) > 0.004f || fabsf(peaks[b] - s_peaks[b]) > 0.004f) {
            moved = true;
        }
    }
    if (moved) {
        memcpy(s_bands, bands, sizeof(s_bands));
        memcpy(s_peaks, peaks, sizeof(s_peaks));
        lv_obj_invalidate(view->np_spectrum);
    }
}

void UiView::spectrum_draw_cb(lv_event_t* e)
{
    lv_obj_t* obj = (lv_obj_t*)lv_event_get_current_target(e);
    lv_layer_t* layer = lv_event_get_layer(e);
    lv_area_t area;
    lv_obj_get_coords(obj, &area);
    int32_t w = lv_area_get_width(&area);
    int32_t h = lv_area_get_height(&area);
    if (w < SPECTRUM_BANDS || h < 8) {
        return;
    }
    int32_t stride = w / SPECTRUM_BANDS;
    int32_t gap = stride / 6 > 2 ? stride / 6 : 2;
    int32_t margin = (w - stride * SPECTRUM_BANDS) / 2;
    int32_t radius = gap;

    lv_draw_rect_dsc_t bar;
    lv_draw_rect_dsc_init(&bar);
    bar.radius = radius;
    bar.bg_grad.dir = LV_GRAD_DIR_VER;
    bar.bg_grad.stops_count = 2;
    bar.bg_grad.stops[0].frac = 0;
    bar.bg_grad.stops[0].opa = LV_OPA_COVER;
    bar.bg_grad.stops[1].frac = 255;
    bar.bg_grad.stops[1].opa = LV_OPA_COVER;

    lv_draw_rect_dsc_t cap;
    lv_draw_rect_dsc_init(&cap);
    cap.bg_color = lv_color_hex(theme::TEXT);
    cap.bg_opa = LV_OPA_70;
    cap.radius = 1;

    for (int b = 0; b < SPECTRUM_BANDS; b++) {
        float mag = s_bands[b] < 0 ? 0 : s_bands[b] > 1 ? 1 : s_bands[b];
        int32_t bar_h = (int32_t)(mag * (h - 6));
        int32_t x1 = area.x1 + margin + b * stride;
        int32_t x2 = x1 + stride - gap - 1;
        // Blue at the bass to red at the treble; brighter at the top of each bar
        uint16_t hue = (uint16_t)(240 - b * 240 / SPECTRUM_BANDS);
        if (bar_h > 0) {
            bar.bg_grad.stops[0].color = lv_color_hsv_to_rgb(hue, 85, 100);
            bar.bg_grad.stops[1].color = lv_color_hsv_to_rgb(hue, 90, 45);
            bar.bg_color = bar.bg_grad.stops[0].color;
            lv_area_t a = {x1, area.y2 - bar_h + 1, x2, area.y2};
            lv_draw_rect(layer, &bar, &a);
        }
        float peak = s_peaks[b] < 0 ? 0 : s_peaks[b] > 1 ? 1 : s_peaks[b];
        int32_t peak_y = area.y2 - (int32_t)(peak * (h - 6)) - 4;
        if (peak > 0.02f) {
            lv_area_t c = {x1, peak_y, x2, peak_y + 2};
            lv_draw_rect(layer, &cap, &c);
        }
    }
}

// ---------------------------------------------------------------------------
// Events (LVGL task, display lock held): hand over to the app task
// ---------------------------------------------------------------------------

void UiView::row_clicked_cb(lv_event_t* e)
{
    UiView* view = (UiView*)lv_event_get_user_data(e);
    lv_obj_t* row = (lv_obj_t*)lv_event_get_current_target(e);
    if (view && view->on_tap) {
        view->on_tap((int)(intptr_t)lv_obj_get_user_data(row));
    }
}

void UiView::back_clicked_cb(lv_event_t* e)
{
    UiView* view = (UiView*)lv_event_get_user_data(e);
    if (view && view->on_back) {
        view->on_back();
    }
}

void UiView::field_clicked_cb(lv_event_t* e)
{
    UiView* view = (UiView*)lv_event_get_user_data(e);
    view->show_keyboard(true);   // Touching the field brings the keyboard back
}

void UiView::keyboard_cb(lv_event_t* e)
{
    UiView* view = (UiView*)lv_event_get_user_data(e);
    if (lv_event_get_code(e) == LV_EVENT_READY) {
        if (view->on_submit) {
            view->on_submit();
        }
    } else {
        view->show_keyboard(false);   // Its close key: just put it away
    }
}

void UiView::reveal_cb(lv_event_t* e)
{
    UiView* view = (UiView*)lv_event_get_user_data(e);
    view->toggle_reveal();
}

void UiView::button_cb(lv_event_t* e)
{
    UiView* view = (UiView*)lv_event_get_user_data(e);
    lv_obj_t* b = (lv_obj_t*)lv_event_get_current_target(e);
    if (view->on_button) {
        view->on_button((int)(intptr_t)lv_obj_get_user_data(b));
    }
}

void UiView::gesture_cb(lv_event_t* e)
{
    UiView* view = (UiView*)lv_event_get_user_data(e);
    lv_indev_t* indev = lv_indev_active();
    if (indev && view->on_swipe) {
        view->on_swipe(lv_indev_get_gesture_dir(indev));
        lv_indev_wait_release(indev);
    }
}
