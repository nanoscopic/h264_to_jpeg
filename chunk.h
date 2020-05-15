typedef struct chunk_s chunk;

typedef struct chunk_tracker_s {
    chunk *curchunk;
    int pos;
} chunk_tracker;

struct chunk_s {
    char type;
    char easyType;
    char *data;
    char *rawptr;
    uint32_t size;
    chunk *next;
    int dtype;
    uint64_t time;
};