rjl-client:
	gcc -Wall -Wextra rjl-client.c libusb-1.0.24/libusb/*.o SDL2-2.0.14/build/*.o -lm -ldl -lpthread -lrt -ludev -o $@