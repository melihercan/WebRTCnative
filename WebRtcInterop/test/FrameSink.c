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
typedef rtc_status(__cdecl* fgs)(rtc_media_track*, int32_t*, int32_t*, int32_t*);
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
  L(rtc_video_track_get_settings, fgs);
  L(rtc_media_track_release, ftr); L(rtc_string_free, fsf);
  if (!rtc_video_track_add_sink) { printf("missing export\n"); return 1; }

  rtc_initialize();
  rtc_factory* f = NULL; rtc_factory_create(&f);

  int32_t n = 0; rtc_video_device_count(f, &n);
  if (n <= 0) { printf("no camera; skipping\n"); rtc_terminate(); return 0; }
  char *name = NULL, *id = NULL;
  rtc_video_device_info(f, 0, &name, &id);
  printf("camera: %s\n", name);

  /* A size no webcam publishes. The capture module answers an unsupported
   * request with its nearest supported format and says nothing, so the point
   * is not that this fails -- it will not -- but that get_settings reports
   * what was really opened instead of echoing the request back. Done before
   * the real track, because the device can only be opened once. */
  printf("\n--- an unsupported request ---\n");
  rtc_media_track* odd = NULL;
  if (rtc_video_track_create(f, id, "odd", 999, 777, 29, &odd) == RTC_OK) {
    int32_t ow = 0, oh = 0, ofps = 0;
    printf("  asked for          999x777@29\n");
    printf("  get_settings       %d\n",
           (int)rtc_video_track_get_settings(odd, &ow, &oh, &ofps));
    printf("  really opened at   %dx%d@%d\n", (int)ow, (int)oh, (int)ofps);
    printf("  reports the truth  %s\n",
           (ow != 999 || oh != 777) ? "yes (differs from the request)"
                                    : "the device really does support it");
    rtc_media_track_release(odd);
    Sleep(300);  /* let the device close before reopening it */
  } else {
    printf("  could not open; skipping\n");
  }

  printf("\n--- the real track ---\n");
  rtc_media_track* vt = NULL;
  rtc_status s = rtc_video_track_create(f, id, "cam0", 640, 480, 30, &vt);
  printf("video_track_create   %d\n", (int)s);
  if (s != RTC_OK) return 1;

  int32_t sw = 0, sh = 0, sfps = 0;
  printf("get_settings         %d\n",
         (int)rtc_video_track_get_settings(vt, &sw, &sh, &sfps));
  printf("  opened at          %dx%d@%d\n", (int)sw, (int)sh, (int)sfps);
  printf("get_settings(NULL)   %d (expect -1)\n",
         (int)rtc_video_track_get_settings(NULL, &sw, &sh, &sfps));

  printf("add_sink             %d\n", (int)rtc_video_track_add_sink(vt, on_frame, NULL));
  printf("add_sink twice       %d (expect -2 INVALID_STATE)\n",
         (int)rtc_video_track_add_sink(vt, on_frame, NULL));

  /* The camera is open now, so a second open of the same device must report
   * INVALID_STATE -- in use -- and not NOT_FOUND, which would send a caller
   * hunting for hardware that is present all along. */
  rtc_media_track* busy = NULL;
  printf("open while in use    %d (expect -2, NOT -3)\n",
         (int)rtc_video_track_create(f, id, "cam1", 640, 480, 30, &busy));
  if (busy) { rtc_media_track_release(busy); busy = NULL; }

  printf("open unknown device  %d (expect -3 NOT_FOUND)\n",
         (int)rtc_video_track_create(f, "no-such-device", "cam2", 640, 480, 30, &busy));
  if (busy) { rtc_media_track_release(busy); busy = NULL; }

  rtc_media_track* at = NULL;
  rtc_audio_track_create(f, "mic", &at);
  printf("add_sink on audio    %d (expect -1 INVALID_ARG)\n",
         (int)rtc_video_track_add_sink(at, on_frame, NULL));
  printf("settings on audio    %d (expect -3 NOT_FOUND)\n",
         (int)rtc_video_track_get_settings(at, NULL, NULL, NULL));

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

  /* The whole point of reporting the format: the numbers a caller reads back
   * have to be the numbers the frames arrive at, or they are worth nothing. */
  int settings_match = sw == g_w && sh == g_h;
  printf("\nsettings match the frames  %s (%dx%d reported, %dx%d delivered)\n",
         settings_match ? "yes" : "NO", (int)sw, (int)sh, g_w, g_h);

  int pass = frames > 30 && g_bad_planes == 0 && g_w > 0 && settings_match;
  printf("\n%s\n", pass ? "FRAME SINK PASSED" : "FAILED");
  return pass ? 0 : 1;
}
