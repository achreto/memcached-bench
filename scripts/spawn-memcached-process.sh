#! /bin/bash

# COMMON SETTINGS

function usage() {
    echo "usage: $0 <ID> <tcp|tcp://host:port|unix://path> <memory-limit> <threads>"
}

ID=$1
PROTOCOL=$2

####################################################################################################
# Checking the ID
####################################################################################################

# getting the ID
case $ID in
  0|1|2|3|4|5|6|7)
    echo "preparing memcached instance $ID"
    ;;
  *)
    echo "unknown ID"
    usage
    exit 1
    ;;
esac

####################################################################################################
# Checking the protocol
####################################################################################################

# getting the PROTOCOL
case $PROTOCOL in
  tcp)
    echo "with protocol $PROTOCOL"
    ;;
  tcp://localhost*)
    echo "with protocol $PROTOCOL"
    URL=${PROTOCOL#$"tcp://"}; # remove the protocol prefix
    PORT=${URL#*:}; # we're just interested in the port
    re='^[0-9]+$'
    if ! [[ $PORT =~ $re ]] ; then
        echo "Supplied port is not a number."
        usage
        exit 1
    fi
    ;;
  unix://*)
    echo "with protocol $PROTOCOL"
    SOCK_PATH=${PROTOCOL#$"unix://"}; # remove the protocol prefix
     if [[ -f $SOCK_PATH ]] ; then
        echo "Socket file $SOCK_PATH already exists."
        usage
        exit 1
     fi
    ;;
  *)
    echo "unknown protocol $PROTOCOL"
    usage
    exit 1
    ;;
esac

####################################################################################################
# Obtain the memory limit
####################################################################################################

if [ "$3" == "" ]; then
    MEMORY_LIMIT=64
else
    re='^[0-9]+$'
    if ! [[ $3 =~ $re ]] ; then
        echo "Supplied memory limit is not a number."
        usage
        exit 1
    fi
    MEMORY_LIMIT=$3
fi

####################################################################################################
# Number of threads
####################################################################################################

if [ "$4" == "" ]; then
    THREADS=4
else
    re='^[0-9]+$'
    if ! [[ $4 =~ $re ]] ; then
        echo "Supplied number of threads is not a number."
        usage
        exit 1
    fi
    THREADS=$4
fi

####################################################################################################
# Default Memcached Options
####################################################################################################

# the default port of memcached
DEFAULT_PORT=11211

MEMCACHED_OPTIONS="--x-benchmark-no-run"

# -M, --disable-evictions   return error on memory exhausted instead of evicting
MEMCACHED_OPTIONS+=" --disable-evictions"

# -c, --conn-limit=<num>    max simultaneous connections (default: 1024)
MEMCACHED_OPTIONS+=" --conn-limit=1024"

# -m, --memory-limit=<num>  item memory in megabytes (default: 64)
MEMCACHED_OPTIONS+=" --memory-limit=${MEMORY_LIMIT} --x-benchmark-mem=${MEMORY_LIMIT}"

# -k, --lock-memory         lock down all paged memory
# MEMCACHED_OPTIONS+=" --lock-memory"

# -t, --threads=<num>       number of threads to use (default: 4)
MEMCACHED_OPTIONS+=" --threads=${THREADS} "

####################################################################################################
# Configuring the communication protocol
####################################################################################################

case $PROTOCOL in
  tcp)
    # -p, --port=<num>          TCP port to listen on (default: 11211)
    PORT=$((DEFAULT_PORT + ID))
    MEMCACHED_OPTIONS+=" --port=${PORT}"
    ;;
  tcp://localhost*)
    URL=${PROTOCOL#$"tcp://"}; # remove the protocol prefix
    PORT=${URL#*:}; # we're just interested in the port
    # -p, --port=<num>          TCP port to listen on (default: 11211)
    MEMCACHED_OPTIONS+=" --port=${PORT}"
    ;;
  unix://*)
    SOCK_PATH=${PROTOCOL#"unix://"}; # remove the protocol prefix
    # -s, --unix-socket=<file>  UNIX socket to listen on (disables network support)
    MEMCACHED_OPTIONS+=" --unix-socket=${SOCK_PATH}"
    ;;
  *)
    echo "unknown protocol"
    usage
    exit 1
    ;;
esac


# -f, --slab-growth-factor=<num> chunk size growth factor (default: 1.25)
# -n, --slab-min-size=<bytes> min space used for key+value+flags (default: 48)
# -B, --protocol=<name>     protocol - one of ascii, binary, or auto (default: auto-negotiate)
# -I, --max-item-size=<num> adjusts max item size
#                           (default: 1m, min: 1k, max: 1024m)

# -o, --extended            comma separated list of extended options
#                           most options have a 'no_' prefix to disable
#    - hashpower:           an integer multiplier for how large the hash
#                           table should be. normally grows at runtime. (default starts at: 0)
#                           set based on "STAT hash_power_level"


####################################################################################################
# Spawn it!
####################################################################################################

# get the NUMA nodes
# available: 1 nodes (0)
NUMA_NODES=$(numactl --hardware | grep available | cut -d ':' -f 2 | cut -d ' ' -f2)
TARGET_NODE=$((ID % NUMA_NODES))

# restrict the memcached process to run on the target node only
echo "numactl -m ${TARGET_NODE} -N ${TARGET_NODE} -- ./build/bin/memcached ${MEMCACHED_OPTIONS}"
numactl -m ${TARGET_NODE} -N ${TARGET_NODE} -- ./build/bin/memcached ${MEMCACHED_OPTIONS}