/* Unit test of the per-token session counter of patches/0008 (janus.c, between the
 * "qjanus:token-sessions begin/end" markers). The block is compiled AS IT IS in the patched Janus source:
 *
 *   sed -n '/qjanus:token-sessions begin/,/qjanus:token-sessions end/p' $SRC/janus/src/janus.c > block.inc
 *   cc -std=gnu11 -Wall -Wextra -Werror -fsanitize=address,undefined -I$SRC/janus/src \
 *      -DTOKEN_SESSIONS_BLOCK='"block.inc"' test_token_sessions.c $(pkg-config --cflags --libs glib-2.0) -o t
 *
 * Under AddressSanitizer + LeakSanitizer (a key that the table does not take over is a leak: the churn loops
 * below would show it), UndefinedBehaviorSanitizer, and again under ThreadSanitizer (-fsanitize=thread). */
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mutex.h"

int lock_debug = 0;
/* the lock-debug printer of mutex.h (never reached with lock_debug = 0, but referenced) */
void janus_vprintf(const char *format, ...) { (void)format; }

#include TOKEN_SESSIONS_BLOCK

static int failures = 0;
#define CHECK(cond, ...) do { if(!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); failures++; } } while(0)

static guint count_of(const char *token) {
	janus_mutex_lock(&token_sessions_mutex);
	guint n = GPOINTER_TO_UINT(g_hash_table_lookup(token_sessions, token));
	janus_mutex_unlock(&token_sessions_mutex);
	return n;
}
static guint tokens_tracked(void) {
	janus_mutex_lock(&token_sessions_mutex);
	guint n = g_hash_table_size(token_sessions);
	janus_mutex_unlock(&token_sessions_mutex);
	return n;
}

static void test_basic(void) {
	/* before the table exists nothing is counted and nothing crashes (the first create path never runs without init) */
	CHECK(!janus_token_sessions_acquire("t"), "acquire before init must not succeed");
	janus_token_sessions_release("t");
	janus_token_sessions_init();
	janus_token_sessions_init();   /* idempotent */
	CHECK(token_sessions != NULL && tokens_tracked() == 0, "empty after init");

	CHECK(max_sessions_per_token == 4, "default 4, got %u", max_sessions_per_token);
	for(guint i = 0; i < max_sessions_per_token; i++)
		CHECK(janus_token_sessions_acquire("tok-a"), "acquire %u", i);
	CHECK(count_of("tok-a") == 4, "count 4");
	/* refusals neither count nor free */
	for(int i = 0; i < 5; i++)
		CHECK(!janus_token_sessions_acquire("tok-a"), "N+1 refused");
	CHECK(count_of("tok-a") == 4, "refusals did not count");
	/* a different string is its own counter */
	CHECK(janus_token_sessions_acquire("tok-b"), "other token independent");
	/* a string that differs only by a space is another token: it has its own quota although tok-a is full */
	CHECK(janus_token_sessions_acquire("tok-a "), "tok-a<space> is not tok-a");
	janus_token_sessions_release("tok-a ");
	/* release frees exactly one */
	janus_token_sessions_release("tok-a");
	CHECK(count_of("tok-a") == 3, "one slot freed");
	CHECK(janus_token_sessions_acquire("tok-a"), "the freed slot is usable");
	CHECK(!janus_token_sessions_acquire("tok-a"), "and only one");
	for(int i = 0; i < 4; i++)
		janus_token_sessions_release("tok-a");
	CHECK(count_of("tok-a") == 0, "all slots free");
	/* the entry is removed at zero (no growth) */
	janus_mutex_lock(&token_sessions_mutex);
	CHECK(!g_hash_table_contains(token_sessions, "tok-a"), "entry removed at zero");
	janus_mutex_unlock(&token_sessions_mutex);
	/* underflow: releasing what was never acquired changes nothing, then the quota is intact */
	for(int i = 0; i < 10; i++)
		janus_token_sessions_release("tok-a");
	janus_token_sessions_release("never-seen");
	CHECK(count_of("tok-a") == 0, "no negative count");
	for(guint i = 0; i < max_sessions_per_token; i++)
		CHECK(janus_token_sessions_acquire("tok-a"), "full quota after the spurious releases");
	CHECK(!janus_token_sessions_acquire("tok-a"), "and not more");
	for(guint i = 0; i < max_sessions_per_token; i++)
		janus_token_sessions_release("tok-a");
	janus_token_sessions_release("tok-b");
	CHECK(tokens_tracked() == 0, "empty again, %u tracked", tokens_tracked());
}

static void test_limits(void) {
	/* other values of N (the config accepts 1..1024) */
	guint saved = max_sessions_per_token;
	for(guint n = 1; n <= MAX_MAX_SESSIONS_PER_TOKEN; n = n < 8 ? n + 1 : n * 2) {
		max_sessions_per_token = n;
		for(guint i = 0; i < n; i++)
			CHECK(janus_token_sessions_acquire("lim"), "N=%u acquire %u", n, i);
		CHECK(!janus_token_sessions_acquire("lim"), "N=%u: N+1 refused", n);
		for(guint i = 0; i < n; i++)
			janus_token_sessions_release("lim");
		CHECK(tokens_tracked() == 0, "N=%u: nothing left", n);
	}
	max_sessions_per_token = saved;
}

/* many distinct tokens, each opened and closed several times: the table must come back to empty, and the
 * duplicated keys of the "already there" paths must not leak (LeakSanitizer) */
static void test_churn(void) {
	char t[64];
	for(int round = 0; round < 3; round++) {
		for(int i = 0; i < 20000; i++) {
			snprintf(t, sizeof(t), "1793445000,janus,janus.plugin.videoroom,n.%012x:sig%d", i, i % 7);
			for(guint k = 0; k < max_sessions_per_token; k++)
				if(!janus_token_sessions_acquire(t)) { CHECK(0, "churn acquire"); }
			if(janus_token_sessions_acquire(t)) { CHECK(0, "churn over limit accepted"); }
		}
		CHECK(tokens_tracked() == 20000, "20000 tokens tracked, got %u", tokens_tracked());
		for(int i = 0; i < 20000; i++) {
			snprintf(t, sizeof(t), "1793445000,janus,janus.plugin.videoroom,n.%012x:sig%d", i, i % 7);
			for(guint k = 0; k < max_sessions_per_token; k++)
				janus_token_sessions_release(t);
		}
		CHECK(tokens_tracked() == 0, "table empty after the round, got %u", tokens_tracked());
	}
}

#define THREADS 8
#define TOKENS 16
#define ITERS 60000
static const char *tokname[TOKENS] = { "k0","k1","k2","k3","k4","k5","k6","k7","k8","k9","k10","k11","k12","k13","k14","k15" };
static gpointer worker(gpointer data) {
	guint seed = GPOINTER_TO_UINT(data) * 2654435761u + 1;
	guint held[TOKENS] = { 0 };
	for(int i = 0; i < ITERS; i++) {
		seed = seed * 1103515245u + 12345u;
		int k = (seed >> 16) % TOKENS;
		if((seed >> 8) & 1) {
			if(janus_token_sessions_acquire(tokname[k]))
				held[k]++;
		} else if(held[k] > 0) {
			janus_token_sessions_release(tokname[k]);
			held[k]--;
		}
		if((i & 1023) == 0) {
			guint n = count_of(tokname[k]);
			if(n > max_sessions_per_token) { printf("FAIL: %s count %u over the limit\n", tokname[k], n); failures++; }
		}
	}
	for(int k = 0; k < TOKENS; k++)
		while(held[k]-- > 0)
			janus_token_sessions_release(tokname[k]);
	return NULL;
}
static void test_threads(void) {
	GThread *th[THREADS];
	for(int i = 0; i < THREADS; i++)
		th[i] = g_thread_new("w", worker, GUINT_TO_POINTER(i + 1));
	for(int i = 0; i < THREADS; i++)
		g_thread_join(th[i]);
	CHECK(tokens_tracked() == 0, "after the concurrent run every slot is back, %u tracked", tokens_tracked());
}

int main(void) {
	test_basic();
	test_limits();
	test_churn();
	test_threads();
	g_hash_table_destroy(token_sessions);
	token_sessions = NULL;
	if(failures) { printf("%d failure(s)\n", failures); return 1; }
	printf("token-sessions: ok\n");
	return 0;
}
