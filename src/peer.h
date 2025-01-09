#pragma once

#include <vector>

#include "struct.h"

using namespace std;

struct peer_file_data {
    file_data file;
    int nr_owned_chunks;
    bool has_chunk[MAX_CHUNKS]; // true daca detine chunk-ul i
};

struct chunk_request {
    char filename[MAX_FILENAME];
    int chunk_index;
};

struct chunk_response {
    bool has_chunk;
    char hash[HASH_SIZE];
};

class PeerManager {
  private:
  public:
    int rank;
    int numtasks;
    int nr_files;
    int nr_owned_files;
    peer_file_data peer_files[MAX_FILES];

    swarm_data cur_swarm; // folosit pt descarcarea fisierului curent

    void read_input_file();
    void update_swarm();
    bool request_chunk(int rank_request, int file_index, int chunk_index);
    void send_chunk();
    void download_file_using_swarm(int file_index);
    void save_output_file(int index);

  public:
    void DEBUG_PRINT();

    PeerManager(int rank, int numtasks);
    void send_my_nr_files();
    void send_own_files_data();
};

void* download_thread_func(void* arg);
void* upload_thread_func(void* arg);

void peer(int numtasks, int rank);