#include <mpi.h>
#include <pthread.h>
#include <cstdio>
#include <unistd.h>
#include <iostream>
#include <vector>
#include <fstream>
#include <cstring>   // for strcpy, strcmp, memcmp, memcpy
#include "struct.h"
#include "peer.h"

using namespace std;

PeerManager::PeerManager(int rank, int numtasks) : rank(rank), numtasks(numtasks) {
    nr_owned_files = 0;
    nr_files = 0;
    read_input_file();
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

    // read the name for each file we want
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
        // cout << "[Peer " << rank << "] Sending owned file " << file->filename << " to tracker.\n";
        MPI_Send(file, sizeof(file_data), MPI_BYTE, TRACKER_RANK, MSG_INIT_FILES, MPI_COMM_WORLD);
    }
}

void PeerManager::save_output_file(int index) {
    char output_file_name[MAX_OUTPUT_FILENAME];
    sprintf(output_file_name, "client%d.%s", rank, files[index].filename);

    ofstream fout(output_file_name);
    if (!fout.is_open()) {
        cerr << "[Peer " << rank << "] Error opening output file: " << output_file_name << endl;
        exit(-1);
    }

    // scriem hash-urile detinute in ordine
    for (int i = 0; i < nr_owned_chunks[index]; i++) {
        for (int j = 0; j < HASH_SIZE; j++) {
            fout << files[index].identifiers[i].hash[j];
        }
        fout << endl;
    }

    fout.close();
}

// cerem de la tracker un update la swarm cu ownerii
void PeerManager::update_swarm(char *filename) {
    MPI_Send(filename, MAX_FILENAME, MPI_CHAR, TRACKER_RANK, MSG_REQ_UPDATE_SWARM, MPI_COMM_WORLD);
    MPI_Recv(&(cur_swarm.owners), sizeof(swarm_update), MPI_BYTE, TRACKER_RANK, MSG_UPDATE_SWARM, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
}

// cerem un chunk de la un peer
bool PeerManager::request_chunk(int rank_request, int file_index, int chunk_index) {
    chunk_request req;
    chunk_response res;

    strcpy(req.filename, files[file_index].filename);
    req.chunk_index = chunk_index;

    MPI_Send(&req, sizeof(chunk_request), MPI_BYTE, rank_request, MSG_CHUNK_REQUEST, MPI_COMM_WORLD);
    MPI_Recv(&res, sizeof(chunk_response), MPI_BYTE, rank_request, MSG_CHUNK_RESPONSE, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

    if (!res.has_chunk) {
        return false;
    }

    // verificam daca hash-ul primit este corect (practic echivalent cu descarcarea)
    char* verify_hash = cur_swarm.file_metadata.identifiers[chunk_index].hash;
    if (memcmp(res.hash, verify_hash, HASH_SIZE) != 0) {
        // hash mismatch
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

    // loop pana cand avem toate chunk-urile
    while (nr_owned_chunks[file_index] < file.nr_total_chunks) {
        cur_chk = nr_owned_chunks[file_index]; // = 0 la inceput

        // cutam in lista de seederi din swarm
        for (int i = 1; i < numtasks; i++) {
            if (cur_swarm.owners.is_seed[i]) {
                if (request_chunk(i, file_index, cur_chk)) {
                    break;
                }
            }
        }

        // daca nu am gasit chunk-ul, cautam si in lista de peeri
        for (int i = 1; i < numtasks; i++) {
            if (cur_swarm.owners.is_peer[i]) {
                if (request_chunk(i, file_index, cur_chk)) {
                    break;
                }
            }
        }

        // la fiecare 10 chunk-uri cerem un update la swarm
        if ((count % 10) == 0) {
            update_swarm(file.filename);
        }
        count++;
    }
}

void PeerManager::DEBUG_PRINT() {
    cout << "[Peer " << rank << "] nr_files: " << nr_files
         << " , nr_owned_files: " << nr_owned_files << endl;
    for (int i = 0; i < nr_files; i++) {
        cout << "  File #" << i << ": " << files[i].filename
             << " , total_chunks=" << files[i].nr_total_chunks
             << " , owned=" << nr_owned_chunks[i] << endl;
    }
}

void* download_thread_func(void* arg) {
    PeerManager* pm = static_cast<PeerManager*>(arg);
    MPI_Status status;

    // astptam semnal de la tracker
    MPI_Recv(nullptr, 0, MPI_CHAR, TRACKER_RANK, MSG_TRACKER_READY, MPI_COMM_WORLD, &status);
    // cout << "[Peer " << pm->rank << "] Received tracker READY. Starting downloads. (nr_owed_files=" << pm->nr_owned_files << ", nr_files=" << pm->nr_files << ")\n";

    // pt fiecare fisier dorit, cerem swarm-ul si descarcam
    for (int i = pm->nr_owned_files; i < pm->nr_files; i++) {
        file_data& file = pm->files[i];

        // cerem swarm-ul de la tracker
        MPI_Send(file.filename, MAX_FILENAME, MPI_CHAR, TRACKER_RANK, MSG_REQ_FULL_SWARM, MPI_COMM_WORLD);
        MPI_Recv(&(pm->cur_swarm), sizeof(swarm_data), MPI_BYTE, TRACKER_RANK, MSG_SWARM_DATA, MPI_COMM_WORLD, &status);

        // acum descarcam fisierul
        pm->download_file_using_swarm(i);

        // salvam fisieul si trimitem o confirmare la tracker
        pm->save_output_file(i);
        pm->nr_owned_files++;
        MPI_Send(file.filename, MAX_FILENAME, MPI_CHAR, TRACKER_RANK, MSG_FILE_DONE, MPI_COMM_WORLD);

        // cout << "[Peer " << pm->rank << "] Downloaded file: " << file.filename << " - Owned chunks: " << pm->nr_owned_chunks[i] << endl;
    }

    // am terminat toate fisierele
    MPI_Send(nullptr, 0, MPI_CHAR, TRACKER_RANK, MSG_ALL_DONE, MPI_COMM_WORLD);

    // cout << "[Peer " << pm->rank << "] Finished downloading all files.\n";

    return nullptr;
}

void* upload_thread_func(void* arg) {
    PeerManager* pm = static_cast<PeerManager*>(arg);

    bool finished = false;
    MPI_Status status;

    while (!finished) {
        MPI_Probe(MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &status);
        int source = status.MPI_SOURCE;
        int tag = status.MPI_TAG;

        // cout << "[!!! Peer " << pm->rank << "] Probed imessage with tag " << tag << " from " << source << endl;

        if (tag == MSG_CHUNK_REQUEST) {
            // cerere de chunk
            pm->send_chunk(source);
        }
        if (tag == MSG_TRACKER_STOP) {
            // cerere de stop de la tracker
            MPI_Recv(nullptr, 0, MPI_CHAR, source, MSG_TRACKER_STOP, MPI_COMM_WORLD, &status);
            // cout << "[Peer " << pm->rank << "] Received stop signal from tracker.\n";
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
    // spacing out prints
    // sleep(rank * 0.1);

    // Let tracker know how many files we own
    pm.send_my_nr_files();
    MPI_Barrier(MPI_COMM_WORLD);

    // Send file data for owned files
    pm.send_own_files_data();
    MPI_Barrier(MPI_COMM_WORLD);

    // Launch threads
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

    // join threads
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

    // cout << "[Peer " << rank << "] All threads joined, shutting down.\n";
} 