typedef struct chunk_s chunk;

typedef struct chunk_tracker_s {
    chunk *curchunk;
    int pos;
} chunk_tracker;

struct chunk_s {
    char type;
    char easyType;
    char *data;
    uint32_t size;
    chunk *next;
    int dtype;
};