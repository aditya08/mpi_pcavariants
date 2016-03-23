#include "hdf5.h"
#include "mpi.h"
#include <stdio.h>
#include <stdlib.h>
#include "cblas.h"

// datatype conversions taken from https://docs.oracle.com/cd/E19957-01/806-3593/11_cfort.html
extern void dsaupd_(int * ido, char * bmat, int * n, char * which,
                    int * nev, double * tol, double * resid, 
                    int * ncv, double * v, int * ldv,
                    int * iparam, int * ipntr, double * workd,
                    double * workl, int * lworkl, int * info);

// future optimizations: use memory-aligned mallocs

#define DEBUGFLAG 1

// Computes C = A'*(A*Omega)
// Scratch should have the size of (A*Omega)
void multiplyGramianChunk(double A[], double Omega[], double C[], double Scratch[], int rowsA, int colsA, int colsOmega); 

// computes a distributed matrix vector product against v, and updates v: v <- A'*A*v
void distributedMatVecProd(double v[]);

/* Local variables */
int mpi_size, mpi_rank;
MPI_Comm comm;
MPI_Info info;
double * Alocal, * Scratch, * Scratch2;
int numcols, numrows, numeigs;
int localrows, startingrow;

int main(int argc, char **argv) {

    char * infilename, * datasetname;

    /* HDF5 API definitions */
    hid_t plist_id, daccess_id, file_id, dataset_id, filespace, memspace; 
    herr_t status; 
    hsize_t offset[2], count[2], offset_out[2];

    /* MPI variables */
    comm = MPI_COMM_WORLD;
    info = MPI_INFO_NULL;

    /* Initialize MPI */
    MPI_Init(&argc, &argv);
    MPI_Comm_size(comm, &mpi_size);
    MPI_Comm_rank(comm, &mpi_rank);

    infilename = argv[1];
    datasetname = argv[2];
    numrows = atoi(argv[3]); // if you make this 6349440, the code will transparently ignore the remainder of the matrix
    numcols = atoi(argv[4]);
    numeigs = atoi(argv[5]);

    /* Allocate the correct portion of the input to each processor */
    localrows = numrows/mpi_size;
    startingrow = localrows*mpi_rank;
    printf("Rank %d: assigned %d rows, %d--%d\n", mpi_rank, localrows, startingrow, startingrow + localrows - 1);

    /* Load my portion of the data */

    plist_id = H5Pcreate(H5P_FILE_ACCESS);
    H5Pset_fapl_mpio(plist_id, comm, info);

    file_id = H5Fopen(infilename, H5F_ACC_RDONLY, plist_id);
    dataset_id = H5Dopen2(file_id, datasetname, H5P_DEFAULT);

    count[0] = numrows/mpi_size;
    count[1] = numcols;
    offset[0] = mpi_rank * count[0];
    offset[1] = 0;

    filespace = H5Dget_space(dataset_id);
    status = H5Sselect_hyperslab(filespace, H5S_SELECT_SET, offset, NULL, count, NULL);

    memspace = H5Screate_simple(2, count, NULL);
    offset_out[0] = 0;
    offset_out[1] = 0;
    Alocal = (double *) malloc( count[0]*count[1]*sizeof(double));
    status = H5Sselect_hyperslab(memspace, H5S_SELECT_SET, offset_out, NULL, count, NULL);
    daccess_id = H5Pcreate(H5P_DATASET_XFER);
    H5Pset_dxpl_mpio(daccess_id, H5FD_MPIO_COLLECTIVE);
    status = H5Dread(dataset_id, H5T_NATIVE_DOUBLE, memspace, filespace, daccess_id, Alocal);

    printf("Rank %d: loaded my data\n", mpi_rank);

    if (DEBUGFLAG) { 
       // display rows so we can check they're loaded correctly
       int rowIdx, colIdx;
       for (rowIdx = 0; rowIdx < localrows; rowIdx = rowIdx + 1) {
             for (colIdx = 0; colIdx < numcols; colIdx = colIdx + 1) {
                printf("A[%d][%d] = %f \n", rowIdx + startingrow, colIdx, Alocal[rowIdx*numcols + colIdx]);
            }
        } 
    }

    // distribute the initial vector
    double * vector = (double *) malloc( numcols * sizeof(double));
    if (mpi_rank == 0) {
        int idx;
        for( idx = 0; idx < numcols; idx = idx + 1) {
            vector[idx] = idx + 1;
        } 
    }
    MPI_Bcast(vector, numcols, MPI_DOUBLE, 0, comm);

    Scratch = (double *) malloc( numcols * sizeof(double));
    Scratch2 = (double *) malloc( numcols * sizeof(double));
    distributedMatVecProd(vector);

    if (DEBUGFLAG && mpi_rank < 2) { 
       // display final product so we can check the math
       int idx;
       for (idx = 0; idx < numcols; idx = idx + 1) {
            printf("A'*A x[%d]= %f \n", idx + 1, vector[idx]);
       }
    }

    // initial call to arpack
    int ido = 0;
    int ncv = 2*numeigs;
    int maxiter = 30;
    double tol = 1e-13;
    double * v = (double *) malloc(numcols * ncv *sizeof(double));
    int iparam[11] = {1, 0, 30, 1, 0, 0, 1, 0, 0, 0, 0};
    iparam[2] = maxiter;
    int ipntr[11];
    double * workd = (double *) malloc(3*numcols*sizeof(double));
    int lworkl = ncv*(ncv + 8);
    double * workl = (double *) malloc(lworkl*sizeof(double));
    int arpack_info = 0;

    char bmat = 'I';
    char which[3] = "LM";

    // initialize ARPACK
    if (mpi_rank == 0) {
        dsaupd_(&ido, &bmat, &numcols, which,
                &numeigs, &tol, vector, 
                &ncv, v, &numcols,
                iparam, ipntr, workd, 
                workl, &lworkl, &arpack_info);
    }

    

    H5Dclose(dataset_id);
    H5Sclose(memspace);
    H5Sclose(filespace);
    H5Pclose(plist_id);
    H5Fclose(file_id);

    MPI_Finalize();
    return 0;
}

void distributedMatVecProd(double v[]) {
    multiplyGramianChunk(Alocal, v, v, Scratch, numrows, numcols, 1); // write an appropriate mat-vec function
    MPI_Allreduce(v, Scratch2, numcols, MPI_DOUBLE, MPI_SUM, comm);
    cblas_dcopy(numcols, Scratch2, 1, v, 1);
}

/* computes A'*(A*Omega) = A*S , so Scratch must have size rowsA*colsOmega */
void multiplyGramianChunk(double A[], double Omega[], double C[], double Scratch[], int rowsA, int colsA, int colsOmega) {
    cblas_dgemm(CblasRowMajor, CblasNoTrans, CblasNoTrans, rowsA, colsOmega, colsA, 1.0, A, colsA, Omega, colsOmega, 0.0, Scratch, colsOmega);
    cblas_dgemm(CblasRowMajor, CblasTrans, CblasNoTrans, colsA, colsOmega, rowsA, 1.0, A, colsA, Scratch, colsOmega, 0.0, C, colsOmega);
}
