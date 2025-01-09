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

// read input file
void PeerManager::read_input_file() {
    char input_file_name[MAX_FILENAME];
    sprintf(input_file_name, "in%d.txt", rank);

    ifstream fin(input_file_name);
    if (!fin.is_open()) {
        cerr << "[Peer " << rank << "] Error opening input file: " << input_file_name << endl;
        exit(-1);
    }
    
    // how many files do we OWN
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

    // how many files do we WANT to download
    int nr_files_to_download;
    fin >> nr_files_to_download;
    nr_files = nr_files_to_download + nr_owned_files;

    // read the name for each file we want
    for (int i = nr_owned_files; i < nr_files; i++) {
        file_data& file = peer_files[i].file;
        fin >> file.filename;
        // we might not know how many chunks until we get from tracker, or you can store a placeholder
        // If the input doesn't specify them, set it to 0 or some default
        file.nr_total_chunks = 0;

        for (int j = 0; j < MAX_CHUNKS; j++) {
            peer_files[i].has_chunk[j] = false;
        }
        peer_files[i].nr_owned_chunks = 0;
    }

    fin.close();
}

void PeerManager::send_my_nr_files() {
    MPI_Send(&nr_owned_files, 1, MPI_INT, TRACKER_RANK, MSG_INIT_NR_FILES, MPI_COMM_WORLD);
}

void PeerManager::send_own_files_data() {
    for (int i = 0; i < nr_owned_files; i++) {
        file_data* cur_file_p = &(peer_files[i].file);
        cout << "[Peer " << rank << "] Sending owned file " << cur_file_p->filename << " to tracker.\n";
        MPI_Send(cur_file_p, sizeof(file_data), MPI_BYTE, TRACKER_RANK, MSG_INIT_FILES, MPI_COMM_WORLD);
    }
}

void PeerManager::save_output_file(int index) {
    char output_file_name[MAX_OUTPUT_FILENAME];
    sprintf(output_file_name, "client%d.%s", rank, peer_files[index].file.filename);

    ofstream fout(output_file_name);
    if (!fout.is_open()) {
        cerr << "[Peer " << rank << "] Error opening output file: " << output_file_name << endl;
        exit(-1);
    }

    // write the chunks' hashes in order
    for (int i = 0; i < peer_files[index].nr_owned_chunks; i++) {
        for (int j = 0; j < HASH_SIZE; j++) {
            fout << peer_files[index].file.identifiers[i].hash[j];
        }
        fout << endl;
    }

    fout.close();
}

// Ask tracker for an updated swarm for the current file
void PeerManager::update_swarm() {
    char *filename_p = cur_swarm.file_metadata.filename;
    MPI_Send(filename_p, MAX_FILENAME, MPI_CHAR, TRACKER_RANK, MSG_REQ_UPDATE_SWARM, MPI_COMM_WORLD);
    MPI_Recv(&(cur_swarm.update), sizeof(swarm_update), MPI_BYTE, TRACKER_RANK, MSG_UPDATE_SWARM, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
}

// Request a chunk from a remote peer
bool PeerManager::request_chunk(int rank_request, int file_index, int chunk_index) {
    chunk_request req;
    chunk_response res;

    req.chunk_index = chunk_index;
    strcpy(req.filename, peer_files[file_index].file.filename);

    // send request
    MPI_Send(&req, sizeof(chunk_request), MPI_BYTE, rank_request, MSG_CHUNK_REQUEST, MPI_COMM_WORLD);

    // get response
    MPI_Recv(&res, sizeof(chunk_response), MPI_BYTE, rank_request, MSG_CHUNK_RESPONSE, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

    // If remote peer doesn't have chunk
    if (!res.has_chunk) {
        return false;
    }

    // If it has chunk, verify hash
    if (memcmp(res.hash, cur_swarm.file_metadata.identifiers[chunk_index].hash, HASH_SIZE) != 0) {
        // hash mismatch
        return false;
    }

    // success: copy the chunk's hash
    memcpy(peer_files[file_index].file.identifiers[chunk_index].hash, res.hash, HASH_SIZE);
    return true;
}

// Respond to a chunk request from any other peer
void PeerManager::send_chunk() {
    MPI_Status status;
    chunk_request req;
    chunk_response res;

    MPI_Recv(&req, sizeof(chunk_request), MPI_BYTE, MPI_ANY_SOURCE, MSG_CHUNK_REQUEST, MPI_COMM_WORLD, &status);

    int src_rank = status.MPI_SOURCE;
    
    // find local file index
    int file_index = -1;
    for (int i = 0; i < nr_files; i++) {
        if (strcmp(peer_files[i].file.filename, req.filename) == 0) {
            file_index = i;
            break;
        }
    }

    // if we don't have that file at all
    if (file_index == -1) {
        res.has_chunk = false;
        MPI_Send(&res, sizeof(chunk_response), MPI_BYTE, src_rank, MSG_CHUNK_RESPONSE, MPI_COMM_WORLD);
        return;
    }

    // if we do have the file, check if we have that chunk
    if (peer_files[file_index].has_chunk[req.chunk_index]) {
        res.has_chunk = true;
        memcpy(res.hash, peer_files[file_index].file.identifiers[req.chunk_index].hash, HASH_SIZE);
    } else {
        res.has_chunk = false;
        memset(res.hash, 0, HASH_SIZE);
    }

    // send response
    MPI_Send(&res, sizeof(chunk_response), MPI_BYTE, src_rank, MSG_CHUNK_RESPONSE, MPI_COMM_WORLD);
}

// Download one file using the current swarm data
void PeerManager::download_file_using_swarm(int file_index) {
    // local references
    peer_file_data& cur_peer_file = peer_files[file_index];
    // we trust tracker has the correct total
    int nr_total_chunks = cur_swarm.file_metadata.nr_total_chunks;

    // update local metadata
    cur_peer_file.file.nr_total_chunks = nr_total_chunks;

    int count = 1;

    // while we don't have all chunks
    while (cur_peer_file.nr_owned_chunks < nr_total_chunks) {
        
        // Let's pick the next missing chunk:
        int needed_chunk = -1;
        for (int c = 0; c < nr_total_chunks; c++) {
            if (!cur_peer_file.has_chunk[c]) {
                needed_chunk = c;
                break;
            }
        }
        if (needed_chunk == -1) {
            // means we somehow have them all
            break;
        }

        // attempt to get the chunk from seeds first
        bool gotIt = false;
        int cur_seed = 1;
        while (!gotIt && cur_seed < MAX_CLIENTS) {
            if (cur_swarm.update.is_seed[cur_seed] && cur_seed != rank) {
                gotIt = request_chunk(cur_seed, file_index, needed_chunk);
            }
            cur_seed++;
        }
        
        // if not found from a seed, try from peers
        int cur_peer = 1;
        while (!gotIt && cur_peer < MAX_CLIENTS) {
            if (cur_swarm.update.is_peer[cur_peer] && cur_peer != rank) {
                gotIt = request_chunk(cur_peer, file_index, needed_chunk);
            }
            cur_peer++;
        }

        // if successful, mark the chunk as owned
        if (gotIt) {
            cur_peer_file.has_chunk[needed_chunk] = true;
            cur_peer_file.nr_owned_chunks++;
        }

        // every 10 chunks, let's update swarm from tracker
        if ((count % 10) == 0) {
            update_swarm();
        }
        count++;
    }
}

void PeerManager::DEBUG_PRINT() {
    cout << "[Peer " << rank << "] nr_files: " << nr_files
         << " , nr_owned_files: " << nr_owned_files << endl;
    for (int i = 0; i < nr_files; i++) {
        cout << "  File #" << i << ": " << peer_files[i].file.filename
             << " , total_chunks=" << peer_files[i].file.nr_total_chunks
             << " , owned=" << peer_files[i].nr_owned_chunks << endl;
    }
}

// Download thread: wait for tracker READY, then request swarms, download, etc.
void* download_thread_func(void* arg) {
    PeerManager* pmanager = static_cast<PeerManager*>(arg);
    MPI_Status status;

    // Wait for tracker to say "start"
    MPI_Recv(nullptr, 0, MPI_CHAR, TRACKER_RANK, MSG_TRACKER_READY, MPI_COMM_WORLD, &status);
    cout << "[Peer " << pmanager->rank << "] Received tracker READY. Starting downloads.\n";

    // For each file we want to download
    for (int i = pmanager->nr_owned_files; i < pmanager->nr_files; i++) {
        file_data& file = pmanager->peer_files[i].file;

        // ask tracker for swarm
        MPI_Send(file.filename, MAX_FILENAME, MPI_CHAR, TRACKER_RANK, MSG_REQ_FULL_SWARM, MPI_COMM_WORLD);
        MPI_Recv(&(pmanager->cur_swarm), sizeof(swarm_data), MPI_BYTE,
                 TRACKER_RANK, MSG_SWARM_DATA, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

        // Now let's actually download
        // printam doar daca e peer-ul cu rankul 3
        if (pmanager->rank == 3) {
            cout << "[Peer " << pmanager->rank << "] Downloading file: " << file.filename << endl;
        }
        pmanager->download_file_using_swarm(i);

        // Done with this file: save output, notify tracker
        pmanager->save_output_file(i);
        MPI_Send(file.filename, MAX_FILENAME, MPI_CHAR, TRACKER_RANK, MSG_FILE_DONE, MPI_COMM_WORLD);

        cout << "[Peer " << pmanager->rank << "] Downloaded file: " << file.filename << endl
             << " - Owned chunks: " << pmanager->peer_files[i].nr_owned_chunks << endl;
    }

    // If we finished all desired files, notify tracker
    MPI_Send(nullptr, 0, MPI_CHAR, TRACKER_RANK, MSG_ALL_DONE, MPI_COMM_WORLD);

    return nullptr;
}

// Upload thread: handle chunk requests until we get TRACKER_STOP
void* upload_thread_func(void* arg) {
    PeerManager* pmanager = static_cast<PeerManager*>(arg);

    bool finished = false;
    MPI_Status status;

    while (!finished) {
        MPI_Probe(MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &status);
        int source = status.MPI_SOURCE;
        int tag = status.MPI_TAG;

        if (tag == MSG_CHUNK_REQUEST) {
            // serve chunk request
            pmanager->send_chunk();
        }
        else if (tag == MSG_TRACKER_STOP) {
            // final stop
            MPI_Recv(nullptr, 0, MPI_CHAR, source, MSG_TRACKER_STOP, MPI_COMM_WORLD, &status);
            // cout << "[Peer " << pmanager->rank << "] Received stop signal from tracker.\n";
            finished = true;
        }
        else {
            // unknown or unhandled message → consume it
            MPI_Recv(nullptr, 0, MPI_CHAR, source, tag, MPI_COMM_WORLD, &status);
            cerr << "[Peer " << pmanager->rank << "] Upload thread got unknown tag " 
                 << tag << " from rank " << source << endl;
        }
    }
    return nullptr;
}

// Peer main function
void peer(int numtasks, int rank) {
    pthread_t download_thread;
    pthread_t upload_thread;
    void* statusPtr;

    PeerManager pmanager(rank, numtasks);
    // spacing out prints
    sleep(rank * 0.1);

    // Let tracker know how many files we own
    pmanager.send_my_nr_files();
    MPI_Barrier(MPI_COMM_WORLD);

    // Send file data for owned files
    pmanager.send_own_files_data();
    MPI_Barrier(MPI_COMM_WORLD);

    // Launch threads
    int r = pthread_create(&download_thread, nullptr, download_thread_func, (void*)&pmanager);
    if (r) {
        cerr << "[Peer " << rank << "] Error creating download thread.\n";
        exit(-1);
    }

    r = pthread_create(&upload_thread, nullptr, upload_thread_func, (void*)&pmanager);
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

    cout << "[Peer " << rank << "] All threads joined, shutting down.\n";
}

