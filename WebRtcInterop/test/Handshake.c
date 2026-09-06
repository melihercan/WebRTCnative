/* Drives a complete offer/answer/ICE handshake between two peer connections
 * in one process, the way WebRTCme.Bindings.Maui.Windows will.
 *
 * Candidates are buffered rather than forwarded from inside the callback, for
 * two reasons: a candidate offered before the remote description is set is
 * dropped, and the ABI says calls that mutate a peer connection must not be
 * made from inside a callback. Real signalling has the same shape. */
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include "Interop.h"

typedef rtc_status(__cdecl* fv)(void);
typedef rtc_status(__cdecl* ff)(rtc_factory**);
typedef void(__cdecl* fr)(rtc_factory*);
typedef rtc_status(__cdecl* fat)(rtc_factory*, const char*, rtc_media_track**);
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
typedef rtc_status(__cdecl* fadd)(rtc_peer_connection*, rtc_media_track*, const char*);

static fset g_set_local, g_set_remote;
static fsdp g_create_answer;
static ftr  g_track_release;

static rtc_peer_connection *g_pc1, *g_pc2;
static volatile LONG g_tracks = 0, g_conn1 = 0, g_conn2 = 0, g_answer_done = 0;

/* Buffered candidates, flushed once both sides have their descriptions. */
typedef struct { char mid[64]; int32_t idx; char sdp[512]; } cand;
static cand g_from1[64], g_from2[64];
static volatile LONG g_n1 = 0, g_n2 = 0;

static void __cdecl on_fail(void* tag, const char* err) {
  printf("    !! %s: %s\n", (const char*)tag, err);
}
static void __cdecl on_ok(void* tag) { printf("    ok  %s\n", (const char*)tag); }

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

static const char* kState[] = {"new","connecting","connected","disconnected","failed","closed"};
static void __cdecl on_state1(void* ud, rtc_peer_connection_state s) {
  (void)ud; printf("    pc1 -> %s\n", kState[s]);
  if (s == RTC_PEER_CONNECTION_STATE_CONNECTED) InterlockedExchange(&g_conn1, 1);
}
static void __cdecl on_state2(void* ud, rtc_peer_connection_state s) {
  (void)ud; printf("    pc2 -> %s\n", kState[s]);
  if (s == RTC_PEER_CONNECTION_STATE_CONNECTED) InterlockedExchange(&g_conn2, 1);
}
static void __cdecl on_track2(void* ud, rtc_media_track* t, rtc_media_kind k,
                              const char* stream) {
  (void)ud;
  printf("    pc2 received %s track on stream '%s'\n",
         k == RTC_MEDIA_KIND_AUDIO ? "audio" : "video", stream);
  InterlockedIncrement(&g_tracks);
  g_track_release(t); /* rule 1: this handle is ours */
}

static void __cdecl on_answer(void* ud, const char* type, const char* sdp);

static void __cdecl on_offer(void* ud, const char* type, const char* sdp) {
  (void)ud;
  printf("    offer  created (%s, %zu bytes)\n", type, strlen(sdp));
  g_set_local(g_pc1, type, sdp, on_ok, on_fail, (void*)"pc1 setLocal(offer)");
  g_set_remote(g_pc2, type, sdp, on_ok, on_fail, (void*)"pc2 setRemote(offer)");
  g_create_answer(g_pc2, on_answer, on_fail, NULL);
}
static void __cdecl on_answer(void* ud, const char* type, const char* sdp) {
  (void)ud;
  printf("    answer created (%s, %zu bytes)\n", type, strlen(sdp));
  g_set_local(g_pc2, type, sdp, on_ok, on_fail, (void*)"pc2 setLocal(answer)");
  g_set_remote(g_pc1, type, sdp, on_ok, on_fail, (void*)"pc1 setRemote(answer)");
  InterlockedExchange(&g_answer_done, 1);
}

#define L(n, t) t n = (t)GetProcAddress(h, #n)

int main(void) {
  HMODULE h = LoadLibraryA("WebRtcInterop.dll");
  if (!h) { printf("load failed\n"); return 1; }
  L(rtc_initialize, fv); L(rtc_terminate, fv);
  L(rtc_factory_create, ff); L(rtc_factory_release, fr);
  L(rtc_audio_track_create, fat); L(rtc_media_track_release, ftr);
  L(rtc_peer_connection_create, fpc);
  L(rtc_peer_connection_close, fpcc);
  L(rtc_peer_connection_release, fpcr);
  L(rtc_peer_connection_create_offer, fsdp);
  L(rtc_peer_connection_create_answer, fsdp);
  L(rtc_peer_connection_set_local_description, fset);
  L(rtc_peer_connection_set_remote_description, fset);
  L(rtc_peer_connection_add_ice_candidate, fice);
  L(rtc_peer_connection_add_track, fadd);
  printf("exports resolved\n\n");

  g_set_local = rtc_peer_connection_set_local_description;
  g_set_remote = rtc_peer_connection_set_remote_description;
  g_create_answer = rtc_peer_connection_create_answer;
  g_track_release = rtc_media_track_release;

  rtc_initialize();
  rtc_factory* f = NULL; rtc_factory_create(&f);

  /* No STUN: both peers are on this host, so host candidates suffice and the
   * test stays offline. */
  rtc_configuration cfg = {NULL, 0};
  rtc_peer_connection_observer o1 = {on_ice1, on_state1, NULL, NULL, NULL};
  rtc_peer_connection_observer o2 = {on_ice2, on_state2, NULL, on_track2, NULL};

  rtc_peer_connection_create(f, &cfg, &o1, NULL, &g_pc1);
  rtc_peer_connection_create(f, &cfg, &o2, NULL, &g_pc2);

  rtc_media_track* audio = NULL;
  rtc_audio_track_create(f, "mic", &audio);
  printf("pc1 add_track %d\n\n--- negotiation ---\n",
         (int)rtc_peer_connection_add_track(g_pc1, audio, "stream0"));

  rtc_peer_connection_create_offer(g_pc1, on_offer, on_fail, NULL);
  for (int i = 0; i < 100 && !g_answer_done; i++) Sleep(50);
  if (!g_answer_done) { printf("\nFAILED: no answer in 5s\n"); return 1; }

  /* Let gathering settle, then exchange. */
  Sleep(1500);
  printf("\n--- ICE exchange (%ld from pc1, %ld from pc2) ---\n", g_n1, g_n2);
  int bad = 0;
  for (LONG i = 0; i < g_n1 && i < 64; i++)
    if (rtc_peer_connection_add_ice_candidate(g_pc2, g_from1[i].mid,
                                              g_from1[i].idx, g_from1[i].sdp)) bad++;
  for (LONG i = 0; i < g_n2 && i < 64; i++)
    if (rtc_peer_connection_add_ice_candidate(g_pc1, g_from2[i].mid,
                                              g_from2[i].idx, g_from2[i].sdp)) bad++;
  printf("    rejected candidates: %d\n", bad);

  for (int i = 0; i < 200 && !(g_conn1 && g_conn2); i++) Sleep(50);
  printf("\n    remote tracks: %ld\n    pc1 connected: %s\n    pc2 connected: %s\n",
         g_tracks, g_conn1 ? "YES" : "no", g_conn2 ? "YES" : "no");

  rtc_peer_connection_close(g_pc1); rtc_peer_connection_close(g_pc2);
  rtc_media_track_release(audio);
  rtc_peer_connection_release(g_pc1); rtc_peer_connection_release(g_pc2);
  rtc_factory_release(f); rtc_terminate();

  int pass = g_conn1 && g_conn2 && g_tracks && !bad;
  printf("\n%s\n", pass ? "HANDSHAKE PASSED" : "INCOMPLETE");
  return pass ? 0 : 1;
}
