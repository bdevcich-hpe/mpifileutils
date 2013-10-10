/*
 * This file contains the logic to walk all of the source objects and place
 * them on the queue.
 *
 * In the case of directories, we'll simply read the contents of a directory
 * and place each sub-object back on the queue to be treewalked again. In the
 * case of files, we chunk up each file and place it on the queue as another
 * libcircle action to be later processed by the COPY and CLEANUP stages.
 *
 * See the file "COPYING" for the full license governing this code.
 */

#include "treewalk.h"
#include "dcp.h"

#include <dirent.h>
#include <errno.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <inttypes.h>
#include <sys/time.h>

/** Options specified by the user. */
extern DCOPY_options_t DCOPY_user_opts;

/** Statistics to gather for summary output. */
extern DCOPY_statistics_t DCOPY_statistics;

/* given path, return level within directory tree */
static int compute_depth(const char* path)
{
    const char* c;
    int depth = 0;
    for (c = path; *c != '\0'; c++) {
        if (*c == '/') {
            depth++;
        }
    }
    return depth;
}

/**
 * This function copies a link.
 */
static void DCOPY_stat_process_link(DCOPY_operation_t* op,
                             const struct stat64* statbuf,
                             CIRCLE_handle* handle)
{
    /* increment our link count by one */
    DCOPY_statistics.total_links++;

    /* get source and destination paths */
    const char* src_path  = op->operand;
    const char* dest_path = op->dest_full_path;

    /* read link and terminate string with NUL character */
    char path[PATH_MAX + 1];
    ssize_t rc = bayer_readlink(src_path, path, sizeof(path) - 1);

    if(rc < 0) {
        LOG(DCOPY_LOG_ERR, "Failed to read link `%s' readlink() errno=%d %s",
            src_path, errno, strerror(errno)
           );
        return;
    }

    /* ensure that string ends with NUL */
    path[rc] = '\0';

    /* create new link */
    int symrc = bayer_symlink(path, dest_path);

    if(symrc < 0) {
        LOG(DCOPY_LOG_ERR, "Failed to create link `%s' symlink() errno=%d %s",
            dest_path, errno, strerror(errno)
           );
        return;
    }

    /* set permissions on link */
    if (DCOPY_user_opts.preserve) {
        DCOPY_copy_xattrs(op, statbuf, dest_path);
        DCOPY_copy_ownership(statbuf, dest_path);
        DCOPY_copy_permissions(statbuf, dest_path);
    }

    return;
}

/**
 * This function inputs a file and creates chunk operations that get placed
 * onto the libcircle queue for future processing by the copy stage.
 */
static void DCOPY_stat_process_file(DCOPY_operation_t* op,
                             const struct stat64* statbuf,
                             CIRCLE_handle* handle)
{
    /* increment our file count by one */
    DCOPY_statistics.total_files++;

    /* get file size, and increment total bytes */
    int64_t file_size = (int64_t)statbuf->st_size;
    DCOPY_statistics.total_size += file_size;

    /* compute number of chunks */
    int64_t num_chunks = file_size / (int64_t)DCOPY_user_opts.chunk_size;

    LOG(DCOPY_LOG_DBG, "File `%s' size is `%" PRId64 \
        "' with chunks `%" PRId64 "' (total `%" PRId64 "').", \
        op->operand, file_size, num_chunks, \
        num_chunks * DCOPY_user_opts.chunk_size);

    const char* dest_path = op->dest_full_path;

    /* since file systems like Lustre require xattrs to be set before file is opened,
     * we first create it with mknod and then set xattrs */

    /* create file with mknod
    * for regular files, dev argument is supposed to be ignored,
    * see makedev() to create valid dev */
    dev_t dev;
    memset(&dev, 0, sizeof(dev_t));
    int mknod_rc = bayer_mknod(dest_path, DCOPY_DEF_PERMS_FILE | S_IFREG, dev);

    if(mknod_rc < 0) {
        if(errno == EEXIST) {
            /* TODO: should we unlink and mknod again in this case? */
        }

        LOG(DCOPY_LOG_DBG, "File `%s' mknod() errno=%d %s",
            dest_path, errno, strerror(errno)
           );
    }

    /* copy extended attributes, important to do this first before
     * writing data because some attributes tell file system how to
     * stripe data, e.g., Lustre */
    if (DCOPY_user_opts.preserve) {
        DCOPY_copy_xattrs(op, statbuf, dest_path);
    }

    /* Encode and enqueue each chunk of the file for processing later. */
    int64_t chunk_index = 0;
    while(chunk_index < num_chunks) {
        char* newop = DCOPY_encode_operation(COPY, chunk_index, op->operand, \
                                             op->source_base_offset, \
                                             op->dest_base_appendix, file_size);
        handle->enqueue(newop);
        free(newop);
        chunk_index++;
    }

    /* Encode and enqueue the last partial chunk. */
    if((num_chunks * DCOPY_user_opts.chunk_size) < file_size || num_chunks == 0) {
        char* newop = DCOPY_encode_operation(COPY, chunk_index, op->operand, \
                                             op->source_base_offset, \
                                             op->dest_base_appendix, file_size);
        handle->enqueue(newop);
        free(newop);
    }
}

/**
 * This function reads the contents of a directory and generates appropriate
 * libcircle operations for every object in the directory. It then places those
 * operations on the libcircle queue and returns.
 */
static void DCOPY_stat_process_dir(DCOPY_operation_t* op,
                            const struct stat64* statbuf,
                            CIRCLE_handle* handle)
{
    /* increment our directory count by one */
    DCOPY_statistics.total_dirs++;

    /* get destination path */
    const char* dest_path = op->dest_full_path;

    /* first, create the destination directory */
    LOG(DCOPY_LOG_DBG, "Creating directory: %s", dest_path);
    int rc = bayer_mkdir(dest_path, DCOPY_DEF_PERMS_DIR);
    if(rc != 0) {
        LOG(DCOPY_LOG_ERR, "Failed to create directory: %s (errno=%d %s)", \
            dest_path, errno, strerror(errno));
        return;
    }

    /* copy extended attributes on directory */
    if (DCOPY_user_opts.preserve) {
        DCOPY_copy_xattrs(op, statbuf, dest_path);
    }

    /* iterate through source directory and add items to queue */
    DIR* curr_dir = bayer_opendir(op->operand);

    if(curr_dir == NULL) {
        /* failed to open directory */
        LOG(DCOPY_LOG_ERR, "Unable to open dir `%s'. errno=%d %s", \
            op->operand, errno, strerror(errno));

        DCOPY_retry_failed_operation(TREEWALK, handle, op);
        return;
    }
    else {
        struct dirent* curr_ent;
        while((curr_ent = bayer_readdir(curr_dir)) != NULL) {
            char* curr_dir_name = curr_ent->d_name;

            /* We don't care about . or .. */
            if((strncmp(curr_dir_name, ".", 2)) && (strncmp(curr_dir_name, "..", 3))) {

                /* build new object name */
                char newop_path[PATH_MAX];
                sprintf(newop_path, "%s/%s", op->operand, curr_dir_name);

                LOG(DCOPY_LOG_DBG, "Stat operation is enqueueing `%s'", newop_path);

                /* Distributed recursion here. */
                char* newop = DCOPY_encode_operation(TREEWALK, 0, newop_path, \
                                               op->source_base_offset, op->dest_base_appendix, op->file_size);
                handle->enqueue(newop);

                free(newop);
            }
        }
    }

    bayer_closedir(curr_dir);

    return;
}

/**
 * This is the entry point for the "file stat stage". This function is called
 * from the jump table required for the main libcircle callbacks.
 */
void DCOPY_do_treewalk(DCOPY_operation_t* op,
                       CIRCLE_handle* handle)
{
    struct stat64 statbuf;

    /* stat the item */
    if(bayer_lstat64(op->operand, &statbuf) < 0) {
        LOG(DCOPY_LOG_DBG, "Could not get info for `%s'. errno=%d %s", op->operand, errno, strerror(errno));
        DCOPY_retry_failed_operation(TREEWALK, handle, op);
        return;
    }

    /* first check that we handle this file type */
    if(! S_ISDIR(statbuf.st_mode) &&
       ! S_ISREG(statbuf.st_mode) &&
       ! S_ISLNK(statbuf.st_mode))
    {
        if (S_ISCHR(statbuf.st_mode)) {
          LOG(DCOPY_LOG_ERR, "Encountered an unsupported file type S_ISCHR at `%s'.", op->operand);
        } else if (S_ISBLK(statbuf.st_mode)) {
          LOG(DCOPY_LOG_ERR, "Encountered an unsupported file type S_ISBLK at `%s'.", op->operand);
        } else if (S_ISFIFO(statbuf.st_mode)) {
          LOG(DCOPY_LOG_ERR, "Encountered an unsupported file type S_ISFIFO at `%s'.", op->operand);
        } else if (S_ISSOCK(statbuf.st_mode)) {
          LOG(DCOPY_LOG_ERR, "Encountered an unsupported file type S_ISSOCK at `%s'.", op->operand);
        } else {
          LOG(DCOPY_LOG_ERR, "Encountered an unsupported file type %x at `%s'.", statbuf.st_mode, op->operand);
        }
        return;
    }

    /* create new element to record file path and stat info */
    DCOPY_stat_elem_t* elem = (DCOPY_stat_elem_t*) BAYER_MALLOC(sizeof(DCOPY_stat_elem_t));
    elem->file = BAYER_STRDUP(op->dest_full_path);
    elem->sb = (struct stat64*) BAYER_MALLOC(sizeof(struct stat64));
    elem->depth = compute_depth(op->dest_full_path);
    memcpy(elem->sb, &statbuf, sizeof(struct stat64));
    elem->next = NULL;

    /* append element to tail of linked list */
    if (DCOPY_list_head == NULL) {
        DCOPY_list_head = elem;
    }
    if (DCOPY_list_tail != NULL) {
        DCOPY_list_tail->next = elem;
    }
    DCOPY_list_tail = elem;

    /* handle item depending on its type */
    if(S_ISDIR(statbuf.st_mode)) {
        /* LOG(DCOPY_LOG_DBG, "Stat operation found a directory at `%s'.", op->operand); */
        DCOPY_stat_process_dir(op, &statbuf, handle);
    }
    else if(S_ISREG(statbuf.st_mode)) {
        /* LOG(DCOPY_LOG_DBG, "Stat operation found a file at `%s'.", op->operand); */
        DCOPY_stat_process_file(op, &statbuf, handle);
    }
    else if(S_ISLNK(statbuf.st_mode)) {
        /* LOG(DCOPY_LOG_DBG, "Stat operation found a link at `%s'.", op->operand); */
        DCOPY_stat_process_link(op, &statbuf, handle);
    }
    else {
        LOG(DCOPY_LOG_ERR, "Encountered an unsupported file type %x at `%s'.", statbuf.st_mode, op->operand);
        DCOPY_retry_failed_operation(TREEWALK, handle, op);
        return;
    }
}

/* EOF */
