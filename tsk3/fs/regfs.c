/*
** The Sleuth Kit 
**
** Willi Ballenthin [william.ballenthin <at> mandiant [dot] com]
** Copyright (c) 2012 Willi Ballenthin.  All rights reserved
**
** This software is distributed under the Common Public License 1.0 
*/

/**
 *\file regfs.c
 * Contains the internal TSK Registry file system functions.
 */

#include "tsk_fs_i.h"
#include "tsk_regfs.h"

/**
 * @see ntfs.c
 */
static uint32_t
nt2unixtime(uint64_t ntdate) {
  #define NSEC_BTWN_1601_1970 (uint64_t)(116444736000000000ULL)
  
  ntdate -= (uint64_t) NSEC_BTWN_1601_1970;
  ntdate /= (uint64_t) 10000000;
  return (uint32_t) ntdate;
}

/**
 * @see ntfs.c
 */
static uint32_t
nt2nano(uint64_t ntdate) {
  return (int32_t) (ntdate % 10000000);
}


static TSK_RETVAL_ENUM
regfs_utf16to8(TSK_ENDIAN_ENUM endian, char *error_class,
	       uint8_t *utf16, ssize_t utf16_length,
	       char *utf8, ssize_t utf8_length) {
  UTF16 *name16;
  UTF8 *name8;
  int retVal;
  
  name16 = (UTF16 *) utf16;
  name8 = (UTF8 *) utf8;
  retVal = tsk_UTF16toUTF8(endian, 
			   (const UTF16 **) &name16,
			   (UTF16 *) ((uintptr_t) name16 + utf16_length),
			   &name8,
			   (UTF8 *) ((uintptr_t) name8 + utf8_length),
			   TSKlenientConversion);
  if (retVal != TSKconversionOK) {
    if (tsk_verbose)
      tsk_fprintf(stderr, "fsstat: Error converting %s to UTF8: %d",
		  error_class, retVal);
    *name8 = '\0';
  }
  else if ((uintptr_t) name8 >= (uintptr_t) utf8 + utf8_length) {
    /* Make sure it is NULL Terminated */
    utf8[utf8_length - 1] = '\0';
  }
  else {
    *name8 = '\0';
  }
  return TSK_OK;
}

/**
 * Given the address as `inum`, load metadata about the Cell into 
 * the cell pointed to by `cell`.
 * @return TSK_OK on success, TSK_ERR on error.
 */
static TSK_RETVAL_ENUM
reg_load_cell(TSK_FS_INFO *fs, REGFS_CELL *cell, TSK_INUM_T inum) {
  ssize_t count;
  uint32_t len;
  uint16_t type;
  uint8_t  buf[6];

  if (inum < fs->first_block || inum > fs->last_block) {
    tsk_error_reset();
    tsk_error_set_errno(TSK_ERR_FS_BLK_NUM);
    tsk_error_set_errstr("Invalid block number to load: %" PRIuDADDR "", inum);
    return TSK_ERR;
  }

  cell->inum = inum;

  count = tsk_fs_read(fs, inum, (char *)buf, 6);
  if (count != 6) {
    tsk_error_reset();
    tsk_error_set_errno(TSK_ERR_FS_READ);
    tsk_error_set_errstr("Failed to read cell structure");
    return TSK_ERR;
  }

  len = (tsk_getu32(fs->endian, buf));
  if (len & 1 << 31) {
    cell->is_allocated = 1;
    cell->length = (-1 * tsk_gets32(fs->endian, buf));
  } else {
    cell->is_allocated = 0;
    cell->length = (tsk_getu32(fs->endian, buf));
  }
  if (cell->length >= HBIN_SIZE) {
    tsk_error_reset();
    tsk_error_set_errno(TSK_ERR_FS_INODE_COR);
    tsk_error_set_errstr("Registry cell corrupt: size too large (%" PRIuINUM ")",
			 (unsigned long)cell->length);
    return TSK_ERR;
  }

  type = (tsk_getu16(fs->endian, buf + 4));

  switch (type) {
  case 0x6b76:
    cell->type = TSK_REGFS_RECORD_TYPE_VK;
    break;
  case 0x6b6e:
    cell->type = TSK_REGFS_RECORD_TYPE_NK;
    break;
  case 0x666c:
    cell->type = TSK_REGFS_RECORD_TYPE_LF;
    break;
  case 0x686c:
    cell->type = TSK_REGFS_RECORD_TYPE_LH;
    break;
  case 0x696c:
    cell->type = TSK_REGFS_RECORD_TYPE_LI;
    break;
  case 0x6972:
    cell->type = TSK_REGFS_RECORD_TYPE_RI;
    break;
  case 0x6b73:
    cell->type = TSK_REGFS_RECORD_TYPE_SK;
    break;
  case 0x6264:
    cell->type = TSK_REGFS_RECORD_TYPE_DB;
    break;
  default:
    cell->type = TSK_REGFS_RECORD_TYPE_UNKNOWN;
    break;
  }
  
  return TSK_OK;
}

/**
 * reg_file_add_meta
 * Load the associated metadata for the file with inode at `inum`
 * into the file structure `a_fs_file`.
 * If the `meta` field of `a_fs_file` is already set, it will be
 *   cleared and reset.
 * As for the `meta.type`:
 *   - vk records --> file
 *   - nk records --> directories
 *   - else       --> virtual files
 * Until we do some parsing of security info, the mode
 *   is 0777 for all keys and values.
 *
 * 
 * @return 1 on error, 0 otherwise.

    struct TSK_FS_FILE {
        int tag;                ///< \internal Will be set to TSK_FS_FILE_TAG 
	                              if structure is allocated
        TSK_FS_NAME *name;      ///< Pointer to name of file (or NULL 
	                              if file was opened using metadata address)
        TSK_FS_META *meta;      ///< Pointer to metadata of file (or NULL 
	                              if name has invalid metadata address)
        TSK_FS_INFO *fs_info;   ///< Pointer to file system that the file 
	                              is located in.
    };
    
    ^^ this is done, now just need to set the `meta` field


    typedef struct {
[x]        int tag;                ///< \internal Will be set to TSK_FS_META_TAG 
	                                         if structure is allocated
[x]        TSK_FS_META_FLAG_ENUM flags;    ///< Flags for this file for its 
	                                        allocation status etc.
[x]        TSK_INUM_T addr;        ///< Address of the meta data structure 
	                                        for this file
[x]        TSK_FS_META_TYPE_ENUM type;     ///< File type
[x]        TSK_FS_META_MODE_ENUM mode;     ///< Unix-style permissions
[x]        int nlink;              ///< link count (number of file names 
	                                pointing to this)
[x]        TSK_OFF_T size;         ///< file size (in bytes)
[x]        TSK_UID_T uid;          ///< owner id
[x]        TSK_GID_T gid;          ///< group id
[x]        time_t mtime;           ///< last file content modification 
	                                time (stored in number of seconds 
					since Jan 1, 1970 UTC)
[x]        uint32_t mtime_nano;    ///< nano-second resolution in addition 
                                        to m_time
[x]        time_t atime;           ///< last file content accessed time 
	                                (stored in number of seconds 
					since Jan 1, 1970 UTC)
[x]        uint32_t atime_nano;    ///< nano-second resolution in addition 
                                        to a_time
[x]        time_t ctime;           ///< last file / metadata status change time 
	                                (stored in number of seconds 
					since Jan 1, 1970 UTC)
[x]        uint32_t ctime_nano;    ///< nano-second resolution in addition 
                                        to c_time
[x]        time_t crtime;          ///< Created time (stored in number of 
	                                seconds since Jan 1, 1970 UTC)
[x]        uint32_t crtime_nano;   ///< nano-second resolution in addition 
                                        to cr_time
        / filesystem specific times /
        union {
            struct {
[x]                time_t dtime;   ///< Linux deletion time
[x]                uint32_t dtime_nano;    ///< nano-second resolution in 
		                               addition to d_time
            } ext2;
            struct {
[x]                time_t bkup_time;       ///< HFS+ backup time
[x]                uint32_t bkup_time_nano;        ///< nano-second resolution 
		                                     in addition to bkup_time
            } hfs;
        } time2;

[x]        void *content_ptr;      ///< Pointer to file system specific data 
	                               that is used to store references 
				       to file content
[x]        size_t content_len;     ///< size of content  buffer
[x]        uint32_t seq;           ///< Sequence number for file 
	                               (NTFS only, is incremented when 
				       entry is reallocated) 
        / Contains run data on the file content 
	    (specific locations where content is stored).  
        * Check attr_state to determine if data in here 
	    is valid because not all file systems 
        * load this data when a file is loaded.  
	    It may not be loaded until needed by one
        * of the APIs. Most file systems will have only 
	    one attribute, but NTFS will have several. /
[ ]        TSK_FS_ATTRLIST *attr;
[ ]        TSK_FS_META_ATTR_FLAG_ENUM attr_state;  ///< State of the data in 
	                                               the TSK_FS_META::attr 
						       structure
[x]        TSK_FS_META_NAME_LIST *name2;   ///< Name of file stored in 
	                                       metadata (FAT and NTFS Only)
[x]        char *link;             ///< Name of target file if this is 
	                                a symbolic link
    } TSK_FS_META;


 */
uint8_t
reg_file_add_meta(TSK_FS_INFO * fs, TSK_FS_FILE * a_fs_file, TSK_INUM_T inum) {
    REGFS_INFO *reg = (REGFS_INFO *) fs;
    TSK_RETVAL_ENUM retval;
    REGFS_CELL cell;
    ssize_t count;

    tsk_error_reset();

    if (inum < fs->first_inum || inum > fs->last_inum) {
        tsk_error_reset();
        tsk_error_set_errno(TSK_ERR_FS_INODE_NUM);
        tsk_error_set_errstr("regfs_file_add_meta: %" PRIuINUM " too large/small", 
			     inum);
        return 1;
    }
    if (a_fs_file == NULL) {
        tsk_error_set_errno(TSK_ERR_FS_ARG);
        tsk_error_set_errstr("regfs_inode_lookup: fs_file is NULL");
        return 1;
    }

    if (fs == NULL) {
        tsk_error_set_errno(TSK_ERR_FS_ARG);
        tsk_error_set_errstr("regfs_inode_lookup: fs is NULL");
        return 1;
    }
    a_fs_file->fs_info = fs;

    if (reg_load_cell(fs, &cell, inum) != TSK_OK) {
        return 1;
    }

    // we will always reset the meta field
    // because this is simple.
    if (a_fs_file->meta != NULL) {
        tsk_fs_meta_close(a_fs_file->meta);
    }

    // for the time being, stuff the entire Record into the 
    // meta content field. On average, it won't be very big.
    // And it shouldn't ever be larger than 4096 bytes.
    if (cell.length > HBIN_SIZE) {
        tsk_error_set_errno(TSK_ERR_FS_INODE_COR);
        tsk_error_set_errstr("regfs_inode_lookup: cell too large");
        return 1;
    }
    if ((a_fs_file->meta = tsk_fs_meta_alloc(cell.length)) == NULL) {
        return 1;
    }

    a_fs_file->meta->addr = inum;
    a_fs_file->meta->flags = TSK_FS_META_FLAG_ALLOC;
    if (cell.type == TSK_REGFS_RECORD_TYPE_VK) {
      a_fs_file->meta->type = TSK_FS_META_TYPE_REG;
    }
    else if (cell.type == TSK_REGFS_RECORD_TYPE_NK) {
      a_fs_file->meta->type = TSK_FS_META_TYPE_DIR;
    }
    else {
      a_fs_file->meta->type = TSK_FS_META_TYPE_VIRT;
    }
    a_fs_file->meta->mode = 0007777;
    a_fs_file->meta->nlink = 1;

    // TODO(wb): parse the size of vk record data
    a_fs_file->meta->size = cell.length;

    // TODO(wb): parse security info
    a_fs_file->meta->uid = 0;
    a_fs_file->meta->gid = 0;

    if (cell.type == TSK_REGFS_RECORD_TYPE_NK) {
      REGFS_CELL_NK *nk = (REGFS_CELL_NK *)&cell;
      uint64_t nttime = tsk_getu64(fs->endian, nk->timestamp);
      a_fs_file->meta->mtime = nt2unixtime(nttime);
      a_fs_file->meta->mtime_nano = nt2nano(nttime);
    }
    else {
      a_fs_file->meta->mtime = 0;
      a_fs_file->meta->mtime_nano = 0;
    }

    // The Registry does not have an Access timestamp
    a_fs_file->meta->atime = 0;
    a_fs_file->meta->atime_nano = 0;

    // The Registry does not have a Changed timestamp
    a_fs_file->meta->ctime = 0;
    a_fs_file->meta->ctime_nano = 0;

    // The Registry does not have a Created timestamp
    a_fs_file->meta->crtime = 0;
    a_fs_file->meta->crtime_nano = 0;

    // The Registry does not have a Deleted timestamp
    a_fs_file->meta->time2.ext2.dtime = 0;
    a_fs_file->meta->time2.ext2.dtime_nano = 0;

    count = tsk_fs_read(fs, inum, (char *)(a_fs_file->meta->content_ptr), 
			cell.length);
    if (count != cell.length) {
      tsk_error_reset();
      tsk_error_set_errno(TSK_ERR_FS_READ);
      tsk_error_set_errstr("Failed to read cell structure");
      return TSK_ERR;
    }

    a_fs_file->meta->seq = 0;

    a_fs_file->meta->link = "";

    return TSK_OK;
}



/**
 * @return 1 on error, 0 otherwise.
 */
uint8_t
reg_block_walk(TSK_FS_INFO * fs,
    TSK_DADDR_T a_start_blk, TSK_DADDR_T a_end_blk,
    TSK_FS_BLOCK_WALK_FLAG_ENUM a_flags, TSK_FS_BLOCK_WALK_CB a_action,
    void *a_ptr)
{
    TSK_FS_BLOCK *fs_block;
    REGFS_INFO *reg;
    TSK_DADDR_T blknum;
    uint8_t retval;
    reg = (REGFS_INFO *) fs;
    
    tsk_error_reset();

    if (tsk_verbose) {
      tsk_fprintf(stderr,
		  "regfs_block_walk: Block Walking %" PRIuDADDR " to %"
		  PRIuDADDR "\n", a_start_blk, a_end_blk);
    }

    if (a_start_blk < fs->first_block || a_start_blk > fs->last_block) {
        tsk_error_reset();
        tsk_error_set_errno(TSK_ERR_FS_WALK_RNG);
	tsk_error_set_errstr("Invalid block walk start block");
        return 1;
    }
    if (a_end_blk < fs->first_block || a_end_blk > fs->last_block) {
        tsk_error_reset();
        tsk_error_set_errno(TSK_ERR_FS_WALK_RNG);
	tsk_error_set_errstr("Invalid block walk end Block");
        return 1;
    }

    // Sanity check on a_flags -- make sure at least one ALLOC is set 
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

    blknum = a_start_blk;

    while (blknum < a_end_blk) {
      ssize_t count;
      uint8_t data_buf[HBIN_SIZE];

      if (tsk_verbose) {
	tsk_fprintf(stderr,
		    "\nregfs_block_walk: Reading block %"  PRIuDADDR 
		    " (offset %"  PRIuDADDR  
		    ") for %" PRIuDADDR  " bytes\n",
		    blknum, blknum * 4096, HBIN_SIZE);
      }

      count = tsk_fs_read_block(fs, blknum, (char *)data_buf, HBIN_SIZE);
      if (count != HBIN_SIZE) {
	tsk_fs_block_free(fs_block);
	return 1;
      }

      if (tsk_fs_block_set(fs, fs_block, blknum,
			   TSK_FS_BLOCK_FLAG_ALLOC | 
			   TSK_FS_BLOCK_FLAG_META | 
			   TSK_FS_BLOCK_FLAG_CONT | 
			   TSK_FS_BLOCK_FLAG_RAW,
			   (char *)data_buf) != 0) {
	tsk_fs_block_free(fs_block);
	return 1;
      }

      retval = a_action(fs_block, a_ptr);
      if (retval == TSK_WALK_STOP) {
	tsk_fs_block_free(fs_block);
	return 0;
      }
      else if (retval == TSK_WALK_ERROR) {
	tsk_fs_block_free(fs_block);
	return 1;
      }
      
      blknum += 1;
    }

    tsk_fs_block_free(fs_block);
    return 0;
}

/**
 * HBINs are always allocated, if they exist in the Registry, and they
 *   may contain both value content and key structures.
 */ 
TSK_FS_BLOCK_FLAG_ENUM
reg_block_getflags(TSK_FS_INFO * fs, TSK_DADDR_T a_addr)
{
    return TSK_FS_BLOCK_FLAG_ALLOC | 
      TSK_FS_BLOCK_FLAG_META | 
      TSK_FS_BLOCK_FLAG_CONT;
}

static uint8_t
reg_inode_walk(TSK_FS_INFO * fs, TSK_INUM_T start_inum,
    TSK_INUM_T end_inum, TSK_FS_META_FLAG_ENUM flags,
    TSK_FS_META_WALK_CB a_action, void *ptr)
{
  return 0;
  /*
    // TODO(wb): old implementation of blk_walk. Adapt here

    REGFS_INFO *reg = (REGFS_INFO *) fs;
    REGFS_CELL cell;

    tsk_error_reset();

    if (a_start_blk < fs->first_block || a_start_blk > fs->last_block) {
        tsk_error_reset();
        tsk_error_set_errno(TSK_ERR_FS_WALK_RNG);
        tsk_error_set_errstr("%s: Start block: %" PRIuDADDR "", myname,
            a_start_blk);
        return 1;
    }
    if (a_end_blk < fs->first_block || a_end_blk > fs->last_block) {
        tsk_error_reset();
        tsk_error_set_errno(TSK_ERR_FS_WALK_RNG);
        tsk_error_set_errstr("%s: End block: %" PRIuDADDR "", myname,
            a_end_blk);
        return 1;
    }

    if (tsk_verbose) {
      tsk_fprintf(stderr,
		  "regfs_block_walk: Block Walking %" PRIuDADDR " to %"
		  PRIuDADDR "\n", a_start_blk, a_end_blk);
    }

    // Sanity check on a_flags -- make sure at least one ALLOC is set 
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

    addr = a_start_blk;

    uint32_t current_hbin_start;

    current_hbin_start = addr - (addr % HBIN_SIZE);

    while (addr < a_end_blk) {
      int myflags;      

      // TODO(wb) be sure not to overrun image


      if (reg_load_cell(fs, &cell, addr) == TSK_ERR) {
	tsk_fs_block_free(fs_block);
	return 1;
      }
      myflags = 0;

      if (cell.is_allocated) {
	myflags = TSK_FS_BLOCK_FLAG_ALLOC;
      } else {
	myflags = TSK_FS_BLOCK_FLAG_UNALLOC;
      }

      switch (cell.type) {
      case TSK_REGFS_RECORD_TYPE_NK: // fall through intended
      case TSK_REGFS_RECORD_TYPE_LF: // fall through intended
      case TSK_REGFS_RECORD_TYPE_LH: // fall through intended
      case TSK_REGFS_RECORD_TYPE_LI: // fall through intended
      case TSK_REGFS_RECORD_TYPE_RI: // fall through intended
      case TSK_REGFS_RECORD_TYPE_DB: // fall through intended
      case TSK_REGFS_RECORD_TYPE_SK: // fall through intended
      case TSK_REGFS_RECORD_TYPE_VK: // fall through intended
	myflags |= TSK_FS_BLOCK_FLAG_META;
	break;
      case TSK_REGFS_RECORD_TYPE_UNKNOWN:
      default:
	myflags |= TSK_FS_BLOCK_FLAG_CONT;
	break;
      }

      if (addr + cell.length > current_hbin_start + HBIN_SIZE - 1) {
	// The Cell overran into the next HBIN header
        tsk_error_reset();
        tsk_error_set_errno(TSK_ERR_FS_BLK_NUM);
        tsk_error_set_errstr("Cell overran into subsequent HBIN header");
	tsk_fs_block_free(fs_block);
        return 1;
      }


      addr += cell.length;

      // skip HBIN headers
      if (addr > current_hbin_start + HBIN_SIZE) {
	current_hbin_start += HBIN_SIZE;
	addr = current_hbin_start + 0x20;
      }
    }

    tsk_fs_block_free(fs_block);
    return 0;
*/
}

static TSK_FS_ATTR_TYPE_ENUM
reg_get_default_attr_type(const TSK_FS_FILE * a_file)
{
    if ((a_file == NULL) || (a_file->meta == NULL))
        return TSK_FS_ATTR_TYPE_DEFAULT;

    /* Use DATA for files and IDXROOT for dirs */
    if (a_file->meta->type == TSK_FS_META_TYPE_DIR)
        return TSK_FS_ATTR_TYPE_NTFS_IDXROOT;
    else
        return TSK_FS_ATTR_TYPE_NTFS_DATA;
}

/** 
 * Load the attributes.
 * @param a_fs_file File to load attributes for.
 * @returns 1 on error
 */
static uint8_t
reg_load_attrs(TSK_FS_FILE * a_fs_file)
{
    return 0;
}

TSK_RETVAL_ENUM
reg_dir_open_meta(TSK_FS_INFO * fs, TSK_FS_DIR ** a_fs_dir,
    TSK_INUM_T a_addr)
{
  //    REGFS_INFO *reg = (REGFS_INFO *) fs;
    return 0;
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
reg_fsstat(TSK_FS_INFO * fs, FILE * hFile)
{
    REGFS_INFO *reg = (REGFS_INFO *) fs;
    char asc[512];

    tsk_fprintf(hFile, "\nFILE SYSTEM INFORMATION\n");
    tsk_fprintf(hFile, "--------------------------------------------\n");
    tsk_fprintf(hFile, "File System Type: Windows Registry\n");

    // TODO(wb): print readable versions
    tsk_fprintf(hFile, "Major Version: %d\n", 
		(tsk_getu32(fs->endian, reg->regf.major_version)));
    tsk_fprintf(hFile, "Minor Version: %d\n", 
		(tsk_getu32(fs->endian, reg->regf.minor_version)));

    if ((tsk_getu32(fs->endian, reg->regf.seq1) == 
	 (tsk_getu32(fs->endian, reg->regf.seq2)))) {
      tsk_fprintf(hFile, "Synchronized: %s\n", "Yes");
    } else {
      tsk_fprintf(hFile, "Synchronized: %s\n", "No");
    }

    if (regfs_utf16to8(fs->endian, "REGF hive name label",
		       reg->regf.hive_name, 30,
		       asc, 512) != TSK_OK) {
	tsk_error_reset();
	tsk_error_set_errno(TSK_ERR_FS_UNICODE);
	tsk_error_set_errstr("Failed to convert REGF hive name string to UTF-8");
	return 1;
    }
    tsk_fprintf(hFile, "Hive name: %s\n", asc);    


    tsk_fprintf(hFile, "\nMETADATA INFORMATION\n");
    tsk_fprintf(hFile, "--------------------------------------------\n");

    tsk_fprintf(hFile, "Offset to first key: %" PRIu32 "\n",
		(tsk_getu32(fs->endian, reg->regf.first_key_offset)));

    tsk_fprintf(hFile, "Offset to last HBIN: %" PRIu32 "\n",
		(tsk_getu32(fs->endian, reg->regf.last_hbin_offset)));

    tsk_fprintf(hFile, "\nCONTENT INFORMATION\n");
    tsk_fprintf(hFile, "--------------------------------------------\n");
    tsk_fprintf(hFile, "Number of active cells: <unknown>\n"); //  TODO(wb)
    tsk_fprintf(hFile, "Number of inactive cells: <unknown>\n"); //  TODO(wb)
    tsk_fprintf(hFile, "Number of active bytes: <unknown>\n"); //  TODO(wb)
    tsk_fprintf(hFile, "Number of inactive bytes: <unknown>\n"); //  TODO(wb)
    tsk_fprintf(hFile, "Number of VK records: <unknown>\n"); //  TODO(wb)
    tsk_fprintf(hFile, "Number of NK records: <unknown>\n"); //  TODO(wb)
    tsk_fprintf(hFile, "Number of LF records: <unknown>\n"); //  TODO(wb)
    tsk_fprintf(hFile, "Number of LH records: <unknown>\n"); //  TODO(wb)
    tsk_fprintf(hFile, "Number of LI records: <unknown>\n"); //  TODO(wb)
    tsk_fprintf(hFile, "Number of RI records: <unknown>\n"); //  TODO(wb)
    tsk_fprintf(hFile, "Number of SK records: <unknown>\n"); //  TODO(wb)
    tsk_fprintf(hFile, "Number of DB records: <unknown>\n"); //  TODO(wb)

    return 0;
}

static uint8_t
reg_fscheck(TSK_FS_INFO * fs, FILE * hFile)
{
    tsk_error_reset();
    tsk_error_set_errno(TSK_ERR_FS_UNSUPFUNC);
    tsk_error_set_errstr("fscheck not implemented for Windows Registries yet");
    return 1;
}

static TSK_RETVAL_ENUM
reg_istat_vk(TSK_FS_INFO * fs, FILE * hFile,
		  REGFS_CELL *cell, TSK_DADDR_T numblock, int32_t sec_skew) {
  //    REGFS_INFO *reg = (REGFS_INFO *) fs;
    tsk_fprintf(hFile, "\nRECORD INFORMATION\n");
    tsk_fprintf(hFile, "--------------------------------------------\n");
    tsk_fprintf(hFile, "Record Type: %s\n", "VK");
    return TSK_OK;
}


static TSK_RETVAL_ENUM
reg_istat_nk(TSK_FS_INFO * fs, FILE * hFile,
		  REGFS_CELL *cell, TSK_DADDR_T numblock, int32_t sec_skew) {
  //    REGFS_INFO *reg = (REGFS_INFO *) fs;
    ssize_t count;
    uint8_t buf[HBIN_SIZE];
    REGFS_CELL_NK *nk;
    char s[512]; // to be used throughout, temporarily
    uint16_t name_length;

    // TODO(wb): need a call to the following function
    // tsk_fs_file_open_meta(TSK_FS_INFO * a_fs, TSK_FS_FILE * a_fs_file, TSK_INUM_T a_addr)
    // and use the TSK_FS_FILE structure instead of
    // directly accessing the REGFS_CELL_NK memory structure


    if (cell->length > HBIN_SIZE) {
      tsk_error_reset();
      tsk_error_set_errno(TSK_ERR_FS_INODE_COR);
      tsk_error_set_errstr("Registry cell corrupt: size too large 4");
      return TSK_ERR;
    }

    count = tsk_fs_read(fs, (cell->inum), (char *)buf, cell->length);
    if (count != cell->length) {
      tsk_error_reset();
      tsk_error_set_errno(TSK_ERR_FS_READ);
      tsk_error_set_errstr("Failed to read cell structure");
      return TSK_ERR;
    }

    tsk_fprintf(hFile, "\nRECORD INFORMATION\n");
    tsk_fprintf(hFile, "--------------------------------------------\n");
    tsk_fprintf(hFile, "Record Type: %s\n", "NK");

    nk = (REGFS_CELL_NK *)(buf + 4);

    if ((tsk_gets32(fs->endian, nk->classname_offset)) == 0xFFFFFFFF) {
      tsk_fprintf(hFile, "Class Name: %s\n", "None");
    } else {
      char asc[512];
      uint32_t classname_offset;
      uint32_t classname_length;

      classname_offset = (tsk_gets32(fs->endian, nk->classname_offset));
      classname_length = (tsk_gets16(fs->endian, nk->classname_length));

      if (classname_length > 512) {
	tsk_error_reset();
	tsk_error_set_errno(TSK_ERR_FS_INODE_COR);
	tsk_error_set_errstr("NK classname string too long");
	return TSK_ERR;
      }

      count = tsk_fs_read(fs, FIRST_HBIN_OFFSET + classname_offset + 4, 
			  s, classname_length);
      if (count != classname_length) {
	tsk_error_reset();
	tsk_error_set_errno(TSK_ERR_FS_READ);
	tsk_error_set_errstr("Failed to read NK classname string");
	return TSK_ERR;
      }

      if (regfs_utf16to8(fs->endian, "NK class name", (uint8_t *)s, 
			 512, asc, 512) != TSK_OK) {
	tsk_error_reset();
	tsk_error_set_errno(TSK_ERR_FS_UNICODE);
	tsk_error_set_errstr("Failed to convert NK classname string to UTF-8");
	return TSK_ERR;
      }

      tsk_fprintf(hFile, "Class Name: %s\n", asc);    
    }

    name_length = (tsk_getu16(fs->endian, nk->name_length));
    if (name_length > 512) {
	tsk_error_reset();
	tsk_error_set_errno(TSK_ERR_FS_INODE_COR);
	tsk_error_set_errstr("NK key name string too long");
	return TSK_ERR;
    }

    strncpy(s, (char *)nk->name, name_length);
    tsk_fprintf(hFile, "Key Name: %s\n", s);

    if ((tsk_getu16(fs->endian, nk->is_root)) == 0x2C) {
      tsk_fprintf(hFile, "Root Record: %s\n", "Yes");
    } else {
      tsk_fprintf(hFile, "Root Record: %s\n", "No");
    }

    /* TODO(wb): Once we have to fs_meta down...
    if (sec_skew != 0) {
        tsk_fprintf(hFile, "\nAdjusted Entry Times:\n");

        if (fs_meta->mtime)
            fs_meta->mtime -= sec_skew;

        tsk_fprintf(hFile, "Modified:\t%s\n",
            tsk_fs_time_to_str(fs_meta->mtime, timeBuf));

        if (fs_meta->mtime == 0)
            fs_meta->mtime += sec_skew;

        tsk_fprintf(hFile, "\nOriginal Entry Times:\n");
    }
    else {
        tsk_fprintf(hFile, "\Entry Times:\n");
    }
    tsk_fprintf(hFile, "Modified:\t%s\n", tsk_fs_time_to_str(fs_meta->mtime,
            timeBuf));
    */

    tsk_fprintf(hFile, "Parent Record: %" PRIuINUM "\n", 
		FIRST_HBIN_OFFSET + (tsk_getu32(fs->endian, nk->parent_nk_offset)));

    return TSK_OK;
}

static TSK_RETVAL_ENUM
reg_istat_lf(TSK_FS_INFO * fs, FILE * hFile,
		  REGFS_CELL *cell, TSK_DADDR_T numblock, int32_t sec_skew) {
  //    REGFS_INFO *reg = (REGFS_INFO *) fs;
    tsk_fprintf(hFile, "RECORD INFORMATION\n");
    tsk_fprintf(hFile, "--------------------------------------------\n");
    tsk_fprintf(hFile, "Record Type: %s\n", "LF");
    return TSK_OK;
}

static TSK_RETVAL_ENUM
reg_istat_lh(TSK_FS_INFO * fs, FILE * hFile,
		  REGFS_CELL *cell, TSK_DADDR_T numblock, int32_t sec_skew) {
  //    REGFS_INFO *reg = (REGFS_INFO *) fs;
    tsk_fprintf(hFile, "RECORD INFORMATION\n");
    tsk_fprintf(hFile, "--------------------------------------------\n");
    tsk_fprintf(hFile, "Record Type: %s\n", "LH");
    return TSK_OK;
}

static TSK_RETVAL_ENUM
reg_istat_li(TSK_FS_INFO * fs, FILE * hFile,
		  REGFS_CELL *cell, TSK_DADDR_T numblock, int32_t sec_skew) {
  //    REGFS_INFO *reg = (REGFS_INFO *) fs;
    tsk_fprintf(hFile, "RECORD INFORMATION\n");
    tsk_fprintf(hFile, "--------------------------------------------\n");
    tsk_fprintf(hFile, "Record Type: %s\n", "LI");
    return TSK_OK;
}

static TSK_RETVAL_ENUM
reg_istat_ri(TSK_FS_INFO * fs, FILE * hFile,
		  REGFS_CELL *cell, TSK_DADDR_T numblock, int32_t sec_skew) {
  //    REGFS_INFO *reg = (REGFS_INFO *) fs;
    tsk_fprintf(hFile, "RECORD INFORMATION\n");
    tsk_fprintf(hFile, "--------------------------------------------\n");
    tsk_fprintf(hFile, "Record Type: %s\n", "RI");
    return TSK_OK;
}

static TSK_RETVAL_ENUM
reg_istat_sk(TSK_FS_INFO * fs, FILE * hFile,
		  REGFS_CELL *cell, TSK_DADDR_T numblock, int32_t sec_skew) {
  //    REGFS_INFO *reg = (REGFS_INFO *) fs;
    tsk_fprintf(hFile, "RECORD INFORMATION\n");
    tsk_fprintf(hFile, "--------------------------------------------\n");
    tsk_fprintf(hFile, "Record Type: %s\n", "SK");
    return TSK_OK;
}

static TSK_RETVAL_ENUM
reg_istat_db(TSK_FS_INFO * fs, FILE * hFile,
		  REGFS_CELL *cell, TSK_DADDR_T numblock, int32_t sec_skew) {
  //    REGFS_INFO *reg = (REGFS_INFO *) fs;
    tsk_fprintf(hFile, "RECORD INFORMATION\n");
    tsk_fprintf(hFile, "--------------------------------------------\n");
    tsk_fprintf(hFile, "Record Type: %s\n", "DB");
    return TSK_OK;
}

static TSK_RETVAL_ENUM
reg_istat_unknown(TSK_FS_INFO * fs, FILE * hFile,
		  REGFS_CELL *cell, TSK_DADDR_T numblock, int32_t sec_skew) {
  //    REGFS_INFO *reg = (REGFS_INFO *) fs;
    ssize_t count;
    uint8_t buf[HBIN_SIZE];

    if (cell->length > HBIN_SIZE) {
      tsk_error_reset();
      tsk_error_set_errno(TSK_ERR_FS_INODE_COR);
      tsk_error_set_errstr("Registry cell corrupt: size too large 2");
      return TSK_ERR;
    }

    count = tsk_fs_read(fs, (cell->inum), (char *)buf, cell->length);
    if (count != cell->length) {
      tsk_error_reset();
      tsk_error_set_errno(TSK_ERR_FS_READ);
      tsk_error_set_errstr("Failed to read cell structure");
      return TSK_ERR;
    }

    tsk_fprintf(hFile, "RECORD INFORMATION\n");
    tsk_fprintf(hFile, "--------------------------------------------\n");
    tsk_fprintf(hFile, "Record Type: %s\n", "Unknown (Data Record?)");
    tsk_fprintf(hFile, "Type identifier: 0x%x%x\n", *(buf + 4), *(buf + 5));
    return TSK_OK;
}





/**
 * Print details on a specific file to a file handle.
 *
 * @param fs File system file is located in
 * @param hFile File name to print text to
 * @param inum Address of file in file system
 * @param numblock The number of blocks in file to force print (can go beyond file size)
 * @param sec_skew Clock skew in seconds to also print times in
 *
 * @returns 1 on error and 0 on success
 */
static uint8_t
reg_istat(TSK_FS_INFO * fs, FILE * hFile,
    TSK_INUM_T inum, TSK_DADDR_T numblock, int32_t sec_skew)
{
  //    REGFS_INFO *reg = (REGFS_INFO *) fs;
    REGFS_CELL cell;

    tsk_fprintf(hFile, "\nCELL INFORMATION\n");
    tsk_fprintf(hFile, "--------------------------------------------\n");

    if (reg_load_cell(fs, &cell, inum) != TSK_OK) {
      return 1;
    }

    tsk_fprintf(hFile, "Cell: %" PRIuINUM "\n", inum);    
    if (cell.is_allocated) {
      tsk_fprintf(hFile, "Allocated: %s\n", "Yes");    
    } else {
      tsk_fprintf(hFile, "Allocated: %s\n", "No");    
    }
    tsk_fprintf(hFile, "Cell Size: %" PRIuINUM "\n", cell.length);

    switch (cell.type) {
    case TSK_REGFS_RECORD_TYPE_VK:
      reg_istat_vk(fs, hFile, &cell, numblock, sec_skew);
      break;
    case TSK_REGFS_RECORD_TYPE_NK:
      reg_istat_nk(fs, hFile, &cell, numblock, sec_skew);
      break;
    case TSK_REGFS_RECORD_TYPE_LF:
      reg_istat_lf(fs, hFile, &cell, numblock, sec_skew);
      break;
    case TSK_REGFS_RECORD_TYPE_LH:
      reg_istat_lh(fs, hFile, &cell, numblock, sec_skew);
      break;
    case TSK_REGFS_RECORD_TYPE_LI:
      reg_istat_li(fs, hFile, &cell, numblock, sec_skew);
      break;
    case TSK_REGFS_RECORD_TYPE_RI:
      reg_istat_ri(fs, hFile, &cell, numblock, sec_skew);
      break;
    case TSK_REGFS_RECORD_TYPE_SK:
      reg_istat_sk(fs, hFile, &cell, numblock, sec_skew);
      break;
    case TSK_REGFS_RECORD_TYPE_DB:
      reg_istat_db(fs, hFile, &cell, numblock, sec_skew);
      break;
    case TSK_REGFS_RECORD_TYPE_UNKNOWN:
      // fall through intended
    default:
      reg_istat_unknown(fs, hFile, &cell, numblock, sec_skew);
      break;
    }

    return 0;
}

static void
reg_close(TSK_FS_INFO * fs)
{
  //    REGFS_INFO *reg = (REGFS_INFO *) fs;

    if (fs == NULL)
        return;
    tsk_fs_free(fs);
}

int
reg_name_cmp(TSK_FS_INFO * a_fs_info, const char *s1, const char *s2)
{
    return strcasecmp(s1, s2);
}

/**
 * @brief reg_journal_unsupported
 */
static void
reg_journal_unsupported() {
    tsk_error_reset();
    tsk_error_set_errno(TSK_ERR_FS_UNSUPFUNC);
    tsk_error_set_errstr("The Windows Registry does not have a journal.\n");
    return;
}

/**
 * @brief reg_jblk_walk
 * @param fs
 * @param start
 * @param end
 * @param flags
 * @param a_action
 * @param ptr
 * @return 1, as this is unsupported.
 */
static uint8_t
reg_jblk_walk(TSK_FS_INFO * fs, TSK_DADDR_T start,
    TSK_DADDR_T end, int flags, TSK_FS_JBLK_WALK_CB a_action, void *ptr)
{
    reg_journal_unsupported();
    return 1;
}

/**
 * @brief reg_jentry_walk
 * @param fs
 * @param flags
 * @param a_action
 * @param ptr
 * @return 1, as this is unsupported.
 */
static uint8_t
reg_jentry_walk(TSK_FS_INFO * fs, int flags,
    TSK_FS_JENTRY_WALK_CB a_action, void *ptr)
{
    reg_journal_unsupported();
    return 1;
}

/**
 * @brief ntfs_jopen
 * @param fs
 * @param inum
 * @return 1, as this is unsupported.
 */
static uint8_t
reg_jopen(TSK_FS_INFO * fs, TSK_INUM_T inum)
{
    reg_journal_unsupported();
    return 1;
}


static uint8_t
reg_file_get_sidstr(TSK_FS_FILE * a_fs_file, char **sid_str)
{
  return 1;
}


















/**
 * reg_load_regf
 *   Read data into the supplied REGF, and do some sanity checking.
 */
TSK_RETVAL_ENUM
reg_load_regf(TSK_FS_INFO *fs_info, REGF *regf) {
    ssize_t count;

    count = tsk_fs_read(fs_info, 0, (char *)regf, sizeof(REGF));
    if (count != sizeof(REGF)) {
        tsk_error_reset();
        tsk_error_set_errno(TSK_ERR_FS_READ);
        tsk_error_set_errstr("Failed to read REGF header structure");
        return TSK_ERR;
    }

    if ((tsk_getu32(fs_info->endian, regf->magic)) != REG_REGF_MAGIC) {
        tsk_error_set_errno(TSK_ERR_FS_INODE_COR);
        tsk_error_set_errstr("REGF header has an invalid magic header");
        return TSK_ERR;
    }

    return TSK_OK;
}



/**
 * \internal
 * Open part of a disk image as a Windows Registry.
 *
 * @param img_info Disk image to analyze
 * @param offset Byte offset where file system starts
 * @param ftype Specific type of file system
 * @param test NOT USED * @returns NULL on error or if data is not a Registry
 */
TSK_FS_INFO *
regfs_open(TSK_IMG_INFO * img_info, TSK_OFF_T offset,
    TSK_FS_TYPE_ENUM ftype, uint8_t test)
{
    TSK_FS_INFO *fs;
    REGFS_INFO *reg;

    tsk_error_reset();

    if (TSK_FS_TYPE_ISREG(ftype) == 0) {
        tsk_error_reset();
        tsk_error_set_errno(TSK_ERR_FS_ARG);
        tsk_error_set_errstr("Invalid FS type in reg_open");
        return NULL;
    }

    if ((reg = (REGFS_INFO *) tsk_fs_malloc(sizeof(REGFS_INFO))) == NULL) {
        return NULL;
    }
    fs = &(reg->fs_info);

    fs->ftype = TSK_FS_TYPE_REG;
    fs->duname = "Cell";
    fs->flags = TSK_FS_INFO_FLAG_NONE;
    fs->tag = TSK_FS_INFO_TAG;
    fs->endian = TSK_LIT_ENDIAN;

    fs->img_info = img_info;
    fs->offset = offset;

    if (reg_load_regf(fs, &(reg->regf)) != TSK_OK) {
        free(reg);
        return NULL;
    }


    fs->first_inum = FIRST_HBIN_OFFSET;
    fs->last_inum  = (tsk_getu32(fs->endian, reg->regf.last_hbin_offset)) + HBIN_SIZE;
    // TODO(wb): set root inode
    // TODO(wb): set num inodes
    fs->block_size = HBIN_SIZE;
    fs->first_block = 0;
    // TODO(wb): from where is this offset relative?
    fs->last_block = (tsk_getu32(fs->endian, reg->regf.last_hbin_offset)); 
    fs->last_block_act = (img_info->size - (img_info->size % HBIN_SIZE)) / HBIN_SIZE;

    fs->inode_walk = reg_inode_walk;
    fs->block_walk = reg_block_walk;
    fs->block_getflags = reg_block_getflags;

    fs->get_default_attr_type = reg_get_default_attr_type;
    fs->load_attrs = reg_load_attrs;

    fs->file_add_meta = reg_file_add_meta;
    fs->dir_open_meta = reg_dir_open_meta;
    fs->fsstat = reg_fsstat;
    fs->fscheck = reg_fscheck;
    fs->istat = reg_istat;
    fs->close = reg_close;
    fs->name_cmp = reg_name_cmp;

    fs->fread_owner_sid = reg_file_get_sidstr;
    fs->jblk_walk = reg_jblk_walk;
    fs->jentry_walk = reg_jentry_walk;
    fs->jopen = reg_jopen;
    fs->journ_inum = 0;


    return (fs);
}
