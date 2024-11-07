CFLAGS = -Wall -O2
screencast: screencast.o ssdp.o alsa.o
	gcc -o screencast $^ -pthread -lm -lX11 -lavcodec -lavformat -lavutil -lswscale -lasound

clean:
	rm -f screencast ssdp.o screencast.o alsa.o
