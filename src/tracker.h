#pragma once

#include "struct.h"

class TrackerManager {
public:
    int numtasks;
    int nr_files;            // cate fisiere exista in total
    int nr_initial_files;    // cate fisiere vor fi procesate initial (pot exista dubluri)
    swarm_data swarms[MAX_FILES]; 

    TrackerManager(int numtasks);

    int find_file_index(const char* filename);

    // functii de initializare
    void receive_nr_files_to_process();
    void receive_all_initial_files_data();
    void signal_clients_to_start();
};

void tracker_main_loop(TrackerManager& tm);
void tracker(int numtasks, int rank);
