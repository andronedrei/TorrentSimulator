#include <mpi.h>
#include <pthread.h>

#include <iostream>
#include <vector>

#include <unistd.h>

#include "struct.h"
#include "tracker.h"

using namespace std;

void TrackerManager::DEBUG_PRINT() {
    cout << "Nr files: " << nr_files << endl;
    for (int i = 0; i < nr_files; i++) {
        cout << "File " << i << ": " << swarms[i].file_metadata.filename << endl;
        cout << "Nr total chunks: " << swarms[i].file_metadata.nr_total_chunks << endl;
        for (int j = 0; j < swarms[i].file_metadata.nr_total_chunks; j++) {
            cout << "Chunk " << j << ": ";
            for (int k = 0; k < HASH_SIZE; k++) {
                cout << swarms[i].file_metadata.identifiers[j].hash[k];
            }
            cout << endl;
        }

        // printam ownerii
        for (int j = 1; j <= MAX_CLIENTS; j++) {
            if (swarms[i].update.is_seed[j]) {
                cout << "Seed: " << j << endl;
            }
            if (swarms[i].update.is_peer[j]) {
                cout << "Peer: " << j << endl;
            }
        }
    }
}

int TrackerManager::find_file_index(char* filename) {
    for (int i = 0; i < nr_files; i++) {
        if (strcmp(swarms[i].file_metadata.filename, filename) == 0) {
            return i;
        }
    }
    return NOT_FOUND;
}

TrackerManager::TrackerManager(int numtasks) : numtasks(numtasks) {
    for (int i = 0; i < MAX_FILES; i++) {
        for (int j = 0; j < MAX_CLIENTS; j++) {
            swarms[i].update.is_seed[j] = false;
            swarms[i].update.is_peer[j] = false;
        }
    }
}

void TrackerManager::receive_nr_files_to_process() {
    nr_initial_files = 0;
    int num;

    // calculam numarul de fisiere pe care le vom primi
    for (int rank = 1; rank <= numtasks - 1; rank++) {
        MPI_Recv(&num, 1, MPI_INT, rank, MSG_INIT_NR_FILES, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        nr_initial_files += num;
    }
}

// primeste date despre un fisier initial de la un peer
void TrackerManager::receive_all_initial_files_data() {
    nr_files = 0;

    for (int i = 0; i < nr_initial_files; i++) {
        MPI_Status status;
        file_data* cur_file_p = &(swarms[nr_files].file_metadata);
        int sender_rank;


        MPI_Recv(cur_file_p, sizeof(file_data), MPI_BYTE, MPI_ANY_SOURCE, MSG_INIT_FILES, MPI_COMM_WORLD, &status);
        sender_rank = status.MPI_SOURCE;
        cout << "Received file: " << cur_file_p->filename << " from " << sender_rank << endl;

        // verificam daca fisierul este deja in lista
        int found_index = find_file_index(cur_file_p->filename);

        /* Daca fisierul exista deja, nu incrementam nr_files pt a suprascrie.
        Daca fisierul nu exista, incrementam nr_files. */
        if (found_index == NOT_FOUND) {
            found_index = nr_files;
            nr_files++;
        }

        // facem update la swarm (marcand senderul ca seed)
        swarms[found_index].update.is_seed[sender_rank] = true;
    }
}

void TrackerManager::signal_clients_to_start() {
    for (int rank = 1; rank <= numtasks - 1; rank++) {
        MPI_Send(nullptr, 0, MPI_BYTE, rank, MSG_TRACKER_READY, MPI_COMM_WORLD);
    }
}

void tracker_main_loop(TrackerManager& tm) {
    int nr_done_clients = 0;
    bool finished = false;

    while (!finished) {
        MPI_Status status;

        // probam pentru mesaje primite de la clienti
        MPI_Probe(MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &status);

        int tag = status.MPI_TAG;
        int source = status.MPI_SOURCE;
        char filename[MAX_FILENAME];
        int idx; // index fisier swarm

        switch(tag) {
            // cazul cand un client cere swarm-ul pentru a putea downloada un fisier
            case MSG_REQ_FULL_SWARM:
                MPI_Recv(filename, MAX_FILENAME, MPI_CHAR, source, MSG_REQ_FULL_SWARM, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            
                idx = tm.find_file_index(filename);

                // marcam sender-ul ca peer
                tm.swarms[idx].update.is_peer[source] = true;

                // trimitem inapoi datele despre swarm
                MPI_Send(&(tm.swarms[idx]), sizeof(swarm_data), MPI_BYTE, source, MSG_SWARM_DATA, MPI_COMM_WORLD);
                break;

            // cazul cand un client cere update la swarm
            case MSG_REQ_UPDATE_SWARM:
                MPI_Recv(filename, MAX_FILENAME, MPI_CHAR, source, MSG_REQ_UPDATE_SWARM, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            
                idx = tm.find_file_index(filename);

                // trimitem inapoi update-ul la swarm
                MPI_Send(&(tm.swarms[idx].update), sizeof(swarm_update), MPI_BYTE, source, MSG_UPDATE_SWARM, MPI_COMM_WORLD);
                break;

            // cazul cand un client a terminat de downloadat un fisier
            case MSG_FILE_DONE:
                MPI_Recv(filename, MAX_FILENAME, MPI_CHAR, source, MSG_FILE_DONE, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            
                idx = tm.find_file_index(filename);

                // marcam sender-ul ca seed
                tm.swarms[idx].update.is_seed[source] = true;
                tm.swarms[idx].update.is_peer[source] = false;
                break;

            // cazul cand un client semnaleaza ca a terminat de downloadat fisierele
            case MSG_ALL_DONE:
                MPI_Recv(nullptr, 0, MPI_CHAR, source, MSG_ALL_DONE, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            
                cout << "Peer " << source << " finished all downloads.\n";
                nr_done_clients++;

                // Daca toti clientii au terminat, notificam toti clientii sa se opreasca
                if (nr_done_clients == tm.numtasks - 1) {
                    cout << "All peers are done. Stopping...\n";

                    for (int rank = 1; rank <= tm.numtasks - 1; rank++) {
                        MPI_Send(nullptr, 0, MPI_CHAR, rank, MSG_TRACKER_STOP, MPI_COMM_WORLD);
                    }
                    finished = true;
                }
                break;

            default:
                break;
        }
    }
}

void tracker(int numtasks, int rank)
{
    TrackerManager tracker_manager(numtasks);
    
    tracker_manager.receive_nr_files_to_process();
    MPI_Barrier(MPI_COMM_WORLD);

    tracker_manager.receive_all_initial_files_data();
    MPI_Barrier(MPI_COMM_WORLD);

    sleep(0.8);
    tracker_manager.signal_clients_to_start();

    tracker_main_loop(tracker_manager);
}
