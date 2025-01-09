#include <mpi.h>
#include <pthread.h>
#include <iostream>
#include <vector>

#include "struct.h"
#include "tracker.h"
#include "peer.h"

using namespace std;

int main(int argc, char* argv[]) {
    int numtasks, rank;

    int provided;
    MPI_Init_thread(&argc, &argv, MPI_THREAD_MULTIPLE, &provided);
    if (provided < MPI_THREAD_MULTIPLE) {
        cerr << "MPI does not support multi-threading" << endl;
        exit(-1);
    }

    MPI_Comm_size(MPI_COMM_WORLD, &numtasks);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    if (rank == TRACKER_RANK) {
        tracker(numtasks, rank);
    } else {
        peer(numtasks, rank);
    }

    MPI_Finalize();
    return 0;
}
