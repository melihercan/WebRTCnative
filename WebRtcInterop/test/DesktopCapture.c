/* Enumerates screens and windows, captures a screen, and checks the frames.
 *
 * The frame check matters more here than for the camera: the conversion is
 * ours (BGRA to I420 through libyuv) rather than the capture module's, so a
 * stride or plane mistake would show as a sheared or grey image that every
 * status code would still call success. */
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include "Interop.h"

typedef rtc_status(__cdecl* fv)(void);
typedef rtc_status(__cdecl* ff)(rtc_factory**);
typedef void(__cdecl* fr)(rtc_factory*);
typedef rtc_status(__cdecl* fdc)(rtc_desktop_source_kind, int32_t*);
typedef rtc_status(__cdecl* fdi)(rtc_desktop_source_kind, int32_t, char**, int64_t*);
typedef rtc_status(__cdecl* fdt)(rtc_factory*, rtc_desktop_source_kind, int64_t,
                                 const char*, int32_t, rtc_media_track**);
typedef rtc_status(__cdecl* fas)(rtc_media_track*, rtc_on_frame_fn, void*);
typedef rtc_status(__cdecl* frs)(rtc_media_track*);
typedef void(__cdecl* ftr)(rtc_media_track*);
typedef void(__cdecl* fsf)(char*);

static volatile LONG g_frames = 0, g_bad = 0, g_blank = 0;
static int32_t g_w, g_h, g_sy, g_su, g_sv;
static int64_t g_first_us, g_last_us;

static void __cdecl on_frame(void* ud, const rtc_video_frame* f) {
  (void)ud;
  LONG n = InterlockedIncrement(&g_frames);
  if (f->y == NULL || f->u == NULL || f->v == NULL || f->width <= 0 ||
      f->height <= 0 || f->stride_y < f->width) {
    InterlockedIncrement(&g_bad);
    return;
  }
  /* A desktop is never one flat colour across a whole row; if it is, the
   * conversion produced nothing useful. Sample rather than scan. */
  unsigned char first = f->y[0];
  int varied = 0;
  for (int x = 1; x < f->width && x < 512; x++) {
    if (f->y[x] != first) { varied = 1; break; }
  }
  if (!varied) InterlockedIncrement(&g_blank);

  if (n == 1) { g_first_us = f->timestamp_us; g_w = f->width; g_h = f->height;
                g_sy = f->stride_y; g_su = f->stride_u; g_sv = f->stride_v; }
  g_last_us = f->timestamp_us;
}

#define L(n, t) t n = (t)GetProcAddress(h, #n)

int main(void) {
  HMODULE h = LoadLibraryA("WebRtcInterop.dll");
  if (!h) { printf("load failed\n"); return 1; }
  L(rtc_initialize, fv); L(rtc_terminate, fv);
  L(rtc_factory_create, ff); L(rtc_factory_release, fr);
  L(rtc_desktop_source_count, fdc); L(rtc_desktop_source_info, fdi);
  L(rtc_desktop_track_create, fdt);
  L(rtc_video_track_add_sink, fas); L(rtc_video_track_remove_sink, frs);
  L(rtc_media_track_release, ftr); L(rtc_string_free, fsf);
  if (!rtc_desktop_track_create) { printf("missing export\n"); return 1; }

  rtc_initialize();
  rtc_factory* f = NULL; rtc_factory_create(&f);

  int32_t screens = 0, windows = 0;
  printf("screens\n");
  rtc_status s = rtc_desktop_source_count(RTC_DESKTOP_SOURCE_SCREEN, &screens);
  if (s != RTC_OK) { printf("  count failed %d\n", (int)s); return 1; }

  int64_t screen_id = 0;
  for (int32_t i = 0; i < screens; i++) {
    char* title = NULL; int64_t id = 0;
    if (rtc_desktop_source_info(RTC_DESKTOP_SOURCE_SCREEN, i, &title, &id) == RTC_OK) {
      printf("  [%d] %-40s id=%lld\n", (int)i, title, (long long)id);
      if (i == 0) screen_id = id;
      rtc_string_free(title);
    }
  }
  if (!screens) printf("  (none)\n");

  printf("\nwindows\n");
  rtc_desktop_source_count(RTC_DESKTOP_SOURCE_WINDOW, &windows);
  for (int32_t i = 0; i < windows && i < 5; i++) {
    char* title = NULL; int64_t id = 0;
    if (rtc_desktop_source_info(RTC_DESKTOP_SOURCE_WINDOW, i, &title, &id) == RTC_OK) {
      printf("  [%d] %.50s\n", (int)i, title);
      rtc_string_free(title);
    }
  }
  printf("  ... %d windows total\n", (int)windows);

  printf("\nerror paths\n");
  int32_t ignored = 0;
  printf("  count(bad kind)       %d (expect -1)\n",
         (int)rtc_desktop_source_count(99, &ignored));
  printf("  count(NULL out)       %d (expect -1)\n",
         (int)rtc_desktop_source_count(RTC_DESKTOP_SOURCE_SCREEN, NULL));
  char* t = NULL; int64_t i64 = 0;
  printf("  info(index 999)       %d (expect -3)\n",
         (int)rtc_desktop_source_info(RTC_DESKTOP_SOURCE_SCREEN, 999, &t, &i64));
  printf("  info(negative)        %d (expect -1)\n",
         (int)rtc_desktop_source_info(RTC_DESKTOP_SOURCE_SCREEN, -1, &t, &i64));

  rtc_media_track* track = NULL;
  printf("  track(fps 0)          %d (expect -1)\n",
         (int)rtc_desktop_track_create(f, RTC_DESKTOP_SOURCE_SCREEN, screen_id,
                                       "screen0", 0, &track));

  if (!screens) { printf("\nno screen to capture; skipping\n"); rtc_terminate(); return 0; }

  printf("\ncapture\n");
  s = rtc_desktop_track_create(f, RTC_DESKTOP_SOURCE_SCREEN, screen_id,
                               "screen0", 15, &track);
  printf("  desktop_track_create  %d\n", (int)s);
  if (s != RTC_OK) { printf("\nFAILED\n"); return 1; }

  printf("  add_sink              %d\n",
         (int)rtc_video_track_add_sink(track, on_frame, NULL));
  printf("  capturing for 3 seconds...\n");
  Sleep(3000);

  LONG frames = g_frames;
  double span = (double)(g_last_us - g_first_us) / 1e6;
  printf("\n  frames delivered      %ld\n", frames);
  printf("  resolution            %dx%d\n", g_w, g_h);
  printf("  strides               y=%d u=%d v=%d\n", g_sy, g_su, g_sv);
  if (span > 0.5) printf("  effective rate        %.1f fps\n", frames / span);
  printf("  bad planes            %ld\n", g_bad);
  printf("  flat rows             %ld (a desktop should have almost none)\n", g_blank);

  printf("\n  remove_sink           %d\n", (int)rtc_video_track_remove_sink(track));
  LONG after = g_frames;
  Sleep(700);
  printf("  frames after remove   %ld (expect 0)\n", g_frames - after);

  rtc_media_track_release(track);
  printf("  released the track    no crash\n");

  rtc_factory_release(f);
  rtc_terminate();

  int pass = frames > 10 && g_bad == 0 && g_w > 0 && g_blank < frames;
  printf("\n%s\n", pass ? "DESKTOP CAPTURE PASSED" : "FAILED");
  return pass ? 0 : 1;
}
