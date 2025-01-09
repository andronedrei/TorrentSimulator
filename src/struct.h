#pragma once

#define TRACKER_RANK 0
#define MAX_FILES 10
#define MAX_FILENAME 15
#define MAX_OUTPUT_FILENAME 33
#define HASH_SIZE 32
#define MAX_CHUNKS 100

// We assume ranks go from 0..N-1
// Let's allow up to 11 clients (rank 1..10)
#define MAX_CLIENTS 11

// tag-uri initializare metadate
#define MSG_INIT_NR_FILES     10000
#define MSG_INIT_FILES        10001

// tag-uri comunicatie tracker-peer
#define MSG_TRACKER_READY     20000
#define MSG_REQ_FULL_SWARM    20001
#define MSG_REQ_UPDATE_SWARM  20002
#define MSG_SWARM_DATA        20003
// CORRECTED from 2004 -> 20004
#define MSG_UPDATE_SWARM      20004

#define MSG_CHUNK_REQUEST     20005
#define MSG_CHUNK_RESPONSE    20006
#define MSG_FILE_DONE         20007
#define MSG_ALL_DONE          20008
#define MSG_TRACKER_STOP      20009

// Data structures
struct identifier {
    char hash[HASH_SIZE];
};

struct file_data {
    char filename[MAX_FILENAME];
    identifier identifiers[MAX_CHUNKS];
    int nr_total_chunks;
};

struct swarm_update {
    bool is_seed[MAX_CLIENTS]; // if a client fully owns the file
    bool is_peer[MAX_CLIENTS]; // if a client partially owns the file
};

struct swarm_data {
    swarm_update update;
    file_data file_metadata;
};
