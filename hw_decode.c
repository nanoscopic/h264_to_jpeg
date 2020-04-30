// Based on ffmpeg hw_decode.c example code
// Copyright (c) 2020 David Helkowski
// Copyright (c) 2017 Jun Zhao
// Copyright (c) 2017 Kaixuan Liu

#include <stdio.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/pixdesc.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libavutil/avassert.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <turbojpeg.h>
#include <VideoToolbox/VideoToolbox.h>
#include <time.h>

int64_t timespecDiff(struct timespec *timeA_p, struct timespec *timeB_p) {
  return ((timeA_p->tv_sec * 1000000000) + timeA_p->tv_nsec) - ((timeB_p->tv_sec * 1000000000) + timeB_p->tv_nsec);
}

#include "tracker.h"

static enum AVPixelFormat hw_pix_fmt;

static AVBufferRef *hw_device_ctx = NULL;

int oneout = 0;

static int hw_decoder_init(AVCodecContext *ctx, const enum AVHWDeviceType type) {
    int err = 0;

    if( ( err = av_hwdevice_ctx_create(&hw_device_ctx, type, NULL, NULL, 0 ) ) < 0 ) {
        fprintf(stderr, "Failed to create specified HW device.\n");
        return err;
    }
    ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);

    return err;
}

static enum AVPixelFormat get_hw_format(AVCodecContext *ctx, const enum AVPixelFormat *pix_fmts) {
    const enum AVPixelFormat *p;
    for( p = pix_fmts; *p != -1; p++ ) if( *p == hw_pix_fmt ) return *p;
    fprintf(stderr, "Failed to get HW surface format.\n");
    return AV_PIX_FMT_NONE;
}

typedef struct myjpeg_s {
    unsigned char *data;
    long unsigned int size;
} myjpeg;

myjpeg *raw_to_jpeg( tjhandle compressor, unsigned char * buffer, int w, int h, const char* outfilename, int linesize );
void send_jpeg( myjpeg *jpeg, myzmq *dest );

myjpeg *process_frame( tjhandle compressor, AVCodecContext *avctx, AVPacket *packet ) {
    AVFrame *frame = NULL, *frame2 = NULL;
    int size;
    int ret = 0;

    ret = avcodec_send_packet(avctx, packet);
    if (ret < 0) {
        fprintf(stderr, "Error during decoding %i\n", ret);
        return NULL;
    }

    if (!(frame = av_frame_alloc()) || !(frame2 = av_frame_alloc())) {
        fprintf(stderr, "Can not alloc frame\n");
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    oneout++;
    
    ret = avcodec_receive_frame(avctx, frame);
    if( ( oneout % 3 ) != 0 ) goto fail;
    
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        av_frame_free( &frame );
        av_frame_free( &frame2 );
        return NULL;
    }
    else if (ret < 0) {
        fprintf(stderr, "Error while decoding\n");
        goto fail;
    }

    frame2->format = AV_PIX_FMT_NV12;
    av_hwframe_transfer_data(frame2, frame, 0 );
    
    int w = frame2->width;
    int h = frame2->height;

    struct SwsContext *sws_ctx = sws_getContext(
        w, h, frame2->format,
        w, h, AV_PIX_FMT_RGB24,
        SWS_POINT, NULL, NULL, NULL );

    AVFrame *frame3 = av_frame_alloc();
    frame3->format = AV_PIX_FMT_RGB24;
    frame3->width = w;
    frame3->height = h;
    av_frame_get_buffer( frame3, 32 );
    
    sws_scale( sws_ctx,
        (const uint8_t *const *) frame2->data, frame2->linesize, 0, h,
        frame3->data, frame3->linesize );
    
    myjpeg *jpeg = raw_to_jpeg( compressor, (unsigned char *) frame3->data[0], w, h, "test.jpg", frame3->linesize[0] );
    
    return jpeg;
    
    fail:
    if( frame ) av_frame_free(&frame);
    if( frame2 ) av_frame_free(&frame2);

    return NULL;
}

myjpeg *raw_to_jpeg( tjhandle compressor, unsigned char * buffer, int w, int h, const char* outfilename, int linesize ) {
    const int JPEG_QUALITY = 75;
    const int COLOR_COMPONENTS = 3;
    myjpeg *jpeg = calloc( sizeof( myjpeg ), 1 );

    tjCompress2( compressor, buffer, w, linesize, h, TJPF_RGB, &jpeg->data, &jpeg->size, TJSAMP_420, JPEG_QUALITY, TJFLAG_FASTDCT );
    
    return jpeg;
}

void write_jpeg( myjpeg *jpeg, char *filename ) {
    if( filename ) {
        FILE *fh = fopen( filename, "wb" );
        fwrite( jpeg->data, 1, jpeg->size, fh );
        fclose( fh );
    }
    tjFree( jpeg->data );
    free( jpeg );
}

void myzmq__send_jpeg( myjpeg *jpeg, myzmq *dest ) {
    if( dest ) myzmq__send( dest, jpeg->data, jpeg->size );
    tjFree( jpeg->data );
    free( jpeg );
}

void mynano__send_jpeg( myjpeg *jpeg, int n ) {
    if(n) mynano__send( n, jpeg->data, jpeg->size );
    tjFree( jpeg->data );
    free( jpeg );
}

// **** START BUFFER STUFF

static int read_packet( void *opaque, uint8_t *buf, int buf_size ) {
    chunk_tracker *tracker = (chunk_tracker *) opaque;
    chunk *curchunk = tracker->curchunk;
    if( !curchunk ) return AVERROR_EOF;
    
    int bufpos = 0;
    int bufleft = buf_size;
    int chunkleft = curchunk->size;
    chunkleft -= tracker->pos;
    
    while( bufleft ) {
        if( chunkleft > bufleft ) {
            chunkleft -= bufleft;
            memcpy( &buf[bufpos], &curchunk->data[ tracker->pos ], bufleft );
            tracker->pos += bufleft;
            return buf_size;            
        }
        if( chunkleft == bufleft ) {
            memcpy( &buf[bufpos], &curchunk->data[ tracker->pos ], bufleft );
            tracker->curchunk = curchunk->next;
            chunk__del( curchunk );
            tracker->pos = 0;
            return buf_size;
        }
        // chunkleft < bufleft
        memcpy( &buf[bufpos], &curchunk->data[ tracker->pos ], chunkleft );
        bufpos += chunkleft;
        
        // shift to the next chunk
        chunk *todel = curchunk;
        curchunk = curchunk->next;
        chunk__del( todel );
        
        tracker->curchunk = curchunk;
        tracker->pos = 0;
        
        if( !curchunk ) return bufpos; // if no chunk is ready; just return what we have so far
        
        bufleft -= chunkleft;
        chunkleft = curchunk->size;
    }
    return AVERROR_EOF; // unreachable...
}


AVFormatContext *new_memory_ctx( chunk_tracker **ret ) {
    int err;
    
    chunk_tracker *tracker = calloc( sizeof( chunk_tracker ), 1 );
    tracker->curchunk = NULL;
    tracker->pos = 0;
    
    size_t avio_ctx_buffer_size = 50000;
    uint8_t *avio_ctx_buffer = av_malloc( avio_ctx_buffer_size );
    if (!avio_ctx_buffer) { err = AVERROR(ENOMEM); goto new_mctx_err; }
    
    AVIOContext *avio_ctx = avio_alloc_context( avio_ctx_buffer, avio_ctx_buffer_size, 0, tracker, &read_packet, NULL, NULL );
    if (!avio_ctx) { err = AVERROR(ENOMEM); goto new_mctx_err; }
    
    AVFormatContext *fmt_ctx = avformat_alloc_context();
    if( !fmt_ctx ) { err = AVERROR(ENOMEM); goto new_mctx_err; }
    fmt_ctx->pb = avio_ctx;
    
    *ret = tracker;
    return fmt_ctx;
    
  new_mctx_err:
    printf("new_mctx_err\n");
    return NULL;
}

// **** END BUFFER STUFF

int main( int argc, char *argv[] ) {
    struct timespec main_start, loop_start, diff;
    clock_gettime(CLOCK_MONOTONIC, &main_start);
    int ret;
    
    if( argc < 2 ) {
        fprintf(stderr, "Usage: %s <input file>\n", argv[0]);
        return -1;
    }

    enum AVHWDeviceType type = av_hwdevice_find_type_by_name( "videotoolbox" );
    if( type == AV_HWDEVICE_TYPE_NONE ) {
        fprintf( stderr, "Cannot find videotoolbox hw decoder.\n" );
        return -1;
    }
    
    chunk_tracker *tracker;
    AVFormatContext *input_ctx = new_memory_ctx( &tracker );
    
    myzmq *zmqIn = NULL, *zmqOut = NULL;
    int nanoIn = 0, nanoOut = 0;
    char mode = 0; // 0->file, 1->zmq, 2->nanomsg
    FILE *fh = NULL;
    if( !strncmp( argv[1], "zmq", 3 ) ) {
        mode = 1;
        if( argc < 3 ) {
            fprintf(stderr, "Usage: %s zmq <zmq spec to pull from> <zmq spec to push to>\n", argv[0] );
            return -1;
        }
        
        char *spec = argv[2];
        zmqIn = myzmq__new( spec, 1 ); // 1 means bind to socket
        printf("Receiving data from zmq %s\n", spec );
        
        if( argc > 3 ) {
            char *specOut = argv[3];
            zmqOut = myzmq__new( spec, 0 ); // 0 means connect to socket
            printf("Send data to zmq %s\n", spec );
        }
    }
    else if( !strncmp( argv[1], "nano", 4 ) ) {
        mode = 2;
        if( argc < 3 ) {
            fprintf(stderr, "Usage: %s nano <nanomsg spec to pull from> <nanomsg spec to push to>\n", argv[0] );
            return -1;
        }
        
        char *spec = argv[2];
        nanoIn = mynano__new( spec, 1 ); // 1 means bind to socket
        printf("Receiving data from nanomsg %s\n", spec );
        
        if( argc > 3 ) {
            char *specOut = argv[3];
            nanoOut = mynano__new( specOut, 0 ); // 0 means connect to socket
            printf("Send data to nanomsg %s\n", specOut );
        }
    }
    else {    
        fh = fopen( argv[1], "rb" );
        if( !fh ) {
            fprintf( stderr, "Cannot open input file '%s'\n", argv[1] );
            return -1;
        }
    }
    
    printf("Fetching headers to start decoder\n");
    if( mode == 0 ) tracker__read_headers( tracker, fh );
    else if( mode == 1 ) tracker__myzmq__recv_headers( tracker, zmqIn );
    else if( mode == 2 ) tracker__mynano__recv_headers( tracker, nanoIn );
    
    AVInputFormat *format = av_find_input_format("h264");
    ret = avformat_open_input( &input_ctx, NULL, format, NULL );
    if( ret != 0 ) {
        fprintf( stderr, "Cannot open input; err=%i\n", ret );
        return -1;
    }
    
    AVCodec *decoder = NULL;
    int video_stream = av_find_best_stream( input_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
    if( video_stream < 0 ) {
        fprintf(stderr, "Cannot find a video stream in the input file\n");
        return -1;
    }
    
    int gotframe = 0;
    
    printf("Fetching first frame to initialize decoder\n");
    if( mode == 0 ) gotframe = tracker__read_frame( tracker, fh );
    else if( mode == 1 ) tracker__myzmq__recv_frame( tracker, zmqIn );
    else if( mode == 2 ) tracker__mynano__recv_frame( tracker, nanoIn );
    
    int i;
    for( i = 0;; i++ ) {
        const AVCodecHWConfig *config = avcodec_get_hw_config( decoder, i );
        if( !config ) {
            fprintf(stderr, "Decoder %s does not support device type %s.\n", decoder->name, av_hwdevice_get_type_name(type));
            return -1;
        }
        if( config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX && config->device_type == type ) {
            hw_pix_fmt = config->pix_fmt;
            const char *pixStr = av_get_pix_fmt_name( hw_pix_fmt );
            break;
        }
    }
    
    AVCodecContext *decoder_ctx = avcodec_alloc_context3( decoder );
    if( !decoder_ctx ) return AVERROR( ENOMEM );

    AVStream *video = input_ctx->streams[ video_stream ];
    if( avcodec_parameters_to_context( decoder_ctx, video->codecpar ) < 0 ) return -1;

    decoder_ctx->get_format  = get_hw_format;

    if( hw_decoder_init(decoder_ctx, type) < 0 ) return -1;

    if( avcodec_open2( decoder_ctx, decoder, NULL ) < 0 ) {
        fprintf(stderr, "Failed to open codec for stream #%u\n", video_stream);
        return -1;
    }
    
    tjhandle compressor = tjInitCompress();
    AVPacket packet;
    
    clock_gettime(CLOCK_MONOTONIC, &loop_start);
    
    printf("Decoder is started; beginning loop reading frames\n");
    
    uint64_t timeElapsed = timespecDiff(&loop_start, &main_start);
            
    printf("Time from start of main till video loop: %f\n", (double) timeElapsed / ( double ) 1000000 );
            
    int frameCount = 1;
    while( ret >= 0 ) {
        if( frameCount > 1 ) {
            if( mode == 0 ) gotframe = tracker__read_frame( tracker, fh );
            else if( mode == 1 ) tracker__myzmq__recv_frame( tracker, zmqIn );
            else if( mode == 2 ) tracker__mynano__recv_frame( tracker, nanoIn );
        }
        
        frameCount++;
        //if( !gotframe ) break;
        
        if( ( ret = av_read_frame( input_ctx, &packet ) ) < 0 ) break;

        if( video_stream == packet.stream_index ) {
            myjpeg *jpeg = process_frame( compressor, decoder_ctx, &packet );
            if( jpeg ) {
                if( mode == 1 ) myzmq__send_jpeg( jpeg, zmqOut );
                else if( mode == 2 ) mynano__send_jpeg( jpeg, nanoOut );
            }
        }

        av_packet_unref(&packet);
    }
    
    struct timespec loop_done;
    clock_gettime(CLOCK_MONOTONIC, &loop_done);
    
    uint64_t timeElapsed2 = timespecDiff(&loop_done, &loop_start);
            
    printf("Total time in loop (ms) : %f\n", (double) timeElapsed2 / ( double ) 1000000 );

    printf("Total framecount: %i\n", frameCount );
    printf("Time per frame (ms): %f\n", (double) timeElapsed2 / ( double ) 1000000 / (double) frameCount );
    
    /* flush the decoder */
    packet.data = NULL;
    packet.size = 0;
    
    myjpeg *jpeg = process_frame( compressor, decoder_ctx, &packet );
    if( mode == 1 ) myzmq__send_jpeg( jpeg, zmqOut );
    else if( mode == 2 ) mynano__send_jpeg( jpeg, nanoOut );
    
    tjDestroy(compressor);
    
    av_packet_unref(&packet);

    avcodec_free_context(&decoder_ctx);
    avformat_close_input(&input_ctx);
    av_buffer_unref(&hw_device_ctx);

    if( zmqIn ) myzmq__del( zmqIn );
    if( zmqOut ) myzmq__del( zmqOut );
    
    return 0;
}
