all: decode send

decode: hw_decode.c tracker.h chunk.h ffmpeg ujsonin/ujsonin.c ujsonin/ujsonin.h
	./brewser.pl installdeps brew_deps
	gcc -g hw_decode.c ujsonin/ujsonin.c ujsonin/red_black_tree.c ujsonin/string-tree.c -I ffmpeg/include -I /usr/local/opt/libjpeg-turbo/include -L ffmpeg/lib -L/usr/local/opt/libjpeg-turbo/lib -lavcodec -lavutil -lavformat -lturbojpeg -lswscale -lzmq -lnanomsg -o decode
	install_name_tool -change "/usr/local/lib/libavcodec.58.dylib" "@executable_path/ffmpeg/lib/libavcodec.58.dylib" decode
	install_name_tool -change "/usr/local/lib/libavformat.58.dylib" "@executable_path/ffmpeg/lib/libavformat.58.dylib" decode
	install_name_tool -change "/usr/local/lib/libavutil.56.dylib" "@executable_path/ffmpeg/lib/libavutil.56.dylib" decode
	install_name_tool -change "/usr/local/lib/libswscale.5.dylib" "@executable_path/ffmpeg/lib/libswscale.5.dylib" decode

send: send_video.c tracker.h chunk.h
	./brewser.pl installdeps brew_deps
	gcc send_video.c ujsonin/ujsonin.c ujsonin/red_black_tree.c ujsonin/string-tree.c -lzmq -lnanomsg -o send

ffmpeg-for-h264_to_jpeg.tgz:
	wget https://github.com/nanoscopic/ffmpeg/releases/download/v1.0/ffmpeg-for-h264_to_jpeg.tgz

ffmpeg: ffmpeg-for-h264_to_jpeg.tgz
	mkdir ffmpeg
	tar xf ffmpeg-for-h264_to_jpeg.tgz -C ffmpeg
	ln -s libavcodec.58.dylib ffmpeg/lib/libavcodec.dylib
	ln -s libavformat.58.dylib ffmpeg/lib/libavformat.dylib
	ln -s libavutil.56.dylib ffmpeg/lib/libavutil.dylib
	ln -s libswscale.5.dylib ffmpeg/lib/libswscale.dylib
	ln -s libswresample.3.dylib ffmpeg/lib/libswresample.dylib
	install_name_tool -change "/usr/local/lib/libswresample.3.dylib" "@executable_path/ffmpeg/lib/libswresample.3.dylib" ffmpeg/lib/libswscale.5.dylib
	install_name_tool -change "/usr/local/lib/libswresample.3.dylib" "@executable_path/ffmpeg/lib/libswresample.3.dylib" ffmpeg/lib/libavformat.58.dylib
	install_name_tool -change "/usr/local/lib/libswresample.3.dylib" "@executable_path/ffmpeg/lib/libswresample.3.dylib" ffmpeg/lib/libavcodec.58.dylib
	install_name_tool -change "/usr/local/lib/libavutil.56.dylib" "@executable_path/ffmpeg/lib/libavutil.56.dylib" ffmpeg/lib/libavcodec.58.dylib
	install_name_tool -change "/usr/local/lib/libavutil.56.dylib" "@executable_path/ffmpeg/lib/libavutil.56.dylib" ffmpeg/lib/libswresample.3.dylib
	install_name_tool -change "/usr/local/lib/libavutil.56.dylib" "@executable_path/ffmpeg/lib/libavutil.56.dylib" ffmpeg/lib/libswscale.5.dylib
	install_name_tool -change "/usr/local/lib/libavutil.56.dylib" "@executable_path/ffmpeg/lib/libavutil.56.dylib" ffmpeg/lib/libavformat.58.dylib
	install_name_tool -change "/usr/local/lib/libavcodec.58.dylib" "@executable_path/ffmpeg/lib/libavcodec.58.dylib" ffmpeg/lib/libavformat.58.dylib

clean:
	rm -rf ffmpeg
	rm ffmpeg-for-h264_to_jpeg.tgz
	rm decode
	rm send

install: ffmpeg
	sudo cp ffmpeg/lib/* /usr/local/lib/
