all: decode send

decode: hw_decode.c tracker.h chunk.h
	gcc hw_decode.c -I/opt/libjpeg-turbo/include -L lib -L/opt/libjpeg-turbo/lib -lavcodec -lavutil -lavformat -lturbojpeg -lswscale -lzmq -lnanomsg -o decode

send: send_video.c tracker.h chunk.h
	gcc send_video.c -lzmq -lnanomsg -o send
