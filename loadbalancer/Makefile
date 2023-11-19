# flags
LIB_MEMCACHED=../build/lib
LIB_MEMCACHED_INCLUDE=../build/include

CXXFLAGS += \
	-I$(LIB_MEMCACHED_INCLUDE) \
	-O2 \

LIBS += \
	-L$(LIB_MEMCACHED) \
	-lmemcached \
	-lmemcachedutil \
	-lpthread


loadbalancer: main.cc
	g++ $(CXXFLAGS) -o loadbalancer main.cc $(LIBS)

