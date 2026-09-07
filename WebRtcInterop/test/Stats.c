/* Collects statistics from a connected peer connection.
 *
 * The point of this test is the shape of the JSON, not the numbers. The
 * managed side parses it into a dictionary keyed by stats id, so the three
 * things worth asserting are that the payload is an array, that every entry
 * carries "id", "type" and "timestamp", and that the report is non-empty once
 * media is actually flowing. A report that arrives empty, or with a member set
 * WebRTC renamed, would otherwise be discovered as an empty dictionary in C#.
 *
 * Candidates are buffered rather than forwarded from inside the callback, for
 * the same reasons as Handshake.c. */
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
typedef void(__cdecl* fpcr)(rtc_peer_connection*);
typedef rtc_status(__cdecl* fsdp)(rtc_peer_connection*, rtc_on_sdp_success_fn,
                                  rtc_on_failure_fn, void*);
typedef rtc_status(__cdecl* fset)(rtc_peer_connection*, const char*, const char*,
                                  rtc_on_void_success_fn, rtc_on_failure_fn, void*);
typedef rtc_status(__cdecl* fice)(rtc_peer_connection*, const char*, int32_t, const char*);
typedef rtc_status(__cdecl* fadd)(rtc_peer_connection*, rtc_media_track*, const char*,
                                  rtc_rtp_sender**);
typedef rtc_status(__cdecl* fstats)(rtc_peer_connection*, rtc_on_stats_success_fn,
                                    rtc_on_failure_fn, void*);

static fset g_set_local, g_set_remote;
static fsdp g_create_answer;
static ftr g_track_release;

static rtc_peer_connection *g_pc1, *g_pc2;
static volatile LONG g_conn1 = 0, g_conn2 = 0, g_answer_done = 0;

typedef struct { char mid[64]; int32_t idx; char sdp[512]; } cand;
static cand g_from1[64], g_from2[64];
static volatile LONG g_n1 = 0, g_n2 = 0;

/* The collected report, copied out of the borrowed string. */
static volatile LONG g_stats_done = 0, g_stats_failed = 0;
static char g_json[1 << 20];

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

/* The string is borrowed, so copy before returning — the same contract the
 * SDP callbacks have. */
static void __cdecl on_stats(void* ud, const char* json) {
  (void)ud;
  strncpy_s(g_json, sizeof(g_json), json ? json : "", _TRUNCATE);
  InterlockedExchange(&g_stats_done, 1);
}
static void __cdecl on_stats_fail(void* ud, const char* err) {
  (void)ud;
  printf("    !! get_stats: %s\n", err);
  InterlockedExchange(&g_stats_failed, 1);
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

/* Counts non-overlapping occurrences, enough to assert every entry has the
 * three standard keys without linking a JSON parser into a smoke test. */
static int count(const char* haystack, const char* needle) {
  int n = 0;
  size_t len = strlen(needle);
  for (const char* p = strstr(haystack, needle); p; p = strstr(p + len, needle)) n++;
  return n;
}

#define L(n, t) t n = (t)GetProcAddress(h, #n)

int main(void) {
  HMODULE h = LoadLibraryA("WebRtcInterop.dll");
  if (!h) { printf("load failed\n"); return 1; }
  L(rtc_initialize, fv); L(rtc_terminate, fv);
  L(rtc_factory_create, ff); L(rtc_factory_release, fr);
  L(rtc_audio_track_create, fat); L(rtc_media_track_release, ftr);
  L(rtc_peer_connection_create, fpc);
  L(rtc_peer_connection_release, fpcr);
  L(rtc_peer_connection_create_offer, fsdp);
  L(rtc_peer_connection_create_answer, fsdp);
  L(rtc_peer_connection_set_local_description, fset);
  L(rtc_peer_connection_set_remote_description, fset);
  L(rtc_peer_connection_add_ice_candidate, fice);
  L(rtc_peer_connection_add_track, fadd);
  L(rtc_peer_connection_get_stats, fstats);
  if (!rtc_peer_connection_get_stats) { printf("missing export\n"); return 1; }
  printf("exports resolved\n\n");

  g_set_local = rtc_peer_connection_set_local_description;
  g_set_remote = rtc_peer_connection_set_remote_description;
  g_create_answer = rtc_peer_connection_create_answer;
  g_track_release = rtc_media_track_release;

  rtc_initialize();
  rtc_factory* f = NULL; rtc_factory_create(&f);

  rtc_configuration cfg = {NULL, 0};
  rtc_peer_connection_observer o1 = {on_ice1, on_state1, NULL, NULL, NULL, NULL};
  rtc_peer_connection_observer o2 = {on_ice2, on_state2, NULL, on_track2, NULL, NULL};
  rtc_peer_connection_create(f, &cfg, &o1, NULL, &g_pc1);
  rtc_peer_connection_create(f, &cfg, &o2, NULL, &g_pc2);

  rtc_media_track* audio = NULL;
  rtc_audio_track_create(f, "micA", &audio);
  rtc_peer_connection_add_track(g_pc1, audio, "stream0", NULL);

  printf("--- argument checks ---\n");
  printf("    null pc             %d (expect %d)\n",
         (int)rtc_peer_connection_get_stats(NULL, on_stats, on_stats_fail, NULL),
         RTC_ERR_INVALID_ARG);
  printf("    no callbacks        %d (expect %d)\n",
         (int)rtc_peer_connection_get_stats(g_pc1, NULL, NULL, NULL),
         RTC_ERR_INVALID_ARG);

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
  if (!g_conn1 || !g_conn2) { printf("FAILED: not connected\n"); return 1; }

  /* Let a little media flow, so the report has outbound-rtp with non-zero
   * counters rather than only the transport and codec entries. */
  Sleep(2000);

  printf("\n--- get_stats ---\n");
  rtc_status s = rtc_peer_connection_get_stats(g_pc1, on_stats, on_stats_fail, NULL);
  printf("    accepted            %d (expect 0)\n", (int)s);
  if (s != RTC_OK) return 1;
  for (int i = 0; i < 100 && !g_stats_done; i++) Sleep(50);
  if (!g_stats_done) { printf("FAILED: no report delivered\n"); return 1; }
  if (g_stats_failed) { printf("FAILED: collection reported failure\n"); return 1; }

  size_t len = strlen(g_json);
  int entries = count(g_json, "\"type\":");
  int ids = count(g_json, "\"id\":");
  int timestamps = count(g_json, "\"timestamp\":");
  printf("    length              %zu\n", len);
  printf("    is an array         %s\n", (len > 1 && g_json[0] == '[') ? "yes" : "NO");
  printf("    entries             %d\n", entries);
  printf("    with id/timestamp   %d / %d\n", ids, timestamps);
  printf("    has outbound-rtp    %s\n",
         strstr(g_json, "\"outbound-rtp\"") ? "yes" : "NO");
  printf("    has candidate-pair  %s\n",
         strstr(g_json, "\"candidate-pair\"") ? "yes" : "NO");

  /* The managed parser keys on id and reads type and timestamp off every
   * entry, so a missing one of the three is a real break, not cosmetic. */
  int ok = len > 1 && g_json[0] == '[' && entries > 0 && ids == entries &&
           timestamps == entries;

  printf("\n--- first 400 chars ---\n%.400s\n\n", g_json);

  rtc_media_track_release(audio);
  rtc_peer_connection_release(g_pc1);
  rtc_peer_connection_release(g_pc2);
  rtc_factory_release(f);
  rtc_terminate();

  printf("%s\n", ok ? "PASSED" : "FAILED: unexpected report shape");
  return ok ? 0 : 1;
}
