/* Caps a live sender: reads its encodings, changes them, and proves through
 * the stats that the change reached the wire.
 *
 * Two assertions here, and the difference between them is worth knowing.
 *
 * frameWidth from outbound-rtp proves the cap was applied SOMEWHERE. It is not
 * evidence about the source: VideoStreamEncoder scales the frame itself when
 * it does not match the configured encoder resolution
 * (video_stream_encoder.cc), so a capturer that hands every frame through
 * untouched still produces a halved picture. Measured both ways on M152 -- the
 * width halves either way.
 *
 * media-source width is the assertion that discriminates.
 * AdaptedVideoTrackSource::GetStats reports nothing until AdaptFrame has
 * recorded a size, so a source that never calls it is invisible there, and by
 * the same token never acts on the sink wants that carry the encoder's
 * requests for fewer pixels when it falls behind. That is the defect: not that
 * the picture fails to shrink, but that the capturer keeps producing and
 * converting full-size frames for the encoder to shrink per frame, and that
 * adaptation under pressure has nowhere to land. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "Interop.h"

typedef rtc_status(__cdecl* fv)(void);
typedef rtc_status(__cdecl* ff)(rtc_factory**);
typedef void(__cdecl* fr)(rtc_factory*);
typedef rtc_status(__cdecl* fvc)(rtc_factory*, int32_t*);
typedef rtc_status(__cdecl* fvi)(rtc_factory*, int32_t, char**, char**);
typedef rtc_status(__cdecl* fvt)(rtc_factory*, const char*, const char*, int32_t,
                                 int32_t, int32_t, rtc_media_track**);
typedef rtc_status(__cdecl* fdc)(rtc_desktop_source_kind, int32_t*);
typedef rtc_status(__cdecl* fdi)(rtc_desktop_source_kind, int32_t, char**, int64_t*);
typedef rtc_status(__cdecl* fdt)(rtc_factory*, rtc_desktop_source_kind, int64_t,
                                 const char*, int32_t, rtc_media_track**);
typedef void(__cdecl* ftr)(rtc_media_track*);
typedef rtc_status(__cdecl* fpc)(rtc_factory*, const rtc_configuration*,
                                 const rtc_peer_connection_observer*, void*,
                                 rtc_peer_connection**);
typedef rtc_status(__cdecl* fpcc)(rtc_peer_connection*);
typedef void(__cdecl* fpcr)(rtc_peer_connection*);
typedef rtc_status(__cdecl* fsdp)(rtc_peer_connection*, rtc_on_sdp_success_fn,
                                  rtc_on_failure_fn, void*);
typedef rtc_status(__cdecl* fset)(rtc_peer_connection*, const char*, const char*,
                                  rtc_on_void_success_fn, rtc_on_failure_fn, void*);
typedef rtc_status(__cdecl* fice)(rtc_peer_connection*, const char*, int32_t,
                                  const char*);
typedef rtc_status(__cdecl* fadd)(rtc_peer_connection*, rtc_media_track*,
                                  const char*, rtc_rtp_sender**);
typedef rtc_status(__cdecl* fgp)(rtc_rtp_sender*, rtc_rtp_encoding*, int32_t,
                                 int32_t*);
typedef rtc_status(__cdecl* fsp)(rtc_rtp_sender*, const rtc_rtp_encoding*, int32_t);
typedef rtc_status(__cdecl* fss)(rtc_peer_connection*, rtc_rtp_sender*,
                                 rtc_on_stats_success_fn, rtc_on_failure_fn, void*);
typedef rtc_status(__cdecl* fpcs)(rtc_peer_connection*, rtc_on_stats_success_fn,
                                  rtc_on_failure_fn, void*);
typedef void(__cdecl* fsr)(rtc_rtp_sender*);
typedef void(__cdecl* fsf)(char*);

static fset g_set_local, g_set_remote;
static fsdp g_create_answer;
static ftr  g_track_release;

static rtc_peer_connection *g_pc1, *g_pc2;
static volatile LONG g_answer_done = 0, g_conn1 = 0, g_conn2 = 0;

typedef struct { char mid[64]; int32_t idx; char sdp[512]; } cand;
static cand g_from1[64], g_from2[64];
static volatile LONG g_n1 = 0, g_n2 = 0;

static char g_json[1 << 20];
static volatile LONG g_stats_done = 0;

static void __cdecl on_fail(void* tag, const char* err) {
  printf("    !! %s: %s\n", (const char*)tag, err);
}
static void __cdecl on_ok(void* tag) { (void)tag; }

static void stash(cand* buf, volatile LONG* n, const char* mid, int32_t idx,
                  const char* sdp) {
  LONG i = InterlockedIncrement(n) - 1;
  if (i >= 64) return;
  strncpy_s(buf[i].mid, sizeof(buf[i].mid), mid, _TRUNCATE);
  strncpy_s(buf[i].sdp, sizeof(buf[i].sdp), sdp, _TRUNCATE);
  buf[i].idx = idx;
}
static void __cdecl on_ice1(void* ud, const char* mid, int32_t i, const char* sdp) {
  (void)ud; stash(g_from1, &g_n1, mid, i, sdp);
}
static void __cdecl on_ice2(void* ud, const char* mid, int32_t i, const char* sdp) {
  (void)ud; stash(g_from2, &g_n2, mid, i, sdp);
}
static void __cdecl on_state1(void* ud, rtc_peer_connection_state s) {
  (void)ud;
  InterlockedExchange(&g_conn1, s == RTC_PEER_CONNECTION_STATE_CONNECTED);
}
static void __cdecl on_state2(void* ud, rtc_peer_connection_state s) {
  (void)ud;
  InterlockedExchange(&g_conn2, s == RTC_PEER_CONNECTION_STATE_CONNECTED);
}
static void __cdecl on_track2(void* ud, rtc_media_track* t, rtc_media_kind k,
                              const char* stream) {
  (void)ud; (void)k; (void)stream;
  g_track_release(t);
}
static void __cdecl on_stats(void* ud, const char* json) {
  (void)ud;
  strncpy_s(g_json, sizeof(g_json), json ? json : "", _TRUNCATE);
  InterlockedExchange(&g_stats_done, 1);
}
static void __cdecl on_stats_fail(void* ud, const char* err) {
  (void)ud; printf("    !! get_stats: %s\n", err);
  InterlockedExchange(&g_stats_done, 1);
}

static void __cdecl on_answer(void* ud, const char* type, const char* sdp);
static void __cdecl on_offer(void* ud, const char* type, const char* sdp) {
  (void)ud;
  g_set_local(g_pc1, type, sdp, on_ok, on_fail, (void*)"pc1 setLocal");
  g_set_remote(g_pc2, type, sdp, on_ok, on_fail, (void*)"pc2 setRemote");
  g_create_answer(g_pc2, on_answer, on_fail, NULL);
}
static void __cdecl on_answer(void* ud, const char* type, const char* sdp) {
  (void)ud;
  g_set_local(g_pc2, type, sdp, on_ok, on_fail, (void*)"pc2 setLocal");
  g_set_remote(g_pc1, type, sdp, on_ok, on_fail, (void*)"pc1 setRemote");
  InterlockedExchange(&g_answer_done, 1);
}

/* Pulls one integer field out of the stats JSON. -1 when it is not there,
 * which is a normal answer before any frame has been encoded. */
static int field(const char* json, const char* name) {
  char key[64];
  sprintf_s(key, sizeof(key), "\"%s\":", name);
  const char* at = strstr(json, key);
  if (at == NULL) return -1;
  return atoi(at + strlen(key));
}

/* The same, but only inside the stats object of the given type. The report is
 * one flat array, so a bare search for "width" would find whichever object
 * happens to come first. */
static int field_of(const char* json, const char* type, const char* name) {
  char key[64];
  sprintf_s(key, sizeof(key), "\"%s\"", type);
  const char* at = strstr(json, key);
  if (at == NULL) return -1;
  const char* end = strchr(at, '}');
  char object[2048];
  size_t len = end != NULL ? (size_t)(end - at) : strlen(at);
  if (len >= sizeof(object)) len = sizeof(object) - 1;
  memcpy(object, at, len);
  object[len] = '\0';
  return field(object, name);
}

static fss g_sender_stats;
/* Polls until the encoded width stops moving, rather than waiting a fixed
 * time. Two reasons it cannot be a sleep: the first seconds of a call report
 * no frameWidth at all, and after a cap is lifted WebRTC raises the resolution
 * back in steps over several seconds rather than in one go. */
static int sender_width(rtc_peer_connection* pc, rtc_rtp_sender* sender,
                        int budget_ms) {
  int width = -1;
  int previous = -1;
  int stable = 0;

  for (int i = 0; i < budget_ms / 250; i++) {
    Sleep(250);
    InterlockedExchange(&g_stats_done, 0);
    g_json[0] = '\0';
    if (g_sender_stats(pc, sender, on_stats, on_stats_fail, NULL) != RTC_OK) {
      return -1;
    }
    for (int j = 0; j < 40 && !g_stats_done; j++) Sleep(50);
    width = field(g_json, "frameWidth");

    if (width > 0 && width == previous) {
      if (++stable >= 6) break;
    } else {
      stable = 0;
    }
    previous = width;
  }
  return width;
}

#define L(n, t) t n = (t)GetProcAddress(h, #n)

int main(void) {
  HMODULE h = LoadLibraryA("WebRtcInterop.dll");
  if (!h) { printf("load failed\n"); return 1; }
  L(rtc_initialize, fv); L(rtc_terminate, fv);
  L(rtc_factory_create, ff); L(rtc_factory_release, fr);
  L(rtc_video_device_count, fvc); L(rtc_video_device_info, fvi);
  L(rtc_video_track_create, fvt);
  L(rtc_desktop_source_count, fdc); L(rtc_desktop_source_info, fdi);
  L(rtc_desktop_track_create, fdt);
  L(rtc_media_track_release, ftr);
  L(rtc_peer_connection_create, fpc);
  L(rtc_peer_connection_close, fpcc);
  L(rtc_peer_connection_release, fpcr);
  L(rtc_peer_connection_create_offer, fsdp);
  L(rtc_peer_connection_create_answer, fsdp);
  L(rtc_peer_connection_set_local_description, fset);
  L(rtc_peer_connection_set_remote_description, fset);
  L(rtc_peer_connection_add_ice_candidate, fice);
  L(rtc_peer_connection_add_track, fadd);
  L(rtc_rtp_sender_get_parameters, fgp);
  L(rtc_rtp_sender_set_parameters, fsp);
  L(rtc_rtp_sender_get_stats, fss);
  L(rtc_peer_connection_get_stats, fpcs);
  L(rtc_rtp_sender_release, fsr);
  L(rtc_string_free, fsf);
  if (!rtc_rtp_sender_get_parameters || !rtc_rtp_sender_set_parameters) {
    printf("missing export\n"); return 1;
  }
  printf("exports resolved\n\n");

  g_set_local = rtc_peer_connection_set_local_description;
  g_set_remote = rtc_peer_connection_set_remote_description;
  g_create_answer = rtc_peer_connection_create_answer;
  g_track_release = rtc_media_track_release;
  g_sender_stats = rtc_rtp_sender_get_stats;

  rtc_initialize();
  rtc_factory* f = NULL; rtc_factory_create(&f);

  /* The camera is the source issue #3 is about, so prefer it. A screen is the
   * fallback so this still runs on a machine with no camera, and it exercises
   * the same fix in the desktop capturer. */
  printf("--- source ---\n");
  rtc_media_track* video = NULL;
  const char* source_kind = NULL;
  int32_t cams = 0;
  rtc_video_device_count(f, &cams);
  if (cams > 0) {
    char *name = NULL, *id = NULL;
    if (rtc_video_device_info(f, 0, &name, &id) == RTC_OK) {
      if (rtc_video_track_create(f, id, "cam0", 640, 480, 30, &video) == RTC_OK) {
        printf("    camera              %s at 640x480@30\n", name);
        source_kind = "camera";
      } else {
        printf("    camera              %s would not open; falling back\n", name);
      }
      rtc_string_free(name); rtc_string_free(id);
    }
  }
  if (video == NULL) {
    int32_t screens = 0;
    rtc_desktop_source_count(RTC_DESKTOP_SOURCE_SCREEN, &screens);
    if (screens > 0) {
      char* title = NULL; int64_t id = 0;
      if (rtc_desktop_source_info(RTC_DESKTOP_SOURCE_SCREEN, 0, &title, &id) == RTC_OK) {
        if (rtc_desktop_track_create(f, RTC_DESKTOP_SOURCE_SCREEN, id, "screen0",
                                     15, &video) == RTC_OK) {
          printf("    screen              %s at 15fps\n", title);
          source_kind = "screen";
        }
        rtc_string_free(title);
      }
    }
  }
  if (video == NULL) { printf("    no video source; cannot run\n"); return 1; }

  rtc_configuration cfg = {NULL, 0};
  rtc_peer_connection_observer o1 = {on_ice1, on_state1, NULL, NULL, NULL, NULL};
  rtc_peer_connection_observer o2 = {on_ice2, on_state2, NULL, on_track2, NULL, NULL};
  rtc_peer_connection_create(f, &cfg, &o1, NULL, &g_pc1);
  rtc_peer_connection_create(f, &cfg, &o2, NULL, &g_pc2);

  rtc_rtp_sender* sender = NULL;
  rtc_status s = rtc_peer_connection_add_track(g_pc1, video, "stream0", &sender);
  if (s != RTC_OK || !sender) { printf("add_track failed %d\n", (int)s); return 1; }

  printf("\n--- negotiation ---\n");
  rtc_peer_connection_create_offer(g_pc1, on_offer, on_fail, NULL);
  for (int i = 0; i < 100 && !g_answer_done; i++) Sleep(50);
  if (!g_answer_done) { printf("FAILED: no answer\n"); return 1; }

  Sleep(1500);
  for (LONG i = 0; i < g_n1 && i < 64; i++)
    rtc_peer_connection_add_ice_candidate(g_pc2, g_from1[i].mid, g_from1[i].idx,
                                          g_from1[i].sdp);
  for (LONG i = 0; i < g_n2 && i < 64; i++)
    rtc_peer_connection_add_ice_candidate(g_pc1, g_from2[i].mid, g_from2[i].idx,
                                          g_from2[i].sdp);
  for (int i = 0; i < 300 && !(g_conn1 && g_conn2); i++) Sleep(50);
  printf("    connected           %s / %s\n",
         g_conn1 ? "pc1" : "pc1 NO", g_conn2 ? "pc2" : "pc2 NO");
  if (!g_conn1 || !g_conn2) { printf("FAILED: never connected\n"); return 1; }

  /* ---------------------------------------------------------- get ------- */
  printf("\n--- get_parameters ---\n");
  int32_t count = -1;
  printf("    count call          %d (expect 0)\n",
         (int)rtc_rtp_sender_get_parameters(sender, NULL, 0, &count));
  printf("    encodings           %d (expect 1)\n", (int)count);

  rtc_rtp_encoding enc[4];
  memset(enc, 0, sizeof(enc));
  printf("    buffer too small    %d (expect -1)\n",
         (int)rtc_rtp_sender_get_parameters(sender, enc, 0, NULL));
  printf("    null sender         %d (expect -1)\n",
         (int)rtc_rtp_sender_get_parameters(NULL, enc, 4, &count));

  int32_t got = 0;
  s = rtc_rtp_sender_get_parameters(sender, enc, 4, &got);
  printf("    read                %d, %d encoding(s)\n", (int)s, (int)got);
  if (s != RTC_OK || got != 1) { printf("FAILED: cannot read\n"); return 1; }
  printf("    defaults            active=%d max_bitrate=%d max_framerate=%d "
         "scale=%.1f\n", (int)enc[0].active, (int)enc[0].max_bitrate,
         (int)enc[0].max_framerate, enc[0].scale_resolution_down_by);
  int defaults_ok = enc[0].active == 1 && enc[0].max_bitrate == -1 &&
                    enc[0].max_framerate == -1 &&
                    enc[0].scale_resolution_down_by == 0.0;
  printf("    unset is unset      %s\n", defaults_ok ? "yes" : "NO");

  /* --------------------------------------------------------- baseline --- */
  printf("\n--- baseline (no cap) ---\n");
  int base_width = sender_width(g_pc1, sender, 12000);
  printf("    frameWidth          %d\n", base_width);
  if (base_width <= 0) { printf("FAILED: no frames encoded\n"); return 1; }

  /* ---------------------------------------------------------- set ------- */
  printf("\n--- set_parameters ---\n");
  printf("    null sender         %d (expect -1)\n",
         (int)rtc_rtp_sender_set_parameters(NULL, enc, 1));
  printf("    wrong count         %d (expect -1, count may not change)\n",
         (int)rtc_rtp_sender_set_parameters(sender, enc, 2));

  rtc_rtp_encoding low = enc[0];
  low.rid = NULL;                     /* not written by a set; left as is   */
  low.scalability_mode = NULL;
  low.max_bitrate = 150000;           /* DirectCallMe's low-data cap        */
  low.max_framerate = 10;
  low.scale_resolution_down_by = 2.0;
  printf("    low-data profile    %d (expect 0)\n",
         (int)rtc_rtp_sender_set_parameters(sender, &low, 1));

  rtc_rtp_encoding back[4];
  memset(back, 0, sizeof(back));
  rtc_rtp_sender_get_parameters(sender, back, 4, &got);
  printf("    read back           max_bitrate=%d max_framerate=%d scale=%.1f\n",
         (int)back[0].max_bitrate, (int)back[0].max_framerate,
         back[0].scale_resolution_down_by);
  int roundtrip_ok = back[0].max_bitrate == 150000 &&
                     back[0].max_framerate == 10 &&
                     back[0].scale_resolution_down_by == 2.0;
  printf("    round trip          %s\n", roundtrip_ok ? "yes" : "NO");

  /* -------------------------------------------------------- the point --- */
  printf("\n--- did it reach the wire? ---\n");
  int capped_width = sender_width(g_pc1, sender, 15000);
  int target = field(g_json, "targetBitrate");
  printf("    frameWidth          %d (was %d; expect about half)\n",
         capped_width, base_width);
  printf("    targetBitrate       %d (cap 150000)\n", target);

  int halved = capped_width > 0 && capped_width <= base_width * 3 / 5;
  int capped = target < 0 || target <= 150000;
  printf("    resolution dropped  %s\n", halved ? "yes" : "NO");
  printf("    bitrate held        %s\n", capped ? "yes" : "NO");

  /* frameWidth alone does NOT prove the source adapted: VideoStreamEncoder
   * scales the frame itself when it does not match the configured encoder
   * resolution (video_stream_encoder.cc), so a capturer that hands every frame
   * through untouched still produces a halved picture -- it just does the work
   * per frame in the encoder and keeps the adapter's decisions unapplied.
   *
   * media-source is the honest signal. AdaptedVideoTrackSource::GetStats
   * returns false until AdaptFrame has recorded a size, so a source that never
   * calls it reports no width at all, and neither does it act on the sink
   * wants that carry the encoder's requests under pressure. */
  printf("\n--- did the SOURCE adapt? ---\n");
  InterlockedExchange(&g_stats_done, 0);
  g_json[0] = '\0';
  rtc_peer_connection_get_stats(g_pc1, on_stats, on_stats_fail, NULL);
  for (int j = 0; j < 40 && !g_stats_done; j++) Sleep(50);
  int source_width = field_of(g_json, "media-source", "width");
  int source_fps = field_of(g_json, "media-source", "framesPerSecond");
  printf("    media-source width  %d (absent reads as -1)\n", source_width);
  printf("    media-source fps    %d (max_framerate asked for 10)\n",
         source_fps);
  int source_adapts = source_width > 0;
  printf("    source adapts       %s\n", source_adapts ? "yes" : "NO");

  /* ------------------------------------------------------- lift it ------ */
  printf("\n--- lifting the cap ---\n");
  rtc_rtp_encoding full = back[0];
  full.rid = NULL;
  full.scalability_mode = NULL;
  full.max_bitrate = -1;              /* -1 clears rather than is ignored   */
  full.max_framerate = -1;
  full.scale_resolution_down_by = 1.0;
  printf("    restore             %d (expect 0)\n",
         (int)rtc_rtp_sender_set_parameters(sender, &full, 1));
  int restored_width = sender_width(g_pc1, sender, 25000);
  printf("    frameWidth          %d (capped at %d, baseline %d)\n",
         restored_width, capped_width, base_width);
  /* Recovery, not a full return: WebRTC raises resolution back in steps after
   * sustained good conditions, so how far it gets inside any one window is a
   * property of the ramp rather than of the cap having been lifted. */
  int restored = restored_width > capped_width;
  printf("    resolution recovers %s%s\n", restored ? "yes" : "NO",
         restored_width >= base_width ? " (all the way to baseline)" : "");

  rtc_rtp_encoding after[4];
  memset(after, 0, sizeof(after));
  rtc_rtp_sender_get_parameters(sender, after, 4, &got);
  int cleared = after[0].max_bitrate == -1 && after[0].max_framerate == -1;
  printf("    cap cleared         %s (max_bitrate=%d)\n",
         cleared ? "yes" : "NO", (int)after[0].max_bitrate);

  /* ---------------------------------------------------------- tidy ------ */
  for (int i = 0; i < got; i++) {
    rtc_string_free((char*)after[i].rid);
    rtc_string_free((char*)after[i].scalability_mode);
  }
  for (int i = 0; i < 1; i++) {
    rtc_string_free((char*)enc[i].rid);
    rtc_string_free((char*)enc[i].scalability_mode);
    rtc_string_free((char*)back[i].rid);
    rtc_string_free((char*)back[i].scalability_mode);
  }

  rtc_rtp_sender_release(sender);
  rtc_media_track_release(video);
  rtc_peer_connection_close(g_pc1);
  rtc_peer_connection_close(g_pc2);
  rtc_peer_connection_release(g_pc1);
  rtc_peer_connection_release(g_pc2);
  rtc_factory_release(f);
  rtc_terminate();

  int pass = defaults_ok && roundtrip_ok && halved && capped && restored &&
             cleared && source_adapts;
  printf("\nsource: %s\n%s\n", source_kind,
         pass ? "SENDER PARAMETERS PASSED" : "FAILED");
  return pass ? 0 : 1;
}
