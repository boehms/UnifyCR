/*
 * Copyright (c) 2017, Lawrence Livermore National Security, LLC.
 * Produced at the Lawrence Livermore National Laboratory.
 *
 * Copyright 2017, UT-Battelle, LLC.
 *
 * LLNL-CODE-741539
 * All rights reserved.
 *
 * This is the license for UnifyCR.
 * For details, see https://github.com/LLNL/UnifyCR.
 * Please read https://github.com/LLNL/UnifyCR/LICENSE for full license text.
 */

/*
 * Copyright (c) 2017, Lawrence Livermore National Security, LLC.
 * Produced at the Lawrence Livermore National Laboratory.
 * Copyright (c) 2017, Florida State University. Contributions from
 * the Computer Architecture and Systems Research Laboratory (CASTL)
 * at the Department of Computer Science.
 *
 * Written by: Teng Wang, Adam Moody, Weikuan Yu, Kento Sato, Kathryn Mohror
 * LLNL-CODE-728877. All rights reserved.
 *
 * This file is part of burstfs.
 * For details, see https://github.com/llnl/burstfs
 * Please read https://github.com/llnl/burstfs/LICENSE for full license text.
 */

/*
 * Copyright (c) 2013, Lawrence Livermore National Security, LLC.
 * Produced at the Lawrence Livermore National Laboratory.
 * code Written by
 *   Raghunath Rajachandrasekar <rajachan@cse.ohio-state.edu>
 *   Kathryn Mohror <kathryn@llnl.gov>
 *   Adam Moody <moody20@llnl.gov>
 * All rights reserved.
 * This file is part of CRUISE.
 * For details, see https://github.com/hpc/cruise
 * Please also read this file LICENSE.CRUISE
 */
#include <config.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <mpi.h>
#include <sys/time.h>
#include <aio.h>
#include <unifycr.h>

#define GEN_STR_LEN 1024

struct timeval read_start, read_end;
double readtime;

struct timeval write_start, write_end;
double write_time;

struct timeval meta_start, meta_end;
double meta_time;

struct timeval read_start, read_end;
double read_time;

typedef struct {
    int fid;
    long offset;
    long length;
    char *buf;
} read_req_t;

int main(int argc, char *argv[])
{
    static const char *opts = "b:s:t:f:p:u:";
    char tmpfname[GEN_STR_LEN], fname[GEN_STR_LEN];
    long blk_sz = 0, seg_num = 0, tran_sz = 0, num_reqs = 0;
    int pat = 0, c, rank_num, rank, fd, to_unmount = 0;

    MPI_Init(&argc, &argv);
    MPI_Comm_size(MPI_COMM_WORLD, &rank_num);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    while ((c = getopt(argc, argv, opts)) != -1) {
        switch (c) {
        case 'b': /*size of block*/
            blk_sz = atol(optarg); break;
        case 's': /*number of blocks each process writes*/
            seg_num = atol(optarg); break;
        case 't': /*size of each write */
            tran_sz = atol(optarg); break;
        case 'f':
            strcpy(fname, optarg); break;
        case 'p':
            pat = atoi(optarg); break; /* 0: N-1 segment/strided, 1: N-N*/
        case 'u':
            to_unmount = atoi(optarg); break;
        }
    }

    unifycr_mount("/tmp", rank, rank_num, 0);

    char *buf = malloc(tran_sz);

    if (buf == NULL)
        return -1;

    memset(buf, 0, tran_sz);

    MPI_Barrier(MPI_COMM_WORLD);

    if (pat == 1)
        sprintf(tmpfname, "%s%d", fname, rank);
    else
        sprintf(tmpfname, "%s", fname);

    fd = open(tmpfname, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        printf("open file failure\n");
        fflush(stdout);
        return -1;
    }

    gettimeofday(&write_start, NULL);

    long i, j, offset = 0, rc;

    for (i = 0; i < seg_num; i++) {
        for (j = 0; j < blk_sz/tran_sz; j++) {
            if (pat == 0)
                offset = i*rank_num*blk_sz + rank*blk_sz + j*tran_sz;
            else if (pat == 1)
                offset = i*blk_sz + j*tran_sz;
            rc = pwrite(fd, buf, tran_sz, offset);

            if (rc < 0)
                perror("pwrite failed");
        }
    }

    gettimeofday(&meta_start, NULL);
    fsync(fd);
    gettimeofday(&meta_end, NULL);
    meta_time += 1000000 * (meta_end.tv_sec - meta_start.tv_sec)
                 + meta_end.tv_usec - meta_start.tv_usec;
    meta_time /= 1000000;
    gettimeofday(&write_end, NULL);
    write_time += 1000000 * (write_end.tv_sec - write_start.tv_sec)
                + write_end.tv_usec - write_start.tv_usec;
    write_time = write_time/1000000;
    MPI_Barrier(MPI_COMM_WORLD);

    free(buf);

    num_reqs = blk_sz*seg_num/tran_sz;

    char *read_buf = malloc(blk_sz * seg_num); /*read buffer*/
    struct aiocb *aiocb_list = (struct aiocb *)malloc(num_reqs
            * sizeof(struct aiocb));
    struct aiocb **cb_list = (struct aiocb **)malloc(num_reqs *
            sizeof(struct aiocb *)); /*list of read requests in lio_listio*/

    gettimeofday(&read_start, NULL);

    long index;

    if (pat == 0) { /* N-1 */
        long i, j;

        for (i = 0; i < seg_num; i++) {
            for (j = 0; j < blk_sz/tran_sz; j++) {
                index = i * (blk_sz/tran_sz) + j;
                aiocb_list[index].aio_fildes = fd;
                aiocb_list[index].aio_buf = read_buf + index*tran_sz;
                aiocb_list[index].aio_nbytes = tran_sz;
                aiocb_list[index].aio_offset = i*rank_num*blk_sz
                                               + rank*blk_sz + j*tran_sz;
                aiocb_list[index].aio_lio_opcode = LIO_READ;
                cb_list[index] = &aiocb_list[index];
            }
        }
    } else {
        if (pat == 1) { /* N-N */
            long i, j;

            for (i = 0; i < seg_num; i++) {
                for (j = 0; j < blk_sz/tran_sz; j++) {
                    index = i * (blk_sz/tran_sz) + j;
                    aiocb_list[index].aio_fildes = fd;
                    aiocb_list[index].aio_buf = read_buf + index * tran_sz;
                    aiocb_list[index].aio_nbytes = tran_sz;
                    aiocb_list[index].aio_offset = i * blk_sz + j * tran_sz;
                    aiocb_list[index].aio_lio_opcode = LIO_READ;
                    cb_list[index] = &aiocb_list[index];
                }
            }

        } else {
            printf("unsupported I/O pattern");
            fflush(stdout);
        }
    }

    int ret = lio_listio(LIO_WAIT, cb_list, num_reqs, NULL);

    if (ret < 0)
        perror("lio_listio failed");

    gettimeofday(&read_end, NULL);

    read_time = (read_end.tv_sec - read_start.tv_sec)*1000000
                + read_end.tv_usec - read_start.tv_usec;
    read_time = read_time/1000000;

    close(fd);

    MPI_Barrier(MPI_COMM_WORLD);

    if (to_unmount) {
        if (rank == 0)
            unifycr_unmount();
    }
    free(read_buf);

    double write_bw = (double) blk_sz*seg_num/1048576/write_time;
    double agg_write_bw;

    MPI_Reduce(&write_bw, &agg_write_bw, 1, MPI_DOUBLE, MPI_SUM,
               0, MPI_COMM_WORLD);

    double max_write_time;

    MPI_Reduce(&write_time, &max_write_time, 1, MPI_DOUBLE, MPI_MAX,
               0, MPI_COMM_WORLD);

    double min_write_bw;

    min_write_bw = (double) blk_sz*seg_num*rank_num/1048576/max_write_time;

    if (rank == 0) {
        printf("Aggregate Write BW is %lfMB/s\n"
               "Min Write BW is %lfMB/s\n",
               agg_write_bw, min_write_bw);
        fflush(stdout);
    }

    double read_bw = (double) blk_sz*seg_num/1048576/read_time;
    double agg_read_bw;
    double max_read_time, min_read_bw;

    MPI_Reduce(&read_bw, &agg_read_bw, 1, MPI_DOUBLE, MPI_SUM,
               0, MPI_COMM_WORLD);
    MPI_Reduce(&read_time, &max_read_time, 1, MPI_DOUBLE, MPI_MAX,
               0, MPI_COMM_WORLD);

    min_read_bw = (double) blk_sz*seg_num*rank_num/1048576/max_read_time;
    if (rank == 0) {
        printf("Aggregate Read BW is %lfMB/s\n"
               "Min Read BW is %lf\n",
               agg_read_bw,  min_read_bw);
        fflush(stdout);
    }

    MPI_Finalize();

    return 0;
}
