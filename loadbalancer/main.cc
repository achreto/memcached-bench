/* A simple loadbalancer-like benchmark for memcached */

#include <cstdio>
#include <cstring>
#include <getopt.h>
#include <iostream>
#include <libmemcached-1.0/memcached.h>
#include <pthread.h>
#include <stdint.h>
#include <unistd.h>

////////////////////////////////////////////////////////////////////////////////////////////////////
// Option Parsing
////////////////////////////////////////////////////////////////////////////////////////////////////

#define SERVER_MAX 8
#define DEFAULT_MEMCACHED_PORT 11211

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
static size_t opt_num_queries = 0;
// the amount of memory to be used
static size_t opt_max_mem = 0;
// the number of threads for the benchmark
static size_t opt_num_threads = 1;

// basic options
enum memcached_options {
    OPT_SERVERS = 's',
    OPT_VERBOSE = 'v',
    OPT_NUM_QUERIES = 'n',
    OPT_MAX_MEM = 'm',
    OPT_BINARY = 'b',
    OPT_DEBUG = 'd',
    OPT_THREADS = 'c'
};

static void options_parse_server(const char* _server_list)
{
    char* server_list = strdup(_server_list);
    if (server_list == NULL) {
        std::cerr << "Could not allocate server string." << std::endl;
        exit(EXIT_FAILURE);
    }

    size_t num_servers = 0;
    char* server = strtok_r(server_list, ",", &server_list);
    while (server != NULL) {
        if (num_servers >= SERVER_MAX) {
            std::cerr << "Too many servers specified. Maximum " << SERVER_MAX << " supported." << std::endl;
            break;
        }
        if (strncmp(server, "unix://", 7) == 0) {
            opt_server_info.servers[num_servers].is_unix = true;
            opt_server_info.servers[num_servers].ux.path = server + 7;
            std::cout << "Server [" << num_servers << "] unix " << opt_server_info.servers[num_servers].ux.path << std::endl;
        } else if (strncmp(server, "tcp://", 6) == 0) {
            char* port;
            char* hostname = strtok_r(server + 6, ":", &port);
            opt_server_info.servers[num_servers].is_unix = false;
            opt_server_info.servers[num_servers].tcp.hostname = hostname;
            if (port) {
                opt_server_info.servers[num_servers].tcp.port = strtoul(port, NULL, 10);
            } else {
                opt_server_info.servers[num_servers].tcp.port = DEFAULT_MEMCACHED_PORT;
            }
            std::cout << "Server [" << num_servers << "] tcp  " << opt_server_info.servers[num_servers].tcp.hostname << " port " << opt_server_info.servers[num_servers].tcp.port << std::endl;
        } else {
            std::cerr << "Invalid server specification: " << server << std::endl;
            continue;
        }
        server = strtok_r(server_list, ",", &server_list);
        num_servers++;
    }

    opt_server_info.num_servers = num_servers;

    if (num_servers == 0) {
        std::cerr << "No server specified: " << _server_list << std::endl;
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
        { "num-queries", required_argument, NULL, OPT_NUM_QUERIES },
        { "max-memory", required_argument, NULL, OPT_MAX_MEM },
        { "num-threads", required_argument, NULL, OPT_THREADS },

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

void* thread_main(void* arg)
{
    memcached_return_t rc;
    char* string;
    size_t string_length;

    uint64_t tid = (uint64_t)arg;
    if (opt_verbose) {
        std::cout << "Thread " << tid << " started" << std::endl;
    }

    // create the memcached client connection
    memcached_st** memc = (memcached_st**)calloc(opt_server_info.num_servers, sizeof(*memc));
    if (memc == NULL) {
        std::cerr << "Failed to allocate memory for server array" << std::endl;
        exit(EXIT_FAILURE);
    }

    for (size_t i = 0; i < opt_server_info.num_servers; i++) {
        if (opt_verbose) {
            std::cout << "Thread " << tid << " initializing connection to server " << i << std::endl;
        }

        memc[i] = memcached_create(NULL);
        if (memc[i] == NULL) {
            std::cerr << "Failed to create memcachd client" << std::endl;
            exit(EXIT_FAILURE);
        }
        memcached_behavior_set(memc[i], MEMCACHED_BEHAVIOR_BINARY_PROTOCOL, (uint64_t)opt_binary);

        if (opt_server_info.servers[i].is_unix) {
            if (opt_verbose) {
                std::cout << "Thread " << tid << " connecting to unix:// " << opt_server_info.servers[i].ux.path << std::endl;
            }
            rc = memcached_server_add_unix_socket(memc[i], opt_server_info.servers[i].ux.path);
        } else {
            if (opt_verbose) {
                std::cout << "Thread " << tid << " connecting to tcp:// " << opt_server_info.servers[i].tcp.hostname << ":" << opt_server_info.servers[i].tcp.port << std::endl;
            }
            rc = memcached_server_add(memc[i], opt_server_info.servers[i].tcp.hostname,
                opt_server_info.servers[i].tcp.port);
        }

        if (rc != MEMCACHED_SUCCESS) {
            std::cerr << "Failed to add server " << i << " to memcached client" << std::endl;
            exit(EXIT_FAILURE);
        }
    }

    bool had_failure = false;
    for (size_t i = 0; i < opt_server_info.num_servers; i++) {
        memcached_st *m = memc[i];

        const char* key = "abc";
        uint32_t flags;
        string = memcached_get(m, key, strlen(key), &string_length, &flags, &rc);
        switch (rc) {
            case MEMCACHED_SUCCESS:
                if (opt_verbose) {
                    std::cout << "Connected to server " << i << ", key `" << key << "` found" << std::endl;
                }
                free(string);
                break;
            case MEMCACHED_NOTFOUND:
                std::cerr << "Connected to server" << i << ", but key `" << key << "` not found" << std::endl;
                break;
            case MEMCACHED_HOST_LOOKUP_FAILURE:
                std::cerr << "Failed to connect to server " << i << " (hostname lookup)" << std::endl;
                break;
            case MEMCACHED_CONNECTION_FAILURE:
                std::cerr << "Failed to connect to server " << i << " (connection failure)" << std::endl;
                break;
            default:
                std::cerr << "Failed to connect to server " << i << std::endl;
                std::cerr << "error (" << memcached_strerror(m, rc) << ")" << std::endl;
                break;
        }

        had_failure |= (rc != MEMCACHED_SUCCESS);
    }

    if (had_failure) {
        std::cerr << "Had connection failure. Exiting." << std::endl;
        exit(EXIT_FAILURE);
    }

    exit(0);

    // wait until all have connected to the memcached instances
    pthread_barrier_wait(&barrier);

    size_t num_sucecss = 0;
    size_t num_not_found = 0;
    size_t num_errors = 0;
    for (size_t i = 0; i < opt_num_queries; i++) {
        const char* key = "abc";
        uint32_t flags;
        string = memcached_get(m, key, strlen(key), &string_length, &flags, &rc);
        if (rc == MEMCACHED_SUCCESS) {
            if (opt_verbose) {
                std::cout << "key: " << key << " = " << string << std::endl;
            }
            free(string);
            num_sucecss++;
        } else if (rc == MEMCACHED_NOTFOUND) {
            if (opt_verbose) {
                std::cout << "key: " << key << " not found" << std::endl;
            }
            num_not_found++;
        } else {
            std::cerr << "failed to get keyerror (" << memcached_strerror(m, rc) << ")" << std::endl;
            num_errors++;
        }
    }

    if (opt_verbose) {
        std::cout << "Thread " << tid << " finished." << std::endl;
    }

    if (num_errors > 0) {
        std::cerr << "Thread " << tid << " had " << num_errors << " errors" << std::endl;
    }

    for (size_t i = 0; i < opt_server_info.num_servers; i++) {
        memcached_free(memc[i]);
    }

    free(memc);

    return NULL;
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

    // initialize the barrier
    pthread_barrier_init(&barrier, NULL, opt_num_threads + 1);

    // create the threads
    pthread_t* threads = (pthread_t*)calloc(opt_num_threads, sizeof(pthread_t));
    if (threads == NULL) {
        std::cerr << "Failed to allocate memory for threads" << std::endl;
        return EXIT_FAILURE;
    }

    // create a thread to run the queries
    for (size_t tid = 0; tid < opt_num_threads; tid++) {
        if (pthread_create(&threads[tid], NULL, thread_main, (void*)tid) != 0) {
            std::cerr << "Failed to create thread " << tid << std::endl;
            return EXIT_FAILURE;
        }
    }

    pthread_barrier_wait(&barrier);

    // wait for all threads to finish
    for (size_t tid = 0; tid < opt_num_threads; tid++) {
        void* retval;
        pthread_join(threads[tid], &retval);
    }

    // destroy the barrier
    pthread_barrier_destroy(&barrier);

    return EXIT_SUCCESS;
}
