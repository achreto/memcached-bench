#!/bin/bash

NUM_QUERIES=100000
NUM_THREADS=4
MEMORY=64

MEMCACHED_OPTIONS="--x-benchmark-queries=${NUM_QUERIES} --x-benchmark-mem=${MEMORY}"
LOAD_BALANCER_OPTIONS="--binary --num-queries=${NUM_QUERIES} --num-threads ${NUM_THREADS} --max-memory=${MEMORY}"


killall memcached

# internal benchnmark
echo "benchmark took internal"
taskset  --cpu-list 0-3 ./build/bin/memcached $MEMCACHED_OPTIONS

# load balancer benchmark
echo "benchmark took 1x tcp"
bash scripts/spawn-memcached-process.sh 0 tcp
sleep 2
./loadbalancer/loadbalancer $LOAD_BALANCER_OPTIONS  --server=tcp://localhost
killall memcached

echo "benchmark took 1x unix"
bash scripts/spawn-memcached-process.sh 0 unix
sleep 2
./loadbalancer/loadbalancer $LOAD_BALANCER_OPTIONS --server=unix://memcached0.sock
killall memcached

echo "benchmark took 2x tcp"
bash scripts/spawn-memcached-process.sh 0 tcp
bash scripts/spawn-memcached-process.sh 1 tcp
sleep 2
./loadbalancer/loadbalancer $LOAD_BALANCER_OPTIONS --server=tcp://localhost:11211,tcp://localhost:11212
killall memcached

echo "benchmark took 2x unix"
bash scripts/spawn-memcached-process.sh 0 unix
bash scripts/spawn-memcached-process.sh 1 unix
sleep 2
./loadbalancer/loadbalancer $LOAD_BALANCER_OPTIONS --server=unix://memcached0.sock,unix://memcached1.sock
killall memcached

echo "benchmark took 4x tcp"
bash scripts/spawn-memcached-process.sh 0 tcp
bash scripts/spawn-memcached-process.sh 1 tcp
bash scripts/spawn-memcached-process.sh 2 tcp
bash scripts/spawn-memcached-process.sh 3 tcp
sleep 2
./loadbalancer/loadbalancer $LOAD_BALANCER_OPTIONS --server=tcp://localhost:11211,tcp://localhost:11212,tcp://localhost:11213,tcp://localhost:11214
killall memcached


echo "benchmark 4x unix"
bash scripts/spawn-memcached-process.sh 0 unix
bash scripts/spawn-memcached-process.sh 1 unix
bash scripts/spawn-memcached-process.sh 2 unix
bash scripts/spawn-memcached-process.sh 3 unix
sleep 2
./loadbalancer/loadbalancer $LOAD_BALANCER_OPTIONS --server=unix://memcached0.sock,unix://memcached1.sock,unix://memcached2.sock,unix://memcached3.sock
killall memcached