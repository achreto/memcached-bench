# the build directory
BUILD_DIR=build
BUILD_DIR_ABS=$(shell pwd)/$(BUILD_DIR)

####################################################################################################
# Memcached Build Options
####################################################################################################

# git repository of the memcached source
MEMCACHED_GIT=https://github.com/achreto/memcached.git

# the commit to checkout
MEMCACHED_COMMIT=97ef9a11c4282e2a22b3db821709d8856350a5a8

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

LIBMEMCACHED_CONF_OPTS += \
	--disable-sasl \
	--prefix=$(BUILD_DIR_ABS)

####################################################################################################
# Building Targets
####################################################################################################

build: $(BUILD_DIR)/bin/memcached $(BUILD_DIR)/bin/loadbalancer

memcached/.stamp:
	git clone $(MEMCACHED_GIT)
	(cd memcached && git checkout $(MEMCACHED_COMMIT))
	(cd memcached && ./autogen.sh || true)
	(cd memcached && ./configure $(MEMCACHED_CONF_OPTS))
	touch $@

libmemcached/.stamp:
	wget -O $(LIBMEMCACHED_FILE) $(LIBMEMCACHED_SOURCE)
	tar -xvzf $(LIBMEMCACHED_FILE)
	rm -rf $(LIBMEMCACHED_FILE)
	mv libmemcached-$(LIBMEMCACHED_VERSION) libmemcached
	(cd libmemcached && $(LIBMEMCACHED_CONF_ENV) ./configure $(LIBMEMCACHED_CONF_OPTS))
	$(MAKE) -C libmemcached $(MAKEFLAGS)
	$(MAKE) -C libmemcached install
	touch $@

$(BUILD_DIR)/bin/memcached: memcached/.stamp
	$(MAKE) -C memcached $(MAKEFLAGS)
	$(MAKE) -C memcached install

$(BUILD_DIR)/bin/loadbalancer: libmemcached/.stamp
	$(MAKE) -C loadbalancer

clean:
	rm -rf memcached
	rm -rf build
	rm -rf libmemcached*



####################################################################################################
# Running Targets
####################################################################################################

