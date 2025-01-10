#pragma once

#define TRACKER_RANK 0
#define MAX_FILES 10
#define MAX_FILENAME 15
#define MAX_OUTPUT_FILENAME 33
#define HASH_SIZE 32
#define MAX_CHUNKS 100

// 10 clienti maxim, dar numeratom 1 .. 10 fara 0
#define MAX_CLIENTS 11

// tag-uri initializare metadate
#define MSG_INIT_NR_FILES     59000
#define MSG_INIT_FILES        59001

// tag-uri comunicatie tracker-peer
#define MSG_CLIENT_READY_DOWNLOAD     69000
#define MSG_CLIENT_READY_UPLOAD       69001
#define MSG_REQ_FULL_SWARM    69002
#define MSG_REQ_UPDATE_SWARM  69003
#define MSG_SWARM_DATA        69004
#define MSG_UPDATE_SWARM      69005

#define MSG_CHUNK_REQUEST     69006
#define MSG_CHUNK_RESPONSE    69007
#define MSG_FILE_DONE         69008
#define MSG_ALL_DONE          69009
#define MSG_TRACKER_STOP      69010

#define NOT_FOUND -1
#define FIND_NUM_TRIES 2
#define BIG_VALUE 1 << 30
#define PEER_DECISSION_TRESHOLD 1

// structuri date comune folosite in comunicatie
struct identifier {
    char hash[HASH_SIZE];
};

struct file_data {
    char filename[MAX_FILENAME];
    identifier identifiers[MAX_CHUNKS];
    int nr_total_chunks;
};

struct swarm_update {
    bool is_seed[MAX_CLIENTS];
    bool is_peer[MAX_CLIENTS];
};

struct swarm_data {
    swarm_update owners;
    file_data file_metadata;
};
