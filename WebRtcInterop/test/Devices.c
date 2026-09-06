/* Enumerates every device kind the shim exposes and checks the error paths. */
#include <stdio.h>
#include <windows.h>
#include "Interop.h"

typedef rtc_status(__cdecl* fv)(void);
typedef rtc_status(__cdecl* ff)(rtc_factory**);
typedef void(__cdecl* fr)(rtc_factory*);
typedef rtc_status(__cdecl* fvc)(rtc_factory*, int32_t*);
typedef rtc_status(__cdecl* fvi)(rtc_factory*, int32_t, char**, char**);
typedef rtc_status(__cdecl* fac)(rtc_factory*, rtc_audio_device_kind, int32_t*);
typedef rtc_status(__cdecl* fai)(rtc_factory*, rtc_audio_device_kind, int32_t,
                                 char**, char**);
typedef void(__cdecl* fsf)(char*);

#define L(n, t) t n = (t)GetProcAddress(h, #n)

int main(void) {
  HMODULE h = LoadLibraryA("WebRtcInterop.dll");
  if (!h) { printf("load failed\n"); return 1; }
  L(rtc_initialize, fv); L(rtc_terminate, fv);
  L(rtc_factory_create, ff); L(rtc_factory_release, fr);
  L(rtc_video_device_count, fvc); L(rtc_video_device_info, fvi);
  L(rtc_audio_device_count, fac); L(rtc_audio_device_info, fai);
  L(rtc_string_free, fsf);
  if (!rtc_audio_device_info) { printf("missing export\n"); return 1; }

  rtc_initialize();
  rtc_factory* f = NULL;
  printf("factory_create %d\n\n", (int)rtc_factory_create(&f));

  int32_t n = 0, total = 0;
  char *name = NULL, *id = NULL;

  printf("video input\n");
  rtc_video_device_count(f, &n); total += n;
  for (int32_t i = 0; i < n; i++)
    if (rtc_video_device_info(f, i, &name, &id) == RTC_OK) {
      printf("  [%d] %s\n", (int)i, name);
      rtc_string_free(name); rtc_string_free(id);
    }
  if (!n) printf("  (none)\n");

  const rtc_audio_device_kind kinds[] = {RTC_AUDIO_DEVICE_RECORDING,
                                         RTC_AUDIO_DEVICE_PLAYOUT};
  const char* labels[] = {"audio input (microphones)", "audio output (speakers)"};
  for (int k = 0; k < 2; k++) {
    printf("\n%s\n", labels[k]);
    rtc_status s = rtc_audio_device_count(f, kinds[k], &n);
    if (s != RTC_OK) { printf("  count failed: %d\n", (int)s); continue; }
    total += n;
    for (int32_t i = 0; i < n; i++) {
      s = rtc_audio_device_info(f, kinds[k], i, &name, &id);
      if (s == RTC_OK) {
        printf("  [%d] %s\n      id=%.60s\n", (int)i, name, id);
        rtc_string_free(name); rtc_string_free(id);
      } else printf("  [%d] failed %d\n", (int)i, (int)s);
    }
    if (!n) printf("  (none)\n");
  }

  printf("\nerror paths\n");
  printf("  count(bad kind)      %d (expect -1)\n",
         (int)rtc_audio_device_count(f, 99, &n));
  printf("  count(NULL out)      %d (expect -1)\n",
         (int)rtc_audio_device_count(f, RTC_AUDIO_DEVICE_RECORDING, NULL));
  printf("  info(index 999)      %d (expect -3)\n",
         (int)rtc_audio_device_info(f, RTC_AUDIO_DEVICE_RECORDING, 999, &name, &id));
  printf("  info(negative)       %d (expect -1)\n",
         (int)rtc_audio_device_info(f, RTC_AUDIO_DEVICE_PLAYOUT, -1, &name, &id));

  rtc_factory_release(f);
  rtc_terminate();
  printf("\n%s\n", total > 0 ? "DEVICES PASSED" : "FAILED: nothing enumerated");
  return total > 0 ? 0 : 1;
}
