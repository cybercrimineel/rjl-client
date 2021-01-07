rjl-client:
	gcc -L SDL2-2.0.14/build/.libs -L libusb-1.0.24/libusb/.libs -l usb-1.0 -l SDL2 -Wall -Wextra rjl-client.c -o $@