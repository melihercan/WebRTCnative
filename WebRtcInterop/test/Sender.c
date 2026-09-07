/* Exercises what add_track hands back: replacing the track on a live sender,
 * and removing it.
 *
 * Replacement is the interesting case. It is meant to happen mid-call without
 * renegotiating, so the test asserts that the peer connection stays connected
 * and that no renegotiation is asked for after the swap -- which is the whole
 * point of replaceTrack over remove-then-add. */
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include "Interop.h"

typedef rtc_status(__cdecl* fv)(void);
typedef rtc_status(__cdecl* ff)(rtc_factory**);
typedef void(__cdecl* fr)(rtc_factory*);
typedef rtc_status(__cdecl* fat)(rtc_factory*, const char*, rtc_media_track**);
typedef rtc_status(__cdecl* fvc)(rtc_factory*, int32_t*);
typedef rtc_status(__cdecl* fvi)(rtc_factory*, int32_t, char**, char**);
typedef rtc_status(__cdecl* fvt)(rtc_factory*, const char*, const char*, int32_t,
                                 int32_t, int32_t, rtc_media_track**);
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
typedef rtc_status(__cdecl* fice)(rtc_peer_connection*, const char*, int32_t, const char*);
typedef rtc_status(__cdecl* fadd)(rtc_peer_connection*, rtc_media_track*, const char*,
                                  rtc_rtp_sender**);
typedef rtc_status(__cdecl* frep)(rtc_rtp_sender*, rtc_media_track*);
typedef rtc_status(__cdecl* frem)(rtc_peer_connection*, rtc_rtp_sender*);
typedef void(__cdecl* fsr)(rtc_rtp_sender*);
typedef void(__cdecl* fsf)(char*);

static fset g_set_local, g_set_remote;
static fsdp g_create_answer;
static ftr  g_track_release;

static rtc_peer_connection *g_pc1, *g_pc2;
static volatile LONG g_answer_done = 0, g_conn1 = 0, g_conn2 = 0;
static volatile LONG g_renegotiations = 0, g_tracks2 = 0;

typedef struct { char mid[64]; int32_t idx; char sdp[512]; } cand;
static cand g_from1[64], g_from2[64];
static volatile LONG g_n1 = 0, g_n2 = 0;

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
static void __cdecl on_reneg1(void* ud) {
  (void)ud; InterlockedIncrement(&g_renegotiations);
}
static void __cdecl on_track2(void* ud, rtc_media_track* t, rtc_media_kind k,
                              const char* stream) {
  (void)ud; (void)k; (void)stream;
  InterlockedIncrement(&g_tracks2);
  g_track_release(t);
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

#define L(n, t) t n = (t)GetProcAddress(h, #n)

int main(void) {
  HMODULE h = LoadLibraryA("WebRtcInterop.dll");
  if (!h) { printf("load failed\n"); return 1; }
  L(rtc_initialize, fv); L(rtc_terminate, fv);
  L(rtc_factory_create, ff); L(rtc_factory_release, fr);
  L(rtc_audio_track_create, fat); L(rtc_media_track_release, ftr);
  L(rtc_video_device_count, fvc); L(rtc_video_device_info, fvi);
  L(rtc_video_track_create, fvt);
  L(rtc_peer_connection_create, fpc);
  L(rtc_peer_connection_close, fpcc);
  L(rtc_peer_connection_release, fpcr);
  L(rtc_peer_connection_create_offer, fsdp);
  L(rtc_peer_connection_create_answer, fsdp);
  L(rtc_peer_connection_set_local_description, fset);
  L(rtc_peer_connection_set_remote_description, fset);
  L(rtc_peer_connection_add_ice_candidate, fice);
  L(rtc_peer_connection_add_track, fadd);
  L(rtc_rtp_sender_replace_track, frep);
  L(rtc_peer_connection_remove_track, frem);
  L(rtc_rtp_sender_release, fsr);
  L(rtc_string_free, fsf);
  if (!rtc_rtp_sender_replace_track) { printf("missing export\n"); return 1; }
  printf("exports resolved\n\n");

  g_set_local = rtc_peer_connection_set_local_description;
  g_set_remote = rtc_peer_connection_set_remote_description;
  g_create_answer = rtc_peer_connection_create_answer;
  g_track_release = rtc_media_track_release;

  rtc_initialize();
  rtc_factory* f = NULL; rtc_factory_create(&f);

  rtc_configuration cfg = {NULL, 0};
  rtc_peer_connection_observer o1 = {on_ice1, on_state1, NULL, NULL, on_reneg1, NULL};
  rtc_peer_connection_observer o2 = {on_ice2, on_state2, NULL, on_track2, NULL, NULL};
  rtc_peer_connection_create(f, &cfg, &o1, NULL, &g_pc1);
  rtc_peer_connection_create(f, &cfg, &o2, NULL, &g_pc2);

  rtc_media_track *audio = NULL, *audio2 = NULL;
  rtc_audio_track_create(f, "micA", &audio);
  rtc_audio_track_create(f, "micB", &audio2);

  printf("--- add_track ---\n");
  rtc_rtp_sender* sender = NULL;
  rtc_status s = rtc_peer_connection_add_track(g_pc1, audio, "stream0", &sender);
  printf("    add_track           %d\n", (int)s);
  printf("    sender handle       %s\n", sender ? "yes" : "NO");
  if (s != RTC_OK || !sender) return 1;

  /* A null out-parameter is the "I will never touch this again" case. */
  rtc_media_track* extra = NULL;
  rtc_audio_track_create(f, "micC", &extra);
  printf("    add_track(NULL out) %d (expect 0)\n",
         (int)rtc_peer_connection_add_track(g_pc1, extra, "stream0", NULL));

  printf("\n--- negotiation ---\n");
  rtc_peer_connection_create_offer(g_pc1, on_offer, on_fail, NULL);
  for (int i = 0; i < 100 && !g_answer_done; i++) Sleep(50);
  if (!g_answer_done) { printf("FAILED: no answer\n"); return 1; }

  Sleep(1500);
  int bad = 0;
  for (LONG i = 0; i < g_n1 && i < 64; i++)
    if (rtc_peer_connection_add_ice_candidate(g_pc2, g_from1[i].mid,
                                              g_from1[i].idx, g_from1[i].sdp)) bad++;
  for (LONG i = 0; i < g_n2 && i < 64; i++)
    if (rtc_peer_connection_add_ice_candidate(g_pc1, g_from2[i].mid,
                                              g_from2[i].idx, g_from2[i].sdp)) bad++;
  for (int i = 0; i < 300 && !(g_conn1 && g_conn2); i++) Sleep(50);
  printf("    connected           %s / %s\n",
         g_conn1 ? "pc1" : "pc1 NO", g_conn2 ? "pc2" : "pc2 NO");
  if (!g_conn1 || !g_conn2) { printf("FAILED: never connected\n"); return 1; }

  LONG reneg_before = g_renegotiations;

  printf("\n--- replace_track ---\n");
  printf("    same kind           %d (expect 0)\n",
         (int)rtc_rtp_sender_replace_track(sender, audio2));
  printf("    null track          %d (expect 0, stops sending)\n",
         (int)rtc_rtp_sender_replace_track(sender, NULL));
  printf("    back to a track     %d (expect 0)\n",
         (int)rtc_rtp_sender_replace_track(sender, audio2));

  /* A video track on an audio sender must be refused. */
  int32_t cams = 0;
  rtc_video_device_count(f, &cams);
  if (cams > 0) {
    char *name = NULL, *id = NULL;
    rtc_video_device_info(f, 0, &name, &id);
    rtc_media_track* video = NULL;
    if (rtc_video_track_create(f, id, "cam0", 640, 480, 30, &video) == RTC_OK) {
      printf("    wrong kind          %d (expect -1)\n",
             (int)rtc_rtp_sender_replace_track(sender, video));
      rtc_media_track_release(video);
    }
    rtc_string_free(name); rtc_string_free(id);
  } else {
    printf("    wrong kind          skipped, no camera\n");
  }

  Sleep(500);
  LONG reneg_after = g_renegotiations;
  printf("    still connected     %s\n", g_conn1 && g_conn2 ? "YES" : "no");
  printf("    renegotiations      %ld (expect 0 -- that is the point)\n",
         reneg_after - reneg_before);

  printf("\n--- remove_track ---\n");
  printf("    remove              %d (expect 0)\n",
         (int)rtc_peer_connection_remove_track(g_pc1, sender));
  /* Idempotent, per W3C: removeTrack aborts quietly when the sender's track
   * is already null, so a second removal succeeds rather than erroring. */
  printf("    remove twice        %d (expect 0, idempotent)\n",
         (int)rtc_peer_connection_remove_track(g_pc1, sender));

  printf("\n--- error paths ---\n");
  printf("    replace(NULL sender)%d (expect -1)\n",
         (int)rtc_rtp_sender_replace_track(NULL, audio));
  printf("    remove(NULL sender) %d (expect -1)\n",
         (int)rtc_peer_connection_remove_track(g_pc1, NULL));
  printf("    remove(NULL pc)     %d (expect -1)\n",
         (int)rtc_peer_connection_remove_track(NULL, sender));

  int replaced_ok = (reneg_after - reneg_before) == 0 && g_conn1 && g_conn2;

  rtc_rtp_sender_release(sender);
  printf("\n    released the sender  no crash\n");

  rtc_peer_connection_close(g_pc1); rtc_peer_connection_close(g_pc2);
  rtc_media_track_release(audio); rtc_media_track_release(audio2);
  rtc_media_track_release(extra);
  rtc_peer_connection_release(g_pc1); rtc_peer_connection_release(g_pc2);
  rtc_factory_release(f); rtc_terminate();

  int pass = replaced_ok && !bad && g_tracks2 > 0;
  printf("\n%s\n", pass ? "SENDER PASSED" : "FAILED");
  return pass ? 0 : 1;
}
