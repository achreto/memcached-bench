/* A simple loadbalancer-like benchmark for memcached */

#include <cstdio>
#include <cstring>
#include <getopt.h>
#include <iostream>
#include <iomanip>
#include <libmemcached-1.0/memcached.h>
#include <pthread.h>
#include <stdint.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>

#define PERIODIC_PRINT_INTERVAL 1 // seconds

struct xor_shift {
    uint64_t state;
};

static inline void xor_shift_init(struct xor_shift *st, uint64_t tid)
{
    st->state = 0xdeadbeefdeadbeef ^ tid;
}

static inline uint64_t xor_shift_next(struct xor_shift *st, uint64_t num_elements) {
    // https://en.wikipedia.org/wiki/Xorshift
    uint64_t x = st->state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    st->state = x;
    return x % num_elements;
}


////////////////////////////////////////////////////////////////////////////////////////////////////
// Option Parsing
////////////////////////////////////////////////////////////////////////////////////////////////////

#define SERVER_MAX 8
#define DEFAULT_MEMCACHED_PORT 11211

// we set 64-bit keys
#define KEY_SIZE 8
// we set 64 byte values
#define VALUE_SIZE 64

typedef unsigned int rel_time_t;

/**
 * Structure for storing items within memcached.
 */
typedef struct _stritem {
    /* Protected by LRU locks */
    struct _stritem *next;
    struct _stritem *prev;
    /* Rest are protected by an item lock */
    struct _stritem *h_next;    /* hash chain next */
    rel_time_t      time;       /* least recent access */
    rel_time_t      exptime;    /* expire time */
    int             nbytes;     /* size of data */
    unsigned short  refcount;
    uint16_t        it_flags;   /* ITEM_* above */
    uint8_t         slabs_clsid;/* which slab class we're in */
    uint8_t         nkey;       /* key length, w/terminating null and padding */
    /* this odd type prevents type-punning issues when we do
     * the little shuffle to save space when not using CAS. */
    union {
        uint64_t cas;
        char end;
    } data[];
    /* if it_flags & ITEM_CAS we have 8 bytes CAS */
    /* then null-terminated key */
    /* then " flags length\r\n" (no terminating null) */
    /* then data with terminating \r\n (no terminating null; it's binary!) */
} item;

#define ITEM_SIZE (sizeof(item) + VALUE_SIZE + KEY_SIZE + 34)


struct server_info {
    size_t num_servers;
    struct {
        bool is_unix;
        union {
            struct {
                char* path;
            } ux;
            struct {
                char* hostname;
                uint16_t port;
            } tcp;
        };
    } servers[SERVER_MAX + 1];
};

// whether to use the binary protocol
static int opt_binary = 0;
// verbose logging
static int opt_verbose = 0;
// list of servers to be used
static struct server_info opt_server_info = { 0 };
// the number of queries to be performed
static size_t opt_num_queries = 1000;
// the amount of memory to be used in MB
static size_t opt_max_mem = 16;
// the number of threads for the benchmark
static size_t opt_num_threads = 1;
// the duration of the benchmark
static size_t opt_duration = 5;

// basic options
enum memcached_options {
    OPT_SERVERS = 's',
    OPT_VERBOSE = 'v',
    OPT_NUM_QUERIES = 'n',
    OPT_MAX_MEM = 'm',
    OPT_BINARY = 'b',
    OPT_DEBUG = 'd',
    OPT_THREADS = 'c',
    OPT_DURATION = 't'
};

static void options_parse_server(const char* _server_list)
{
    char* server_list = strdup(_server_list);
    if (server_list == NULL) {
        exit(EXIT_FAILURE);
    }

    size_t num_servers = 0;
    char* server = strtok_r(server_list, ",", &server_list);
    while (server != NULL) {
        if (num_servers >= SERVER_MAX) {
            printf("Too many servers specified. Maximum %u supported", SERVER_MAX);
            break;
        }
        if (strncmp(server, "unix://", 7) == 0) {
            opt_server_info.servers[num_servers].is_unix = true;
            opt_server_info.servers[num_servers].ux.path = server + 7;
            printf("Server [%zu] unix %s\n", num_servers, opt_server_info.servers[num_servers].ux.path);
        } else if (strncmp(server, "tcp://", 6) == 0) {
            char* port;
            char* hostname = strtok_r(server + 6, ":", &port);
            opt_server_info.servers[num_servers].is_unix = false;
            opt_server_info.servers[num_servers].tcp.hostname = hostname;
            printf("port: %s %p\n", port, port);
            if (port && *port != 0) {
                opt_server_info.servers[num_servers].tcp.port = strtoul(port, NULL, 10);
            }
            if (opt_server_info.servers[num_servers].tcp.port == 0) {
                opt_server_info.servers[num_servers].tcp.port = DEFAULT_MEMCACHED_PORT;
            }

            printf("Server [%zu] tcp  %s port %d\n", num_servers, opt_server_info.servers[num_servers].tcp.hostname, opt_server_info.servers[num_servers].tcp.port);
        } else {
            printf("Invalid server specification: %s\n", server);
            continue;
        }
        server = strtok_r(server_list, ",", &server_list);
        num_servers++;
    }

    opt_server_info.num_servers = num_servers;

    if (num_servers == 0) {
        printf("No server specified: ");
        exit(EXIT_FAILURE);
    }
}

static void options_parse(int argc, char* argv[])
{
    int option_index = 0;

    static struct option long_options[] = {
        { "verbose", no_argument, &opt_verbose, OPT_VERBOSE },
        { "debug", no_argument, &opt_verbose, OPT_DEBUG },
        { "servers", required_argument, NULL, OPT_SERVERS },
        { "binary", no_argument, &opt_binary, OPT_BINARY },
        { "num-threads", required_argument, NULL, OPT_THREADS },
        { "x-benchmark-mem", required_argument, NULL, OPT_MAX_MEM },
        { "x-benchmark-num-queries", required_argument, NULL, OPT_NUM_QUERIES },
        { "x-benchmark-query-duration", required_argument, NULL, OPT_DURATION },
        { 0, 0, 0, 0 },
    };

    while (1) {
        int option_rv = getopt_long(argc, argv, "vds:n:m:c:", long_options, &option_index);
        if (option_rv == -1)
            break;
        switch (option_rv) {
        case 0:
            break;
        case OPT_BINARY:
            opt_binary = 1;
            break;
        case OPT_VERBOSE: /* --verbose or -v */
            opt_verbose = 1;
            break;
        case OPT_DEBUG: /* --debug or -d */
            opt_verbose = 1;
            break;
        case OPT_SERVERS: /* --servers or -s */
            options_parse_server(optarg);
            break;
        case OPT_NUM_QUERIES:
            opt_num_queries = strtoull(optarg, NULL, 10);
            break;
        case OPT_MAX_MEM:
            opt_max_mem = strtoull(optarg, NULL, 10);
            break;
        case OPT_THREADS:
            opt_num_threads = strtoull(optarg, NULL, 10);
            break;
        case OPT_DURATION:
            opt_duration = strtoull(optarg, NULL, 10);
            break;
        case '?':
            exit(EXIT_FAILURE);
        default:
            abort();
        }
    }
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// Benchmark Function
////////////////////////////////////////////////////////////////////////////////////////////////////

pthread_barrier_t barrier;

size_t num_queries = 0;
size_t num_missed = 0;
size_t num_errors = 0;
size_t num_populated = 0;

void* thread_main(void* arg)
{
    memcached_return_t rc;
    char* string;
    size_t string_length;

    uint64_t tid = (uint64_t)arg;

    printf("thread:%03zu started\n", tid);

    struct xor_shift rand;
    xor_shift_init(&rand, tid);

    pthread_barrier_wait(&barrier);

    // ---------------------------------------------------------------------------------------------
    // Init Phase
    // ---------------------------------------------------------------------------------------------

    // create the memcached client connection
    memcached_st** memc = (memcached_st**)calloc(opt_server_info.num_servers, sizeof(*memc));
    if (memc == NULL) {
        printf("thread.%lu Failed to allocate memory for server array\n", tid);
        exit(EXIT_FAILURE);
    }

    for (size_t i = 0; i < opt_server_info.num_servers; i++) {
        if (opt_verbose) {
            printf("thread.%lu initializing connection to server %zu\n", tid, i);
        }

        memc[i] = memcached_create(NULL);
        if (memc[i] == NULL) {
            printf("thread.%lu failed to create memcached client %zu\n", tid, i);
            exit(EXIT_FAILURE);
        }
        memcached_behavior_set(memc[i], MEMCACHED_BEHAVIOR_BINARY_PROTOCOL, (uint64_t)opt_binary);

        if (opt_server_info.servers[i].is_unix) {
            if (opt_verbose) {
                printf("thread.%lu connecting to unix://%s\n", tid, opt_server_info.servers[i].ux.path);
            }
            rc = memcached_server_add_unix_socket(memc[i], opt_server_info.servers[i].ux.path);
        } else {
            if (opt_verbose) {
                printf("thread.%lu connecting to tcp://%s:%d\n", tid, opt_server_info.servers[i].tcp.hostname, opt_server_info.servers[i].tcp.port);
            }
            rc = memcached_server_add(memc[i], opt_server_info.servers[i].tcp.hostname,
                opt_server_info.servers[i].tcp.port);
        }

        if (rc != MEMCACHED_SUCCESS) {
            std::cerr << "Failed to add server " << i << " to memcached client" << std::endl;
            printf("thread.%lu failed to add server\n", tid);
            exit(EXIT_FAILURE);
        }
    }

    bool had_failure = false;
    for (size_t i = 0; i < opt_server_info.num_servers; i++) {
        memcached_st* m = memc[i];

        string = (char*)"my data";
        const char* key = "abc";
        rc = memcached_set(m, key, strlen(key),
            string, strlen(string),
            0 /* fexpires */, 0xcafebabe /* flags */);
        switch (rc) {
        case MEMCACHED_SUCCESS:
            if (opt_verbose) {
                printf("thread.%lu connected to server %zu\n", tid, i);
            }
            break;
        case MEMCACHED_HOST_LOOKUP_FAILURE:
            printf("thread.%lu failed to connect to server %zu (hostname lookup)u\n", tid, i);
            break;
        case MEMCACHED_CONNECTION_FAILURE:
            printf("thread.%lu failed to connect to server %zu (connection failure)\n", tid, i);
            break;
        default:
            printf("thread.%lu failed to connect to server %zu (%s)\n", tid, i, memcached_strerror(m, rc) );
            break;
        }

        had_failure |= (rc != MEMCACHED_SUCCESS);

        uint32_t flags;
        string = memcached_get(m, key, strlen(key), &string_length, &flags, &rc);
        switch (rc) {
        case MEMCACHED_SUCCESS:
            if (opt_verbose) {
                printf("thread.%lu connected %zu  key %s found: %s (%x) not found\n", tid, i, key, string, flags);
            }
            free(string);
            break;
        case MEMCACHED_NOTFOUND:
            printf("thread.%lu server %zu  key %s not found\n", tid, i, key);
            break;
        default:
            printf("thread.%lu failed to get key from server %zu (%s)\n", tid, i, memcached_strerror(m, rc) );
            break;
        }

        had_failure |= (rc != MEMCACHED_SUCCESS);
    }

    if (had_failure) {
        printf("thread.%lu connection failure. Exiting.\n", tid);
        exit(EXIT_FAILURE);
    }

    // ---------------------------------------------------------------------------------------------
    // Population Phase
    // ---------------------------------------------------------------------------------------------

    size_t num_keys = (opt_max_mem << 20) / ITEM_SIZE; // how much memory it stores
    size_t num_not_added = 0;
    size_t num_existed = 0;
    size_t num_keys_added = 0;

    printf("thread:%03zu populating\n", tid);

    for (size_t i = tid; i < num_keys; i += opt_num_threads) {
        if (i % (num_keys/ 10) == 0) {
            printf("thread.%lu added %zu keys to %zu servers\n", tid, num_keys_added, opt_server_info.num_servers);
        }

        char key[KEY_SIZE + 1];
        snprintf(key, KEY_SIZE + 1, "%08x", (unsigned int)i);

        char value[VALUE_SIZE + 1];
        snprintf(value, VALUE_SIZE, "value-%016lx", i);

        memcached_st* m = memc[i % opt_server_info.num_servers];
        rc = memcached_set(m, key, KEY_SIZE, value, VALUE_SIZE,
            0 /* expires */, 0 /* flags */);
        if (memcached_failed(rc)) {
            num_not_added++;
        } else {
            num_keys_added++;
        }
    }

    __atomic_fetch_add(&num_populated, num_keys_added, __ATOMIC_RELAXED);

    printf("populate: thread.%zu done. added %zu elements, %zu not added of which %zu already existed (%zu servers)\n",
             tid, num_keys_added, num_not_added, num_existed, opt_server_info.num_servers);

    printf("thread:%03zu ready\n", tid);
    pthread_barrier_wait(&barrier);
    sleep(1);
    pthread_barrier_wait(&barrier);

    // ---------------------------------------------------------------------------------------------
    // Benchmark Phase
    // ---------------------------------------------------------------------------------------------

    printf("execute: thread.%zu startes executing\n", tid);

    size_t num_success = 0;
    size_t num_not_found = 0;
    size_t num_erroneous = 0;

    struct timeval thread_start, thread_current, thread_elapsed, thread_stop;
    thread_current.tv_usec = 0;
    thread_current.tv_sec = opt_duration;
    if (opt_duration == 0) {
        thread_current.tv_sec = 3600 * 24; // let's set a timeout to 24 hours...
    }

    size_t max_queries =  opt_num_queries;
    if (max_queries == 0) {
        max_queries = 0xffffffffffffffff;  // set it to a large number
    }

        // record the start time and calculate the end time
    gettimeofday(&thread_start, NULL);
    timeradd(&thread_start, &thread_current, &thread_stop);

    size_t thread_queries = 0;
    size_t query_counter = 0;

    do {
        if (query_counter == max_queries) {
            break;
        }

        // only check the time so often...
        if ((query_counter % 128) == 0) {
            gettimeofday(&thread_current, NULL);

            timersub(&thread_current, &thread_start, &thread_elapsed);
            if (thread_elapsed.tv_sec == PERIODIC_PRINT_INTERVAL) {

                uint64_t thread_elapsed_us = (thread_elapsed.tv_sec * 1000000) + thread_elapsed.tv_usec;
                printf("thread:%03zu executed %lu queries in %lu ms\n", tid,
                    (query_counter)-thread_queries, thread_elapsed_us / 1000);

                // reset the thread start time
                thread_start = thread_current;
                thread_queries = query_counter;
            }
        }

        query_counter++;
        uint64_t objid = xor_shift_next(&rand, num_keys);

        // format the key
        char key[KEY_SIZE + 1];
        snprintf(key, KEY_SIZE + 1, "%08x", (unsigned int)objid);

        // pick the server, doing some dummy load balance here based on the number of threads
        memcached_st* m = memc[objid % opt_server_info.num_servers];

        uint32_t flags;
        string = memcached_get(m, key, KEY_SIZE, &string_length, &flags, &rc);
        if (rc == MEMCACHED_SUCCESS) {
            if (opt_verbose) {
                printf("thread.%lu key %s = %s...\n", tid, key, string);
            }
            free(string);
            num_success++;
        } else if (rc == MEMCACHED_NOTFOUND) {
            if (opt_verbose) {
                printf("thread.%lu key %s = NOT_FOUND...\n", tid, key);
            }
            num_not_found++;
        } else {
            if (opt_verbose) {
                printf("thread.%lu key %s = ERROR (%s)...\n", tid, key, memcached_strerror(m, rc) );
            }
            num_erroneous++;
        }
    } while(timercmp(&thread_current, &thread_stop, <));


    printf("thread:%03zu done. executed %zu found %zu, missed %zu  (checksum: %lx)\n", tid, query_counter, num_success, num_not_found, num_errors);

    pthread_barrier_wait(&barrier);

    if (num_not_found > 0) {
        printf("thread.%lu had %zu keys not found\n", tid, num_not_found);
    }

    if (num_erroneous > 0) {
        printf("thread.%lu had %zu errors\n", tid, num_errors);
    }

    __atomic_fetch_add(&num_queries, query_counter, __ATOMIC_RELAXED);
    __atomic_fetch_add(&num_missed, num_not_found, __ATOMIC_RELAXED);
    __atomic_fetch_add(&num_errors, num_erroneous, __ATOMIC_RELAXED);

    for (size_t i = 0; i < opt_server_info.num_servers; i++) {
        memcached_free(memc[i]);
    }

    free(memc);

    return (void *)(num_success + num_not_found);
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// Main
////////////////////////////////////////////////////////////////////////////////////////////////////

int main(int argc, char* argv[])
{
    options_parse(argc, argv);

    if (opt_num_threads == 0) {
        opt_num_threads = 1;
    }

    if (opt_server_info.num_servers == 0) {
        printf("no servers given!\n");
        exit(1);
    }

    printf( "=====================================\n");
    printf("LOADBALANCER CONFIGURE\n");
    printf("=====================================\n");

    printf("------------------------------------------\n");
    printf(" - x_benchmark_mem = %zu MB\n", opt_max_mem);
    printf(" - x_benchmark_num_queries = %zu\n", opt_num_queries);
    printf(" - x_benchmark_query_time = %zu s\n", opt_duration);
    printf(" - num_threads = %zu\n", opt_num_threads);
    printf(" - maxbytes = %zu MB\n", opt_max_mem);
    printf("------------------------------------------\n");

    // initialize the barrier
    pthread_barrier_init(&barrier, NULL, opt_num_threads + 1);

    size_t num_items = opt_max_mem / (ITEM_SIZE);

    printf("@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@\n");
    printf("Populating %zu key-value pairs....\n", num_items);
    printf("@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@\n");

    // create the threads
    pthread_t* threads = (pthread_t*)calloc(opt_num_threads, sizeof(pthread_t));
    if (threads == NULL) {
        printf("ERROR: failed to allocate memory for threads\n");
        return EXIT_FAILURE;
    }

    // create a thread to run the queries
    for (size_t tid = 0; tid < opt_num_threads; tid++) {
        printf("starting thread %zu / %zu\n", tid, opt_num_threads);
        if (pthread_create(&threads[tid], NULL, thread_main, (void*)tid) != 0) {
            printf("ERROR: failed to create thread!\n");
            return EXIT_FAILURE;
        }
    }

    pthread_barrier_wait(&barrier);
    struct timespec t_start;
    clock_gettime(CLOCK_MONOTONIC, &t_start);
    printf("Start populating...\n");

    // ---------------------------------------------------------------------------------------------
    // Population Phase
    // ---------------------------------------------------------------------------------------------


    pthread_barrier_wait(&barrier);
    struct timespec t_end;
    clock_gettime(CLOCK_MONOTONIC, &t_end);

    struct timespec t_elapsed;
    t_elapsed.tv_sec = t_end.tv_sec - t_start.tv_sec;
    if (t_start.tv_nsec > t_end.tv_nsec) {
        t_elapsed.tv_sec--;
        t_elapsed.tv_nsec = 1000000000UL + t_end.tv_nsec - t_start.tv_nsec;
    } else {
        t_elapsed.tv_nsec = t_end.tv_nsec - t_start.tv_nsec;
    }
    uint64_t elapsed_ms = (t_elapsed.tv_sec * 1000000000UL + t_elapsed.tv_nsec) / 1000000;

    fprintf(stderr, "@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@\n");
    fprintf(stderr, "Populated %zu / %zu key-value pairs in %lu ms:\n", num_populated, num_items, elapsed_ms);
    fprintf(stderr, "@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@\n");

    fprintf(stderr, "@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@\n");
    fprintf(stderr, "Executing %zu queries with %zu threads for %zu seconds.\n", opt_num_threads * opt_num_queries, opt_num_threads, opt_duration);
    fprintf(stderr, "@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@\n");

    pthread_barrier_wait(&barrier);
    clock_gettime(CLOCK_MONOTONIC, &t_start);

    // ---------------------------------------------------------------------------------------------
    // Benchmark Phase
    // ---------------------------------------------------------------------------------------------

    pthread_barrier_wait(&barrier);
    clock_gettime(CLOCK_MONOTONIC, &t_end);


    t_elapsed.tv_sec = t_end.tv_sec - t_start.tv_sec;
    if (t_start.tv_nsec > t_end.tv_nsec) {
        t_elapsed.tv_sec--;
        t_elapsed.tv_nsec = 1000000000UL + t_end.tv_nsec - t_start.tv_nsec;
    } else {
        t_elapsed.tv_nsec = t_end.tv_nsec - t_start.tv_nsec;
    }

    // wait for all threads to finish
    for (size_t tid = 0; tid < opt_num_threads; tid++) {
        void *retval;
        pthread_join(threads[tid], &retval);
    }

    size_t num_queries_expected = opt_num_queries * opt_num_threads;

    fprintf(stderr, "@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@\n");
    fprintf(stderr, "Benchmark Done.\n");
    fprintf(stderr, "@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@\n");

    elapsed_ms = (t_elapsed.tv_sec * 1000000000UL + t_elapsed.tv_nsec) / 1000000;
    printf("===============================================================================\n");
    printf("benchmark took %lu ms (of %lu ms)\n", elapsed_ms, opt_duration * 1000);
    printf("benchmark took %lu queries / second\n", (num_queries * 1000 / elapsed_ms) );
    printf("benchmark executed %zu / %zu queries   (%zu missed) \n",  num_queries, num_queries_expected, num_missed);
    if (num_missed > 0) {
        printf("benchmark missed %zu queries!\n", num_missed);
    }
    printf("terminating.\n");
    printf("===============================================================================\n");
    printf("===============================================================================\n");

    // destroy the barrier
    pthread_barrier_destroy(&barrier);

    return EXIT_SUCCESS;
}
