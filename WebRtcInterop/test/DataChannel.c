/* Opens a data channel across a real handshake and sends in both directions.
 *
 * The channel is created before the offer, so it appears as an m=application
 * section rather than triggering renegotiation. Candidates are buffered and
 * flushed from main, for the reasons Handshake.c explains. */
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include "Interop.h"

typedef rtc_status(__cdecl* fv)(void);
typedef rtc_status(__cdecl* ff)(rtc_factory**);
typedef void(__cdecl* fr)(rtc_factory*);
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
typedef rtc_status(__cdecl* fdcc)(rtc_peer_connection*, const char*,
                                  const rtc_data_channel_init*, rtc_data_channel**);
typedef rtc_status(__cdecl* fdco)(rtc_data_channel*, const rtc_data_channel_observer*, void*);
typedef rtc_status(__cdecl* fdcs)(rtc_data_channel*, const uint8_t*, int32_t, int32_t);
typedef rtc_status(__cdecl* fdcl)(rtc_data_channel*, char**);
typedef rtc_status(__cdecl* fdci)(rtc_data_channel*, int32_t*);
typedef rtc_status(__cdecl* fdcst)(rtc_data_channel*, rtc_data_channel_state*);
typedef rtc_status(__cdecl* fdcb)(rtc_data_channel*, uint64_t*);
typedef rtc_status(__cdecl* fdccl)(rtc_data_channel*);
typedef void(__cdecl* fdcr)(rtc_data_channel*);
typedef void(__cdecl* fsf)(char*);

static fset g_set_local, g_set_remote;
static fsdp g_create_answer;
static fdco g_set_observer;

static rtc_peer_connection *g_pc1, *g_pc2;
static rtc_data_channel *g_dc1, *g_dc2;
static volatile LONG g_answer_done = 0, g_conn1 = 0, g_conn2 = 0;
static volatile LONG g_open1 = 0, g_open2 = 0;

/* What each side received. */
static char g_text2[256];
static volatile LONG g_text2_len = 0, g_text2_binary = 0;
static unsigned char g_bin1[256];
static volatile LONG g_bin1_len = 0, g_bin1_binary = 0;

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
  if (s == RTC_PEER_CONNECTION_STATE_CONNECTED) InterlockedExchange(&g_conn1, 1);
}
static void __cdecl on_state2(void* ud, rtc_peer_connection_state s) {
  (void)ud;
  if (s == RTC_PEER_CONNECTION_STATE_CONNECTED) InterlockedExchange(&g_conn2, 1);
}

static const char* kDc[] = {"connecting", "open", "closing", "closed"};

/* pc1's channel. */
static void __cdecl on_dc1_state(void* ud, rtc_data_channel_state s) {
  (void)ud; printf("    dc1 -> %s\n", kDc[s]);
  if (s == RTC_DATA_CHANNEL_STATE_OPEN) InterlockedExchange(&g_open1, 1);
}
static void __cdecl on_dc1_message(void* ud, const uint8_t* data, int32_t size,
                                   int32_t is_binary) {
  (void)ud;
  LONG n = size < 256 ? size : 255;
  memcpy(g_bin1, data, (size_t)n);
  InterlockedExchange(&g_bin1_binary, is_binary);
  InterlockedExchange(&g_bin1_len, n);
}

/* pc2's channel, which arrives through on_data_channel. */
static void __cdecl on_dc2_state(void* ud, rtc_data_channel_state s) {
  (void)ud; printf("    dc2 -> %s\n", kDc[s]);
  if (s == RTC_DATA_CHANNEL_STATE_OPEN) InterlockedExchange(&g_open2, 1);
}
static void __cdecl on_dc2_message(void* ud, const uint8_t* data, int32_t size,
                                   int32_t is_binary) {
  (void)ud;
  LONG n = size < 255 ? size : 254;
  memcpy(g_text2, data, (size_t)n);
  g_text2[n] = '\0';
  InterlockedExchange(&g_text2_binary, is_binary);
  InterlockedExchange(&g_text2_len, n);
}

static void __cdecl on_data_channel2(void* ud, rtc_data_channel* channel) {
  (void)ud;
  printf("    pc2 received a data channel\n");
  g_dc2 = channel; /* rule 1: ours now */

  /* Registered here, inside the callback: the open transition can follow
   * immediately and would otherwise be missed. */
  rtc_data_channel_observer o = {on_dc2_state, on_dc2_message, NULL};
  g_set_observer(channel, &o, NULL);
}

static void __cdecl on_answer(void* ud, const char* type, const char* sdp);

static void __cdecl on_offer(void* ud, const char* type, const char* sdp) {
  (void)ud;
  printf("    offer created (%zu bytes, m=application present: %s)\n",
         strlen(sdp), strstr(sdp, "m=application") ? "yes" : "NO");
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
  L(rtc_peer_connection_create, fpc);
  L(rtc_peer_connection_close, fpcc);
  L(rtc_peer_connection_release, fpcr);
  L(rtc_peer_connection_create_offer, fsdp);
  L(rtc_peer_connection_create_answer, fsdp);
  L(rtc_peer_connection_set_local_description, fset);
  L(rtc_peer_connection_set_remote_description, fset);
  L(rtc_peer_connection_add_ice_candidate, fice);
  L(rtc_peer_connection_create_data_channel, fdcc);
  L(rtc_data_channel_set_observer, fdco);
  L(rtc_data_channel_send, fdcs);
  L(rtc_data_channel_get_label, fdcl);
  L(rtc_data_channel_get_id, fdci);
  L(rtc_data_channel_get_state, fdcst);
  L(rtc_data_channel_get_buffered_amount, fdcb);
  L(rtc_data_channel_close, fdccl);
  L(rtc_data_channel_release, fdcr);
  L(rtc_string_free, fsf);
  if (!rtc_data_channel_send) { printf("missing export\n"); return 1; }
  printf("exports resolved\n\n");

  g_set_local = rtc_peer_connection_set_local_description;
  g_set_remote = rtc_peer_connection_set_remote_description;
  g_create_answer = rtc_peer_connection_create_answer;
  g_set_observer = rtc_data_channel_set_observer;

  rtc_initialize();
  rtc_factory* f = NULL; rtc_factory_create(&f);

  rtc_configuration cfg = {NULL, 0};
  rtc_peer_connection_observer o1 = {on_ice1, on_state1, NULL, NULL, NULL, NULL};
  rtc_peer_connection_observer o2 = {on_ice2, on_state2, NULL, NULL, NULL,
                                     on_data_channel2};
  rtc_peer_connection_create(f, &cfg, &o1, NULL, &g_pc1);
  rtc_peer_connection_create(f, &cfg, &o2, NULL, &g_pc2);

  printf("--- create ---\n");
  rtc_data_channel_init init = {"chat", 1, -1, -1, 0, -1};
  rtc_status s = rtc_peer_connection_create_data_channel(g_pc1, "hello", &init, &g_dc1);
  printf("    create_data_channel %d\n", (int)s);
  if (s != RTC_OK) return 1;

  rtc_data_channel_observer dco1 = {on_dc1_state, on_dc1_message, NULL};
  printf("    set_observer        %d\n", (int)g_set_observer(g_dc1, &dco1, NULL));

  char* label = NULL;
  rtc_data_channel_get_label(g_dc1, &label);
  printf("    label               '%s'\n", label ? label : "(null)");
  int label_ok = label && strcmp(label, "hello") == 0;
  if (label) rtc_string_free(label);

  rtc_data_channel_state st = -1;
  rtc_data_channel_get_state(g_dc1, &st);
  printf("    state before open   %s\n", kDc[st]);

  printf("    send before open    %d (expect -2 INVALID_STATE)\n",
         (int)rtc_data_channel_send(g_dc1, (const uint8_t*)"x", 1, 0));

  printf("\n--- negotiation ---\n");
  rtc_peer_connection_create_offer(g_pc1, on_offer, on_fail, NULL);
  for (int i = 0; i < 100 && !g_answer_done; i++) Sleep(50);
  if (!g_answer_done) { printf("\nFAILED: no answer in 5s\n"); return 1; }

  Sleep(1500);
  printf("\n--- ICE exchange (%ld from pc1, %ld from pc2) ---\n", g_n1, g_n2);
  int bad = 0;
  for (LONG i = 0; i < g_n1 && i < 64; i++)
    if (rtc_peer_connection_add_ice_candidate(g_pc2, g_from1[i].mid,
                                              g_from1[i].idx, g_from1[i].sdp)) bad++;
  for (LONG i = 0; i < g_n2 && i < 64; i++)
    if (rtc_peer_connection_add_ice_candidate(g_pc1, g_from2[i].mid,
                                              g_from2[i].idx, g_from2[i].sdp)) bad++;

  for (int i = 0; i < 300 && !(g_open1 && g_open2); i++) Sleep(50);
  printf("\n    dc1 open: %s    dc2 open: %s\n",
         g_open1 ? "YES" : "no", g_open2 ? "YES" : "no");
  if (!g_open1 || !g_open2) { printf("\nFAILED: channel never opened\n"); return 1; }

  int32_t id1 = -99, id2 = -99;
  rtc_data_channel_get_id(g_dc1, &id1);
  rtc_data_channel_get_id(g_dc2, &id2);
  printf("    ids                 dc1=%d dc2=%d (must match)\n", id1, id2);

  printf("\n--- messages ---\n");
  const char* text = "hello from pc1";
  printf("    pc1 send text       %d\n",
         (int)rtc_data_channel_send(g_dc1, (const uint8_t*)text,
                                    (int32_t)strlen(text), 0));

  const unsigned char bytes[] = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x42};
  printf("    pc2 send binary     %d\n",
         (int)rtc_data_channel_send(g_dc2, bytes, (int32_t)sizeof(bytes), 1));

  for (int i = 0; i < 100 && !(g_text2_len && g_bin1_len); i++) Sleep(50);

  printf("\n    pc2 got '%s' (binary flag %ld, expect 0)\n", g_text2, g_text2_binary);
  printf("    pc1 got %ld bytes (binary flag %ld, expect 1): ", g_bin1_len, g_bin1_binary);
  for (LONG i = 0; i < g_bin1_len; i++) printf("%02X ", g_bin1[i]);
  printf("\n");

  int text_ok = strcmp(g_text2, text) == 0 && g_text2_binary == 0;
  int bin_ok = g_bin1_len == (LONG)sizeof(bytes) && g_bin1_binary == 1 &&
               memcmp(g_bin1, bytes, sizeof(bytes)) == 0;

  uint64_t buffered = 999;
  rtc_data_channel_get_buffered_amount(g_dc1, &buffered);
  printf("    buffered amount     %llu\n", (unsigned long long)buffered);

  printf("\n--- error paths ---\n");
  printf("    send(NULL channel)  %d (expect -1)\n",
         (int)rtc_data_channel_send(NULL, (const uint8_t*)"x", 1, 0));
  printf("    send(negative size) %d (expect -1)\n",
         (int)rtc_data_channel_send(g_dc1, (const uint8_t*)"x", -1, 0));
  printf("    send(NULL data)     %d (expect -1)\n",
         (int)rtc_data_channel_send(g_dc1, NULL, 4, 0));
  printf("    get_state(NULL out) %d (expect -1)\n",
         (int)rtc_data_channel_get_state(g_dc1, NULL));
  printf("    empty payload       %d (expect 0)\n",
         (int)rtc_data_channel_send(g_dc1, NULL, 0, 0));

  printf("\n--- teardown ---\n");
  printf("    close               %d\n", (int)rtc_data_channel_close(g_dc1));
  Sleep(500);
  rtc_data_channel_get_state(g_dc1, &st);
  printf("    state after close   %s\n", kDc[st]);

  /* Release with an observer still registered: the destructor must unregister
   * it, because the peer connection still holds a reference to the channel. */
  rtc_data_channel_release(g_dc1);
  rtc_data_channel_release(g_dc2);
  printf("    released with a live observer  no crash\n");

  rtc_peer_connection_close(g_pc1); rtc_peer_connection_close(g_pc2);
  rtc_peer_connection_release(g_pc1); rtc_peer_connection_release(g_pc2);
  rtc_factory_release(f); rtc_terminate();

  int pass = label_ok && text_ok && bin_ok && id1 == id2 && !bad;
  printf("\n%s\n", pass ? "DATA CHANNEL PASSED" : "FAILED");
  return pass ? 0 : 1;
}
