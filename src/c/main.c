#include <pebble.h>

// === Layout for Emery 200x228 ===
// Background fills the screen.
// Foreground PCB ("board") is 178x178 placed at (11, 11).
// Digits (36x51) sit inside the 7-segment display area near the top of the board.
// Dot (7x7) is the decimal point after the second digit.
// Activity label band is a rounded "framed" rectangle along the bottom.

#define BOARD_X         11
#define BOARD_Y         11
#define BOARD_W         178
#define BOARD_H         178

#define DIGIT_W         36
#define DIGIT_H         51
#define DIGIT_Y         (BOARD_Y + 9)
#define DIGIT_H1_X      (BOARD_X + 14)
#define DIGIT_H2_X      (BOARD_X + 55)
#define DIGIT_M1_X      (BOARD_X + 96)
#define DIGIT_M2_X      (BOARD_X + 137)

#define DOT_W           7
#define DOT_H           7
#define DOT_X           (BOARD_X + 85)
#define DOT_Y           (BOARD_Y + 60)

#define LABEL_X         8
#define LABEL_Y         170
#define LABEL_W         184
#define LABEL_H         50

#define LABEL_CREAM     GColorIcterine
#define LABEL_BLACK     GColorBlack

// Activity cycle states
typedef enum {
  ACT_HIDDEN = 0,
  ACT_DATE,
  ACT_DOW,
  ACT_AMPM,
  ACT_STEPS,
  ACT_CALORIES,
  ACT_DISTANCE,
  ACT_ACTIVE,
  ACT_HEARTRATE,
  ACT_BATTERY,
  ACT_COUNT
} ActivityState;

// Persistent storage keys
#define PERSIST_TIME_COLOR     1
#define PERSIST_ACTIVITY_INDEX 2

// AppMessage keys (must match package.json messageKeys)
// pebble-clay produces these as MESSAGE_KEY_* macros at build time.

static Window *s_window;
static Layer *s_root_layer;

static GBitmap *s_bg_bitmap;
static GBitmap *s_board_bitmap;
static GBitmap *s_digit_bitmaps[10];
static GBitmap *s_dot_bitmap;

static BitmapLayer *s_bg_layer;
static BitmapLayer *s_board_layer;
static BitmapLayer *s_h1_layer;
static BitmapLayer *s_h2_layer;
static BitmapLayer *s_m1_layer;
static BitmapLayer *s_m2_layer;
static BitmapLayer *s_dot_layer;

static Layer *s_label_layer;

static GColor s_time_color;
static int s_activity_index = ACT_HIDDEN;
static char s_label_text[40] = "";
static int s_current_h1 = -1, s_current_h2 = -1, s_current_m1 = -1, s_current_m2 = -1;
static time_t s_last_tap_time = 0;
static int s_last_hours_24 = 0;

// === Helpers ===

static void apply_color_to_digit(GBitmap *bmp, GColor color) {
  if (!bmp) return;
  GColor *palette = gbitmap_get_palette(bmp);
  if (!palette) return;
  // Index 0 is transparent, index 1 is the ink. We retint index 1.
  // For 1-bit palette format from a 2-color PNG with transparency, this works.
  palette[1] = color;
}

static void apply_color_to_all_digits(void) {
  for (int i = 0; i < 10; ++i) {
    apply_color_to_digit(s_digit_bitmaps[i], s_time_color);
  }
  apply_color_to_digit(s_dot_bitmap, s_time_color);
  // Trigger redraw on all digit layers
  layer_mark_dirty(bitmap_layer_get_layer(s_h1_layer));
  layer_mark_dirty(bitmap_layer_get_layer(s_h2_layer));
  layer_mark_dirty(bitmap_layer_get_layer(s_m1_layer));
  layer_mark_dirty(bitmap_layer_get_layer(s_m2_layer));
  layer_mark_dirty(bitmap_layer_get_layer(s_dot_layer));
}

static void set_digit(BitmapLayer *layer, int *cache, int digit) {
  if (digit < 0 || digit > 9) return;
  if (*cache == digit) return;
  *cache = digit;
  bitmap_layer_set_bitmap(layer, s_digit_bitmaps[digit]);
}

// === Activity label drawing ===
// Three nested rounded rectangles, mimicking the original SVG: cream / black / cream.

static void label_layer_update(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);

  // Outer cream rounded rect
  graphics_context_set_fill_color(ctx, LABEL_CREAM);
  graphics_fill_rect(ctx, b, 8, GCornersAll);

  // Black middle ring (inset 4px)
  GRect mid = GRect(b.origin.x + 4, b.origin.y + 4, b.size.w - 8, b.size.h - 8);
  graphics_context_set_fill_color(ctx, LABEL_BLACK);
  graphics_fill_rect(ctx, mid, 6, GCornersAll);

  // Inner cream (inset 2px more)
  GRect inner = GRect(mid.origin.x + 2, mid.origin.y + 2, mid.size.w - 4, mid.size.h - 4);
  graphics_context_set_fill_color(ctx, LABEL_CREAM);
  graphics_fill_rect(ctx, inner, 4, GCornersAll);

  // Centered text
  graphics_context_set_text_color(ctx, LABEL_BLACK);
  GFont font = fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD);
  GSize size = graphics_text_layout_get_content_size(
      s_label_text, font, inner,
      GTextOverflowModeWordWrap, GTextAlignmentCenter);
  GRect text_rect = inner;
  // Vertically center within inner
  int y_offset = (inner.size.h - size.h) / 2 - 4;  // -4 nudge for font baseline
  text_rect.origin.y += y_offset;
  text_rect.size.h = size.h + 8;
  graphics_draw_text(ctx, s_label_text, font, text_rect,
                     GTextOverflowModeTrailingEllipsis,
                     GTextAlignmentCenter, NULL);
}

// === Activity computation ===

static const char *DOW_NAMES[] = {
  "SUNDAY", "MONDAY", "TUESDAY", "WEDNESDAY", "THURSDAY", "FRIDAY", "SATURDAY"
};
static const char *MONTH_NAMES[] = {
  "JANUARY", "FEBRUARY", "MARCH", "APRIL", "MAY", "JUNE",
  "JULY", "AUGUST", "SEPTEMBER", "OCTOBER", "NOVEMBER", "DECEMBER"
};

static void format_distance(char *buf, size_t buf_size, int32_t meters) {
  // Pebble doesn't easily expose user units; default to whatever the watch is set to.
  MeasurementSystem ms = health_service_get_measurement_system_for_display(
      HealthMetricWalkedDistanceMeters);
  if (ms == MeasurementSystemImperial) {
    int tenths = (meters * 10) / 1609;  // miles * 10
    snprintf(buf, buf_size, "%d.%d mi", tenths / 10, tenths % 10);
  } else {
    int tenths = meters / 100;  // km * 10
    snprintf(buf, buf_size, "%d.%d km", tenths / 10, tenths % 10);
  }
}

static int32_t safe_health_sum_today(HealthMetric m) {
  time_t start = time_start_of_today();
  time_t end = time(NULL);
  HealthServiceAccessibilityMask mask = health_service_metric_accessible(m, start, end);
  if (mask & HealthServiceAccessibilityMaskAvailable) {
    return health_service_sum_today(m);
  }
  return 0;
}

static void compute_label_text(void) {
  time_t now = time(NULL);
  struct tm *t = localtime(&now);

  switch (s_activity_index) {
    case ACT_HIDDEN:
      s_label_text[0] = '\0';
      break;
    case ACT_DATE:
      snprintf(s_label_text, sizeof(s_label_text), "%s %d",
               MONTH_NAMES[t->tm_mon], t->tm_mday);
      break;
    case ACT_DOW:
      snprintf(s_label_text, sizeof(s_label_text), "%s", DOW_NAMES[t->tm_wday]);
      break;
    case ACT_AMPM:
      if (clock_is_24h_style()) {
        snprintf(s_label_text, sizeof(s_label_text), "24H");
      } else {
        snprintf(s_label_text, sizeof(s_label_text),
                 s_last_hours_24 >= 12 ? "PM" : "AM");
      }
      break;
    case ACT_STEPS: {
      int32_t v = safe_health_sum_today(HealthMetricStepCount);
      snprintf(s_label_text, sizeof(s_label_text), "%ld steps", (long)v);
      break;
    }
    case ACT_CALORIES: {
      int32_t a = safe_health_sum_today(HealthMetricActiveKCalories);
      int32_t r = safe_health_sum_today(HealthMetricRestingKCalories);
      snprintf(s_label_text, sizeof(s_label_text), "%ld cals", (long)(a + r));
      break;
    }
    case ACT_DISTANCE: {
      int32_t d = safe_health_sum_today(HealthMetricWalkedDistanceMeters);
      format_distance(s_label_text, sizeof(s_label_text), d);
      break;
    }
    case ACT_ACTIVE: {
      int32_t secs = safe_health_sum_today(HealthMetricActiveSeconds);
      int hours = secs / 3600;
      int mins = (secs % 3600) / 60;
      snprintf(s_label_text, sizeof(s_label_text),
               "Active %02d:%02d", hours, mins);
      break;
    }
    case ACT_HEARTRATE: {
#if defined(PBL_HEALTH)
      HealthValue hr = health_service_peek_current_value(HealthMetricHeartRateBPM);
      if (hr > 0) {
        snprintf(s_label_text, sizeof(s_label_text), "%ld bpm", (long)hr);
      } else {
        snprintf(s_label_text, sizeof(s_label_text), "... bpm");
      }
#else
      snprintf(s_label_text, sizeof(s_label_text), "... bpm");
#endif
      break;
    }
    case ACT_BATTERY: {
      BatteryChargeState bs = battery_state_service_peek();
      snprintf(s_label_text, sizeof(s_label_text), "Power %d%%", bs.charge_percent);
      break;
    }
    default:
      s_label_text[0] = '\0';
      break;
  }

  layer_set_hidden(s_label_layer, s_activity_index == ACT_HIDDEN);
  layer_mark_dirty(s_label_layer);
}

// === Time update ===

static void update_time(struct tm *t) {
  int hour = t->tm_hour;
  s_last_hours_24 = hour;
  if (!clock_is_24h_style()) {
    hour = hour % 12;
    if (hour == 0) hour = 12;
  }

  set_digit(s_h1_layer, &s_current_h1, hour / 10);
  set_digit(s_h2_layer, &s_current_h2, hour % 10);
  set_digit(s_m1_layer, &s_current_m1, t->tm_min / 10);
  set_digit(s_m2_layer, &s_current_m2, t->tm_min % 10);
}

static void tick_handler(struct tm *tick_time, TimeUnits units_changed) {
  update_time(tick_time);
  compute_label_text();
}

// === Tap-to-cycle ===

static void tap_handler(AccelAxisType axis, int32_t direction) {
  // Debounce: ignore taps within 1 second of the last one
  time_t now = time(NULL);
  if (now - s_last_tap_time < 1) return;
  s_last_tap_time = now;

  s_activity_index = (s_activity_index + 1) % ACT_COUNT;
  persist_write_int(PERSIST_ACTIVITY_INDEX, s_activity_index);
  compute_label_text();
}

// === AppMessage (Clay) ===

static void inbox_received_handler(DictionaryIterator *iter, void *context) {
  Tuple *color_t = dict_find(iter, MESSAGE_KEY_TIME_COLOR);
  if (color_t) {
    int color = color_t->value->int32;
    s_time_color = GColorFromHEX(color);
    persist_write_int(PERSIST_TIME_COLOR, color);
    apply_color_to_all_digits();
  }
}

// === Lifecycle ===

static void load_resources(void) {
  s_bg_bitmap = gbitmap_create_with_resource(RESOURCE_ID_BACKGROUND);
  s_board_bitmap = gbitmap_create_with_resource(RESOURCE_ID_BOARD);
  s_digit_bitmaps[0] = gbitmap_create_with_resource(RESOURCE_ID_DIGIT_0);
  s_digit_bitmaps[1] = gbitmap_create_with_resource(RESOURCE_ID_DIGIT_1);
  s_digit_bitmaps[2] = gbitmap_create_with_resource(RESOURCE_ID_DIGIT_2);
  s_digit_bitmaps[3] = gbitmap_create_with_resource(RESOURCE_ID_DIGIT_3);
  s_digit_bitmaps[4] = gbitmap_create_with_resource(RESOURCE_ID_DIGIT_4);
  s_digit_bitmaps[5] = gbitmap_create_with_resource(RESOURCE_ID_DIGIT_5);
  s_digit_bitmaps[6] = gbitmap_create_with_resource(RESOURCE_ID_DIGIT_6);
  s_digit_bitmaps[7] = gbitmap_create_with_resource(RESOURCE_ID_DIGIT_7);
  s_digit_bitmaps[8] = gbitmap_create_with_resource(RESOURCE_ID_DIGIT_8);
  s_digit_bitmaps[9] = gbitmap_create_with_resource(RESOURCE_ID_DIGIT_9);
  s_dot_bitmap = gbitmap_create_with_resource(RESOURCE_ID_DIGIT_DOT);
}

static void unload_resources(void) {
  if (s_bg_bitmap) gbitmap_destroy(s_bg_bitmap);
  if (s_board_bitmap) gbitmap_destroy(s_board_bitmap);
  for (int i = 0; i < 10; ++i) {
    if (s_digit_bitmaps[i]) gbitmap_destroy(s_digit_bitmaps[i]);
  }
  if (s_dot_bitmap) gbitmap_destroy(s_dot_bitmap);
}

static void window_load(Window *w) {
  s_root_layer = window_get_root_layer(w);
  GRect bounds = layer_get_bounds(s_root_layer);

  // Background
  s_bg_layer = bitmap_layer_create(bounds);
  bitmap_layer_set_bitmap(s_bg_layer, s_bg_bitmap);
  layer_add_child(s_root_layer, bitmap_layer_get_layer(s_bg_layer));

  // Board (foreground PCB)
  s_board_layer = bitmap_layer_create(GRect(BOARD_X, BOARD_Y, BOARD_W, BOARD_H));
  bitmap_layer_set_bitmap(s_board_layer, s_board_bitmap);
  bitmap_layer_set_compositing_mode(s_board_layer, GCompOpAssign);
  layer_add_child(s_root_layer, bitmap_layer_get_layer(s_board_layer));

  // Digits
  s_h1_layer = bitmap_layer_create(GRect(DIGIT_H1_X, DIGIT_Y, DIGIT_W, DIGIT_H));
  s_h2_layer = bitmap_layer_create(GRect(DIGIT_H2_X, DIGIT_Y, DIGIT_W, DIGIT_H));
  s_m1_layer = bitmap_layer_create(GRect(DIGIT_M1_X, DIGIT_Y, DIGIT_W, DIGIT_H));
  s_m2_layer = bitmap_layer_create(GRect(DIGIT_M2_X, DIGIT_Y, DIGIT_W, DIGIT_H));
  for (BitmapLayer **L = (BitmapLayer*[]){s_h1_layer, s_h2_layer, s_m1_layer, s_m2_layer, NULL}; *L; ++L) {
    bitmap_layer_set_compositing_mode(*L, GCompOpSet);
    layer_add_child(s_root_layer, bitmap_layer_get_layer(*L));
  }

  // Dot
  s_dot_layer = bitmap_layer_create(GRect(DOT_X, DOT_Y, DOT_W, DOT_H));
  bitmap_layer_set_bitmap(s_dot_layer, s_dot_bitmap);
  bitmap_layer_set_compositing_mode(s_dot_layer, GCompOpSet);
  layer_add_child(s_root_layer, bitmap_layer_get_layer(s_dot_layer));

  // Activity label band (drawn with code, on top)
  s_label_layer = layer_create(GRect(LABEL_X, LABEL_Y, LABEL_W, LABEL_H));
  layer_set_update_proc(s_label_layer, label_layer_update);
  layer_add_child(s_root_layer, s_label_layer);

  // Apply initial color and digits
  apply_color_to_all_digits();
  time_t now = time(NULL);
  update_time(localtime(&now));
  compute_label_text();
}

static void window_unload(Window *w) {
  bitmap_layer_destroy(s_bg_layer);
  bitmap_layer_destroy(s_board_layer);
  bitmap_layer_destroy(s_h1_layer);
  bitmap_layer_destroy(s_h2_layer);
  bitmap_layer_destroy(s_m1_layer);
  bitmap_layer_destroy(s_m2_layer);
  bitmap_layer_destroy(s_dot_layer);
  layer_destroy(s_label_layer);
}

static void init(void) {
  // Load persisted settings
  if (persist_exists(PERSIST_TIME_COLOR)) {
    s_time_color = GColorFromHEX(persist_read_int(PERSIST_TIME_COLOR));
  } else {
    s_time_color = GColorYellow;
  }
  if (persist_exists(PERSIST_ACTIVITY_INDEX)) {
    s_activity_index = persist_read_int(PERSIST_ACTIVITY_INDEX);
    if (s_activity_index < 0 || s_activity_index >= ACT_COUNT) s_activity_index = 0;
  }

  load_resources();

  s_window = window_create();
  window_set_window_handlers(s_window, (WindowHandlers){
    .load = window_load,
    .unload = window_unload,
  });
  window_stack_push(s_window, true);

  tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);
  accel_tap_service_subscribe(tap_handler);

  app_message_register_inbox_received(inbox_received_handler);
  app_message_open(128, 128);
}

static void deinit(void) {
  accel_tap_service_unsubscribe();
  tick_timer_service_unsubscribe();
  window_destroy(s_window);
  unload_resources();
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
