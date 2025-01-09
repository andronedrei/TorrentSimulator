#pragma once

#include "struct.h"

#define NOT_FOUND -1

class TrackerManager {
  private:
  public:
    int numtasks;
    int nr_files;
    int nr_initial_files;
    swarm_data swarms[MAX_FILES]; 

  public:
    int find_file_index(char* filename);
    void DEBUG_PRINT();

    TrackerManager(int numtasks);
    void receive_nr_files_to_process();
    void receive_all_initial_files_data();
    void signal_clients_to_start();
};


void tracker_main_loop(TrackerManager& tm);
void tracker(int numtasks, int rank);
