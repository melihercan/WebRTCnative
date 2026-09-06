/* Opens the first camera, attaches a frame sink, and checks that real frames
 * arrive at a plausible rate with usable planes.
 *
 * Deliberately does the minimum inside the callback: this runs on a WebRTC
 * capture thread and anything slower than the frame interval drops frames. */
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include "Interop.h"

typedef rtc_status(__cdecl* fv)(void);
typedef rtc_status(__cdecl* ff)(rtc_factory**);
typedef void(__cdecl* fr)(rtc_factory*);
typedef rtc_status(__cdecl* fc)(rtc_factory*, int32_t*);
typedef rtc_status(__cdecl* fi)(rtc_factory*, int32_t, char**, char**);
typedef rtc_status(__cdecl* fvt)(rtc_factory*, const char*, const char*, int32_t,
                                 int32_t, int32_t, rtc_media_track**);
typedef rtc_status(__cdecl* fas)(rtc_media_track*, rtc_on_frame_fn, void*);
typedef rtc_status(__cdecl* frs)(rtc_media_track*);
typedef rtc_status(__cdecl* fat)(rtc_factory*, const char*, rtc_media_track**);
typedef void(__cdecl* ftr)(rtc_media_track*);
typedef void(__cdecl* fsf)(char*);

static volatile LONG g_frames = 0;
static int32_t g_w, g_h, g_sy, g_su, g_sv;
static int64_t g_first_us, g_last_us;
static volatile LONG g_bad_planes = 0, g_blank = 0;

static void __cdecl on_frame(void* ud, const rtc_video_frame* f) {
  (void)ud;
  LONG n = InterlockedIncrement(&g_frames);
  if (f->y == NULL || f->u == NULL || f->v == NULL || f->width <= 0 ||
      f->height <= 0 || f->stride_y < f->width) {
    InterlockedIncrement(&g_bad_planes);
    return;
  }
  /* A frame of identical bytes usually means the planes are not really there.
   * Sample a row rather than scanning: this is the capture thread. */
  unsigned char first = f->y[0];
  int varied = 0;
  for (int x = 1; x < f->width && x < 256; x++) {
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
  L(rtc_video_device_count, fc); L(rtc_video_device_info, fi);
  L(rtc_video_track_create, fvt); L(rtc_audio_track_create, fat);
  L(rtc_video_track_add_sink, fas); L(rtc_video_track_remove_sink, frs);
  L(rtc_media_track_release, ftr); L(rtc_string_free, fsf);
  if (!rtc_video_track_add_sink) { printf("missing export\n"); return 1; }

  rtc_initialize();
  rtc_factory* f = NULL; rtc_factory_create(&f);

  int32_t n = 0; rtc_video_device_count(f, &n);
  if (n <= 0) { printf("no camera; skipping\n"); rtc_terminate(); return 0; }
  char *name = NULL, *id = NULL;
  rtc_video_device_info(f, 0, &name, &id);
  printf("camera: %s\n", name);

  rtc_media_track* vt = NULL;
  rtc_status s = rtc_video_track_create(f, id, "cam0", 640, 480, 30, &vt);
  printf("video_track_create   %d\n", (int)s);
  if (s != RTC_OK) return 1;

  printf("add_sink             %d\n", (int)rtc_video_track_add_sink(vt, on_frame, NULL));
  printf("add_sink twice       %d (expect -2 INVALID_STATE)\n",
         (int)rtc_video_track_add_sink(vt, on_frame, NULL));

  rtc_media_track* at = NULL;
  rtc_audio_track_create(f, "mic", &at);
  printf("add_sink on audio    %d (expect -1 INVALID_ARG)\n",
         (int)rtc_video_track_add_sink(at, on_frame, NULL));

  printf("\ncapturing for 3 seconds...\n");
  Sleep(3000);

  LONG frames = g_frames;
  double span = (double)(g_last_us - g_first_us) / 1e6;
  printf("\n  frames delivered   %ld\n", frames);
  printf("  resolution         %dx%d\n", g_w, g_h);
  printf("  strides            y=%d u=%d v=%d\n", g_sy, g_su, g_sv);
  printf("  timestamp span     %.2f s\n", span);
  if (span > 0.5) printf("  effective rate     %.1f fps\n", frames / span);
  printf("  bad planes         %ld\n", g_bad_planes);
  printf("  blank rows         %ld\n", g_blank);

  printf("\nremove_sink          %d\n", (int)rtc_video_track_remove_sink(vt));
  LONG after = g_frames;
  Sleep(700);
  printf("frames after remove  %ld (expect 0)\n", g_frames - after);
  printf("remove_sink twice    %d (expect -2 INVALID_STATE)\n",
         (int)rtc_video_track_remove_sink(vt));

  /* Release with a sink attached: the destructor must unregister it. */
  rtc_video_track_add_sink(vt, on_frame, NULL);
  rtc_media_track_release(vt);
  printf("released with a live sink  no crash\n");

  rtc_media_track_release(at);
  rtc_string_free(name); rtc_string_free(id);
  rtc_factory_release(f); rtc_terminate();

  int pass = frames > 30 && g_bad_planes == 0 && g_w > 0;
  printf("\n%s\n", pass ? "FRAME SINK PASSED" : "FAILED");
  return pass ? 0 : 1;
}
