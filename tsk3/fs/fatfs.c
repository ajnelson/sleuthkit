/*
** fatfs
** The Sleuth Kit 
**
** Content and meta data layer support for the FAT file system 
**
** Brian Carrier [carrier <at> sleuthkit [dot] org]
** Copyright (c) 2006-2008 Brian Carrier, Basis Technology.  All Rights reserved
** Copyright (c) 2003-2005 Brian Carrier.  All rights reserved 
**
** TASK
** Copyright (c) 2002 Brian Carrier, @stake Inc.  All rights reserved
**
**
** This software is distributed under the Common Public License 1.0
**
** Unicode added with support from I.D.E.A.L. Technology Corp (Aug '05)
**
*/

/**
 * \file fatfs.c
 * Contains the internal TSK FAT file system code to handle basic file system 
 * processing for opening file system, processing sectors, and directory entries. 
 */
#include "tsk_fs_i.h"
#include "tsk_fatfs.h"

#define XTAF_SECTOR_SIZE 512
#define XTAF_SECT_PER_FAT 64
#define XTAF_FIRST_FAT_SECT 8
#define XTAF_ROOT_SECT 116808 
#define HD_VOID_AREA 8

/*
 * Implementation NOTES 
 *
 * TSK_FS_META contains the first cluster.  file_walk will return sector
 * values though because the cluster numbers do not start until after
 * the FAT.  That makes it very hard to address the first few blocks!
 *
 * Inodes numbers do not exist in FAT.  To make up for this we will count
 * directory entries as the inodes.   As the root directory does not have
 * any records in FAT, we will give it times of 0 and call it inode 2 to
 * keep consistent with UNIX.  After that, each 32-byte slot is numbered
 * as though it were a directory entry (even if it is not).  Therefore,
 * when an inode walk is performed, not all inode values will be displayed
 * even when '-e' is given for ils. 
 *
 * Progs like 'ils -e' are very slow because we have to look at each
 * block to see if it is a file system structure.
 */




/* TTL is 0 if the entry has not been used.  TTL of 1 means it was the
 * most recently used, and TTL of FAT_CACHE_N means it was the least 
 * recently used.  This function has a LRU replacement algo
 */
// return -1 on error, or cache index on success (0 to FAT_CACHE_N)

static int
getFATCacheIdx(FATFS_INFO * fatfs, TSK_DADDR_T sect)
{
    int i, cidx;
    ssize_t cnt;
    TSK_FS_INFO *fs = (TSK_FS_INFO *) & fatfs->fs_info;

    // see if we already have it in the cache
    for (i = 0; i < FAT_CACHE_N; i++) {
        if ((fatfs->fatc_ttl[i] > 0) &&
            (sect >= fatfs->fatc_addr[i]) &&
            (sect < (fatfs->fatc_addr[i] + FAT_CACHE_S))) {
            int a;

            // update the TTLs to push i to the front
            for (a = 0; a < FAT_CACHE_N; a++) {
                if (fatfs->fatc_ttl[a] == 0)
                    continue;

                if (fatfs->fatc_ttl[a] < fatfs->fatc_ttl[i])
                    fatfs->fatc_ttl[a]++;
            }
            fatfs->fatc_ttl[i] = 1;
//          fprintf(stdout, "FAT Hit: %d\n", sect);
//          fflush(stdout);
            return i;
        }
    }

//    fprintf(stdout, "FAT Miss: %d\n", (int)sect);
//    fflush(stdout);

    // Look for an unused entry or an entry with a TTL of FAT_CACHE_N
    cidx = 0;
    for (i = 0; i < FAT_CACHE_N; i++) {
        if ((fatfs->fatc_ttl[i] == 0) ||
            (fatfs->fatc_ttl[i] >= FAT_CACHE_N)) {
            cidx = i;
        }
    }
//    fprintf(stdout, "FAT Removing: %d\n", (int)fatfs->fatc_addr[cidx]);
    //   fflush(stdout);

    // read the data
    cnt =
        tsk_fs_read(fs, sect * fs->block_size, fatfs->fatc_buf[cidx],
        FAT_CACHE_B);
    if (cnt != FAT_CACHE_B) {
        if (cnt >= 0) {
            tsk_error_reset();
            tsk_errno = TSK_ERR_FS_READ;
        }
        snprintf(tsk_errstr2, TSK_ERRSTR_L,
            "getFATCacheIdx: FAT: %" PRIuDADDR, sect);
        return -1;
    }

    // update the TTLs
    if (fatfs->fatc_ttl[cidx] == 0)     // special case for unused entry
        fatfs->fatc_ttl[cidx] = FAT_CACHE_N + 1;

    for (i = 0; i < FAT_CACHE_N; i++) {
        if (fatfs->fatc_ttl[i] == 0)
            continue;

        if (fatfs->fatc_ttl[i] < fatfs->fatc_ttl[cidx])
            fatfs->fatc_ttl[i]++;
    }

    fatfs->fatc_ttl[cidx] = 1;
    fatfs->fatc_addr[cidx] = sect;
    return cidx;
}


/*
 * Set *value to the entry in the File Allocation Table (FAT) 
 * for the given cluster
 *
 * *value is in clusters and may need to be coverted to
 * sectors by the calling function
 *
 * Invalid values in the FAT (i.e. greater than the largest
 * cluster have a value of 0 returned and a 0 return value.
 *
 * Return 1 on error and 0 on success
 */
uint8_t
fatfs_getFAT(FATFS_INFO * fatfs, TSK_DADDR_T clust, TSK_DADDR_T * value)
{
    uint8_t *a_ptr;
    uint16_t tmp16;
    TSK_FS_INFO *fs = (TSK_FS_INFO *) & fatfs->fs_info;
    TSK_DADDR_T sect, offs;
    ssize_t cnt;
    int cidx;

    /* Sanity Check */
    if (clust > fatfs->lastclust) {
        /* silently ignore requests for the unclustered sectors... */
        if ((clust == fatfs->lastclust + 1) &&
            ((fatfs->firstclustsect + fatfs->csize * fatfs->clustcnt -
                    1) != fs->last_block)) {
            if (tsk_verbose)
                tsk_fprintf(stderr,
                    "fatfs_getFAT: Ignoring request for non-clustered sector\n");
            return 0;
        }

        tsk_error_reset();
        tsk_errno = TSK_ERR_FS_ARG;
        snprintf(tsk_errstr, TSK_ERRSTR_L,
            "fatfs_getFAT: invalid cluster address: %" PRIuDADDR, clust);
        return 1;
    }

    switch (fatfs->fs_info.ftype) {
    case TSK_FS_TYPE_FAT12:
        if (clust & 0xf000) {
            tsk_error_reset();
            tsk_errno = TSK_ERR_FS_ARG;
            snprintf(tsk_errstr, TSK_ERRSTR_L,
                "fatfs_getFAT: TSK_FS_TYPE_FAT12 Cluster %" PRIuDADDR
                " too large", clust);
            return 1;
        }

        /* id the sector in the FAT */
        sect = fatfs->firstfatsect +
            ((clust + (clust >> 1)) >> fatfs->ssize_sh);

        /* Load the FAT if we don't have it */
        // see if it is in the cache
        if (-1 == (cidx = getFATCacheIdx(fatfs, sect)))
            return 1;

        /* get the offset into the cache */
        offs = ((sect - fatfs->fatc_addr[cidx]) << fatfs->ssize_sh) +
            (clust + (clust >> 1)) % fatfs->ssize;

        /* special case when the 12-bit value goes across the cache
         * we load the cache to start at this sect.  The cache
         * size must therefore be at least 2 sectors large 
         */
        if (offs == (FAT_CACHE_B - 1)) {

            // read the data -- TTLs will already have been updated
            cnt =
                tsk_fs_read(fs, sect * fs->block_size,
                fatfs->fatc_buf[cidx], FAT_CACHE_B);
            if (cnt != FAT_CACHE_B) {
                if (cnt >= 0) {
                    tsk_error_reset();
                    tsk_errno = TSK_ERR_FS_READ;
                }
                snprintf(tsk_errstr2, TSK_ERRSTR_L,
                    "fatfs_getFAT: TSK_FS_TYPE_FAT12 FAT overlap: %"
                    PRIuDADDR, sect);
                return 1;
            }
            fatfs->fatc_addr[cidx] = sect;

            offs = (clust + (clust >> 1)) % fatfs->ssize;
        }

        /* get pointer to entry in current buffer */
        a_ptr = (uint8_t *) fatfs->fatc_buf[cidx] + offs;

        tmp16 = tsk_getu16(fs->endian, a_ptr);

        /* slide it over if it is one of the odd clusters */
        if (clust & 1)
            tmp16 >>= 4;

        *value = tmp16 & FATFS_12_MASK;

        /* sanity check */
        if ((*value > (fatfs->lastclust)) &&
            (*value < (0x0ffffff7 & FATFS_12_MASK))) {
            if (tsk_verbose)
                tsk_fprintf(stderr,
                    "fatfs_getFAT: TSK_FS_TYPE_FAT12 cluster (%" PRIuDADDR
                    ") too large (%" PRIuDADDR ") - resetting\n", clust,
                    *value);
            *value = 0;
        }
        return 0;

    case TSK_FS_TYPE_FAT16:
        /* Get sector in FAT for cluster and load it if needed */
        sect = fatfs->firstfatsect + ((clust << 1) >> fatfs->ssize_sh);
        if (-1 == (cidx = getFATCacheIdx(fatfs, sect)))
            return 1;

        /* get pointer to entry in the cache buffer */
        a_ptr = (uint8_t *) fatfs->fatc_buf[cidx] +
            ((sect - fatfs->fatc_addr[cidx]) << fatfs->ssize_sh) +
            ((clust << 1) % fatfs->ssize);

        *value = tsk_getu16(fs->endian, a_ptr) & FATFS_16_MASK;

        /* sanity check */
        if ((*value > (fatfs->lastclust)) &&
            (*value < (0x0ffffff7 & FATFS_16_MASK))) {
            if (tsk_verbose)
                tsk_fprintf(stderr,
                    "fatfs_getFAT: contents of TSK_FS_TYPE_FAT16 entry %"
                    PRIuDADDR " too large - resetting\n", clust);
            *value = 0;
        }
        return 0;

    case TSK_FS_TYPE_FAT32:
        /* Get sector in FAT for cluster and load if needed */
        sect = fatfs->firstfatsect + ((clust << 2) >> fatfs->ssize_sh);
        if (-1 == (cidx = getFATCacheIdx(fatfs, sect)))
            return 1;

        /* get pointer to entry in current buffer */
        a_ptr = (uint8_t *) fatfs->fatc_buf[cidx] +
            ((sect - fatfs->fatc_addr[cidx]) << fatfs->ssize_sh) +
            (clust << 2) % fatfs->ssize;

        *value = tsk_getu32(fs->endian, a_ptr) & FATFS_32_MASK;

        /* sanity check */
        if ((*value > fatfs->lastclust) &&
            (*value < (0x0ffffff7 & FATFS_32_MASK))) {
            if (tsk_verbose)
                tsk_fprintf(stderr,
                    "fatfs_getFAT: contents of entry %" PRIuDADDR
                    " too large - resetting\n", clust);

            *value = 0;
        }
        return 0;

    default:
        tsk_error_reset();
        tsk_errno = TSK_ERR_FS_ARG;
        snprintf(tsk_errstr, TSK_ERRSTR_L,
            "fatfs_getFAT: Unknown FAT type: %d", fatfs->fs_info.ftype);
        return 1;
    }
}


/* Return 1 if allocated, 0 if unallocated, and -1 if error */
int8_t
fatfs_is_clustalloc(FATFS_INFO * fatfs, TSK_DADDR_T clust)
{
    TSK_DADDR_T content;
    if (fatfs_getFAT(fatfs, clust, &content))
        return -1;
    else if (content == FATFS_UNALLOC)
        return 0;
    else
        return 1;
}


/* 
 * Identifies if a sector is allocated
 *
 * If it is less than the data area, then it is allocated
 * else the FAT table is consulted
 *
 * Return 1 if allocated, 0 if unallocated, and -1 if error 
 */
int8_t
fatfs_is_sectalloc(FATFS_INFO * fatfs, TSK_DADDR_T sect)
{
    TSK_FS_INFO *fs = (TSK_FS_INFO *) fatfs;
    /* If less than the first cluster sector, then it is allocated 
     * otherwise check the FAT
     */
    if (sect < fatfs->firstclustsect)
        return 1;

    /* If we are in the unused area, then we are "unalloc" */
    if ((sect <= fs->last_block) &&
        (sect >= (fatfs->firstclustsect + fatfs->csize * fatfs->clustcnt)))
        return 0;

    return fatfs_is_clustalloc(fatfs, FATFS_SECT_2_CLUST(fatfs, sect));
}




TSK_FS_BLOCK_FLAG_ENUM
fatfs_block_getflags(TSK_FS_INFO * a_fs, TSK_DADDR_T a_addr)
{
    FATFS_INFO *fatfs = (FATFS_INFO *) a_fs;
    int flags = 0;

    // FATs and boot sector
    if (a_addr < fatfs->firstdatasect) {
        flags = TSK_FS_BLOCK_FLAG_META | TSK_FS_BLOCK_FLAG_ALLOC;
    }
    // root directory for FAT12/16
    else if (a_addr < fatfs->firstclustsect) {
        flags = TSK_FS_BLOCK_FLAG_CONT | TSK_FS_BLOCK_FLAG_ALLOC;
    }
    else {
        int retval;
        flags = TSK_FS_BLOCK_FLAG_CONT;

        /* Identify its allocation status */
        retval = fatfs_is_sectalloc(fatfs, a_addr);
        if (retval != -1) {
            if (retval == 1)
                flags |= TSK_FS_BLOCK_FLAG_ALLOC;
            else
                flags |= TSK_FS_BLOCK_FLAG_UNALLOC;
        }
    }
    return flags;
}



/**************************************************************************
 *
 * BLOCK WALKING
 * 
 *************************************************************************/
/* 
** Walk the sectors of the partition. 
**
** NOTE: This is by SECTORS and not CLUSTERS
** _flags: TSK_FS_BLOCK_FLAG_ALLOC, TSK_FS_BLOCK_FLAG_UNALLOC, TSK_FS_BLOCK_FLAG_META
**  TSK_FS_BLOCK_FLAG_CONT
**
*/
uint8_t
fatfs_block_walk(TSK_FS_INFO * fs, TSK_DADDR_T a_start_blk,
    TSK_DADDR_T a_end_blk, TSK_FS_BLOCK_WALK_FLAG_ENUM a_flags,
    TSK_FS_BLOCK_WALK_CB a_action, void *a_ptr)
{
    char *myname = "fatfs_block_walk";
    FATFS_INFO *fatfs = (FATFS_INFO *) fs;
    char *data_buf = NULL;
    ssize_t cnt;
    TSK_FS_BLOCK *fs_block;

    TSK_DADDR_T addr;
    int myflags;
    unsigned int i;

    // clean up any error messages that are lying around
    tsk_error_reset();

    /*
     * Sanity checks.
     */
    if (a_start_blk < fs->first_block || a_start_blk > fs->last_block) {
        tsk_error_reset();
        tsk_errno = TSK_ERR_FS_WALK_RNG;
        snprintf(tsk_errstr, TSK_ERRSTR_L,
            "%s: Start block: %" PRIuDADDR "", myname, a_start_blk);
        return 1;
    }
    if (a_end_blk < fs->first_block || a_end_blk > fs->last_block) {
        tsk_error_reset();
        tsk_errno = TSK_ERR_FS_WALK_RNG;
        snprintf(tsk_errstr, TSK_ERRSTR_L,
            "%s: End block: %" PRIuDADDR "", myname, a_end_blk);
        return 1;
    }

    if (tsk_verbose)
        tsk_fprintf(stderr,
            "fatfs_block_walk: Block Walking %" PRIuDADDR " to %"
            PRIuDADDR "\n", a_start_blk, a_end_blk);


    /* Sanity check on a_flags -- make sure at least one ALLOC is set */
    if (((a_flags & TSK_FS_BLOCK_WALK_FLAG_ALLOC) == 0) &&
        ((a_flags & TSK_FS_BLOCK_WALK_FLAG_UNALLOC) == 0)) {
        a_flags |=
            (TSK_FS_BLOCK_WALK_FLAG_ALLOC |
            TSK_FS_BLOCK_WALK_FLAG_UNALLOC);
    }
    if (((a_flags & TSK_FS_BLOCK_WALK_FLAG_META) == 0) &&
        ((a_flags & TSK_FS_BLOCK_WALK_FLAG_CONT) == 0)) {
        a_flags |=
            (TSK_FS_BLOCK_WALK_FLAG_CONT | TSK_FS_BLOCK_WALK_FLAG_META);
    }

    if ((fs_block = tsk_fs_block_alloc(fs)) == NULL) {
        return 1;
    }

    /* cycle through the sectors.  We do the sectors before the first
     * cluster seperate from the data area */
    addr = a_start_blk;

    /* Before the data area beings (FAT, root directory etc.) */
    if ((a_start_blk < fatfs->firstclustsect)
        && (a_flags & TSK_FS_BLOCK_WALK_FLAG_ALLOC)) {

        if (tsk_verbose)
            tsk_fprintf(stderr,
                "fatfs_block_walk: Walking non-data area (pre %"
                PRIuDADDR "\n", fatfs->firstclustsect);

        if ((data_buf = (char *) tsk_malloc(fs->block_size * 8)) == NULL) {
            tsk_fs_block_free(fs_block);
            return 1;
        }

        /* Read 8 sectors at a time to be faster */
        for (; addr < fatfs->firstclustsect && addr <= a_end_blk;) {

            cnt =
                tsk_fs_read_block(fs, addr, data_buf, fs->block_size * 8);
            if (cnt != fs->block_size * 8) {
                if (cnt >= 0) {
                    tsk_error_reset();
                    tsk_errno = TSK_ERR_FS_READ;
                }
                snprintf(tsk_errstr2, TSK_ERRSTR_L,
                    "fatfs_block_walk: pre-data area block: %"
                    PRIuDADDR, addr);
                free(data_buf);
                tsk_fs_block_free(fs_block);
                return 1;
            }

            /* Process the sectors until we get to the clusters, 
             * end of target, or end of buffer */
            for (i = 0;
                i < 8 && (addr) <= a_end_blk
                && (addr) < fatfs->firstclustsect; i++, addr++) {
                int retval;

                myflags = TSK_FS_BLOCK_FLAG_ALLOC;

                /* stuff before the first data sector is the 
                 * FAT and boot sector */
                if (addr < fatfs->firstdatasect)
                    myflags |= TSK_FS_BLOCK_FLAG_META;
                /* This must be the root directory for FAT12/16 */
                else
                    myflags |= TSK_FS_BLOCK_FLAG_CONT;

                // test this sector (we already tested ALLOC)
                if ((myflags & TSK_FS_BLOCK_FLAG_META)
                    && (!(a_flags & TSK_FS_BLOCK_WALK_FLAG_META)))
                    continue;
                else if ((myflags & TSK_FS_BLOCK_FLAG_CONT)
                    && (!(a_flags & TSK_FS_BLOCK_WALK_FLAG_CONT)))
                    continue;


                tsk_fs_block_set(fs, fs_block, addr,
                    myflags | TSK_FS_BLOCK_FLAG_RAW,
                    &data_buf[i * fs->block_size]);

                retval = a_action(fs_block, a_ptr);
                if (retval == TSK_WALK_STOP) {
                    free(data_buf);
                    tsk_fs_block_free(fs_block);
                    return 0;
                }
                else if (retval == TSK_WALK_ERROR) {
                    free(data_buf);
                    tsk_fs_block_free(fs_block);
                    return 1;
                }
            }
        }

        free(data_buf);

        /* Was that it? */
        if (addr >= a_end_blk) {
            tsk_fs_block_free(fs_block);
            return 0;
        }
    }
    /* Reset the first sector to the start of the data area if we did
     * not examine it - the next calculation will screw up otherwise */
    else if (addr < fatfs->firstclustsect) {
        addr = fatfs->firstclustsect;
    }


    /* Now we read in the clusters in cluster-sized chunks,
     * sectors are too small
     */

    /* Determine the base sector of the cluster where the first 
     * sector is located */
    addr = FATFS_CLUST_2_SECT(fatfs, (FATFS_SECT_2_CLUST(fatfs, addr)));

    if ((data_buf = tsk_malloc(fs->block_size * fatfs->csize)) == NULL) {
        tsk_fs_block_free(fs_block);
        return 1;
    }

    if (tsk_verbose)
        tsk_fprintf(stderr,
            "fatfs_block_walk: Walking data area blocks (%" PRIuDADDR
            " to %" PRIuDADDR ")\n", addr, a_end_blk);

    for (; addr <= a_end_blk; addr += fatfs->csize) {
        int retval;
        size_t read_size;

        /* Identify its allocation status */
        retval = fatfs_is_sectalloc(fatfs, addr);
        if (retval == -1) {
            free(data_buf);
            tsk_fs_block_free(fs_block);
            return 1;
        }
        else if (retval == 1) {
            myflags = TSK_FS_BLOCK_FLAG_ALLOC;
        }
        else {
            myflags = TSK_FS_BLOCK_FLAG_UNALLOC;
        }

        /* At this point, there should be no more meta - just content */
        myflags |= TSK_FS_BLOCK_FLAG_CONT;

        // test if we should call the callback with this one
        if ((myflags & TSK_FS_BLOCK_FLAG_CONT)
            && (!(a_flags & TSK_FS_BLOCK_WALK_FLAG_CONT)))
            continue;
        else if ((myflags & TSK_FS_BLOCK_FLAG_ALLOC)
            && (!(a_flags & TSK_FS_BLOCK_WALK_FLAG_ALLOC)))
            continue;
        else if ((myflags & TSK_FS_BLOCK_FLAG_UNALLOC)
            && (!(a_flags & TSK_FS_BLOCK_WALK_FLAG_UNALLOC)))
            continue;


        /* The final cluster may not be full */
        if (a_end_blk - addr + 1 < fatfs->csize)
            read_size = (size_t) (a_end_blk - addr + 1);
        else
            read_size = fatfs->csize;

        cnt = tsk_fs_read_block
            (fs, addr, data_buf, fs->block_size * read_size);
        if (cnt != fs->block_size * read_size) {
            if (cnt >= 0) {
                tsk_error_reset();
                tsk_errno = TSK_ERR_FS_READ;
            }
            snprintf(tsk_errstr2, TSK_ERRSTR_L,
                "fatfs_block_walk: block: %" PRIuDADDR, addr);
            free(data_buf);
            tsk_fs_block_free(fs_block);
            return 1;
        }

        /* go through each sector in the cluster */
        for (i = 0; i < read_size; i++) {
            int retval;

            if (addr + i < a_start_blk)
                continue;
            else if (addr + i > a_end_blk)
                break;

            tsk_fs_block_set(fs, fs_block, addr + i,
                myflags | TSK_FS_BLOCK_FLAG_RAW,
                &data_buf[i * fs->block_size]);

            retval = a_action(fs_block, a_ptr);
            if (retval == TSK_WALK_STOP) {
                free(data_buf);
                tsk_fs_block_free(fs_block);
                return 0;
            }
            else if (retval == TSK_WALK_ERROR) {
                free(data_buf);
                tsk_fs_block_free(fs_block);
                return 1;
            }
        }
    }

    free(data_buf);
    tsk_fs_block_free(fs_block);
    return 0;
}





/* return 1 on error and 0 on success */
static uint8_t
fatfs_fscheck(TSK_FS_INFO * fs, FILE * hFile)
{
    tsk_error_reset();
    tsk_errno = TSK_ERR_FS_UNSUPFUNC;
    snprintf(tsk_errstr, TSK_ERRSTR_L,
        "fscheck not implemented for FAT yet");
    return 1;

    /* Check that allocated dentries point to start of allcated cluster chain */


    /* Size of file is consistent with cluster chain length */


    /* Allocated cluster chains have a corresponding alloc dentry */


    /* Non file dentries have no clusters */


    /* Only one volume label */


    /* Dump Bad Sector Addresses */


    /* Dump unused sector addresses 
     * Reserved area, end of FAT, end of Data Area */

}


/**
 * Print details about the file system to a file handle. 
 *
 * @param fs File system to print details on
 * @param hFile File handle to print text to
 * 
 * @returns 1 on error and 0 on success
 */
static uint8_t
fatfs_fsstat(TSK_FS_INFO * fs, FILE * hFile)
{
    unsigned int i;
    int a;
    TSK_DADDR_T next, snext, sstart, send;
    FATFS_INFO *fatfs = (FATFS_INFO *) fs;
    fatfs_sb *sb = fatfs->sb;
    char *data_buf;
    fatfs_dentry *de;
    ssize_t cnt;

    // clean up any error messages that are lying around
    tsk_error_reset();

    if ((data_buf = (char *) tsk_malloc(fs->block_size)) == NULL) {
        return 1;
    }


    /* Read the root directory sector so that we can get the volume
     * label from it */
    cnt = tsk_fs_read_block(fs, fatfs->rootsect, data_buf, fs->block_size);
    if (cnt != fs->block_size) {
        if (cnt >= 0) {
            tsk_error_reset();
            tsk_errno = TSK_ERR_FS_READ;
        }
        snprintf(tsk_errstr2, TSK_ERRSTR_L,
            "fatfs_fsstat: root directory: %" PRIuDADDR, fatfs->rootsect);
        free(data_buf);
        return 1;
    }


    /* Find the dentry that is set as the volume label */
    de = (fatfs_dentry *) data_buf;
    for (i = 0; i < fatfs->ssize; i += sizeof(*de)) {
        if (de->attrib == FATFS_ATTR_VOLUME)
            break;
        de++;
    }
    /* If we didn't find it, then reset de */
    if (de->attrib != FATFS_ATTR_VOLUME)
        de = NULL;


    /* Print the general file system information */

    tsk_fprintf(hFile, "FILE SYSTEM INFORMATION\n");
    tsk_fprintf(hFile, "--------------------------------------------\n");

    tsk_fprintf(hFile, "File System Type: FAT");
    if (fs->ftype == TSK_FS_TYPE_FAT12)
        tsk_fprintf(hFile, "12\n");
    else if (fs->ftype == TSK_FS_TYPE_FAT16)
        tsk_fprintf(hFile, "16\n");
    else if (fs->ftype == TSK_FS_TYPE_FAT32)
        tsk_fprintf(hFile, "32\n");
    else
        tsk_fprintf(hFile, "\n");

    if (fatfs->fs_info.ftype != TSK_FS_TYPE_FAT32) {
        tsk_fprintf(hFile, "Serial number: 0x%" PRIx32 "\n",
            tsk_getu32(fs->endian, sb->serial_number));

        if ((de) && (de->name)) {
            tsk_fprintf(hFile,
                "Volume Label (Root Directory): %c%c%c%c%c%c%c%c\n",
                de->name[0], de->name[1], de->name[2], de->name[3],
                de->name[4], de->name[5], de->name[6], de->name[7]);
        }
        else {
            tsk_fprintf(hFile, "Volume Label (Root Directory):\n");
        }
    }
    else {

        char *fat_fsinfo_buf;

        if ((fat_fsinfo_buf = (char *)
                tsk_malloc(sizeof(fatfs_fsinfo))) == NULL) {
            free(data_buf);
            return 1;
        }

        tsk_fprintf(hFile, "Serial number: 0x%" PRIx32 "\n",
            tsk_getu32(fs->endian, sb->serial_number));

        if ((de) && (de->name)) {
            tsk_fprintf(hFile,
                "Volume Label (Root Directory): %c%c%c%c%c%c%c%c\n",
                de->name[0], de->name[1], de->name[2], de->name[3],
                de->name[4], de->name[5], de->name[6], de->name[7]);
        }
        else {
            tsk_fprintf(hFile, "Volume Label (Root Directory):\n");
        }

    }

    free(data_buf);

    tsk_fprintf(hFile, "\nFile System Layout (in sectors)\n");

    tsk_fprintf(hFile, "Total Range: %" PRIuDADDR " - %" PRIuDADDR "\n",
        fs->first_block, fs->last_block);

    if (fs->last_block != fs->last_block_act)
        tsk_fprintf(hFile,
            "Total Range in Image: %" PRIuDADDR " - %" PRIuDADDR "\n",
            fs->first_block, fs->last_block_act);

    tsk_fprintf(hFile, "* Reserved: 0 - %" PRIuDADDR "\n",
        fatfs->firstfatsect - 1);

    tsk_fprintf(hFile, "** Boot Sector: 0\n");

    for (i = 0; i < fatfs->numfat; i++) {
        TSK_DADDR_T base = fatfs->firstfatsect + i * (fatfs->sectperfat);

        tsk_fprintf(hFile, "* FAT %d: %" PRIuDADDR " - %" PRIuDADDR "\n",
            i, base, (base + fatfs->sectperfat - 1));
    }

    tsk_fprintf(hFile, "* Data Area: %" PRIuDADDR " - %" PRIuDADDR "\n",
        fatfs->firstdatasect, fs->last_block);

    if (fatfs->fs_info.ftype != TSK_FS_TYPE_FAT32) {
        TSK_DADDR_T x = fatfs->csize * fatfs->clustcnt;

        tsk_fprintf(hFile,
            "** Root Directory: %" PRIuDADDR " - %" PRIuDADDR "\n",
            fatfs->firstdatasect, fatfs->firstclustsect - 1);

        tsk_fprintf(hFile,
            "** Cluster Area: %" PRIuDADDR " - %" PRIuDADDR "\n",
            fatfs->firstclustsect, (fatfs->firstclustsect + x - 1));

        if ((fatfs->firstclustsect + x - 1) != fs->last_block) {
            tsk_fprintf(hFile,
                "** Non-clustered: %" PRIuDADDR " - %" PRIuDADDR "\n",
                (fatfs->firstclustsect + x), fs->last_block);
        }
    }
    else {
        TSK_LIST *list_seen = NULL;
        TSK_DADDR_T x = fatfs->csize * (fatfs->lastclust - 1);
        TSK_DADDR_T clust, clust_p;

        tsk_fprintf(hFile,
            "** Cluster Area: %" PRIuDADDR " - %" PRIuDADDR "\n",
            fatfs->firstclustsect, (fatfs->firstclustsect + x - 1));


        clust_p = fatfs->rootsect;
        clust = FATFS_SECT_2_CLUST(fatfs, fatfs->rootsect);
        while ((clust) && (0 == FATFS_ISEOF(clust, FATFS_32_MASK))) {
            TSK_DADDR_T nxt;
            clust_p = clust;

            /* Make sure we do not get into an infinite loop */
            if (tsk_list_find(list_seen, clust)) {
                if (tsk_verbose)
                    tsk_fprintf(stderr,
                        "Loop found while determining root directory size\n");
                break;
            }
            if (tsk_list_add(&list_seen, clust)) {
                tsk_list_free(list_seen);
                list_seen = NULL;
                return 1;
            }

            if (fatfs_getFAT(fatfs, clust, &nxt))
                break;
            clust = nxt;
        }
        tsk_list_free(list_seen);
        list_seen = NULL;

        tsk_fprintf(hFile,
            "*** Root Directory: %" PRIuDADDR " - %" PRIuDADDR "\n",
            fatfs->rootsect, (FATFS_CLUST_2_SECT(fatfs, clust_p + 1) - 1));

        if ((fatfs->firstclustsect + x - 1) != fs->last_block) {
            tsk_fprintf(hFile,
                "** Non-clustered: %" PRIuDADDR " - %" PRIuDADDR "\n",
                (fatfs->firstclustsect + x), fs->last_block);
        }
    }


    tsk_fprintf(hFile, "\nMETADATA INFORMATION\n");
    tsk_fprintf(hFile, "--------------------------------------------\n");

    tsk_fprintf(hFile, "Range: %" PRIuINUM " - %" PRIuINUM "\n",
        fs->first_inum, fs->last_inum);
    tsk_fprintf(hFile, "Root Directory: %" PRIuINUM "\n", fs->root_inum);


    tsk_fprintf(hFile, "\nCONTENT INFORMATION\n");
    tsk_fprintf(hFile, "--------------------------------------------\n");
    tsk_fprintf(hFile, "Sector Size: %" PRIu16 "\n", fatfs->ssize);
    tsk_fprintf(hFile, "Cluster Size: %" PRIu32 "\n",
        (uint32_t) fatfs->csize << fatfs->ssize_sh);

    tsk_fprintf(hFile, "Total Cluster Range: 2 - %" PRIuDADDR "\n",
        fatfs->lastclust);


    /* cycle via cluster and look at each cluster in the FAT 
     * for clusters marked as bad */
    cnt = 0;
    for (i = 2; i <= fatfs->lastclust; i++) {
        TSK_DADDR_T entry;
        TSK_DADDR_T sect;

        /* Get the FAT table entry */
        if (fatfs_getFAT(fatfs, i, &entry))
            break;

        if (FATFS_ISBAD(entry, fatfs->mask) == 0) {
            continue;
        }

        if (cnt == 0)
            tsk_fprintf(hFile, "Bad Sectors: ");

        sect = FATFS_CLUST_2_SECT(fatfs, i);
        for (a = 0; a < fatfs->csize; a++) {
            tsk_fprintf(hFile, "%" PRIuDADDR " ", sect + a);
            if ((++cnt % 8) == 0)
                tsk_fprintf(hFile, "\n");
        }
    }
    if ((cnt > 0) && ((cnt % 8) != 0))
        tsk_fprintf(hFile, "\n");



    /* Display the FAT Table */
    tsk_fprintf(hFile, "\nFAT CONTENTS (in sectors)\n");
    tsk_fprintf(hFile, "--------------------------------------------\n");

    /* 'sstart' marks the first sector of the current run to print */
    sstart = fatfs->firstclustsect;

    /* cycle via cluster and look at each cluster in the FAT  to make runs */
    for (i = 2; i <= fatfs->lastclust; i++) {

        /* 'send' marks the end sector of the current run, which will extend
         * when the current cluster continues to the next 
         */
        send = FATFS_CLUST_2_SECT(fatfs, i + 1) - 1;

        /* get the next cluster */
        if (fatfs_getFAT(fatfs, i, &next))
            break;

        snext = FATFS_CLUST_2_SECT(fatfs, next);

        /* we are also using the next sector (clust) */
        if ((next & fatfs->mask) == (i + 1)) {
            continue;
        }

        /* The next clust is either further away or the clust is available,
         * print it if is further away 
         */
        else if ((next & fatfs->mask)) {
            if (FATFS_ISEOF(next, fatfs->mask))
                tsk_fprintf(hFile,
                    "%" PRIuDADDR "-%" PRIuDADDR " (%" PRIuDADDR
                    ") -> EOF\n", sstart, send, send - sstart + 1);
            else if (FATFS_ISBAD(next, fatfs->mask))
                tsk_fprintf(hFile,
                    "%" PRIuDADDR "-%" PRIuDADDR " (%" PRIuDADDR
                    ") -> BAD\n", sstart, send, send - sstart + 1);
            else
                tsk_fprintf(hFile,
                    "%" PRIuDADDR "-%" PRIuDADDR " (%" PRIuDADDR
                    ") -> %" PRIuDADDR "\n", sstart, send,
                    send - sstart + 1, snext);
        }

        /* reset the starting counter */
        sstart = send + 1;
    }

    return 0;
}


/************************* istat *******************************/

/* Callback a_action for file_walk to print the sector addresses
 * of a file
 */

typedef struct {
    FILE *hFile;
    int idx;
    int istat_seen;
} FATFS_PRINT_ADDR;

static TSK_WALK_RET_ENUM
print_addr_act(TSK_FS_FILE * fs_file, TSK_OFF_T a_off, TSK_DADDR_T addr,
    char *buf, size_t size, TSK_FS_BLOCK_FLAG_ENUM a_flags, void *a_ptr)
{
    FATFS_PRINT_ADDR *print = (FATFS_PRINT_ADDR *) a_ptr;

    tsk_fprintf(print->hFile, "%" PRIuDADDR " ", addr);

    if (++(print->idx) == 8) {
        tsk_fprintf(print->hFile, "\n");
        print->idx = 0;
    }
    print->istat_seen = 1;

    return TSK_WALK_CONT;
}



/**
 * Print details on a specific file to a file handle. 
 *
 * @param fs File system file is located in
 * @param hFile File handle to print text to
 * @param inum Address of file in file system
 * @param numblock The number of blocks in file to force print (can go beyond file size)
 * @param sec_skew Clock skew in seconds to also print times in
 * 
 * @returns 1 on error and 0 on success
 */
static uint8_t
fatfs_istat(TSK_FS_INFO * fs, FILE * hFile, TSK_INUM_T inum,
    TSK_DADDR_T numblock, int32_t sec_skew)
{
    TSK_FS_META *fs_meta;
    TSK_FS_FILE *fs_file;
    TSK_FS_META_NAME_LIST *fs_name_list;
    FATFS_INFO *fatfs = (FATFS_INFO *) fs;
    FATFS_PRINT_ADDR print;

    // clean up any error messages that are lying around
    tsk_error_reset();

    if ((fs_file = tsk_fs_file_open_meta(fs, NULL, inum)) == NULL) {
        return 1;
    }
    fs_meta = fs_file->meta;

    tsk_fprintf(hFile, "Directory Entry: %" PRIuINUM "\n", inum);

    tsk_fprintf(hFile, "%sAllocated\n",
        (fs_meta->flags & TSK_FS_META_FLAG_UNALLOC) ? "Not " : "");

    tsk_fprintf(hFile, "File Attributes: ");

    /* This should only be null if we have the root directory or special file */
    if (fatfs->dep == NULL) {
        if (inum == FATFS_ROOTINO)
            tsk_fprintf(hFile, "Directory\n");
        else if (fs_file->meta->type == TSK_FS_META_TYPE_VIRT)
            tsk_fprintf(hFile, "Virtual\n");
        else
            tsk_fprintf(hFile, "File\n");
    }
    else if ((fatfs->dep->attrib & FATFS_ATTR_LFN) == FATFS_ATTR_LFN) {
        tsk_fprintf(hFile, "Long File Name\n");
    }
    else {
        if (fatfs->dep->attrib & FATFS_ATTR_DIRECTORY)
            tsk_fprintf(hFile, "Directory");
        else if (fatfs->dep->attrib & FATFS_ATTR_VOLUME)
            tsk_fprintf(hFile, "Volume Label");
        else
            tsk_fprintf(hFile, "File");

        if (fatfs->dep->attrib & FATFS_ATTR_READONLY)
            tsk_fprintf(hFile, ", Read Only");
        if (fatfs->dep->attrib & FATFS_ATTR_HIDDEN)
            tsk_fprintf(hFile, ", Hidden");
        if (fatfs->dep->attrib & FATFS_ATTR_SYSTEM)
            tsk_fprintf(hFile, ", System");
        if (fatfs->dep->attrib & FATFS_ATTR_ARCHIVE)
            tsk_fprintf(hFile, ", Archive");

        tsk_fprintf(hFile, "\n");
    }

    tsk_fprintf(hFile, "Size: %" PRIuOFF "\n", fs_meta->size);

    if (fs_meta->name2) {
        fs_name_list = fs_meta->name2;
        tsk_fprintf(hFile, "Name: %s\n", fs_name_list->name);
    }

    if (sec_skew != 0) {
        tsk_fprintf(hFile, "\nAdjusted Directory Entry Times:\n");
        fs_meta->mtime -= sec_skew;
        fs_meta->atime -= sec_skew;
        fs_meta->crtime -= sec_skew;

        tsk_fprintf(hFile, "Written:\t%s", ctime(&fs_meta->mtime));
        tsk_fprintf(hFile, "Accessed:\t%s", ctime(&fs_meta->atime));
        tsk_fprintf(hFile, "Created:\t%s", ctime(&fs_meta->crtime));

        fs_meta->mtime += sec_skew;
        fs_meta->atime += sec_skew;
        fs_meta->crtime += sec_skew;

        tsk_fprintf(hFile, "\nOriginal Directory Entry Times:\n");
    }
    else
        tsk_fprintf(hFile, "\nDirectory Entry Times:\n");

    tsk_fprintf(hFile, "Written:\t%s", ctime(&fs_meta->mtime));
    tsk_fprintf(hFile, "Accessed:\t%s", ctime(&fs_meta->atime));
    tsk_fprintf(hFile, "Created:\t%s", ctime(&fs_meta->crtime));

    tsk_fprintf(hFile, "\nSectors:\n");

    /* A bad hack to force a specified number of blocks */
    if (numblock > 0)
        fs_meta->size = numblock * fs->block_size;

    print.istat_seen = 0;
    print.idx = 0;
    print.hFile = hFile;

    if (tsk_fs_file_walk(fs_file,
            (TSK_FS_FILE_WALK_FLAG_AONLY | TSK_FS_FILE_WALK_FLAG_SLACK),
            print_addr_act, (void *) &print)) {
        tsk_fprintf(hFile, "\nError reading file\n");
        tsk_error_print(hFile);
        tsk_error_reset();
    }
    else if (print.idx != 0) {
        tsk_fprintf(hFile, "\n");
    }

    tsk_fs_file_close(fs_file);
    return 0;
}


/* return 1 on error and 0 on success */
uint8_t
fatfs_jopen(TSK_FS_INFO * fs, TSK_INUM_T inum)
{
    tsk_error_reset();
    tsk_errno = TSK_ERR_FS_UNSUPFUNC;
    snprintf(tsk_errstr, TSK_ERRSTR_L, "FAT does not have a journal\n");
    return 1;
}

/* return 1 on error and 0 on success */
uint8_t
fatfs_jentry_walk(TSK_FS_INFO * fs, int a_flags,
    TSK_FS_JENTRY_WALK_CB a_action, void *a_ptr)
{
    tsk_error_reset();
    tsk_errno = TSK_ERR_FS_UNSUPFUNC;
    snprintf(tsk_errstr, TSK_ERRSTR_L, "FAT does not have a journal\n");
    return 1;
}


/* return 1 on error and 0 on success */
uint8_t
fatfs_jblk_walk(TSK_FS_INFO * fs, TSK_DADDR_T start, TSK_DADDR_T end,
    int a_flags, TSK_FS_JBLK_WALK_CB a_action, void *a_ptr)
{
    tsk_error_reset();
    tsk_errno = TSK_ERR_FS_UNSUPFUNC;
    snprintf(tsk_errstr, TSK_ERRSTR_L, "FAT does not have a journal\n");
    return 1;
}

static TSK_FS_ATTR_TYPE_ENUM
fatfs_get_default_attr_type(const TSK_FS_FILE * a_file)
{
    return TSK_FS_ATTR_TYPE_DEFAULT;
}

/* fatfs_close - close an fatfs file system */
static void
fatfs_close(TSK_FS_INFO * fs)
{
    FATFS_INFO *fatfs = (FATFS_INFO *) fs;
    fs->tag = 0;

    free(fatfs->dinodes);

    if (fatfs->dir_buf)
        free(fatfs->dir_buf);
    if (fatfs->par_buf)
        free(fatfs->par_buf);

    tsk_list_free(fs->list_inum_named);
    fs->list_inum_named = NULL;
    free(fatfs->sb);
    free(fs);
}


/**
 * \internal
 * Open part of a disk image as a FAT file system. 
 *
 * @param img_info Disk image to analyze
 * @param offset Byte offset where FAT file system starts
 * @param ftype Specific type of FAT file system
 * @param test NOT USED
 * @returns NULL on error or if data is not a FAT file system
 */
TSK_FS_INFO *
fatfs_open(TSK_IMG_INFO * img_info, TSK_OFF_T offset,
    TSK_FS_TYPE_ENUM ftype, uint8_t test)
{
    char *myname = "fatfs_open";
    FATFS_INFO *fatfs;
    unsigned int len;
    TSK_FS_INFO *fs;
    fatfs_sb *fatsb;
    TSK_DADDR_T sectors;
    ssize_t cnt;
    uint32_t fsopen_numfat, fsopen_csize;
    int i;

    // clean up any error messages that are lying around
    tsk_error_reset();

    if (TSK_FS_TYPE_ISFAT(ftype) == 0) {
        tsk_error_reset();
        tsk_errno = TSK_ERR_FS_ARG;
        snprintf(tsk_errstr, TSK_ERRSTR_L, "%s: Invalid FS Type", myname);
        return NULL;
    }

    if ((fatfs = (FATFS_INFO *) tsk_malloc(sizeof(*fatfs))) == NULL)
        return NULL;

    fs = &(fatfs->fs_info);
    fs->ftype = ftype;

    fs->img_info = img_info;
    fs->offset = offset;
    fs->tag = TSK_FS_INFO_TAG;

    /*
     * Read the super block.
     */
    len = sizeof(fatfs_sb);
    fatsb = fatfs->sb = (fatfs_sb *) tsk_malloc(len);
    if (fatsb == NULL) {
        fs->tag = 0;
        free(fatfs);
        return NULL;
    }

    /* Look for the boot sector. We loop because
     * we will try the backup if the first fails.
     * Only FAT32 has a backup though...*/
    for (i = 0; i < 2; i++) {
        TSK_OFF_T sb_off;

        if (i == 0)
            sb_off = 0;
        else
            sb_off = 6 * img_info->sector_size; // the backup is located in sector 6


        cnt = tsk_fs_read(fs, sb_off, (char *) fatsb, len);
        if (cnt != len) {
            if (cnt >= 0) {
                tsk_error_reset();
                tsk_errno = TSK_ERR_FS_READ;
            }
            snprintf(tsk_errstr2, TSK_ERRSTR_L, "%s: boot sector", myname);
            fs->tag = 0;
            free(fatfs->sb);
            free(fatfs);
            return NULL;
        }
        if(!strncmp((char *) fatsb->magic, "XTAF", 4)) break;

        /* Check the magic value and ID endian ordering */
        if (tsk_fs_guessu16(fs, fatsb->magic, FATFS_FS_MAGIC)) {

            // if the magic value is 0, then we will try the backup
            if ((i == 0)
                && (tsk_getu16(TSK_LIT_ENDIAN, fatsb->magic) == 0)) {
                continue;
            }
            else {
                fs->tag = 0;
                free(fatsb);
                free(fatfs);
                tsk_error_reset();
                tsk_errno = TSK_ERR_FS_MAGIC;
                snprintf(tsk_errstr, TSK_ERRSTR_L,
                    "Not a FATFS file system (magic)");
                return NULL;
            }
        }
        else {
            break;
        }
    }

    fs->dev_bsize = img_info->sector_size;

    /* Calculate block sizes and layout info */
    // sector size AJN NOTE: Hard-coded to 512 for now, see macro variable. Is that right?(TODO)
    fatfs->ssize = XTAF_SECTOR_SIZE;
    if (fatfs->ssize == 512) {
        fatfs->ssize_sh = 9;
    }
    else if (fatfs->ssize == 1024) {
        fatfs->ssize_sh = 10;
    }
    else if (fatfs->ssize == 2048) {
        fatfs->ssize_sh = 11;
    }
    else if (fatfs->ssize == 4096) {
        fatfs->ssize_sh = 12;
    }
    else {
        tsk_error_reset();
        tsk_errno = TSK_ERR_FS_MAGIC;
        snprintf(tsk_errstr, TSK_ERRSTR_L,
            "Error: sector size (%d) is not a multiple of device size (%d)\nDo you have a disk image instead of a partition image?",
            fatfs->ssize, fs->dev_bsize);
        fs->tag = 0;
        free(fatsb);
        free(fatfs);
        return NULL;
    }

    // cluster size 
    fsopen_csize = tsk_getu32(fs->endian, fatsb->csize);
    if(fsopen_csize > 256) printf("Sectors per cluster is more than 256!\n");
    fatfs->csize = (uint8_t) fsopen_csize; 
    if ((fatfs->csize != 0x01) &&
        (fatfs->csize != 0x02) &&
        (fatfs->csize != 0x04) &&
        (fatfs->csize != 0x08) &&
        (fatfs->csize != 0x10) &&
        (fatfs->csize != 0x20) &&
        (fatfs->csize != 0x40) && (fatfs->csize != 0x80)) {
        fs->tag = 0;
        free(fatsb);
        free(fatfs);
        tsk_error_reset();
        tsk_errno = TSK_ERR_FS_MAGIC;
        snprintf(tsk_errstr, TSK_ERRSTR_L,
            "Not a FATFS file system (cluster size)");
        return NULL;
    }

    // number of FAT tables
    fsopen_numfat = tsk_getu32(fs->endian, fatsb->numfat);
    if(fsopen_numfat > 256) printf("Number of FATs is more than 256!\n");
    fatfs->numfat = (uint8_t) fsopen_numfat;
    if ((fatfs->numfat == 0) || (fatfs->numfat > 8)) {
        fs->tag = 0;
        free(fatsb);
        free(fatfs);
        tsk_error_reset();
        tsk_errno = TSK_ERR_FS_MAGIC;
        snprintf(tsk_errstr, TSK_ERRSTR_L,
            "Not a FATFS file system (number of FATs)");
        return NULL;
    }

    /* We can't do a sanity check on this b.c. TSK_FS_TYPE_FAT32 has a value of 0 */
    /* num of root entries */
    //fatfs->numroot = tsk_getu16(fs->endian, fatsb->numroot);
    fatfs->numroot = (uint16_t) 1;


    sectors = (TSK_DADDR_T) img_info->size/XTAF_SECTOR_SIZE;

    /* EQS NOTE: sectperfat is hardcoded for the second partition.
                 I found this with hexedit:
                 Offset of 2nd partition FAT  = 0x120eb1000
                 Offset of 2nd partition root = 0x120eba000
                                         - ____________
                                                  0x9000
                 However uxtaf.c shows the FAT size as being 0x8000
                 There is an 8 sector buffer inbetween the FAT and 
                 the root sect?
    */


    if(img_info->size == 146413464 || img_info->size == 4712496640 || img_info->size == 4846714880){
//        printf("Partition 1\n");
        fatfs->rootsect = 1176;
        fatfs->sectperfat = (uint32_t) 1160;
        fatfs->firstclustsect = 1240;
        fatfs->clustcnt = (TSK_DADDR_T) 147910; 
        fatfs->lastclust = (TSK_DADDR_T) 147891;
    }else if(img_info->size == 2147483648 || offset == 0x80000){
        printf("Partition 0x80000\n");
        fatfs->rootsect = 528;
        fatfs->sectperfat = (uint32_t) 512;
        fatfs->firstclustsect = (TSK_DADDR_T) 592;
        fatfs->clustcnt = (TSK_DADDR_T) 65536;
        fatfs->lastclust = (TSK_DADDR_T) 65527;

    }else if(img_info->size == 2348810240 || offset == 0x80080000){
//        printf("Partition 0x80080000\n");
        fatfs->rootsect = 2248;
        fatfs->sectperfat = (uint32_t) 2240;
        fatfs->firstclustsect = (TSK_DADDR_T) 2264;
        fatfs->clustcnt = (TSK_DADDR_T) 65536;
        fatfs->lastclust = (TSK_DADDR_T) 65527;

    }else if(img_info->size == 216203264 || offset == 0x10C080000){
//        printf("Partition 0x10C080000\n");
        fatfs->rootsect = 64;
        fatfs->sectperfat = (uint32_t) 56;
        fatfs->firstclustsect = (TSK_DADDR_T) 96;
        fatfs->clustcnt = (TSK_DADDR_T) 13196;
        fatfs->lastclust = (TSK_DADDR_T) 13194;

    }else if(img_info->size == 134217728 || offset == 0x118eb0000){
//        printf("Partition 0x118eb0000\n");
        fatfs->rootsect = 48;
        fatfs->sectperfat = (uint32_t) 40;
        fatfs->firstclustsect = (TSK_DADDR_T) 80;
        fatfs->clustcnt = (TSK_DADDR_T) 8192;
        fatfs->lastclust = (TSK_DADDR_T) 8190;
   
    }else if(img_info->size == 268435456 || offset == 0x120eb0000){
//        printf("System partition\n");
        fatfs->rootsect = 80;
        fatfs->sectperfat = (uint32_t) XTAF_SECT_PER_FAT;
        fatfs->firstclustsect = (TSK_DADDR_T) 112;
        fatfs->clustcnt = (TSK_DADDR_T) 16384;
        fatfs->lastclust = (TSK_DADDR_T) 16381;
    }else if(img_info->size == 244943674880 || offset == 0x130eb0000){
//        printf("Data Partition\n");
        fatfs->rootsect = 116808;
        fatfs->sectperfat = (uint32_t) 116800;
        fatfs->firstclustsect = (TSK_DADDR_T) 116840;
        fatfs->firstdatasect = fatfs->firstclustsect;
        fatfs->clustcnt = (TSK_DADDR_T) 14950175;
        fatfs->lastclust = (TSK_DADDR_T) 14946525;
    }
    else{
        free(fatsb);
        free(fatfs);
        tsk_error_reset();
        snprintf(tsk_errstr, TSK_ERRSTR_L,
            "Partition was not valid\n");
        return NULL;
    }

    fatfs->firstfatsect = XTAF_FIRST_FAT_SECT;


    if (fatfs->sectperfat == 0) {
        fs->tag = 0;
        free(fatsb);
        free(fatfs);
        tsk_error_reset();
        tsk_errno = TSK_ERR_FS_MAGIC;
        snprintf(tsk_errstr, TSK_ERRSTR_L,
            "Not a FATFS file system (invalid sectors per FAT)");
        return NULL;
    }
    if ((fatfs->firstfatsect == 0) || (fatfs->firstfatsect > sectors)) {
        tsk_error_reset();
        tsk_errno = TSK_ERR_FS_WALK_RNG;
        snprintf(tsk_errstr, TSK_ERRSTR_L,
            "Not a FATFS file system (invalid first FAT sector %"
            PRIuDADDR ")", fatfs->firstfatsect);

        fs->tag = 0;
        free(fatsb);
        free(fatfs);
        return NULL;
    }

    /* Calculate the block info
     * 
     * The sector of the begining of the data area  - which is 
     * after all of the FATs
     *
     * For TSK_FS_TYPE_FAT12 and TSK_FS_TYPE_FAT16, the data area starts with the root
     * directory entries and then the first cluster.  For TSK_FS_TYPE_FAT32,
     * the data area starts with clusters and the root directory
     * is somewhere in the data area
     */
    /* There's a 8 sector void space between the FAT and the first data sector */
//    fatfs->firstdatasect = HD_VOID_AREA + fatfs->firstfatsect +
//        fatfs->sectperfat * fatfs->numfat;
     fatfs->firstdatasect = fatfs->rootsect;
     fatfs->firstclustsect = fatfs->firstdatasect + 32 + 0;


    /* The sector where the first cluster is located.  It will be used
     * to translate cluster addresses to sector addresses 
     *
     * For TSK_FS_TYPE_FAT32, the first cluster is the start of the data area and
     * it is after the root directory for TSK_FS_TYPE_FAT12 and TSK_FS_TYPE_FAT16.  At this
     * point in the program, numroot is set to 0 for TSK_FS_TYPE_FAT32
     */

    /* total number of clusters */
//    fatfs->clustcnt = (sectors - fatfs->firstclustsect) / fatfs->csize;
        

    /* the first cluster is #2, so the final cluster is: */
//    fatfs->lastclust = 1 + fatfs->clustcnt;



    /* identify the FAT type by the total number of data clusters
     * this calculation is from the MS FAT Overview Doc
     *
     * A FAT file system made by another OS could use different values
     */
    if (ftype == TSK_FS_TYPE_FAT_DETECT) {

        if (fatfs->clustcnt < 0xfff4) {
            ftype = TSK_FS_TYPE_FAT16;
        }
        else {
            ftype = TSK_FS_TYPE_FAT32;
        }

        fatfs->fs_info.ftype = ftype;
    }

    /* Some sanity checks */
    else {
        if ((ftype == TSK_FS_TYPE_FAT12)
            && (fatfs->clustcnt >= 4085)) {
            fs->tag = 0;
            free(fatsb);
            free(fatfs);
            tsk_error_reset();
            tsk_errno = TSK_ERR_FS_MAGIC;
            snprintf(tsk_errstr, TSK_ERRSTR_L,
                "Too many sectors for TSK_FS_TYPE_FAT12: try auto-detect mode");
            return NULL;
        }
    }
/*

    if ((ftype == TSK_FS_TYPE_FAT32) && (fatfs->numroot != 0)) {
        fs->tag = 0;
        free(fatsb);
        free(fatfs);
        tsk_error_reset();
        tsk_errno = TSK_ERR_FS_MAGIC;
        snprintf(tsk_errstr, TSK_ERRSTR_L,
            "Invalid TSK_FS_TYPE_FAT32 image (numroot != 0)");
        return NULL;
    }

    if ((ftype != TSK_FS_TYPE_FAT32) && (fatfs->numroot == 0)) {
        fs->tag = 0;
        free(fatsb);
        free(fatfs);
        tsk_error_reset();
        tsk_errno = TSK_ERR_FS_MAGIC;
        snprintf(tsk_errstr, TSK_ERRSTR_L,
            "Invalid FAT image (numroot == 0, and not TSK_FS_TYPE_FAT32)");
        return NULL;
    }
*/



    /* Set the mask to use on the cluster values */
    if (ftype == TSK_FS_TYPE_FAT12) {
        fatfs->mask = FATFS_12_MASK;
    }
    else if (ftype == TSK_FS_TYPE_FAT16) {
        fatfs->mask = FATFS_16_MASK;
    }
    else if (ftype == TSK_FS_TYPE_FAT32) {
        fatfs->mask = FATFS_32_MASK;
    }
    else {
        fs->tag = 0;
        free(fatsb);
        free(fatfs);
        tsk_error_reset();
        tsk_errno = TSK_ERR_FS_ARG;
        snprintf(tsk_errstr, TSK_ERRSTR_L,
            "Unknown FAT type in fatfs_open: %d\n", ftype);
        return NULL;
    }
    fs->duname = "Sector";

    for (i = 0; i < FAT_CACHE_N; i++) {
        fatfs->fatc_addr[i] = 0;
        fatfs->fatc_ttl[i] = 0;
    }

    /* allocate a cluster-sized buffer for inodes */
    if ((fatfs->dinodes = (char *)
            tsk_malloc(fatfs->csize << fatfs->ssize_sh)) == NULL) {
        fs->tag = 0;
        free(fatsb);
        free(fatfs);
        return NULL;
    }


    /*
     * block calculations : although there are no blocks in fat, we will
     * use these fields for sector calculations
     */
    fs->first_block = 0;
    fs->block_count = sectors;
    fs->last_block = fs->last_block_act = fs->block_count - 1;
    fs->block_size = fatfs->ssize;

    // determine the last block we have in this image
    if ((TSK_DADDR_T) ((img_info->size - offset) / fs->block_size) <
        fs->block_count)
        fs->last_block_act =
            (img_info->size - offset) / fs->block_size - 1;

    /*
     * inode calculations
     */

    /* maximum number of dentries in a sector & cluster */
    fatfs->dentry_cnt_se = fatfs->ssize / sizeof(fatfs_dentry);
    fatfs->dentry_cnt_cl = fatfs->dentry_cnt_se * fatfs->csize;

    fs->root_inum = FATFS_ROOTINO;
    fs->first_inum = FATFS_FIRSTINO;
    // Add on extras for Orphan and special files
    fs->last_inum =
        (FATFS_SECT_2_INODE(fatfs,
            fs->last_block_act + 1) - 1) + FATFS_NUM_SPECFILE;
    fs->inum_count = fs->last_inum - fs->first_inum + 1;


    /*
     * Volume ID
     * AJN NOTE: For XTAF, volume ID is replaced with serial number,
     * which might not be distinct on the drive.
     * (TODO: Refer to Bolt or our drive images.)
     */
    for (fs->fs_id_used = 0; fs->fs_id_used < 4; fs->fs_id_used++) {
        fs->fs_id[fs->fs_id_used] =
            fatsb->serial_number[fs->fs_id_used];
    }

    /*
     * Set the function pointers  
     */

    fs->block_walk = fatfs_block_walk;
    fs->block_getflags = fatfs_block_getflags;

    fs->inode_walk = fatfs_inode_walk;
    fs->istat = fatfs_istat;
    fs->file_add_meta = fatfs_inode_lookup;

    fs->get_default_attr_type = fatfs_get_default_attr_type;
    fs->load_attrs = fatfs_make_data_run;

    fs->dir_open_meta = fatfs_dir_open_meta;
    fs->name_cmp = fatfs_name_cmp;

    fs->fsstat = fatfs_fsstat;
    fs->fscheck = fatfs_fscheck;

    fs->close = fatfs_close;

    fs->jblk_walk = fatfs_jblk_walk;
    fs->jentry_walk = fatfs_jentry_walk;
    fs->jopen = fatfs_jopen;
    fs->journ_inum = 0;


    // initialize the caches
    fs->list_inum_named = NULL;

    return fs;
}
