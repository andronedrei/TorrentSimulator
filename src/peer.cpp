#include <mpi.h>
#include <pthread.h>
#include <unistd.h>
#include <iostream>
#include <fstream>
#include "struct.h"
#include "peer.h"

using namespace std;

// constructor si initializare
PeerManager::PeerManager(int rank, int numtasks) : rank(rank), numtasks(numtasks) {
    nr_owned_files = 0;
    nr_files = 0;
    read_input_file();

    for (int i = 0; i < MAX_CLIENTS; i++) {
        used_peer[i] = 0;
    }
    srand(time(nullptr));

    DEBUG_NR_HELPS = 0;
}

// citim datele din fisierul de input
void PeerManager::read_input_file() {
    char input_file_name[MAX_FILENAME];
    sprintf(input_file_name, "in%d.txt", rank);

    ifstream fin(input_file_name);
    if (!fin.is_open()) {
        cerr << "[Peer " << rank << "] Error opening input file: " << input_file_name << endl;
        exit(-1);
    }
    
    // citim datele despre fisierele detinute
    fin >> nr_owned_files;
    for (int i = 0; i < nr_owned_files; i++) {
        file_data& file = files[i];
        fin >> file.filename; 
        fin >> file.nr_total_chunks;
        nr_owned_chunks[i] = file.nr_total_chunks;

        // citim hash-urile chunk-urilor
        for (int j = 0; j < file.nr_total_chunks; j++) {
            fin >> file.identifiers[j].hash;
        }
    }

    // citim fisierele dorite (doar numele)
    int nr_files_to_download;
    fin >> nr_files_to_download;
    nr_files = nr_files_to_download + nr_owned_files;

    for (int i = nr_owned_files; i < nr_files; i++) {
        file_data& file = files[i];
        fin >> file.filename;
        file.nr_total_chunks = 0;
        nr_owned_chunks[i] = 0;
    }

    fin.close();
}

void PeerManager::send_my_nr_files() {
    MPI_Send(&nr_owned_files, 1, MPI_INT, TRACKER_RANK, MSG_INIT_NR_FILES, MPI_COMM_WORLD);
}

void PeerManager::send_own_files_data() {
    for (int i = 0; i < nr_owned_files; i++) {
        file_data* file = &(files[i]);
        MPI_Send(file, sizeof(file_data), MPI_BYTE, TRACKER_RANK, MSG_INIT_FILES, MPI_COMM_WORLD);
    }
}

// salvam hash-urile detinute in fisierul de output
void PeerManager::save_output_file(int index) {
    char output_file_name[MAX_OUTPUT_FILENAME];
    sprintf(output_file_name, "client%d_%s", rank, files[index].filename);

    ofstream fout(output_file_name);
    if (!fout.is_open()) {
        cerr << "[Peer " << rank << "] Error opening output file: " << output_file_name << endl;
        exit(-1);
    }

    // scriem hash-urile detinute in ordine
    for (int i = 0; i < nr_owned_chunks[index] - 1; i++) {
        fout.write(files[index].identifiers[i].hash, HASH_SIZE);
        fout << endl;
    }
    fout.write(files[index].identifiers[nr_owned_chunks[index] - 1].hash, HASH_SIZE); // fara endl la ultimul hash

    fout.close();
}

// cerem de la tracker un update la swarm cu ownerii
void PeerManager::update_swarm(char *filename) {
    MPI_Sendrecv(filename, MAX_FILENAME, MPI_CHAR, TRACKER_RANK, MSG_REQ_UPDATE_SWARM,
                 &(cur_swarm.owners), sizeof(swarm_update), MPI_BYTE, TRACKER_RANK, MSG_UPDATE_SWARM,
                 MPI_COMM_WORLD, MPI_STATUS_IGNORE);
}

// cerem un chunk de la un peer
bool PeerManager::request_chunk(int rank_request, int file_index, int chunk_index) {
    chunk_request req;
    chunk_response res;

    strcpy(req.filename, files[file_index].filename);
    req.chunk_index = chunk_index;

    MPI_Sendrecv(&req, sizeof(chunk_request), MPI_BYTE, rank_request, MSG_CHUNK_REQUEST,
                 &res, sizeof(chunk_response), MPI_BYTE, rank_request, MSG_CHUNK_RESPONSE,
                 MPI_COMM_WORLD, MPI_STATUS_IGNORE);

    if (!res.has_chunk) {
        return false;
    }

    // verificam daca hash-ul primit este corect (practic echivalent cu descarcarea)
    char* verify_hash = cur_swarm.file_metadata.identifiers[chunk_index].hash;
    if (memcmp(res.hash, verify_hash, HASH_SIZE) != 0) {
        return false;
    }

    // daca s-a ajuns aici, inseamna ca chunk-ul este corect
    char* file_hash = files[file_index].identifiers[chunk_index].hash;
    memcpy(file_hash, res.hash, HASH_SIZE);

    nr_owned_chunks[file_index]++;
    
    return true;
}

// Raspunde la cereri de chunk-uri
void PeerManager::send_chunk(int rank_request) {
    chunk_request req;
    chunk_response res;

    MPI_Recv(&req, sizeof(chunk_request), MPI_BYTE, rank_request, MSG_CHUNK_REQUEST, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
 
    // cautam fisierul in lista noastra
    int file_index = NOT_FOUND;
    for (int i = 0; i < nr_owned_files + 1; i++) { // adaugam +1 si pt cazul in care putem fi peer
        if (strcmp(files[i].filename, req.filename) == 0) {
            file_index = i;
            break;
        }
    }

    // daca nu avem fisierul sau chunk-ul, trimitem raspuns negativ
    if (file_index == NOT_FOUND || req.chunk_index >= nr_owned_chunks[file_index]) {
        res.has_chunk = false;
        MPI_Send(&res, sizeof(chunk_response), MPI_BYTE, rank_request, MSG_CHUNK_RESPONSE, MPI_COMM_WORLD);
        return;
    }

    // trimitem raspuns pozitiv
    res.has_chunk = true;
    memcpy(res.hash, files[file_index].identifiers[req.chunk_index].hash, HASH_SIZE);
    MPI_Send(&res, sizeof(chunk_response), MPI_BYTE, rank_request, MSG_CHUNK_RESPONSE, MPI_COMM_WORLD);
}

// functie eurisica pentru alegerea unui seed pentru un chunk
int PeerManager::find_seed_for_chunk() {
    int best_seed = NOT_FOUND;
    int best_usage = BIG_VALUE; // de cate ori am apelat la cel mai bun seeder sau peer
    int start_index = rand() % numtasks;
    int nr_tries = 0;
    int i;

    /* incercam intai sa gasim un seed sau peer putin utilizat (facand "nr_tries" incercari de
    la un index random, pt a fi eficient programul si in cazul in care lista de clienti e mare) */
    for (int offset = 0; offset < numtasks; offset++) {
        i = (start_index + offset) % numtasks; // indexul seed-ului sau peer-ului

        if (i == rank || i == TRACKER_RANK) {
            continue;
        }

        // vrificam daca seed-ul cu acest index detine chunk-ul (seed-eri au prioritate)
        if (cur_swarm.owners.is_seed[i]) {
            if (used_peer[i] < best_usage) {
                best_seed = i;
                best_usage = used_peer[i];
                nr_tries++;
            }
        
        // verificam daca peer-ul cu acest index detine chunk-ul
        } else if (cur_swarm.owners.is_peer[i]) {
            // peerii au un "treshold de decizie" mai mare (au sansa mai mica sa detina chunk-ul)
            if (used_peer[i] << PEER_DECISSION_TRESHOLD < best_usage) {
                best_seed = i;
                best_usage = used_peer[i] << PEER_DECISSION_TRESHOLD;
                nr_tries++;
            }
        }

        if (nr_tries >= FIND_NUM_TRIES) {
            break;
        }
    }

    if (best_seed != NOT_FOUND) {
        used_peer[best_seed]++;
    }

    return best_seed;
}

// descarcam un fisier folosind swarm-ul primit de la tracker
void PeerManager::download_file_using_swarm(int file_index) {
    file_data& file = files[file_index];
    file.nr_total_chunks = cur_swarm.file_metadata.nr_total_chunks;

    if (nr_owned_chunks[file_index] == file.nr_total_chunks && nr_owned_chunks[file_index] > 0) {
        cout << "STH WENR WRONG IN DOWNLOAD_FILE_USING_SWARM\n";
        return;
    }

    int count = 1;
    int cur_chk;
    int seed_chosen;

    // loop pana cand avem toate chunk-urile
    while (nr_owned_chunks[file_index] < file.nr_total_chunks) {
        cur_chk = nr_owned_chunks[file_index]; // = 0 la inceput

        // alegem un seed pentru chunk-ul curent
        seed_chosen = find_seed_for_chunk();
        request_chunk(seed_chosen, file_index, cur_chk);

        // la fiecare 10 chunk-uri cerem un update la swarm
        if ((count % 10) == 0) {
            update_swarm(file.filename);
        }
        count++;
    }
}

void* download_thread_func(void* arg) {
    PeerManager* pm = static_cast<PeerManager*>(arg);
    MPI_Status status;

    // astptam semnal de la tracker
    MPI_Recv(nullptr, 0, MPI_CHAR, TRACKER_RANK, MSG_CLIENT_READY_DOWNLOAD, MPI_COMM_WORLD, &status);

    // pt fiecare fisier dorit, cerem swarm-ul si descarcam
    for (int i = pm->nr_owned_files; i < pm->nr_files; i++) {
        file_data& file = pm->files[i];

        // cerem swarm-ul de la tracker
        MPI_Sendrecv(file.filename, MAX_FILENAME, MPI_CHAR, TRACKER_RANK, MSG_REQ_FULL_SWARM,
                     &(pm->cur_swarm), sizeof(swarm_data), MPI_BYTE, TRACKER_RANK, MSG_SWARM_DATA,
                     MPI_COMM_WORLD, MPI_STATUS_IGNORE);

        // acum descarcam fisierul
        pm->download_file_using_swarm(i);

        // salvam fisieul si trimitem o confirmare la tracker
        pm->save_output_file(i);
        pm->nr_owned_files++;
        MPI_Send(file.filename, MAX_FILENAME, MPI_CHAR, TRACKER_RANK, MSG_FILE_DONE, MPI_COMM_WORLD);
    }

    // am terminat toate fisierele
    MPI_Send(nullptr, 0, MPI_CHAR, TRACKER_RANK, MSG_ALL_DONE, MPI_COMM_WORLD);

    return nullptr;
}

void* upload_thread_func(void* arg) {
    PeerManager* pm = static_cast<PeerManager*>(arg);

    bool finished = false;
    MPI_Status status;
    int source, tag;

    // asteptam semnal de la tracker
    MPI_Recv(nullptr, 0, MPI_CHAR, TRACKER_RANK, MSG_CLIENT_READY_UPLOAD, MPI_COMM_WORLD, &status);

    while (!finished) {
        MPI_Probe(MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &status);
        tag = status.MPI_TAG;

        if (tag == MSG_CHUNK_REQUEST) {
            // cerere de chunk
            source = status.MPI_SOURCE;
            pm->send_chunk(source);
            pm->DEBUG_NR_HELPS++;
        }
        if (tag == MSG_TRACKER_STOP) {
            // cerere de stop de la tracker
            source = status.MPI_SOURCE;
            MPI_Recv(nullptr, 0, MPI_CHAR, source, MSG_TRACKER_STOP, MPI_COMM_WORLD, &status);
            finished = true;
        }
    }
    return nullptr;
}

// Peer main function
void peer(int numtasks, int rank) {
    pthread_t download_thread;
    pthread_t upload_thread;
    void* statusPtr;

    PeerManager pm(rank, numtasks);

    pm.send_my_nr_files();
    pm.send_own_files_data();

    int r = pthread_create(&download_thread, nullptr, download_thread_func, (void*)&pm);
    if (r) {
        cerr << "[Peer " << rank << "] Error creating download thread.\n";
        exit(-1);
    }

    r = pthread_create(&upload_thread, nullptr, upload_thread_func, (void*)&pm);
    if (r) {
        cerr << "[Peer " << rank << "] Error creating upload thread.\n";
        exit(-1);
    }

    r = pthread_join(download_thread, &statusPtr);
    if (r) {
        cerr << "[Peer " << rank << "] Error joining download thread.\n";
        exit(-1);
    }

    r = pthread_join(upload_thread, &statusPtr);
    if (r) {
        cerr << "[Peer " << rank << "] Error joining upload thread.\n";
        exit(-1);
    }

    // cout << "[Peer " << rank << "] Helped " << pm.DEBUG_NR_HELPS << " times.\n";
} 