/* -*- mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
// vim: expandtab:ts=8:sw=4:softtabstop=4:
#ident "$Id$"
#ident "Copyright (c) 2007, 2008 Tokutek Inc.  All rights reserved."

#include "includes.h"
#include "test.h"

// this mutex is used by some of the tests to serialize access to some
// global data, especially between the test thread and the cachetable
// writeback threads

toku_mutex_t  test_mutex;

static inline void test_mutex_init(void) {
    toku_mutex_init(&test_mutex, 0);
}

static inline void test_mutex_destroy(void) {
    toku_mutex_destroy(&test_mutex);
}

static inline void test_mutex_lock(void) {
    toku_mutex_lock(&test_mutex);
}

static inline void test_mutex_unlock(void) {
    toku_mutex_unlock(&test_mutex);
}

static void maybe_flush(CACHETABLE t) {
    toku_cachetable_maybe_flush_some(t);
}

enum { KEYLIMIT = 4, TRIALLIMIT=256000 };
static CACHEKEY  keys[KEYLIMIT];
static void*     vals[KEYLIMIT];
static int       n_keys=0;

static void r_flush (CACHEFILE f      __attribute__((__unused__)),
                     int UU(fd),
		     CACHEKEY k,
		     void *value,
		     void** UU(dd),
		     void *extra      __attribute__((__unused__)),
		     PAIR_ATTR size        __attribute__((__unused__)),
        PAIR_ATTR* new_size      __attribute__((__unused__)),
		     BOOL write_me    __attribute__((__unused__)),
		     BOOL keep_me,
		     BOOL for_checkpoint    __attribute__((__unused__)),
        BOOL UU(is_clone)
		     ) {
    int i;
    //printf("Flush\n");
    if (keep_me) return;

    test_mutex_lock();
    for (i=0; i<n_keys; i++) {
	if (keys[i].b==k.b) {
	    assert(vals[i]==value);
	    if (!keep_me) {
                if (verbose) printf("%s: %d/%d %" PRIx64 "\n", __FUNCTION__, i, n_keys, k.b);
		keys[i]=keys[n_keys-1];
		vals[i]=vals[n_keys-1];
		n_keys--;
                test_mutex_unlock();
		return;
	    }
	}
    }
    fprintf(stderr, "Whoops\n");
    abort();
    test_mutex_unlock();
}

static int r_fetch (CACHEFILE f        __attribute__((__unused__)),
                    int UU(fd),
		    CACHEKEY key       __attribute__((__unused__)),
		    u_int32_t fullhash __attribute__((__unused__)),
		    void**value        __attribute__((__unused__)),
		    void** UU(dd),
		    PAIR_ATTR *sizep        __attribute__((__unused__)),
		    int  *dirtyp       __attribute__((__unused__)),
		    void*extraargs     __attribute__((__unused__))) {
    // fprintf(stderr, "Whoops, this should never be called");
    return -42;
}

static void test_rename (void) {
    CACHETABLE t;
    CACHEFILE f;
    int i;
    int r;
    test_mutex_init();
    const char fname[] = __SRCFILE__ "rename.dat";
    r=toku_create_cachetable(&t, KEYLIMIT, ZERO_LSN, NULL_LOGGER); assert(r==0);
    unlink(fname);
    r = toku_cachetable_openf(&f, t, fname, O_RDWR|O_CREAT, S_IRWXU|S_IRWXG|S_IRWXO);
    assert(r==0);
    for (i=0; i<TRIALLIMIT; i++) {
	int ra = random()%3;
	if (ra<=1) {
	    // Insert something
	    CACHEKEY nkey = make_blocknum(random());
	    long     nval = random();
	    if (verbose) printf("n_keys=%d Insert %08" PRIx64 "\n", n_keys, nkey.b);
	    u_int32_t hnkey = toku_cachetable_hash(f, nkey);
            CACHETABLE_WRITE_CALLBACK wc = def_write_callback(NULL);
            wc.flush_callback = r_flush;
	    r = toku_cachetable_put(f, nkey, hnkey,
				    (void*)nval, make_pair_attr(1),
				    wc);
	    assert(r==0);
            test_mutex_lock();
            while (n_keys >= KEYLIMIT) {
                test_mutex_unlock();
                toku_pthread_yield(); maybe_flush(t);
                test_mutex_lock();
            }
	    assert(n_keys<KEYLIMIT);
	    keys[n_keys] = nkey;
	    vals[n_keys] = (void*)nval;
	    n_keys++;
            test_mutex_unlock();
	    r = toku_cachetable_unpin(f, nkey, hnkey, CACHETABLE_DIRTY, make_pair_attr(1));
	    assert(r==0);
	} else if (ra==2 && n_keys>0) {
	    // Rename something
	    int objnum = random()%n_keys;
	    CACHEKEY nkey = make_blocknum(random());
            test_mutex_lock();
	    CACHEKEY okey = keys[objnum];
            test_mutex_unlock();
	    void *current_value;
	    long current_size;
	    if (verbose) printf("Rename %" PRIx64 " to %" PRIx64 "\n", okey.b, nkey.b);
            CACHETABLE_WRITE_CALLBACK wc = def_write_callback(NULL);
            wc.flush_callback = r_flush;
	    r = toku_cachetable_get_and_pin(f, okey, toku_cachetable_hash(f, okey), &current_value, &current_size, wc, r_fetch, def_pf_req_callback, def_pf_callback, TRUE, 0);
	    if (r == -42) continue;
            assert(r==0);
	    r = toku_cachetable_rename(f, okey, nkey);
	    assert(r==0);
            test_mutex_lock();
            // assert(objnum < n_keys && keys[objnum] == okey);
            // get_and_pin may reorganize the keys[], so we need to find it again
            int j;
            for (j=0; j < n_keys; j++)
                if (keys[j].b == okey.b)
                    break;
            assert(j < n_keys);
	    keys[j]=nkey;
            test_mutex_unlock();
	    r = toku_cachetable_unpin(f, nkey, toku_cachetable_hash(f, nkey), CACHETABLE_DIRTY, make_pair_attr(1));
	}
    }

    // test rename fails if old key does not exist in the cachetable
    CACHEKEY okey, nkey;
    while (1) {
        okey = make_blocknum(random());
        void *v;
        r = toku_cachetable_maybe_get_and_pin(f, okey, toku_cachetable_hash(f, okey), &v);
        if (r != 0)
            break;
        r = toku_cachetable_unpin(f, okey, toku_cachetable_hash(f, okey), CACHETABLE_CLEAN, make_pair_attr(1));
        assert(r == 0);
    }
    nkey = make_blocknum(random());
    r = toku_cachetable_rename(f, okey, nkey);
    assert(r != 0);

    r = toku_cachefile_close(&f, 0, FALSE, ZERO_LSN);
    assert(r == 0);
    r = toku_cachetable_close(&t);
    assert(r == 0);
    test_mutex_destroy();
    assert(n_keys == 0);
}

int
test_main (int argc, const char *argv[]) {
    // parse args
    default_parse_args(argc, argv);
    toku_os_initialize_settings(verbose);

    // run tests
    int i;
    for (i=0; i<1; i++) 
        test_rename();
    return 0;
}