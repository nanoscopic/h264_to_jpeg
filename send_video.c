#include<stdio.h>

#include <time.h>

int64_t timespecDiff(struct timespec *timeA_p, struct timespec *timeB_p) {
  return ((timeA_p->tv_sec * 1000000000) + timeA_p->tv_nsec) - ((timeB_p->tv_sec * 1000000000) + timeB_p->tv_nsec);
}

#include"tracker.h"

int main( int argc, char *argv[] ) {
    if( argc < 4 ) {
        fprintf(stderr, "Usage: %s <input file> <zmq/nano>\n", argv[0]);
        return -1;
    }

    FILE *fh = fopen( argv[1], "rb" );
    if( !fh ) {
        fprintf( stderr, "Cannot open input file '%s'\n", argv[1] );
        return -1;
    }
    
    chunk_tracker *tracker = tracker__new();
    
    tracker__read_headers( tracker, fh );
    
    int mode = 0;
    char *modeZ = argv[2];
    if( !strncmp( modeZ, "zmq", 3 ) ) {
        mode = 1;
    }
    else if( !strncmp( modeZ, "nano", 4 ) ) {
        mode = 2;
    }
    
    char *spec = argv[3];//"tcp://localhost:7878";
    
    if( mode == 1 ) {
        myzmq *z = myzmq__new( spec, 0 );
        tracker__myzmq__send_chunks( tracker, z );
        
        int frameCount = 1;
        while( 1 ) {
            int gotframe = tracker__read_frame( tracker, fh );
            tracker__myzmq__send_chunks( tracker, z );
            if( !gotframe ) break;
            frameCount++;
            printf( "Frame: %i\r", frameCount );
        }
    }
    if( mode == 2 ) {
        int nanoOut = mynano__new( spec, 0 );
        tracker__mynano__send_chunks( tracker, nanoOut );
        
        int frameCount = 1;
        while( 1 ) {
            int gotframe = tracker__read_frame( tracker, fh );
            tracker__mynano__send_chunks( tracker, nanoOut );
            if( !gotframe ) break;
            frameCount++;
            printf( "Frame: %i\r", frameCount );
        }
    }
    
    printf("\nReached end of video file\n");

    return 0;
}