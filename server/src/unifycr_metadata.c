/*
 * Copyright (c) 2017, Lawrence Livermore National Security, LLC.
 * Produced at the Lawrence Livermore National Laboratory.
 *
 * Copyright 2017-2019, UT-Battelle, LLC.
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
 * Written by: Teng Wang, Adam Moody, Wekuan Yu, Kento Sato, Kathryn Mohror
 * LLNL-CODE-728877. All rights reserved.
 *
 * This file is part of burstfs.
 * For details, see https://github.com/llnl/burstfs
 * Please read https://github.com/llnl/burstfs/LICNSE for full license text.
 */

#include <assert.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

#include "arraylist.h"
#include "indexes.h"
#include "mdhim.h"
#include "unifycr_log.h"
#include "unifycr_metadata.h"
#include "unifycr_clientcalls_rpc.h"
#include "ucr_read_builder.h"

unifycr_key_t** unifycr_keys;
unifycr_val_t** unifycr_vals;

fattr_key_t** fattr_keys;
fattr_val_t** fattr_vals;

char* manifest_path;

struct mdhim_brm_t* brm, *brmp;
struct mdhim_bgetrm_t* bgrm, *bgrmp;

mdhim_options_t* db_opts;
struct mdhim_t* md;

int md_size;
int unifycr_key_lens[MAX_META_PER_SEND] = {0};
int unifycr_val_lens[MAX_META_PER_SEND] = {0};

int fattr_key_lens[MAX_FILE_CNT_PER_NODE] = {0};
int fattr_val_lens[MAX_FILE_CNT_PER_NODE] = {0};

/* index structures */
struct index_t* unifycr_indexes[2] = {0};
struct index_t* unify_sec_indexes[2] = {0};

size_t max_recs_per_slice;

void debug_log_key_val(const char* ctx,
                       unifycr_key_t* key,
                       unifycr_val_t* val)
{
    if ((key != NULL) && (val != NULL)) {
        LOGDBG("@%s - key(fid=%d, offset=%lu), "
               "val(del=%d, len=%lu, addr=%lu, app=%d, rank=%d)",
               ctx, key->fid, key->offset,
               val->delegator_id, val->len, val->addr, val->app_id, val->rank);
    } else if (key != NULL) {
        LOGDBG("@%s - key(fid=%d, offset=%lu)",
               ctx, key->fid, key->offset);
    }
}

int unifycr_key_compare(unifycr_key_t* a, unifycr_key_t* b)
{
    assert((NULL != a) && (NULL != b));
    if (a->fid == b->fid) {
        if (a->offset == b->offset) {
            return 0;
        } else if (a->offset < b->offset) {
            return -1;
        } else {
            return 1;
        }
    } else if (a->fid < b->fid) {
        return -1;
    } else {
        return 1;
    }
}

/**
* initialize the key-value store
*/
int meta_init_store(unifycr_cfg_t* cfg)
{
    int rc, ser_ratio;
    size_t path_len;
    long svr_ratio, range_sz;
    MPI_Comm comm = MPI_COMM_WORLD;

    if (cfg == NULL) {
        return -1;
    }

    db_opts = calloc(1, sizeof(struct mdhim_options_t));
    if (db_opts == NULL) {
        return -1;
    }

    /* UNIFYCR_META_DB_PATH: file that stores the metadata */
    db_opts->db_path = strdup(cfg->meta_db_path);
    if (db_opts->db_path == NULL) {
        return -1;
    }

    db_opts->manifest_path = NULL;
    db_opts->db_type = LEVELDB;
    db_opts->db_create_new = 1;

    /* number of metadata servers =
     *   number of unifycr servers / UNIFYCR_META_SERVER_RATIO */
    svr_ratio = 0;
    rc = configurator_int_val(cfg->meta_server_ratio, &svr_ratio);
    if (rc != 0) {
        return -1;
    }
    ser_ratio = (int) svr_ratio;
    db_opts->rserver_factor = ser_ratio;

    db_opts->db_paths = NULL;
    db_opts->num_paths = 0;
    db_opts->num_wthreads = 1;

    path_len = strlen(db_opts->db_path) + strlen(MANIFEST_FILE_NAME) + 2;
    manifest_path = malloc(path_len);
    if (manifest_path == NULL) {
        return -1;
    }
    sprintf(manifest_path, "%s/%s", db_opts->db_path, MANIFEST_FILE_NAME);
    db_opts->manifest_path = manifest_path;

    db_opts->db_name = strdup(cfg->meta_db_name);
    if (db_opts->db_name == NULL) {
        return -1;
    }

    db_opts->db_key_type = MDHIM_UNIFYCR_KEY;
    db_opts->debug_level = MLOG_CRIT;

    /* indices/attributes are striped to servers according
     * to config setting for UNIFYCR_META_RANGE_SIZE.
     */
    range_sz = 0;
    rc = configurator_int_val(cfg->meta_range_size, &range_sz);
    if (rc != 0) {
        return -1;
    }
    max_recs_per_slice = (size_t) range_sz;
    db_opts->max_recs_per_slice = (uint64_t) range_sz;

    md = mdhimInit(&comm, db_opts);

    /*this index is created for storing index metadata*/
    unifycr_indexes[0] = md->primary_index;

    /*this index is created for storing file attribute metadata*/
    unifycr_indexes[1] = create_global_index(md, ser_ratio, 1,
                         LEVELDB, MDHIM_INT_KEY, "file_attr");

    /* secondary indexes */
    unify_sec_indexes[0] = create_global_index(md, ser_ratio, 1,
                           LEVELDB, MDHIM_INT_KEY, NULL);

    MPI_Comm_size(md->mdhim_comm, &md_size);

    rc = meta_init_indices();
    if (rc != 0) {
        return -1;
    }

    return 0;
}

/**
* initialize the key and value list used to
* put/get key-value pairs
* ToDo: split once the number of metadata exceeds MAX_META_PER_SEND
*/
int meta_init_indices(void)
{
    int i;

    /* init index metadata */
    unifycr_keys = (unifycr_key_t**)
        malloc(MAX_META_PER_SEND * sizeof(unifycr_key_t*));
    if (unifycr_keys == NULL) {
        return (int)UNIFYCR_ERROR_NOMEM;
    }

    unifycr_vals = (unifycr_val_t**)
        malloc(MAX_META_PER_SEND * sizeof(unifycr_val_t*));
    if (unifycr_vals == NULL) {
        return (int)UNIFYCR_ERROR_NOMEM;
    }

    for (i = 0; i < MAX_META_PER_SEND; i++) {
        unifycr_keys[i] = (unifycr_key_t*) calloc(1, sizeof(unifycr_key_t));
        if (unifycr_keys[i] == NULL) {
            return (int)UNIFYCR_ERROR_NOMEM;
        }

        unifycr_vals[i] = (unifycr_val_t*) calloc(1, sizeof(unifycr_val_t));
        if (unifycr_vals[i] == NULL) {
            return (int)UNIFYCR_ERROR_NOMEM;
        }
    }

    /* init attribute metadata */
    fattr_keys = (fattr_key_t**)
        malloc(MAX_FILE_CNT_PER_NODE * sizeof(fattr_key_t*));
    if (fattr_keys == NULL) {
        return (int)UNIFYCR_ERROR_NOMEM;
    }

    fattr_vals = (fattr_val_t**)
        malloc(MAX_FILE_CNT_PER_NODE * sizeof(fattr_val_t*));
    if (fattr_vals == NULL) {
        return (int)UNIFYCR_ERROR_NOMEM;
    }

    for (i = 0; i < MAX_FILE_CNT_PER_NODE; i++) {
        fattr_keys[i] = (fattr_key_t*) calloc(1, sizeof(fattr_key_t));
        if (fattr_keys[i] == NULL) {
            return (int)UNIFYCR_ERROR_NOMEM;
        }

        fattr_vals[i] = (fattr_val_t*) calloc(1, sizeof(fattr_val_t));
        if (fattr_vals[i] == NULL) {
            return (int)UNIFYCR_ERROR_NOMEM;
        }
    }

    return 0;
}

void print_bget_indices(int app_id, int cli_id,
                        send_msg_t* msgs, int tot_num)
{
    int i;
    for (i = 0; i < tot_num;  i++) {
        LOGDBG("index:dbg_rank:%d, dest_offset:%zu, "
               "dest_del_rank:%d, dest_cli_id:%d, dest_app_id:%d, "
               "length:%zu, src_app_id:%d, src_cli_id:%d, src_offset:%zu, "
               "src_del_rank:%d, src_fid:%d, num:%d",
               msgs[i].src_dbg_rank, msgs[i].dest_offset,
               msgs[i].dest_delegator_rank, msgs[i].dest_client_id,
               msgs[i].dest_app_id, msgs[i].length,
               msgs[i].src_app_id, msgs[i].src_cli_id,
               msgs[i].src_offset, msgs[i].src_delegator_rank,
               msgs[i].src_fid, tot_num);
    }
}

void print_fsync_indices(unifycr_key_t** unifycr_keys,
                         unifycr_val_t** unifycr_vals,
                         size_t num_entries)
{
    size_t i;
    for (i = 0; i < num_entries; i++) {
        LOGDBG("fid:%d, offset:%lu, addr:%lu, len:%lu, del_id:%d",
               unifycr_keys[i]->fid, unifycr_keys[i]->offset,
               unifycr_vals[i]->addr, unifycr_vals[i]->len,
               unifycr_vals[i]->delegator_id);
    }
}

void meta_free_indices(void)
{
    int i;
    for (i = 0; i < MAX_META_PER_SEND; i++) {
        if (NULL != unifycr_keys[i]) {
            free(unifycr_keys[i]);
        }
        if (NULL != unifycr_vals[i]) {
            free(unifycr_vals[i]);
        }
    }
    free(unifycr_keys);
    free(unifycr_vals);

    for (i = 0; i < MAX_FILE_CNT_PER_NODE; i++) {
        if (NULL != fattr_keys[i]) {
            free(fattr_keys[i]);
        }
        if (NULL != fattr_vals[i]) {
            free(fattr_vals[i]);
        }
    }
    free(fattr_keys);
    free(fattr_vals);
}

int meta_sanitize(void)
{
    int rc = UNIFYCR_SUCCESS;

    char dbfilename[UNIFYCR_MAX_FILENAME] = {0};
    char statfilename[UNIFYCR_MAX_FILENAME] = {0};
    char manifestname[UNIFYCR_MAX_FILENAME] = {0};

    char dbfilename1[UNIFYCR_MAX_FILENAME] = {0};
    char statfilename1[UNIFYCR_MAX_FILENAME] = {0};
    char manifestname1[UNIFYCR_MAX_FILENAME] = {0};
    sprintf(dbfilename, "%s/%s-%d-%d", md->db_opts->db_path,
            md->db_opts->db_name, unifycr_indexes[0]->id, md->mdhim_rank);

    sprintf(statfilename, "%s_stats", dbfilename);
    sprintf(manifestname, "%s%d_%d_%d", md->db_opts->manifest_path,
            unifycr_indexes[0]->type,
            unifycr_indexes[0]->id, md->mdhim_rank);

    sprintf(dbfilename1, "%s/%s-%d-%d", md->db_opts->db_path,
            md->db_opts->db_name, unifycr_indexes[1]->id, md->mdhim_rank);

    sprintf(statfilename1, "%s_stats", dbfilename1);
    sprintf(manifestname1, "%s%d_%d_%d", md->db_opts->manifest_path,
            unifycr_indexes[1]->type,
            unifycr_indexes[1]->id, md->mdhim_rank);

    mdhimClose(md);
    rc = mdhimSanitize(dbfilename, statfilename, manifestname);
    rc = mdhimSanitize(dbfilename1, statfilename1, manifestname1);

    mdhim_options_destroy(db_opts);

    meta_free_indices();

    return rc;
}

// New API
/*
 *
 */
int unifycr_set_file_attribute(unifycr_file_attr_t* fattr_ptr)
{
    int rc = UNIFYCR_SUCCESS;

    int gfid = fattr_ptr->gfid;

    md->primary_index = unifycr_indexes[1];
    brm = mdhimPut(md, &gfid, sizeof(int),
                   fattr_ptr, sizeof(unifycr_file_attr_t),
                   NULL, NULL);
    if (!brm || brm->error) {
        // return UNIFYCR_ERROR_MDHIM on error
        rc = (int)UNIFYCR_ERROR_MDHIM;
    }

    mdhim_full_release_msg(brm);

    return rc;
}

int unify_set_file_attribute(unifycr_file_attr_t* fattr_ptr, int parent_gfid)
{
    int rc = UNIFYCR_SUCCESS;

    struct secondary_info *secondary_info;
    int gfid = fattr_ptr->gfid;
    int **secondary_keys;
    int *secondary_key_lens;

    secondary_keys = malloc(sizeof(int *));
    secondary_keys[0] = malloc(sizeof(int));
    secondary_key_lens = malloc(sizeof(int));

    *secondary_keys[0] = parent_gfid;
    secondary_key_lens[0] = sizeof(parent_gfid);

    /* create info for secondary key */
    secondary_info = mdhimCreateSecondaryInfo(unify_sec_indexes[0],
                                              (void **) secondary_keys, 
                                              secondary_key_lens, 1, 
                                              SECONDARY_GLOBAL_INFO);

    printf("fattr_ptr: {gfid: 0x%x, fid: 0x%x}\n", fattr_ptr->gfid, fattr_ptr->fid); fflush(NULL);

    md->primary_index = unifycr_indexes[1];
    brm = mdhimPut(md, &gfid, sizeof(int), fattr_ptr,
                   sizeof(unifycr_file_attr_t), secondary_info, NULL);

    if (!brm || brm->error) {
        // return UNIFYCR_ERROR_MDHIM on error
        rc = (int)UNIFYCR_ERROR_MDHIM;
    }

    mdhimReleaseSecondaryInfo(secondary_info);
    mdhim_full_release_msg(brm);

    free(secondary_keys[0]);
    free(secondary_keys);
    free(secondary_key_lens);

    //Commit the database
	rc = mdhimCommit(md, md->primary_index);
	if (rc != MDHIM_SUCCESS) {
		printf("Error committing MDHIM database\n");
	} else {
		printf("Committed MDHIM database\n");
	}

    //Get the stats for the primary index
	rc = mdhimStatFlush(md, md->primary_index);
	if (rc != MDHIM_SUCCESS) {
		printf("Error getting stats\n");
	} else {
		printf("Got stats\n");
	}

    //Get the stats for the secondary index
	rc = mdhimStatFlush(md, unify_sec_indexes[0]);
	if (rc != MDHIM_SUCCESS) {
		printf("Error getting stats\n");
	} else {
		printf("Got stats\n");
	}

    return rc;
}

/*
 *
 */
int unifycr_set_file_attributes(int num_entries,
                                fattr_key_t** keys, int* key_lens,
                                unifycr_file_attr_t** fattr_ptr, int* val_lens)
{
    int rc = UNIFYCR_SUCCESS;

    md->primary_index = unifycr_indexes[1];
    brm = mdhimBPut(md, (void**)keys, key_lens, (void**)fattr_ptr,
                    val_lens, num_entries, NULL, NULL);
    brmp = brm;
    if (!brmp || brmp->error) {
        rc = (int)UNIFYCR_ERROR_MDHIM;
        LOGERR("Error inserting keys/values into MDHIM");
    }

    while (brmp) {
        if (brmp->error < 0) {
            rc = (int)UNIFYCR_ERROR_MDHIM;
            break;
        }

        brm = brmp;
        brmp = brmp->next;
        mdhim_full_release_msg(brm);
    }

    return rc;
}

/*
 *
 */
int unifycr_get_file_attribute(int gfid,
                               unifycr_file_attr_t* attr_val_ptr)
{
    int rc = UNIFYCR_SUCCESS;
    unifycr_file_attr_t* tmp_ptr_attr;

    md->primary_index = unifycr_indexes[1];
    bgrm = mdhimGet(md, md->primary_index, &gfid,
                    sizeof(int), MDHIM_GET_EQ);
    if (!bgrm || bgrm->error) {
        rc = (int)UNIFYCR_ERROR_MDHIM;
    } else {
        tmp_ptr_attr = (unifycr_file_attr_t*)bgrm->values[0];
        memcpy(attr_val_ptr, tmp_ptr_attr, sizeof(unifycr_file_attr_t));
        mdhim_full_release_msg(bgrm);
    }

    return rc;
}

/*
 *
 */
int unify_get_child_file_attributes(int gfid, int* num_values,
                                    unifycr_file_attr_t** attr_vals)
{
    int i, tot_num = 0;
    int rc = UNIFYCR_SUCCESS;
    unifycr_file_attr_t* attr_p;
    unifycr_file_attr_t* viter = *attr_vals;

    struct mdhim_bgetrm_t *bgrm;
    struct mdhim_bgetrm_t *bgrmp;

    int **secondary_keys;
    int *secondary_key_lens;
    secondary_keys = malloc(sizeof(int *));
    secondary_keys[0] = malloc(2 * sizeof(int));
    secondary_key_lens = malloc(2 * sizeof(int));

    *(secondary_keys[0]) = gfid;
    *(secondary_keys[1]) = gfid;
    secondary_key_lens[0] = sizeof(gfid);
    secondary_key_lens[1] = sizeof(gfid);

    md->primary_index = unifycr_indexes[1];
    bgrm = mdhimBGet(md, unify_sec_indexes[0], 
	 			     secondary_keys, secondary_key_lens, 2, MDHIM_GET_EQ);
    printf("After mdhimBGetOp\n");fflush(NULL);
	// if (!bgrm || bgrm->error) {
	// 	printf("Rank: %d, Error getting next key/value given key: 0x%x from MDHIM (error: %d)\n", 
	// 	       md->mdhim_rank, gfid, bgrm->error);
	// } else if (bgrm->keys && bgrm->values) {
    //     // attr_p = (unifycr_file_attr_t*)(bgrm->values[0]);
    //     // memcpy(viter, attr_p, sizeof(unifycr_file_attr_t));
    //     // printf("viter: {gfid: 0x%x, fid: 0x%x}, attr_p: {gfid: 0x%x, fid: 0x%x}\n", viter->gfid, viter->fid, attr_p->gfid, attr_p->fid); fflush(NULL);

    //     printf("# keys = %d\n", bgrm->num_keys);fflush(NULL);
    //     for (i = 0; i < bgrm->num_keys; i++) {
    //         attr_p = (unifycr_file_attr_t*)(bgrm->values[i]);
    //         memcpy(viter, attr_p, sizeof(unifycr_file_attr_t));
    //         printf("viter: {gfid: 0x%x, fid: 0x%x}, attr_p: {gfid: 0x%x, fid: 0x%x}\n", viter->gfid, viter->fid, attr_p->gfid, attr_p->fid); fflush(NULL);
    //         viter++;
    //         tot_num++;
    //         if (MAX_META_PER_SEND == tot_num) {
    //             LOGERR("Error: maximum number of values!");
    //             rc = UNIFYCR_FAILURE;
    //             break;
    //         }
    //     }
	// }

    if (!bgrm || bgrm->error) {
		printf("Rank: %d, Error getting next key/value given key: 0x%x from MDHIM (error: %d)\n", 
		       md->mdhim_rank, gfid, bgrm->error);
	} else {
        while (bgrm) {
            bgrmp = bgrm;
            if (bgrmp->error < 0) {
                // TODO: need better error handling
                printf("Got error\n");
                rc = (int)UNIFYCR_ERROR_MDHIM;
                return rc;
            }

            if (tot_num < MAX_META_PER_SEND) {
                printf("# keys = %d\n", bgrmp->num_keys);
                for (i = 0; i < bgrmp->num_keys; i++) {
                    attr_p = (unifycr_file_attr_t*)(bgrmp->values[i]);
                    memcpy(viter, attr_p, sizeof(unifycr_file_attr_t));
                    printf("viter: {gfid: 0x%x, fid: 0x%x}, attr_p: {gfid: 0x%x, fid: 0x%x}\n", viter->gfid, viter->fid, attr_p->gfid, attr_p->fid); fflush(NULL);
                    viter++;
                    tot_num++;
                    if (MAX_META_PER_SEND == tot_num) {
                        LOGERR("Error: maximum number of values!");
                        rc = UNIFYCR_FAILURE;
                        break;
                    }
                }
            }
            bgrm = bgrmp->next;
            mdhim_full_release_msg(bgrmp);
        }
    }



    // printf("Before mdhimBGetOp\n");fflush(NULL);
    // bgrm = mdhimBGetOp(md, unify_sec_indexes[0], 
	// 			       &gfid, sizeof(gfid), 1, MDHIM_GET_NEXT);
    // printf("After mdhimBGetOp\n");fflush(NULL);
	// 	if (!bgrm || bgrm->error) {
	// 		printf("Rank: %d, Error getting next key/value given key: 0x%x from MDHIM (error: %d)\n", 
	// 		       md->mdhim_rank, gfid, bgrm->error);
	// 	} else if (bgrm->keys && bgrm->values) {
    //         attr_p = (unifycr_file_attr_t*)(bgrmp->values[0]);
    //         memcpy(viter, attr_p, sizeof(unifycr_file_attr_t));
    //         printf("viter: {gfid: 0x%x, fid: 0x%x}, attr_p: {gfid: 0x%x, fid: 0x%x}\n", viter->gfid, viter->fid, attr_p->gfid, attr_p->fid); fflush(NULL);
	// 	}

	// 	mdhim_full_release_msg(bgrm);

    // get the primary keys
    // bgrm = mdhimGet(md, unify_sec_indexes[0], 
	// 		&gfid, sizeof(int), 
	// 		MDHIM_GET_PRIMARY_EQ);
    // if (!bgrm || bgrm->error) {
	// 	printf("Error getting value for key: 0x%x from MDHIM\n", gfid);
	// } else if (bgrm->value_lens[0]) {
	// 	printf("Successfully got value: 0x%x from MDHIM\n", *((int *) bgrm->values[0]));
	// }

    *num_values = tot_num;
    return rc;
}

/*
 *
 */
int unifycr_get_file_extents(int num_keys, unifycr_key_t** keys,
                             int* unifycr_key_lens, int* num_values,
                             unifycr_keyval_t** keyvals)
{
    /*
     * This is using a modified version of mdhim. The function will return all
     * key-value pairs within the range of the key tuple.
     * We need to re-evaluate this function to use different key-value stores.
     */

    int i;
    int rc = UNIFYCR_SUCCESS;
    int tot_num = 0;

    unifycr_key_t* tmp_key;
    unifycr_val_t* tmp_val;
    unifycr_keyval_t* kviter = *keyvals;

    md->primary_index = unifycr_indexes[0];
    bgrm = mdhimBGet(md, md->primary_index, (void**)keys,
                     unifycr_key_lens, num_keys, MDHIM_RANGE_BGET);

    while (bgrm) {
        bgrmp = bgrm;
        if (bgrmp->error < 0) {
            // TODO: need better error handling
            rc = (int)UNIFYCR_ERROR_MDHIM;
            return rc;
        }

        if (tot_num < MAX_META_PER_SEND) {
            for (i = 0; i < bgrmp->num_keys; i++) {
                tmp_key = (unifycr_key_t*)bgrmp->keys[i];
                tmp_val = (unifycr_val_t*)bgrmp->values[i];
                memcpy(&(kviter->key), tmp_key, sizeof(unifycr_key_t));
                memcpy(&(kviter->val), tmp_val, sizeof(unifycr_val_t));
                kviter++;
                tot_num++;
                if (MAX_META_PER_SEND == tot_num) {
                    LOGERR("Error: maximum number of values!");
                    rc = UNIFYCR_FAILURE;
                    break;
                }
            }
        }
        bgrm = bgrmp->next;
        mdhim_full_release_msg(bgrmp);
    }

    *num_values = tot_num;

    return rc;
}

/*
 *
 */
int unifycr_set_file_extents(int num_entries,
                             unifycr_key_t** keys, int* unifycr_key_lens,
                             unifycr_val_t** vals, int* unifycr_val_lens)
{
    int rc = UNIFYCR_SUCCESS;

    md->primary_index = unifycr_indexes[0];

    brm = mdhimBPut(md, (void**)(keys), unifycr_key_lens,
                    (void**)(vals), unifycr_val_lens, num_entries,
                    NULL, NULL);
    brmp = brm;
    if (!brmp || brmp->error) {
        rc = (int)UNIFYCR_ERROR_MDHIM;
        LOGERR("Error inserting keys/values into MDHIM");
    }

    while (brmp) {
        if (brmp->error < 0) {
            rc = (int)UNIFYCR_ERROR_MDHIM;
            break;
        }

        brm = brmp;
        brmp = brmp->next;
        mdhim_full_release_msg(brm);
    }

    return rc;
}

