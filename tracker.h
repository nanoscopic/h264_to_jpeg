#include<stdint.h>
#include<stdlib.h>
#include<zmq.h>
#include<nanomsg/nn.h>
#include<nanomsg/pipeline.h>
#include<string.h>
#include<sys/time.h>
#include "time.h"
#include "chunk.h"
#include "ujsonin/ujsonin.h"

void chunk__dump( chunk *c );
char chunk__isheader( chunk *c );
chunk *read_chunk_non_header( FILE *fh );

typedef struct myzmq_s {
    void *context;
    void *socket;
} myzmq;

myzmq *myzmq__new( char *spec, int bind ) {
    myzmq *self = calloc( sizeof( myzmq ), 1 );
    self->context = zmq_ctx_new();
    self->socket = zmq_socket( self->context, bind ? ZMQ_PULL : ZMQ_PUSH );
    int rc;
    if( bind ) {
        rc = zmq_bind( self->socket, spec );
        if( rc ) {
            fprintf(stderr, "ZMQ could not bind to %s ; err = %i\n", spec, rc );
            exit(1);
        }
    }
    else {
        rc = zmq_connect( self->socket, spec );
        if( rc ) {
            fprintf(stderr, "ZMQ could not connect to %s ; err = %i\n", spec, rc );
            exit(1);
        }
    }
    
    return self;
}

int mynano__new( char *spec, int bind ) {
    int sock = nn_socket( AF_SP, bind ? NN_PULL : NN_PUSH );
    if( sock < 0 ) { fprintf(stderr, "nanomsg socket creation err: %i\n", sock ); exit(1); }
    int rv;
    if( bind ) rv = nn_bind( sock, spec );
    else       rv = nn_connect( sock, spec );
    if( rv < 0 ) { fprintf(stderr, "nanomsg bind/connect err: %i\n", rv ); exit(1); }
    return sock; 
}

void myzmq__del( myzmq *self ) {
    if( self ) {
        if( self->socket ) zmq_close( self->socket );
        if( self->context ) zmq_ctx_destroy( self->context );
    }
}

char *decode_err( int e ) {
    if( e == EAGAIN ) return "EAGAIN";
    if( e == ENOTSUP ) return "ENOTSUP";
    if( e == EFSM ) return "EFSM";
    if( e == ETERM ) return "ETERM";
    if( e == ENOTSOCK ) return "ENOTSOCK";
    if( e == EINTR ) return "EINTR";
    return "?";
}

char buffer[ 500000 ];

chunk *myzmq__recv_chunk( myzmq *z ) {
    int size = zmq_recv( z->socket, buffer, 500000, 0 );
    if( !size ) return NULL;
    if( size == -1 ) {
        int err = zmq_errno();
        printf("ZMQ error receiving %i ( %s )\n", err, decode_err( err ) );
        return NULL;
    }
    //printf("Received zmq chunk of size %i\n", size );
    chunk *c = calloc( sizeof( chunk ), 1 );
    c->size = size;
    c->data = malloc( size );
    c->type = buffer[4];
    c->dtype = 2;
    memcpy( c->data, buffer, size );
    chunk__dump( c );
    return c;
}

void chunk__del( chunk *c ) {
    if( !c ) return;
    if( c->dtype == 0 ) free( c->data );
    if( c->dtype == 1 ) nn_freemsg( c->rawptr );
    if( c->dtype == 2 ) free( c->data );
    free( c );
}

uint32_t antol( char *str, int len ) {
    char *dup = strndup( str, len );
    uint32_t res = atol( dup );
    free( dup );
    return res;
}

uint32_t nodetol( node_str *node ) {
    if( !node ) return 0;
    return antol( node->str, node->len );
}

uint64_t antoll( char *str, int len ) {
    char *dup = strndup( str, len );
    uint64_t res = atoll( dup );
    free( dup );
    return res;
}

uint64_t nodetoll( node_str *node ) {
    if( !node ) return 0;
    return antoll( node->str, node->len );
}

chunk *mynano__recv_chunk( int n ) {
    char *buf = NULL;
    int size = nn_recv( n, &buf, NN_MSG, 0 );
    if( !size ) return NULL;
    if( size < 0 ) { fprintf(stderr, "nn_recv err %i\n", size ); return NULL; }
    
    //printf("Received nanomsg chunk of size %i\n", size );
    
    chunk *c = calloc( sizeof( chunk ), 1 );
    //c->size = size;
    
    uint16_t jsonLen = * ( (uint16_t *) buf );
    //printf("Size: %i, JSON: %.*s\n", size, jsonLen, &buf[3] );
    
    uint16_t dataStart = jsonLen + 2;
    
    int err = 0;
    node_hash *root = parse( &buf[3], jsonLen, NULL, &err );
    if( !err ) {
        node_str *nalBytesNode = ( node_str * ) node_hash__get( root, "nalBytes", 8 );
        uint32_t nalBytes = nodetol( nalBytesNode );
        if( nalBytes && nalBytes != ( size - dataStart ) ) {
            printf("JSON size doesn't match data payload; %li != %li\n", (long) nalBytes, (long) size - dataStart );
        }
        node_str *timeNode = (node_str *) node_hash__get( root, "time", 4 );
        if( timeNode ) {
            uint64_t time = nodetoll( timeNode );
            c->time = time;
            uint64_t now = now_msec();
            long dif = now_msec() - time;
            //printf("Time str: %.*s\n", timeNode->len, timeNode->str );
            //printf("MS dif: %lli %lli %li\n", (long long) time, (long long) now, dif );
        }
        else {
            printf("JSON has no time node\n");
        }
        node_hash__delete( root );
    }
    
    
    c->size = size - dataStart;
    c->rawptr = buf;
    c->data = &buf[ dataStart ];
    c->type = buf[ 4 + dataStart ];
    c->dtype = 1;
    //printf("%x %x %x %x %x\n", buffer[0], buffer[1], buffer[2], buffer[3], buffer[4] );
    chunk__dump( c );
    return c;
}

void mynano__send_chunk( int n, chunk *c ) {
    nn_send( n, c->data, c->size, 0 );
}

void myzmq__send_chunk( myzmq *z, chunk *c ) {
    zmq_send( z->socket, c->data, c->size, 0 );
}

void myzmq__send( myzmq *z, void *data, int size ) {
    zmq_send( z->socket, data, size, 0 );
}

void mynano__send( int n, void *data, int size ) {
    nn_send( n, data, size, 0 );
}

chunk *read_chunk( FILE *fh );

void tracker__del( chunk_tracker *tracker ) {
    chunk *cur = tracker->curchunk;
    while( cur ) {
        chunk *next = cur->next;
        chunk__del( cur );
        cur = next;
    }
    free( tracker );
}

void tracker__add_chunk( chunk_tracker *tracker, chunk *c ) {
    chunk *curchunk = tracker->curchunk;
    if( !curchunk ) {
        tracker->curchunk = c;
        tracker->pos = 0; // shouldn't be needed
        return;
    }
    while( curchunk->next ) {
        curchunk = curchunk->next;
    }
    curchunk->next = c;
}

void tracker__read_headers( chunk_tracker *tracker, FILE *fh ) {
    int done = 0; 
    while( !done ) {
        chunk *c = read_chunk( fh );
        if( !c ) break;
        
        if( c->type == 6 ) done = 1; // 6 = SEI
        tracker__add_chunk( tracker, c );
    }
}

void chunk__write( chunk *c, FILE *fh );

void tracker__write_file( chunk_tracker *tracker, FILE *fh ) {
    chunk *cur = tracker->curchunk;
    while( cur ) {
        chunk *next = cur->next;
        chunk__write( cur, fh );
        cur = next;
    }
}

void tracker__myzmq__send_chunks( chunk_tracker *tracker, myzmq *z ) {
    chunk *c = tracker->curchunk;
    while( c ) {
        myzmq__send_chunk( z, c );
        c = c->next;
    }
    tracker->curchunk = NULL;
    tracker->pos = 0;
}

void tracker__mynano__send_chunks( chunk_tracker *tracker, int n ) {
    chunk *c = tracker->curchunk;
    while( c ) {
        mynano__send_chunk( n, c );
        c = c->next;
    }
    tracker->curchunk = NULL;
    tracker->pos = 0;
}

void tracker__myzmq__recv_headers( chunk_tracker *tracker, myzmq *z ) {
    int done = 0;
    while( !done ) {
        chunk *c = myzmq__recv_chunk( z );
        if( !c ) break;
        if( c->type == 0 ) continue; // dummy chunk to initialize zmq
        
        if( c->type == 6 ) done = 1;
        tracker__add_chunk( tracker, c );
    }
}


//"SEI", // 6
//"SPS", // 7
//"PPS"  // 8
char tracker__mynano__recv_headers( chunk_tracker *tracker, int n ) {
    char gotSei = 0;
    char gotSps = 0;
    char gotPps = 0;
    while( !gotSei || !gotSps || !gotPps ) {
        chunk *c = mynano__recv_chunk( n );
        if( !c ) break;
        if( c->type == 0 ) continue; // dummy chunk to initialize zmq
        
        if( c->easyType == 6 ) gotSei = 1;
        else if( c->easyType == 7 ) gotSps = 1;
        else if( c->easyType == 8 ) gotPps = 1;
        else {
            printf("Got chunk type %i while trying to receive headers\n", c->easyType );
            return 0;
        }
        tracker__add_chunk( tracker, c );
    }
    return 1;
}

int tracker__read_frame( chunk_tracker *tracker, FILE *fh ) {
    chunk *c = read_chunk_non_header( fh );
    if( c ) {
        tracker__add_chunk( tracker, c );
        return 1;
    }
    printf("Could not fetch frame chunk\n");
    return 0;
}

int tracker__myzmq__recv_frame( chunk_tracker *tracker, myzmq *z ) {
    chunk *c = myzmq__recv_chunk( z );
    if( c ) {
        tracker__add_chunk( tracker, c );
        return 1;
    }
    printf("Could not fetch frame chunk\n");
    return 0;
}

int tracker__mynano__recv_frame( chunk_tracker *tracker, int n ) {
    chunk *c = mynano__recv_chunk( n );
    if( c ) {
        tracker__add_chunk( tracker, c );
        //printf("Frame type %i\n", c->easyType );
        return 1;
    }
    printf("Could not fetch frame chunk\n");
    return 0;
}

int tracker__mynano__recv_frame_non_header( chunk_tracker *tracker, int n, uint64_t *time ) {
    while( 1 ) {
        chunk *c = mynano__recv_chunk( n );
        if( !c ) {
            printf("Could not fetch frame chunk\n");
            return 0;
        }
        if( chunk__isheader( c ) ) continue;
        if(time) *time = c->time;
        tracker__add_chunk( tracker, c );
        return 1;
    }
    //unreachable
    return 0;
}

struct timespec *lastI = NULL, *nextI = NULL;
char *naltypes[9] = {
    NULL, // 0
    NULL, // 1
    NULL, // 2
    NULL, // 3
    NULL, // 4
    NULL, // 5
    "SEI", // 6
    "SPS", // 7
    "PPS" // 8
};
char chunk__isheader( chunk *c ) {
    char t = c->type;
    int forbidden_zero_bit = ( t & 0x80 ) >> 7;
    int ref_idc = ( t & 0x60 ) >> 5;
    int type = ( t & 0x1F );
    return ( type == 6 || type == 7 || type == 8 );
}
void chunk__dump( chunk *c ) {
    char t = c->type;
    int forbidden_zero_bit = ( t & 0x80 ) >> 7;
    int ref_idc = ( t & 0x60 ) >> 5;
    int type = ( t & 0x1F );
    c->easyType = type;
    if( type == 1 ) {
        //if( ref_idc ) printf(".");
        //else printf("x");
    }
    else {
        if( type == 5 ) {
            if( !nextI ) nextI = malloc( sizeof( struct timespec ) ); 
            clock_gettime( CLOCK_MONOTONIC, nextI );
            if( lastI ) {
                // display time difference
                uint64_t dif = timespecDiff( nextI, lastI );
                printf(" Iframe - size: %li - Timediff:%f\n", (long) c->size, (double) dif / ( double ) 1000000 );
                free( lastI );
                lastI = nextI;
                nextI = NULL;
            }
            else {
                lastI = nextI;
                nextI = NULL;
                printf(" Iframe - size: %li\n", (long) c->size );
            }
        }
        else {
            if( type <= 9 && naltypes[type] ) {
                printf("nalu type: %s, size: %li\n", naltypes[type], (long) c->size );
            }
            else printf("nalu type: %i, size: %li\n", type, (long) c->size );
        }
    }
}

int findseq( char *data, uint32_t len ) {
    for( uint32_t i=0;i<=(len-4);i++ ) { // len-1 is last byte ; len-4 is start of last 4 sequence; ex: len-4,len-3,len-2,len-1
        if( data[ i ] == 0x00 && data[ i+1 ] == 0x00 && data[ i+2 ] == 0x00 && data[ i+3 ] == 0x01 ) {
            return i;
        }
    }
    return -1;
}

chunk *read_chunk( FILE *fh ) {
    char m[4];
    int read = fread( m, 4, 1, fh );
    if( !read ) return NULL;
    if(  m[2] == '{' ) { // b1 = size byte 1, b2 = size byte 2, b3 = {, b4 = first byte of json
        uint16_t size = * ( ( uint16_t * ) m );
        char *jsonbuffer = malloc( size );
        jsonbuffer[0] = '{';
        jsonbuffer[1] = m[3];
        fread( jsonbuffer + 2, 1, size - 2, fh );
        
        // handle the JSON
        
        fread( m, 1, 4, fh );
    }
    { // this file was written by standard qvh; no JSON header
        uint32_t buffersize = 2000;
        uint32_t bufferleft = 2000;
        uint32_t bufferpos = 4;
        int increment = 1000;
        char *buffer = malloc( buffersize );
        if( m[0] == 0x00 && m[1] == 0x00 && m[2] == 0x00 && m[3] == 0x01 ) {
            buffer[0] = 0x00;
            buffer[1] = 0x00;
            buffer[2] = 0x00;
            buffer[3] = 0x01;
            //fread( buffer, 1, 1000, fh );
            /*bufferpos += 1000;
            for( int j=0;j<1000;j++ ) {
                printf("%02x  ", buffer[j] & 0xFF );
            }*/
            for( int i=0;i<5000; i++ ) { // limit this loop to increment*5000 = 5 megabytes; no NALU should be that big
                bufferleft = buffersize - bufferpos;
                if( bufferleft < ( increment + 3 ) ) {
                    uint32_t newsize = buffersize * 2;
                    char *newbuf = malloc( newsize );
                    memcpy( newbuf, buffer, bufferpos );
                    free( buffer );
                    buffer = newbuf;
                    buffersize *= 2;
                    //bufferleft = newsize - bufferpos;
                }
                size_t readbytes = fread( &buffer[ bufferpos ], 1, increment, fh );
                if( readbytes < increment ) {
                    printf("Reached end of file\n");
                    return NULL;
                    buffer[ bufferpos + readbytes ] = 0x00;
                    buffer[ bufferpos + readbytes +1 ] = 0x00;
                    buffer[ bufferpos + readbytes +2 ] = 0x00;
                    buffer[ bufferpos + readbytes +3 ] = 0x01;
                    readbytes += 4;
                }
                int seqpos;
                if( bufferpos ) seqpos = findseq( &buffer[ bufferpos - 3 ], readbytes+3 );
                else            seqpos = findseq( &buffer[ bufferpos ], readbytes );
                //if( seqpos != -1 ) printf("seqpos: %i\n", seqpos );
                
                if( seqpos == -1 ) {
                    bufferpos += readbytes;
                    continue;
                }
                // found the sequence; stop
                if( bufferpos ) {
                    seqpos -= 3;
                    bufferpos += seqpos;
                    // increment - seqpos = data read in we should not have
                    fseek( fh, - ( readbytes - seqpos ), SEEK_CUR );
                }
                else {
                    bufferpos += seqpos;
                    fseek( fh, - ( readbytes - seqpos ), SEEK_CUR );
                }
                
                //printf("Size: %lli\n", ( long long ) bufferpos );
                chunk *c = calloc( sizeof( chunk ), 1 );
                c->data = buffer;
                c->type = buffer[4];
                c->size = bufferpos;
                c->dtype = 0;
                chunk__dump( c );
                return c;
            }
        }
    }
    
    
    fprintf(stderr, "not magic; not a good sign\n");
    return NULL;
}

chunk *read_chunk_non_header( FILE *fh ) {
    while( 1 ) {
        chunk *c = read_chunk( fh );
        if( !c ) return NULL;
        if( !chunk__isheader( c ) ) return c;
        chunk__dump( c );
    }
}

void chunk__write( chunk *c, FILE *fh ) {
    char m[4] = { 0,0,0,0x01 };
    fwrite( m, 1, 4, fh );
    char sz[4];
    long int datasize = c->size;
    sz[0] = ( datasize & 0xFF000000 ) >> 24;
    sz[1] = ( datasize & 0x00FF0000 ) >> 16;
    sz[2] = ( datasize & 0x0000FF00 ) >> 8;
    sz[3] = datasize & 0xFF;
    fwrite( sz, 1, 4, fh );
    fwrite( &c->data[4], 1, datasize, fh );  
}

chunk_tracker *tracker__new() {
    chunk_tracker *tracker = calloc( sizeof( chunk_tracker ), 1 );
    tracker->curchunk = NULL;
    tracker->pos = 0;
    return tracker;
}