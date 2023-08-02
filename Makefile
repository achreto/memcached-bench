# the build directory
BUILD_DIR=build
BUILD_DIR_ABS=$(shell pwd)/$(BUILD_DIR)

####################################################################################################
# Memcached Build Options
####################################################################################################

# git repository of the memcached source
MEMCACHED_GIT=https://github.com/achreto/memcached.git

# the commit to checkout
MEMCACHED_COMMIT=9621b14

# keep in sync with the librettos options
MEMCACHED_CONF_OPTS += \
	--prefix=$(BUILD_DIR_ABS) \
	--disable-extstore


####################################################################################################
# libmemcached Build Options
####################################################################################################

LIBMEMCACHED_VERSION=1.0.18
LIBMEMCACHED_SOURCE=https://launchpad.net/libmemcached/1.0/$(LIBMEMCACHED_VERSION)/+download/libmemcached-$(LIBMEMCACHED_VERSION).tar.gz
LIBMEMCACHED_FILE=libmemcached-$(LIBMEMCACHED_VERSION).tar.gz

LIBMEMCACHED_CONF_ENV += \
	CXXFLAGS=-fpermissive

####################################################################################################
# Building Targets
####################################################################################################

build: $(BUILD_DIR)/bin/memcached

memcached/.stamp:
	git clone $(MEMCACHED_GIT)
	(cd memcached && git checkout $(MEMCACHED_COMMIT))
	touch $@

memcached/Makefile: memcached/.stamp
	(cd memcached && ./autogen.sh)
	(cd memcached && ./configure $(MEMCACHED_CONF_OPTS))

$(BUILD_DIR)/bin/memcached: memcached/Makefile
	$(MAKE) -C memcached
	$(MAKE) -C memcached install

libmemcached/.stamp:
	wget -O $(LIBMEMCACHED_FILE) $(LIBMEMCACHED_SOURCE)
	tar -xvzf $(LIBMEMCACHED_FILE)
	rm -rf $(LIBMEMCACHED_FILE)
	mv libmemcached-$(LIBMEMCACHED_VERSION) libmemcached
	touch $@

libmemcached/Makefile: libmemcached/.stamp
	(cd libmemcached && $(LIBMEMCACHED_CONF_ENV) ./configure --prefix=$(BUILD_DIR_ABS))

$(BUILD_DIR)/lib/memcached.so: libmemcached/Makefile
	$(MAKE) -C libmemcached
	$(MAKE) -C libmemcached install

clean:
	rm -rf memcached
	rm -rf build
	rm -rf libmemcached*



####################################################################################################
# Running Targets
####################################################################################################
