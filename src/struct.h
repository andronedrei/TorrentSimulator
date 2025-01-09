#pragma once

#define TRACKER_RANK 0
#define MAX_FILES 10
#define MAX_FILENAME 15
#define MAX_OUTPUT_FILENAME 33
#define HASH_SIZE 32
#define MAX_CHUNKS 100

#define MAX_CLIENTS 10

// tag-uri initializare metadate
#define MSG_INIT_NR_FILES 10000
#define MSG_INIT_FILES 10001

// tag-uri comunicatie tracker-peer
#define MSG_TRACKER_READY 20000 
#define MSG_REQ_FULL_SWARM 20001
#define MSG_REQ_UPDATE_SWARM 20002
#define MSG_SWARM_DATA 20003
#define MSG_UPDATE_SWARM 2004
#define MSG_CHUNK_REQUEST 20005
#define MSG_CHUNK_RESPONSE 20006
#define MSG_FILE_DONE 20007    // A peer informs tracker "I finished file X"
#define MSG_ALL_DONE 20008    // A peer informs tracker "I finished all my files"
#define MSG_TRACKER_STOP 20009    // Tracker tells all peers "everyone done, stop"

struct identifier {
    char hash[HASH_SIZE];
};

struct file_data {
    char filename[MAX_FILENAME];
    identifier identifiers[MAX_CHUNKS];
    int nr_total_chunks;
};

struct swarm_update {
    bool is_seed[MAX_CLIENTS]; // daca un client detine un fisier full, atunci is_seed[client] = true
    bool is_peer[MAX_CLIENTS]; // cazul cand nu detine fiserul full
};

struct swarm_data {
    swarm_update update;
    file_data file_metadata;
};
