// Based on ffmpeg hw_decode.c example code
// Copyright (c) 2020 David Helkowski
// Original example code:
// Copyright (c) 2017 Jun Zhao
// Copyright (c) 2017 Kaixuan Liu

#include <stdio.h>

#include <libavcodec/avcodec.h>
#include <libavcodec/videotoolbox.h>
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
#include "uclop.h"
#include <unistd.h>
#include <sys/time.h>
#include "ujsonin/ujsonin.h"

int64_t timespecDiff(struct timespec *timeA_p, struct timespec *timeB_p) {
  return ((timeA_p->tv_sec * 1000000000) + timeA_p->tv_nsec) - ((timeB_p->tv_sec * 1000000000) + timeB_p->tv_nsec);
}

#include "tracker.h"

static enum AVPixelFormat hw_pix_fmt;

static AVBufferRef *hw_device_ctx = NULL;

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

char strErr[200];

int skip_frame( AVCodecContext *avctx, AVPacket *packet ) {
    int ret = avcodec_send_packet(avctx, packet);
    if (ret < 0) {
        av_strerror( ret, strErr, 200 );
        fprintf(stderr, "Error during decoding: %s\n", strErr);
        goto SKIPFAIL;
    }
    AVFrame *frame = av_frame_alloc();
    if( !frame ) {
        fprintf(stderr, "Cannot alloc frame\n");
        goto SKIPFAIL;
    }
    ret = avcodec_receive_frame( avctx, frame );
    if( ret < 0 ) {
        av_strerror( ret, strErr, 200 );
        fprintf(stderr, "Error recv frame: %s\n", strErr);
        goto SKIPFAIL;
    }
    av_packet_unref( packet );
    return 1;
  SKIPFAIL:
    av_packet_unref( packet );
    return 0;
}

const uint8_t difmap[ 17 ] = {
  0, // -16
  1, // -32
  3, // -48
  10, // -64
  10, // -80
  20, // -96
  20, // -112
  40, // -128
  40, // -144
  80, // -160
  80, // -176
  160, // -192
  160, // 208 
  255, // 224
  255, // 240
  255, // 256
  255
};

char frameDif( AVFrame *f1, AVFrame *f2 ) {
    int w = f1->width*3;
    int h = f1->height;
    int l1 = f1->linesize[0];
    uint64_t totDif = 0;
    for( int y=0;y<h;y++ ) {
        if( y%3 ) continue;
        int lstart = l1 * y;
        uint8_t *d1 = f1->data[0] + lstart;
        uint8_t *d2 = f2->data[0] + lstart;
        for( int x=0;x<w;x+=9,d1+=9,d2+=9 ) {
            uint8_t r1 = *d1;
            uint8_t g1 = *(d1+1);
            uint8_t b1 = *(d1+2);
            uint8_t r2 = *d2;
            uint8_t g2 = *(d2+1);
            uint8_t b2 = *(d2+2);
            
            uint8_t dr = abs( r1-r2 ) >> 4; 
            uint8_t dg = abs( g1-g2 ) >> 4;
            uint8_t db = abs( b1-b2 ) >> 4;
            
            totDif += difmap[dr] + difmap[dg] + difmap[db];
        }
        if( totDif > 2500 ) return 1;
    }
    //printf("dif: %lli\n", (long long ) totDif );
    if( totDif > 2500 ) return 1;
    return 0;
}

void get_frame_size( AVCodecContext *avctx, AVPacket *packet, int *w, int *h ) {
    int size;
    int ret = avcodec_send_packet(avctx, packet);
    if (ret < 0) {
        av_strerror( ret, strErr, 200 );
        fprintf(stderr, "Error during decoding: %s\n", strErr);
        return;
    }

    AVFrame *frame = av_frame_alloc();
    if( !frame ) {
        fprintf(stderr, "Can not alloc frame\n");
        ret = AVERROR(ENOMEM);
        return;
    }
    
    ret = avcodec_receive_frame(avctx, frame);
    
    *w = frame->width;
    *h = frame->height;
    
    av_frame_free(&frame);
}

myjpeg *process_frame( tjhandle compressor, AVCodecContext *avctx, AVPacket *packet, uint64_t frameTime, char skip, AVFrame **prevframe, uint64_t *prevtime, int dw, int dh ) {
    int size;
    int ret = avcodec_send_packet(avctx, packet);
    if (ret < 0) {
        av_strerror( ret, strErr, 200 );
        fprintf(stderr, "Error during decoding: %s\n", strErr);
        goto fail;
    }

    AVFrame *frame = av_frame_alloc();
    AVFrame *frame2 = av_frame_alloc();
    if( !frame || !frame2 ) {
        fprintf(stderr, "Can not alloc frame\n");
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    
    ret = avcodec_receive_frame(avctx, frame);
    
    if( skip ) {
        /*uint64_t now = now_msec();
        uint64_t dif = now - frameTime;
        printf("MS till drop: %lli\n", (long long) dif );*/
        if( frame ) av_frame_free(&frame);
        if( frame2 ) av_frame_free(&frame2);
        return NULL;
    }
    
    if( ret < 0 ) {
        av_strerror( ret, strErr, 200 );
        fprintf(stderr, "Error while decoding: %s\n", strErr);
        goto fail;
    }

    av_hwframe_transfer_data( frame2, frame, 0 );
    
    /*CVPixelBufferRef pix_buf = (CVPixelBufferRef)frame2->data[3];
    OSType pixel_format = CVPixelBufferGetPixelFormatType(pix_buf);
    const char *pixStr = av_get_pix_fmt_name( pixel_format );
    printf("Decoded Pixel format: %s\n", pixStr );*/
    
    int w = frame2->width;
    int h = frame2->height;
    if( !dw ) {
        dw = w;
        dh = h;
    }

    struct SwsContext *sws_ctx = sws_getContext(
        w, h, frame2->format,
        dw, dh, AV_PIX_FMT_RGB24,
        SWS_POINT, NULL, NULL, NULL );

    AVFrame *frame3 = av_frame_alloc();
    frame3->format = AV_PIX_FMT_RGB24;
    frame3->width = dw;
    frame3->height = dh;
    av_frame_get_buffer( frame3, 32 );
    
    int resultHeight = sws_scale( sws_ctx,
        (const uint8_t *const *) frame2->data, frame2->linesize, 0, h,
        frame3->data, frame3->linesize );
    
    if( resultHeight != dh ) {
        fprintf(stderr, "Result height %i doesn't match destination height %i\n", resultHeight, dh );
    }
    
    sws_freeContext( sws_ctx );
    
    if( *prevframe ) {
        char needFrame = 0;
        if( *prevtime && ( frameTime - *prevtime ) > 1000 ) {
            needFrame = 1;
        }
        if( !needFrame && !frameDif( frame3, *prevframe ) ) {
            if( frame ) av_frame_free(&frame);
            if( frame2 ) av_frame_free(&frame2);
            av_frame_free( &frame3 );
            //printf("Unchanged\n");
            return NULL;
        }
        av_frame_free( prevframe );
    }
    *prevframe = frame3;
    *prevtime = now_msec();
    
    myjpeg *jpeg = raw_to_jpeg( compressor, (unsigned char *) frame3->data[0], dw, dh, "test.jpg", frame3->linesize[0] );
    if( frame ) av_frame_free(&frame);
    if( frame2 ) av_frame_free(&frame2);
    //if( frame3 ) av_frame_free( &frame3 );
    
    uint64_t now = now_msec();
    uint64_t dif = now - frameTime;
    //printf("MS till jpeg: %lli\n", (long long) dif );
    
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

    int res = tjCompress2( compressor, buffer, w, linesize, h, TJPF_RGB, &jpeg->data, &jpeg->size, TJSAMP_420, JPEG_QUALITY, TJFLAG_FASTDCT );
    if( res == -1 ) {
        printf("tjCompress2 failed\n");
    }
    return jpeg;
}

void write_jpeg( myjpeg *jpeg, char *filename ) {
    if( filename ) {
        FILE *fh = fopen( filename, "wb" );
        if( !fh ) {
            fprintf(stderr,"Can't open %s for writing", filename );
        }
        fwrite( jpeg->data, 1, jpeg->size, fh );
        fclose( fh );
    }
    tjFree( jpeg->data );
    free( jpeg );
}

void myzmq__send_jpeg( myjpeg *jpeg, myzmq *dest ) {
    if( dest ) {
        myzmq__send( dest, jpeg->data, jpeg->size );
    }
    tjFree( jpeg->data );
    free( jpeg );
}

void mynano__send_jpeg( myjpeg *jpeg, int n, int ow, int oh, int dw, int dh ) {
    if(n) {
        char buffer[200];
        int jlen = snprintf( buffer, 200, "{\"ow\":%i,\"oh\":%i,\"dw\":%i,\"dh\":%i}", ow, oh, dw, dh );
        long unsigned int totlen = jpeg->size + jlen;
        char *both = malloc( totlen );
        memcpy( both, buffer, jlen );
        memcpy( &both[jlen], jpeg->data, jpeg->size );
        mynano__send( n, both, totlen );
        free( both );
    }
    tjFree( jpeg->data );
    free( jpeg );
}

static int read_packet( void *opaque, uint8_t *buf, int buf_size ) {
    chunk_tracker *tracker = (chunk_tracker *) opaque;
    chunk *curchunk = tracker->curchunk;
    if( !curchunk ) return AVERROR_EOF;
    
    int bufpos = 0;
    int bufleft = buf_size;
    int chunkleft = curchunk->size;
    chunkleft -= tracker->pos;
    
    while( bufleft ) {
        // We can't fit the chunk into the buffer; write some of it
        if( chunkleft > bufleft ) {
            chunkleft -= bufleft;
            memcpy( &buf[bufpos], &curchunk->data[ tracker->pos ], bufleft );
            tracker->pos += bufleft;
            return buf_size;            
        }
        
        // The chunk fits exactly. Write it into the buffer and return
        if( chunkleft == bufleft ) {
            memcpy( &buf[bufpos], &curchunk->data[ tracker->pos ], bufleft );
            tracker->curchunk = curchunk->next;
            chunk__del( curchunk );
            tracker->pos = 0;
            return buf_size;
        }
        
        // The chunk is smaller than the buffer. Write it into the buffer and continue to next chunk
        
        // Write the chunk into the buffer
        memcpy( &buf[bufpos], &curchunk->data[ tracker->pos ], chunkleft );
        // Advance the buffer position
        bufpos += chunkleft;
        bufleft -= chunkleft;
        
        // Continue to the next chunk then delete the chunk we wrote into the buffer
        chunk *todel = curchunk;
        curchunk = curchunk->next;
        chunk__del( todel );
        tracker->curchunk = curchunk;
        tracker->pos = 0;
       
        // If no chunk is ready; just return what we have so far
        if( !curchunk ) return bufpos; 
         
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

void setup_zmq_sockets( ucmd *cmd, myzmq **zmqIn, myzmq **zmqOut ) {
    char *specIn = ucmd__get(cmd,"--in");
    *zmqIn = myzmq__new( specIn, 1 ); // 1 means bind to socket
    printf("Receiving data from zmq %s\n", specIn );
    
    char *specOut = ucmd__get(cmd,"--out");
    if( specOut ) {
        *zmqOut = myzmq__new( specOut, 0 ); // 0 means connect to socket
        printf("Send data to zmq %s\n", specOut );
    }
}

int run_stream( ucmd *cmd, int mode, int nanoIn, int nanoOut, myzmq *zmqIn, myzmq *zmqOut, FILE *fh );

void run_zmq( ucmd *cmd ) {
    myzmq *zmqIn = NULL, *zmqOut = NULL;
    setup_zmq_sockets( cmd, &zmqIn, &zmqOut );
    run_stream( cmd, 1, 0, 0, zmqIn, zmqOut, NULL );
}

void setup_nanomsg_sockets( ucmd *cmd, int *nanoIn, int *nanoOut ) {
  char *specIn = ucmd__get(cmd,"--in");
    *nanoIn = mynano__new( specIn, 1 ); // 1 means bind to socket
    printf("Receiving data from nanomsg %s\n", specIn );
    
    char *specOut = ucmd__get(cmd,"--out");
    if( specOut ) {
        *nanoOut = mynano__new( specOut, 0 ); // 0 means connect to socket
        printf("Send data to nanomsg %s\n", specOut );
    }
}

void run_nano( ucmd *cmd ) {
    int nanoIn = 0, nanoOut = 0;
    setup_nanomsg_sockets( cmd, &nanoIn, &nanoOut );
    run_stream( cmd, 2, nanoIn, nanoOut, NULL, NULL, NULL );
}

void run_file( ucmd *cmd ) {
    char *file = ucmd__get(cmd, "--file");
    FILE *fh = fopen( file, "rb" );
    if( !fh ) {
        fprintf( stderr, "Cannot open input file '%s'\n", file );
        return;
    }
    run_stream( cmd, 0, 0, 0, NULL, NULL, fh );
}

int main( int argc, char *argv[] ) {
    uopt *file_options[] = {
        UOPT_REQUIRED("--file","File to process"),
        UOPT("--loops","Number of times to loop through the file"),
        UOPT("--cacheid","ID to cache headers under"),
        UOPT("--cachedir","Dir to store cache files in"),
        UOPT("--dw","Destination width"),
        UOPT("--dh","Destination height"),
        NULL
    };
    uopt *nano_options[] = {
        UOPT_REQUIRED("--in","Nanomsg input spec"),
        UOPT("--out","Nanomsg output spec"),
        UOPT("--frameSkip","Frame skip mod; 2=half frames, 3=1/3 frames"),
        UOPT("--cacheid","ID to cache headers under"),
        UOPT("--cachedir","Dir to store cache files in"),
        UOPT("--dw","Destination width"),
        UOPT("--dh","Destination height"),
        NULL
    };
    uopt *zmq_options[] = {
        UOPT_REQUIRED("--in","Zeromq input spec"),
        UOPT("--out","Zeromq output spec"),
        UOPT("--frameSkip","Frame skip mod; 2=half frames, 3=1/3 frames"),
        NULL
    };
    uclop *opts = uclop__new( NULL, NULL );
    uclop__addcmd( opts, "file", "Process a file", &run_file, file_options );
    uclop__addcmd( opts, "nano", "Stream using nanomsg", &run_nano, nano_options );
    uclop__addcmd( opts, "zmq", "Stream using zmq", &run_zmq, zmq_options );
    uclop__run( opts, argc, argv );
}

int run_stream( ucmd *cmd, int mode, int nanoIn, int nanoOut, myzmq *zmqIn, myzmq *zmqOut, FILE *fh ) {
    ujsonin_init();
  
    int frameSkip = 0;
    char *frameSkipC = ucmd__get( cmd, "--frameSkip" );
    if( frameSkipC ) {
        frameSkip = atoi( frameSkipC );
    }
    
    int loops = 1;
    int loop = 1;
    char *loopsC = ucmd__get( cmd, "--loops" );
    if( loopsC ) {
        loops = atoi( loopsC );
        printf("Parsing file %i times\n", loops );
    }
    
    int dw = 0;
    int dh = 0;
    char *dwC = ucmd__get( cmd, "--dw" );
    char *dhC = ucmd__get( cmd, "--dh" );
    if( dhC && dhC ) {
        dw = atoi( dwC );
        dh = atoi( dhC );
    }
  
    // mode 0->file, 1->zmq, 2->nanomsg
    struct timespec main_start, loop_start, diff;
    clock_gettime(CLOCK_MONOTONIC, &main_start);
    int ret;
    
    enum AVHWDeviceType type = av_hwdevice_find_type_by_name( "videotoolbox" );
    if( type == AV_HWDEVICE_TYPE_NONE ) {
        fprintf( stderr, "Cannot find videotoolbox hw decoder.\n" );
        return -1;
    }
    
    chunk_tracker *tracker;
    AVFormatContext *input_ctx = new_memory_ctx( &tracker );
    
    char usedCache = 0;
    printf("Fetching headers to start decoder\n");
    
    char *cacheId = ucmd__get( cmd, "--cacheid" );
        
    if( !cacheId ) {
        if( mode == 0 ) tracker__read_headers( tracker, fh );
        else if( mode == 1 ) tracker__myzmq__recv_headers( tracker, zmqIn );
        else if( mode == 2 ) tracker__mynano__recv_headers( tracker, nanoIn );
    }
    else {
        char *cacheDir = ucmd__get( cmd, "--cachedir" );
        if( !cacheDir ) cacheDir = "cache";
        char cacheFile[100];
        snprintf( cacheFile, 100, "%s/%s", cacheDir, cacheId );
        if( access( cacheFile, F_OK ) != -1 ) {
            printf("Using cached headers from %s\n", cacheFile );
            // cache exists; use it
            FILE *fh = fopen( cacheFile, "rb" );
            tracker__read_headers( tracker, fh );
            fclose( fh );
            usedCache = 1;
        }
        else {
            // cache doesn't exist; read headers then store them
            printf("Caching headers at %s\n", cacheFile );
            char res = 0;
            
            if( mode == 0 ) res = tracker__read_headers( tracker, fh );
            else if( mode == 1 ) res = tracker__myzmq__recv_headers( tracker, zmqIn );
            else if( mode == 2 ) res = tracker__mynano__recv_headers( tracker, nanoIn );
            
            if( res == 0 ) {
                fprintf(stderr,"Did not recieve headers; cannot continue\n");
                exit(1);
            }
            FILE *fh = fopen( cacheFile, "wb+" );
            tracker__write_file( tracker, fh );
            fclose( fh );
        }
    }
    
    AVInputFormat *format = av_find_input_format("h264");
    if( !format ) {
        fprintf( stderr, "Cannot find input format h264\n" );
        return -1;
    }
    
    printf("Opening Input\n");
    ret = avformat_open_input( &input_ctx, NULL, format, NULL );
    if( ret != 0 ) {
        char strErr[200];
        av_strerror( ret, strErr, 200 );
        fprintf( stderr, "Cannot open input; %s\n", strErr );
        return -1;
    }
    printf("Input Open\n");
    
    int gotframe = 1;
    
    printf("Fetching first frame to initialize decoder\n");
    
    if( mode == 0 ) {
        gotframe = tracker__read_frame( tracker, fh ); // receives a non header frame
    }
    else if( mode == 1 ) {
        // todo fix for cache
        tracker__myzmq__recv_frame( tracker, zmqIn );
    }
    else if( mode == 2 ) {
        if( usedCache ) tracker__mynano__recv_frame_non_header( tracker, nanoIn, NULL );
        else tracker__mynano__recv_frame( tracker, nanoIn );
    }
    
    // Find Stream Info doesn't "need" a first frame to function, but it complains if you don't give it one
    printf("Finding stream info\n");
    if (avformat_find_stream_info(input_ctx, NULL) < 0) {
        fprintf(stderr, "Cannot find input stream information.\n");
        return -1;
    }
     
    printf("Finding best stream\n");
    AVCodec *decoder = NULL;
    int video_stream = av_find_best_stream( input_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
    if( video_stream < 0 ) {
        fprintf(stderr, "Cannot find a video stream in the input file\n");
        return -1;
    }
    
    printf("Getting hardware config\n");
    int i;
    for( i = 0;; i++ ) {
        const AVCodecHWConfig *config = avcodec_get_hw_config( decoder, i );
        if( !config ) {
            fprintf(stderr, "Decoder %s does not support device type %s.\n", decoder->name, av_hwdevice_get_type_name(type));
            return -1;
        }
        if( config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX && config->device_type == type ) {
            hw_pix_fmt = config->pix_fmt;
            //const char *pixStr = av_get_pix_fmt_name( hw_pix_fmt );
            //printf("Pixel format name: %s\n", pixStr );
            break;
        }
    }
    
    AVCodecContext *decoder_ctx = avcodec_alloc_context3( decoder );
    
    if( !decoder_ctx ) return AVERROR( ENOMEM );
    
    AVStream *video = input_ctx->streams[ video_stream ];
    if( avcodec_parameters_to_context( decoder_ctx, video->codecpar ) < 0 ) return -1;
    
    decoder_ctx->get_format  = get_hw_format;
    // pixel format becomes AV_PIX_FMT_VIDEOTOOLBOX
    
    printf("Initiating decoder\n");
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
    
    int frameCount = 0;
    uint64_t frameTime;
    AVFrame *prevframe = NULL;
    uint64_t prevtime = 0;
    char wroteJpeg = 0;
    
    int srcw, srch;
    
    for( int j=0;j<20;j++ ) {
        if( mode == 0 ) gotframe = tracker__read_frame( tracker, fh );
        else if( mode == 1 ) tracker__myzmq__recv_frame( tracker, zmqIn );
        else if( mode == 2 ) tracker__mynano__recv_frame_non_header( tracker, nanoIn, &frameTime );
            
        if( ( ret = av_read_frame( input_ctx, &packet ) ) < 0 ) break;
        if( video_stream != packet.stream_index ) { av_packet_unref(&packet); continue; }
        get_frame_size( decoder_ctx, &packet, &srcw, &srch );
        printf("Source dimensions %i x %i\n", srcw, srch );
        break;
    }
    if( !dw && !dh ) {
        dw = srcw;
        dh = srch;
    }
    if( dh > 1000 ) {
        dw /= 2;
        dh /= 2;
    }
    printf("Target dimensions %i x %i\n", dw, dh );
    
    while( ret >= 0 ) {
        if( frameCount > 0 ) {
            if( mode == 0 ) gotframe = tracker__read_frame( tracker, fh );
            else if( mode == 1 ) tracker__myzmq__recv_frame( tracker, zmqIn );
            else if( mode == 2 ) tracker__mynano__recv_frame_non_header( tracker, nanoIn, &frameTime );
        }
        if( !gotframe ) {
            if( mode != 0 ) break;
            if( loops > loop ) {
                loop++;
                printf("Starting loop %i\n", loop );
                fseek( fh, 0, SEEK_SET );
                tracker__read_headers( tracker, fh );
                continue;
            }
        }
        if( ( ret = av_read_frame( input_ctx, &packet ) ) < 0 ) break;

        if( video_stream == packet.stream_index ) {
            frameCount++;
            char skipThisFrame = 0;
            if( frameSkip ) {
                if( frameCount % frameSkip ) {
                    //av_packet_unref( &packet );
                    skipThisFrame = 1;
                    //continue;
                }
            }
            myjpeg *jpeg = process_frame( compressor, decoder_ctx, &packet, frameTime, skipThisFrame, &prevframe, &prevtime, dw, dh );
            if( jpeg ) {
                if( mode == 0 ) {
                    if( !wroteJpeg ) {
                        write_jpeg( jpeg, "test.jpg" );
                        wroteJpeg = 1;
                    }
                    else {
                        tjFree( jpeg->data );
                        free( jpeg );
                    }
                }
                if( mode == 1 ) myzmq__send_jpeg( jpeg, zmqOut );
                else if( mode == 2 ) {
                    if( !nanoOut && !wroteJpeg ) {
                        write_jpeg( jpeg, "test.jpg" );
                        wroteJpeg = 1;
                    }
                    else {
                        mynano__send_jpeg( jpeg, nanoOut, srcw, srch, dw, dh );
                    }
                }
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
    
    // flush the decoder
    packet.data = NULL;
    packet.size = 0;
    
    myjpeg *jpeg = process_frame( compressor, decoder_ctx, &packet, 0, 1, NULL, 0, 0, 0 );
    //if( mode == 1 ) myzmq__send_jpeg( jpeg, zmqOut );
    //else if( mode == 2 ) mynano__send_jpeg( jpeg, nanoOut );
    
    tjDestroy(compressor);
    
    avcodec_free_context(&decoder_ctx);
    avformat_close_input(&input_ctx);
    av_buffer_unref(&hw_device_ctx);

    if( zmqIn ) myzmq__del( zmqIn );
    if( zmqOut ) myzmq__del( zmqOut );
    
    return 0;
}
