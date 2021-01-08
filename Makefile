define shared

.ONESHELL: $(1)

$(1):
	cd $(firstword $(subst /, ,$(1)))
	sh configure
	make -j

endef

libs := \
	SDL2-2.0.14/build/.libs/libSDL2.so \
	libusb-1.0.24/libusb/.libs/libusb-1.0.so

CFLAGS := -Wall -Wextra
LDFLAGS := $(foreach lib,$(libs),-L$(dir $(lib)) -l:$(notdir $(lib)))

rjl-client: rjl-client.c $(libs)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

$(eval $(foreach lib,$(libs),$(call shared,$(lib))))