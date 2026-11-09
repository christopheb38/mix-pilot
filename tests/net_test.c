/* SPDX-License-Identifier: GPL-3.0-only
 * The network path for real: Last.fm with an impossible key, from 3 threads.  make net-test */
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "../online.h"

static int results[3];

static int on_tag(const char *tag, int count, void *u) { (void)tag; (void)count; (void)u; return 0; }

static void *worker(void *arg)
{
    int i = (int)(long)arg;
    results[i] = lfm_track_tags("00000000000000000000000000000000", "La Bouche",
                                "Be My Lover (Extended Mix By Fuvi Clan)", on_tag, NULL);
    return NULL;
}

int main(void)
{
    if (net_available() != 0) {
        printf("net-test: no libcurl here, nothing to test\n");
        return 0;
    }
    pthread_t t[3];
    for (long i = 0; i < 3; i++) pthread_create(&t[i], NULL, worker, (void *)i);
    for (int i = 0; i < 3; i++) pthread_join(t[i], NULL);
    int bad = 0;
    for (int i = 0; i < 3; i++) {
        printf("  thread %d: %d (%s)\n", i, results[i],
               results[i] == LFM_BADKEY ? "key refused" : results[i] == LFM_FAIL ? "no answer" : "?");
        if (results[i] != LFM_BADKEY && results[i] != LFM_FAIL) bad = 1;
    }
    printf("  last error: %s\nnet-test: %s\n", net_last_error(), bad ? "FAILED" : "ok");
    return bad;
}
