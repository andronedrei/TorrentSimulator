#include <mpi.h>
#include <pthread.h>
#include <iostream>
#include <vector>
#include <cstring>
#include <unistd.h>
#include "struct.h"
#include "tracker.h"

using namespace std;

// constructor si initializare
TrackerManager::TrackerManager(int numtasks) : numtasks(numtasks) {
    swarm_data* sw;
    nr_files = 0;
    nr_initial_files = 0;

    for (int i = 0; i < MAX_FILES; i++) {
        sw = &(swarms[i]);
    
        // initial presupunem ca nimeni nu are nimic
        for (int j = 0; j < MAX_CLIENTS; j++) {
            sw->owners.is_seed[j] = false;
            sw->owners.is_peer[j] = false;
        }
        sw->file_metadata.nr_total_chunks = 0;
        memset(sw->file_metadata.filename, 0, MAX_FILENAME);
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

void TrackerManager::receive_nr_files_to_process() {
    nr_initial_files = 0;
    for (int rank = 1; rank < numtasks; rank++) {
        int num;
        MPI_Recv(&num, 1, MPI_INT, rank, MSG_INIT_NR_FILES, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        nr_initial_files += num;
    }
}

// primeste datele initiale despre fisiere
void TrackerManager::receive_all_initial_files_data() {
    int found_index, cur_index = 0;
    file_data* cur_file;
    swarm_data* sw;
    int sender_rank;

    nr_files = 0;
    for (int i = 0; i < nr_initial_files; i++) {
        cur_file = &(swarms[cur_index].file_metadata);

        MPI_Status status;
        MPI_Recv(cur_file, sizeof(file_data), MPI_BYTE, MPI_ANY_SOURCE, MSG_INIT_FILES, MPI_COMM_WORLD, &status);
        sender_rank = status.MPI_SOURCE;

        /* verificam daca fisierul nu exista si il adaugam in caz afirmativ
        daca nu exista mentinem indexul curent pt a fi suprascris data viitoare */
        found_index = find_file_index(cur_file->filename);
        if (found_index == NOT_FOUND) {
            found_index = cur_index;
            nr_files++;
            cur_index++;
        }

        sw = &(swarms[found_index]);
        sw->owners.is_seed[sender_rank] = true; 
    }
}

// semnaleaza clientilor sa inceapa dupa partea de initializare
void TrackerManager::signal_clients_to_start() {
    for (int rank = 1; rank < numtasks; rank++) {
        MPI_Send(nullptr, 0, MPI_CHAR, rank, MSG_CLIENT_READY_DOWNLOAD, MPI_COMM_WORLD);
        MPI_Send(nullptr, 0, MPI_CHAR, rank, MSG_CLIENT_READY_UPLOAD, MPI_COMM_WORLD);
    }
}

// loop-ul principal care asteapta mesaje de la clienti
void tracker_main_loop(TrackerManager& tm) {
    int nr_done_clients = 0;
    bool finished = false;

    while (!finished) {
        MPI_Status status;
        // asteapta orice mesaj de la orice sursa
        MPI_Probe(MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &status);

        int tag = status.MPI_TAG;
        int source = status.MPI_SOURCE;

        // actionam in functie de tag
        switch(tag) {
            case MSG_REQ_FULL_SWARM: {
                char filename[MAX_FILENAME];
                MPI_Recv(filename, MAX_FILENAME, MPI_CHAR, source, MSG_REQ_FULL_SWARM, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

                int idx = tm.find_file_index(filename);
                if (idx == NOT_FOUND) {
                    // daca nu exista fisierul, trimitem un mesaj gol
                    swarm_data empty;
                    memset(&empty, 0, sizeof(swarm_data));
                    MPI_Send(&empty, sizeof(swarm_data), MPI_BYTE, source, MSG_SWARM_DATA, MPI_COMM_WORLD);
                } else {
                    // altfel trimitem swarm-ul corespunzator (hash-uri, nr_chunks, cine are ce)
                    MPI_Send(&(tm.swarms[idx]), sizeof(swarm_data), MPI_BYTE, source, MSG_SWARM_DATA, MPI_COMM_WORLD);

                    // marcam sursa ca peer pentru acel fisier
                    tm.swarms[idx].owners.is_peer[source] = true;
                }
                break;
            }

            case MSG_REQ_UPDATE_SWARM: {
                char filename[MAX_FILENAME];
                MPI_Recv(filename, MAX_FILENAME, MPI_CHAR, source, MSG_REQ_UPDATE_SWARM, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

                // cautam index-ul fisierului si trimitem update-ul corespunzator
                int idx = tm.find_file_index(filename);
                if (idx == NOT_FOUND) {
                    swarm_update empty;
                    memset(&empty, 0, sizeof(swarm_update));
                    MPI_Send(&empty, sizeof(swarm_update), MPI_BYTE, source, MSG_UPDATE_SWARM, MPI_COMM_WORLD);
                } else {
                    MPI_Send(&(tm.swarms[idx].owners), sizeof(swarm_update), MPI_BYTE, source, MSG_UPDATE_SWARM, MPI_COMM_WORLD);
                }
                break;
            }

            // un peer a terminat de descarcat un fisier
            case MSG_FILE_DONE: {
                char filename[MAX_FILENAME];
                MPI_Recv(filename, MAX_FILENAME, MPI_CHAR, source, MSG_FILE_DONE, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

                int idx = tm.find_file_index(filename);
                if (idx != NOT_FOUND) {
                    // il facem seed
                    tm.swarms[idx].owners.is_seed[source] = true;
                    tm.swarms[idx].owners.is_peer[source] = false;
                }
                break;
            }

            // un peer a terminat toate descarcarile
            case MSG_ALL_DONE: {
                MPI_Recv(nullptr, 0, MPI_CHAR, source, MSG_ALL_DONE, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
                nr_done_clients++;

                // daca toti au terminat trimitem mesaje de stop
                if (nr_done_clients == (tm.numtasks - 1)) {
                    for (int rank = 1; rank < tm.numtasks; rank++) {
                        MPI_Send(nullptr, 0, MPI_CHAR, rank, MSG_TRACKER_STOP, MPI_COMM_WORLD);
                    }
                    finished = true;
                }
                break;
            }

            default: {
                MPI_Recv(nullptr, 0, MPI_CHAR, source, tag, MPI_COMM_WORLD, &status);
                cerr << "[Tracker] Unknown tag " << tag << " from peer " << source << endl;
                break;
            }
        }
    }
}

void tracker(int numtasks, int rank) {
    TrackerManager tm(numtasks);

    tm.receive_nr_files_to_process();
    tm.receive_all_initial_files_data();

    // semnaleaza clientilor sa inceapa
    tm.signal_clients_to_start();

    // logica principala (swarm-uri si update-uri)
    tracker_main_loop(tm);
}