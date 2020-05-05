all: decode send

decode: hw_decode.c tracker.h chunk.h ffmpeg
	./brewser.pl installdeps brew_deps
	gcc -g hw_decode.c -I/opt/libjpeg-turbo/include -L ffmpeg/lib -L/opt/libjpeg-turbo/lib -lavcodec -lavutil -lavformat -lturbojpeg -lswscale -lzmq -lnanomsg -o decode

send: send_video.c tracker.h chunk.h
	./brewser.pl installdeps brew_deps
	gcc send_video.c -lzmq -lnanomsg -o send

ffmpeg-for-h264_to_jpeg.tgz:
	wget https://github.com/nanoscopic/ffmpeg/releases/download/v1.0/ffmpeg-for-h264_to_jpeg.tgz

ffmpeg: ffmpeg-for-h264_to_jpeg.tgz
	mkdir ffmpeg
	tar xf ffmpeg-for-h264_to_jpeg.tgz -C ffmpeg

clean:
	rm -rf ffmpeg
	rm ffmpeg-for-h264_to_jpeg.tgz
	rm decode
	rm send

install: ffmpeg
	sudo cp ffmpeg/lib/* /usr/local/lib/
