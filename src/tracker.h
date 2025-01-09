#pragma once

#include "struct.h"

#define NOT_FOUND -1

class TrackerManager {
public:
    int numtasks;
    int nr_files;            // how many distinct files exist across all seeds
    int nr_initial_files;    // how many total "owned files" across all peers
    swarm_data swarms[MAX_FILES]; 

    TrackerManager(int numtasks);

    int find_file_index(const char* filename);
    void DEBUG_PRINT();

    void receive_nr_files_to_process();
    void receive_all_initial_files_data();
    void signal_clients_to_start();
};

void tracker_main_loop(TrackerManager& tm);
void tracker(int numtasks, int rank);
