#include <mpi.h>
#include <pthread.h>
#include <iostream>
#include <vector>
#include <cstring>
#include <unistd.h>
#include "struct.h"
#include "tracker.h"

using namespace std;

TrackerManager::TrackerManager(int numtasks) : numtasks(numtasks) {
    nr_files = 0;
    nr_initial_files = 0;
    for (int i = 0; i < MAX_FILES; i++) {
        // initialize is_seed / is_peer
        for (int j = 0; j < MAX_CLIENTS; j++) {
            swarms[i].update.is_seed[j] = false;
            swarms[i].update.is_peer[j] = false;
        }
        swarms[i].file_metadata.nr_total_chunks = 0;
        memset(swarms[i].file_metadata.filename, 0, MAX_FILENAME);
    }
}

int TrackerManager::find_file_index(const char* filename) {
    for (int i = 0; i < nr_files; i++) {
        if (strcmp(swarms[i].file_metadata.filename, filename) == 0) {
            return i;
        }
    }
    return NOT_FOUND;
}

void TrackerManager::DEBUG_PRINT() {
    cout << "[Tracker] We have nr_files = " << nr_files << endl;
    for (int i = 0; i < nr_files; i++) {
        file_data& fd = swarms[i].file_metadata;
        cout << "  File #" << i << ": " << fd.filename 
             << " with " << fd.nr_total_chunks << " chunks.\n";

        // owners
        cout << "    Seeds: ";
        for (int r = 0; r < MAX_CLIENTS; r++) {
            if (swarms[i].update.is_seed[r]) {
                cout << r << " ";
            }
        }
        cout << "\n    Peers: ";
        for (int r = 0; r < MAX_CLIENTS; r++) {
            if (swarms[i].update.is_peer[r]) {
                cout << r << " ";
            }
        }
        cout << endl;
    }
}

void TrackerManager::receive_nr_files_to_process() {
    nr_initial_files = 0;
    for (int rank = 1; rank < numtasks; rank++) {
        int num;
        MPI_Recv(&num, 1, MPI_INT, rank, MSG_INIT_NR_FILES, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        nr_initial_files += num;
    }
}

void TrackerManager::receive_all_initial_files_data() {
    nr_files = 0;
    for (int i = 0; i < nr_initial_files; i++) {
        MPI_Status status;
        file_data temp_file;
        MPI_Recv(&temp_file, sizeof(file_data), MPI_BYTE, MPI_ANY_SOURCE,
                 MSG_INIT_FILES, MPI_COMM_WORLD, &status);
        int sender_rank = status.MPI_SOURCE;

        cout << "[Tracker] Received file " << temp_file.filename 
             << " from peer " << sender_rank << endl;

        // see if already known
        int found_index = find_file_index(temp_file.filename);
        if (found_index == NOT_FOUND) {
            found_index = nr_files;
            nr_files++;
        }

        // copy the file metadata
        swarms[found_index].file_metadata = temp_file;

        // mark sender as seed
        swarms[found_index].update.is_seed[sender_rank] = true;
        swarms[found_index].update.is_peer[sender_rank] = false; // fully owns the file
    }
}

void TrackerManager::signal_clients_to_start() {
    for (int rank = 1; rank < numtasks; rank++) {
        MPI_Send(nullptr, 0, MPI_CHAR, rank, MSG_TRACKER_READY, MPI_COMM_WORLD);
    }
}

// The main loop that processes messages from peers
void tracker_main_loop(TrackerManager& tm) {
    int nr_done_clients = 0;
    bool finished = false;

    while (!finished) {
        MPI_Status status;
        MPI_Probe(MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &status);

        int tag = status.MPI_TAG;
        int source = status.MPI_SOURCE;

        switch(tag) {
            case MSG_REQ_FULL_SWARM: {
                char filename[MAX_FILENAME];
                MPI_Recv(filename, MAX_FILENAME, MPI_CHAR, source, MSG_REQ_FULL_SWARM,
                         MPI_COMM_WORLD, MPI_STATUS_IGNORE);

                int idx = tm.find_file_index(filename);
                if (idx == NOT_FOUND) {
                    // consume message, do something, or create a placeholder
                    swarm_data emptyData;
                    memset(&emptyData, 0, sizeof(swarm_data));
                    MPI_Send(&emptyData, sizeof(swarm_data), MPI_BYTE,
                             source, MSG_SWARM_DATA, MPI_COMM_WORLD);
                } else {
                    // mark sender as peer
                    tm.swarms[idx].update.is_peer[source] = true;
                    // send swarm data
                    MPI_Send(&(tm.swarms[idx]), sizeof(swarm_data), MPI_BYTE,
                             source, MSG_SWARM_DATA, MPI_COMM_WORLD);
                }
                break;
            }

            case MSG_REQ_UPDATE_SWARM: {
                char filename[MAX_FILENAME];
                MPI_Recv(filename, MAX_FILENAME, MPI_CHAR, source, MSG_REQ_UPDATE_SWARM,
                         MPI_COMM_WORLD, MPI_STATUS_IGNORE);

                int idx = tm.find_file_index(filename);
                if (idx == NOT_FOUND) {
                    // send back an empty swarm_update
                    swarm_update emptyUpd;
                    memset(&emptyUpd, 0, sizeof(swarm_update));
                    MPI_Send(&emptyUpd, sizeof(swarm_update), MPI_BYTE,
                             source, MSG_UPDATE_SWARM, MPI_COMM_WORLD);
                } else {
                    MPI_Send(&(tm.swarms[idx].update), sizeof(swarm_update), MPI_BYTE,
                             source, MSG_UPDATE_SWARM, MPI_COMM_WORLD);
                }
                break;
            }

            case MSG_FILE_DONE: {
                char filename[MAX_FILENAME];
                MPI_Recv(filename, MAX_FILENAME, MPI_CHAR, source, MSG_FILE_DONE,
                         MPI_COMM_WORLD, MPI_STATUS_IGNORE);

                int idx = tm.find_file_index(filename);
                if (idx != NOT_FOUND) {
                    // mark source as seed for that file
                    tm.swarms[idx].update.is_seed[source] = true;
                    tm.swarms[idx].update.is_peer[source] = false;
                }
                break;
            }

            case MSG_ALL_DONE: {
                MPI_Recv(nullptr, 0, MPI_CHAR, source, MSG_ALL_DONE, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
                cout << "[Tracker] Peer " << source << " has finished all downloads.\n";
                nr_done_clients++;

                if (nr_done_clients == (tm.numtasks - 1)) {
                    // all done
                    cout << "[Tracker] All peers done. Stopping...\n";
                    // send MSG_TRACKER_STOP to all
                    for (int r = 1; r < tm.numtasks; r++) {
                        MPI_Send(nullptr, 0, MPI_CHAR, r, MSG_TRACKER_STOP, MPI_COMM_WORLD);
                    }
                    finished = true;
                }
                break;
            }

            default: {
                // unknown message → must consume it
                MPI_Recv(nullptr, 0, MPI_CHAR, source, tag, MPI_COMM_WORLD, &status);
                cerr << "[Tracker] Unknown tag " << tag 
                     << " from peer " << source << endl;
                break;
            }
        }
    }
}

void tracker(int numtasks, int rank) {
    cout << "[Tracker] Starting at rank " << rank << endl;

    TrackerManager tm(numtasks);

    // 1. gather how many owned files from each peer
    tm.receive_nr_files_to_process();
    MPI_Barrier(MPI_COMM_WORLD);

    // 2. gather metadata of those files
    tm.receive_all_initial_files_data();
    MPI_Barrier(MPI_COMM_WORLD);

    // optional: a small debug
    tm.DEBUG_PRINT();

    // 3. give them the green light
    sleep(0.8);
    tm.signal_clients_to_start();

    // 4. main loop
    tracker_main_loop(tm);

    cout << "[Tracker] Exiting now.\n";
}


