#pragma once

using namespace std;

struct chunk_request {
    char filename[MAX_FILENAME];
    int chunk_index;
};

struct chunk_response {
    bool has_chunk;
    char hash[HASH_SIZE];
};

class PeerManager {
public:
    int rank;
    int numtasks;
    int nr_files; // nr total file-uri, detinute + dorite
    int nr_owned_files;

    // datele despre fisierele detinute si dorite
    file_data files[MAX_FILES];
    int nr_owned_chunks[MAX_FILES];
    
    // Swarm-ul cerut de la tracker
    swarm_data cur_swarm;
    // Array in care tinem minte de cate ori am apelat la fiecare peer cu un chunk
    int used_peer[MAX_CLIENTS];

    int DEBUG_NR_HELPS;

    PeerManager(int rank, int numtasks);

    void read_input_file();

    // functii de initializare
    void send_my_nr_files();
    void send_own_files_data();

    void update_swarm(char* filename);
    bool request_chunk(int rank_request, int file_index, int chunk_index);
    void send_chunk(int rank_request);
    int find_seed_for_chunk(); // returneaza rank-ul celui mai bun peer pentru un chunk (folosit cel mai putin)
    void download_file_using_swarm(int file_index);
    void save_output_file(int index);
};

void* download_thread_func(void* arg);
void* upload_thread_func(void* arg);

void peer(int numtasks, int rank);
