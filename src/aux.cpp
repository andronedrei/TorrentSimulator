#include <mpi.h>
#include <pthread.h>
#include <iostream>
#include <vector>
#include <unistd.h>
#include "struct.h"
#include "tracker.h"

using namespace std;

void TrackerManager::DEBUG_PRINT() {
    // ...
}

TrackerManager::TrackerManager(int numtasks) : numtasks(numtasks) {
    for (int i = 0; i < MAX_FILES; i++) {
        for (int j = 0; j < MAX_CLIENTS; j++) {
            swarms[i].is_seed[j] = false;
            swarms[i].is_peer[j] = false;
        }
    }
    nr_files = 0;
    nr_initial_files = 0;
}

// Step 1: gather how many initial files each peer has
void TrackerManager::receive_nr_files_to_process() {
    nr_initial_files = 0;
    int num;
    for (int rank = 1; rank < numtasks; rank++) {
        MPI_Recv(&num, 1, MPI_INT, rank, INITIAL_NR_FILES, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        nr_initial_files += num;
    }
}

// Step 2: receive metadata of all initial files
void TrackerManager::receive_all_initial_files_data() {
    nr_files = 0;
    for (int i = 0; i < nr_initial_files; i++) {
        MPI_Status status;
        file_data* cur_file_p = &tracker_files[nr_files];
        MPI_Recv(cur_file_p, sizeof(file_data), MPI_BYTE, MPI_ANY_SOURCE, INITIAL_FILES, MPI_COMM_WORLD, &status);
        int sender_rank = status.MPI_SOURCE;

        cout << "Received file: " << cur_file_p->filename << " from " << sender_rank << endl;

        // check if already known
        int found_index = NOT_FOUND;
        for (int j = 0; j < nr_files; j++) {
            if (strcmp(tracker_files[j].filename, cur_file_p->filename) == 0) {
                found_index = j;
                break;
            }
        }

        if (found_index == NOT_FOUND) {
            found_index = nr_files;
            nr_files++;
        }
        // update data in tracker_files
        tracker_files[found_index] = *cur_file_p;

        // mark the sender as seed
        swarms[found_index].is_seed[sender_rank] = true;
    }
}

// Helper that returns index in tracker_files of a given filename
int TrackerManager::findFileIndex(const char* filename) {
    for (int i = 0; i < nr_files; i++) {
        if (strcmp(tracker_files[i].filename, filename) == 0) {
            return i;
        }
    }
    return NOT_FOUND;
}

// This is a placeholder for the "main loop" that processes messages from peers 
void tracker_main_loop(TrackerManager& tm) {
    int done_clients = 0;
    bool finished = false;

    // After receiving all initial data, we let peers start 
    // by sending them each a MSG_TRACKER_ACK
    for (int r = 1; r < tm.numtasks; r++) {
        MPI_Send(nullptr, 0, MPI_CHAR, r, MSG_TRACKER_ACK, MPI_COMM_WORLD);
    }

    while (!finished) {
        MPI_Status status;
        // Probing any incoming message from any peer
        MPI_Probe(MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &status);

        int source = status.MPI_SOURCE;
        int tag = status.MPI_TAG;

        // We don't expect large data, so let's just handle with small buffers
        if (tag == MSG_REQ_SWARM) {
            // 1) read the filename from peer
            char filename[MAX_FILENAME];
            MPI_Recv(filename, MAX_FILENAME, MPI_CHAR, source, MSG_REQ_SWARM, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            
            int idx = tm.findFileIndex(filename);
            // 2) Build a list of seeds/peers for that file
            //    (here we just store them in a local vector)
            vector<int> owners;
            for (int c = 1; c < tm.numtasks; c++) {
                if (tm.swarms[idx].is_seed[c] || tm.swarms[idx].is_peer[c]) {
                    owners.push_back(c);
                }
            }
            // 3) Mark the requesting client as 'is_peer' for that file
            tm.swarms[idx].is_peer[source] = true;

            // 4) Send back the swarm data
            int owners_size = owners.size();
            MPI_Send(&owners_size, 1, MPI_INT, source, MSG_SWARM_DATA, MPI_COMM_WORLD);
            MPI_Send(owners.data(), owners_size, MPI_INT, source, MSG_SWARM_DATA, MPI_COMM_WORLD);

        } else if (tag == MSG_FILE_DONE) {
            // Peer tells: "I finished downloading file <filename>"
            char filename[MAX_FILENAME];
            MPI_Recv(filename, MAX_FILENAME, MPI_CHAR, source, MSG_FILE_DONE, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            
            int idx = tm.findFileIndex(filename);
            // Mark the peer as seed
            tm.swarms[idx].is_seed[source] = true;
            tm.swarms[idx].is_peer[source] = false;  // or keep it peer + seed, up to you

        } else if (tag == MSG_ALL_DONE) {
            // This peer finished all downloads it wants
            MPI_Recv(nullptr, 0, MPI_CHAR, source, MSG_ALL_DONE, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            
            cout << "Peer " << source << " finished all downloads.\n";
            done_clients++;

            // We do NOT remove it from swarms: it still can serve as seed.

            // If all peers are done, notify everyone to stop.
            if (done_clients == tm.numtasks - 1) {
                cout << "All peers are done. Stopping...\n";
                for (int r = 1; r < tm.numtasks; r++) {
                    MPI_Send(nullptr, 0, MPI_CHAR, r, MSG_TRACKER_STOP, MPI_COMM_WORLD);
                }
                finished = true;
            }
        } else {
            // If we get something else, let's just consume it 
            MPI_Recv(nullptr, 0, MPI_CHAR, source, tag, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            cerr << "[Tracker] Unknown tag " << tag << " from rank " << source << endl;
        }
    }
}

void tracker(int numtasks, int rank) {
    cout << "Tracker initialized at rank: " << rank << endl;

    TrackerManager tracker_manager(numtasks);

    // Step 1
    tracker_manager.receive_nr_files_to_process();
    MPI_Barrier(MPI_COMM_WORLD);

    cout << "Tracker received nr files: " << tracker_manager.nr_initial_files << endl;

    // Step 2
    tracker_manager.receive_all_initial_files_data();
    MPI_Barrier(MPI_COMM_WORLD);

    // Now we can run a loop that processes messages from peers
    tracker_main_loop(tracker_manager);

    cout << "Tracker shutting down.\n";
}
