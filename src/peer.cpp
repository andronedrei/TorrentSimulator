#include <mpi.h>
#include <pthread.h>
#include <cstdio>
#include <unistd.h>

#include <iostream>
#include <vector>
#include <fstream>

#include "struct.h"
#include "peer.h"

using namespace std;

void PeerManager::DEBUG_PRINT() {
    cout << "Rank: " << rank << endl;
    cout << "Nr files: " << nr_files << endl;
    for (int i = 0; i < nr_files; i++) {
        cout << "File " << i << ": " << peer_files[i].file.filename << endl;
        cout << "Nr owned chunks: " << peer_files[i].nr_owned_chunks << endl;
        for (int j = 0; j < peer_files[i].nr_owned_chunks; j++) {
            cout << "Chunk " << j << ": ";
            for (int k = 0; k < HASH_SIZE; k++) {
                cout << peer_files[i].file.identifiers[j].hash[k];
            }
            cout << endl;
        }
    }
}

PeerManager::PeerManager(int rank, int numtasks) : rank(rank), numtasks(numtasks) {
    nr_owned_files = 0;
    nr_files = 0;
    read_input_file();
}

void PeerManager::read_input_file() {
    // deschidem fisierul de input
    char input_file_name[MAX_FILENAME];
    sprintf(input_file_name, "in%d.txt", rank);

    ifstream fin(input_file_name);
    if (!fin.is_open()) {
        cerr << "Error opening input file for rank: " << rank << endl;
        exit(-1);
    }
    
    // citim datele din fisier legate de ce fisiere detine
    fin >> nr_owned_files;
    for (int i = 0; i < nr_owned_files; i++) {
        file_data& file = peer_files[i].file;
    
        fin >> file.filename; 
        fin >> file.nr_total_chunks;

        for (int j = 0; j < file.nr_total_chunks; j++) {
            fin >> file.identifiers[j].hash;
            peer_files[i].has_chunk[j] = true;
        }

        peer_files[i].nr_owned_chunks = file.nr_total_chunks;
    }

    // citim datele legate de ce fisiere doreste sa downloadeze
    int nr_files_to_download;
    fin >> nr_files_to_download;
    nr_files = nr_files_to_download + nr_owned_files;

    for (int i = nr_owned_files; i < nr_files; i++) {
        file_data& file = peer_files[i].file;
    
        fin >> file.filename; 

        for (int j = 0; j < file.nr_total_chunks; j++) {
            peer_files[i].has_chunk[j] = false;
        }

        peer_files[i].nr_owned_chunks = 0;
    }

    fin.close();
}

// trimitem numarul de fisiere ca sa stie tracker-ul dupa cate fisiere sa astepte
void PeerManager::send_my_nr_files() {
    MPI_Send(&nr_owned_files, 1, MPI_INT, TRACKER_RANK, MSG_INIT_NR_FILES, MPI_COMM_WORLD);
}

// trimitem datele (hash-uri, nume fisiere si numari bucati) despre fisierele detinute catre tracker
void PeerManager::send_own_files_data() {
    for (int i = 0; i < nr_owned_files; i++) {
        file_data* cur_file_p = &(peer_files[i].file);
        cout << "Sent from " << rank << " file " << cur_file_p->filename << endl;
        MPI_Send(cur_file_p, sizeof(file_data), MPI_BYTE, TRACKER_RANK, MSG_INIT_FILES, MPI_COMM_WORLD);
    }
}

void PeerManager::save_output_file(int index) {
    // deschidem fisierul de output
    char output_file_name[MAX_OUTPUT_FILENAME];
    sprintf(output_file_name, "client%d.%s", rank, peer_files[index].file.filename);

    ofstream fout(output_file_name);
    if (!fout.is_open()) {
        cerr << "Error opening output file for rank: " << rank << endl;
        exit(-1);
    }

    // scriem hash-urile in fisier
    for (int i = 0; i < peer_files[index].nr_owned_chunks; i++) {
        for (int j = 0; j < HASH_SIZE; j++) {
            fout << peer_files[index].file.identifiers[i].hash[j];
        }
        fout << endl;
    }

    fout.close();
}

// cerem update la swarm
void PeerManager::update_swarm() {
    char *filename_p = cur_swarm.file_metadata.filename;
    MPI_Send(filename_p, MAX_FILENAME, MPI_CHAR, TRACKER_RANK, MSG_REQ_UPDATE_SWARM, MPI_COMM_WORLD);
    MPI_Recv(&(cur_swarm.update), sizeof(swarm_update), MPI_BYTE, TRACKER_RANK, MSG_UPDATE_SWARM, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
}

// cerem un chunk de la un peer
bool PeerManager::request_chunk(int rank_request, int file_index, int chunk_index) {
    chunk_request req;
    chunk_response res;

    // facem request-ul
    req.chunk_index = chunk_index;
    strcpy(req.filename, peer_files[file_index].file.filename);
    MPI_Send(&req, sizeof(chunk_request), MPI_BYTE, rank_request, MSG_CHUNK_REQUEST, MPI_COMM_WORLD);

    // primim raspunsul
    MPI_Recv(&res, sizeof(chunk_response), MPI_BYTE, rank_request, MSG_CHUNK_RESPONSE, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

    // verificam daca chunk-ul a fost gasit si daca hash-ul corespunde
    if (res.has_chunk) {
        return false;
    }

    if (memcmp(res.hash, cur_swarm.file_metadata.identifiers[chunk_index].hash, HASH_SIZE) != 0) {
        return false;
    }

    memcpy(peer_files[file_index].file.identifiers[chunk_index].hash, res.hash, HASH_SIZE);
    
    return true;
}

void PeerManager::send_chunk() {
    chunk_request req;
    chunk_response res;

    MPI_Recv(&req, sizeof(chunk_request), MPI_BYTE, MPI_ANY_SOURCE, MSG_CHUNK_REQUEST, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

    // cautam fisierul
    int file_index = -1;
    for (int i = 0; i < nr_files; i++) {
        if (strcmp(peer_files[i].file.filename, req.filename) == 0) {
            file_index = i;
            break;
        }
    }

    // daca nu gasim fisierul, trimitem raspuns negativ
    if (file_index == -1) {
        res.has_chunk = false;
        MPI_Send(&res, sizeof(chunk_response), MPI_BYTE, req.chunk_index, MSG_CHUNK_RESPONSE, MPI_COMM_WORLD);
        return;
    }

    // daca gasim fisierul, trimitem raspuns pozitiv si hash-ul
    res.has_chunk = true;
    memcpy(res.hash, peer_files[file_index].file.identifiers[req.chunk_index].hash, HASH_SIZE);
    MPI_Send(&res, sizeof(chunk_response), MPI_BYTE, req.chunk_index, MSG_CHUNK_RESPONSE, MPI_COMM_WORLD);
}

// descarcam un fisier folosind swarm-ul
void PeerManager::download_file_using_swarm(int file_index) {
    peer_file_data& cur_peer_file = peer_files[file_index];
    int& nr_owned_chunks = cur_peer_file.nr_owned_chunks;
    int& nr_total_chunks = cur_swarm.file_metadata.nr_total_chunks;
    int count = 1;

    while (nr_owned_chunks < nr_total_chunks) {
        if (count % 10 == 0) {
            update_swarm();
        }

        bool found = false;
        int cur_seed = 1;
        int cur_peer = 1;
        
        // cautam un seed care detine chunk-ul
        while (!found && cur_seed <= MAX_CLIENTS) {
            if (cur_swarm.update.is_seed[cur_seed]) {
                found = request_chunk(cur_seed, file_index, nr_owned_chunks);
                cur_peer_file.has_chunk[nr_owned_chunks] = true;
            }
            cur_seed++;
        }

        // daca n-am gasit un seed, cautam un peer
        while (!found && cur_peer <= MAX_CLIENTS) {
            if (cur_swarm.update.is_peer[cur_peer]) {
                found = request_chunk(cur_peer, file_index, nr_owned_chunks);
                cur_peer_file.has_chunk[nr_owned_chunks] = true;
            }
            cur_peer++;
        }

        nr_owned_chunks++;
        count++;
    }

    // acualizam metadatele fisierului descarcat
    cur_peer_file.nr_owned_chunks = nr_total_chunks;
    cur_peer_file.file.nr_total_chunks = nr_total_chunks;
}

void* download_thread_func(void* arg) {
    PeerManager* pmanager = static_cast<PeerManager*>(arg);
    MPI_Status status;

    // Asteptam semnal de la tracker ca putem incepe download-urile
    MPI_Recv(nullptr, 0, MPI_CHAR, TRACKER_RANK, MSG_TRACKER_READY, MPI_COMM_WORLD, &status);
    cout << "[Peer " << pmanager->rank << "] Received tracker ACK. Starting downloads.\n";
    cout << "[Peer " << pmanager->rank << "] Nr files to download: " << pmanager->nr_files - pmanager->nr_owned_files << endl;
    cout << "[Peer " << pmanager->rank << "] Nr owned files: " << pmanager->nr_owned_files << endl;

    // Pentru fiecare fisier pe care vrem sa-l downloadam cerem swarm-ul
    for (int i = pmanager->nr_owned_files; i < pmanager->nr_files; i++) {

        if (pmanager->rank != 1) {
            break;
        }

        file_data& file = pmanager->peer_files[i].file;

        // cere swarm-ul si il salvam
        MPI_Send(file.filename, MAX_FILENAME, MPI_CHAR, TRACKER_RANK, MSG_REQ_FULL_SWARM, MPI_COMM_WORLD);
        MPI_Recv(&(pmanager->cur_swarm), sizeof(swarm_data), MPI_BYTE, TRACKER_RANK, MSG_SWARM_DATA, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

        // printam swarm-ul
        swarm_data& swarm_file = pmanager->cur_swarm;
        cout << "\n\n[&&Peer " << pmanager->rank << "] Swarm for file " << swarm_file.file_metadata.filename << endl;
        for (int j = 0; j < swarm_file.file_metadata.nr_total_chunks; j++) {
            for (int k = 0; k < HASH_SIZE; k++) {
                cout << swarm_file.file_metadata.identifiers[j].hash[k];
            }
            cout << endl;
        }
        for (int j = 1; j <= MAX_CLIENTS; j++) {
            if (swarm_file.update.is_seed[j]) {
                cout << "Seed: " << j << endl;
            }
            if (swarm_file.update.is_peer[j]) {
                cout << "Peer: " << j << endl;
            }
        }

        pmanager->download_file_using_swarm(i);

        // salvam fisierul notifiam tracker-ul ca am terminat de downloadat fisierul
        pmanager->save_output_file(i);
        MPI_Send(file.filename, MAX_FILENAME, MPI_CHAR, TRACKER_RANK, MSG_FILE_DONE, MPI_COMM_WORLD);
    }

    // Daca am terminat de downloadat toate fisierele, notificam tracker-ul
    MPI_Send(nullptr, 0, MPI_CHAR, TRACKER_RANK, MSG_ALL_DONE, MPI_COMM_WORLD);

    return nullptr;
}


void* upload_thread_func(void* arg) {
    PeerManager* pmanager = static_cast<PeerManager*>(arg);

    bool finished = false;
    MPI_Status status;

    while (!finished) {
        MPI_Probe(MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &status);
        int source = status.MPI_SOURCE;
        int tag = status.MPI_TAG;

        // cazul cand un peer ne cere un chunk
        if (tag == MSG_CHUNK_REQUEST) {
            pmanager->send_chunk();
        }
        // cazul cand tracker-ul ne spune ca s-a terminat
        if (tag == MSG_TRACKER_STOP) {
            MPI_Recv(nullptr, 0, MPI_CHAR, source, MSG_TRACKER_STOP, MPI_COMM_WORLD, &status);
            finished = true;
        }
    }

    return nullptr;
}

void peer(int numtasks, int rank) {
    pthread_t download_thread;
    pthread_t upload_thread;
    void* status;

    PeerManager pmanager(rank, numtasks);
    sleep(rank * 0.1);

    // trimitem numarul de fisiere ca sa stie tracker-ul dupa cate fisiere sa astepte
    pmanager.send_my_nr_files();
    MPI_Barrier(MPI_COMM_WORLD);

    // trimitem datele despre fisierele detinute
    pmanager.send_own_files_data();
    MPI_Barrier(MPI_COMM_WORLD);

    int r = pthread_create(&download_thread, nullptr, download_thread_func, (void*)&pmanager);
    if (r) {
        cerr << "Error creating download thread" << endl;
        exit(-1);
    }

    r = pthread_create(&upload_thread, nullptr, upload_thread_func, (void*)&pmanager);
    if (r) {
        cerr << "Error creating upload thread" << endl;
        exit(-1);
    }

    r = pthread_join(download_thread, &status);
    if (r) {
        cerr << "Error joining download thread" << endl;
        exit(-1);
    }

    r = pthread_join(upload_thread, &status);
    if (r) {
        cerr << "Error joining upload thread" << endl;
        exit(-1);
    }
}
