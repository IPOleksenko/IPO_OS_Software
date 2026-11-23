#ifndef IPO_FS_H
#define IPO_FS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define IPO_FS_BLOCK_SIZE 512
#define IPO_FS_MAGIC_STR "IPO_FS"

#define IPO_INODE_EXTENTS 4
#define IPO_EXTENT_NODE_EXTENTS 20

/* inode types/flags */
#define IPO_INODE_TYPE_DIR 0x1
#define IPO_INODE_TYPE_FILE 0x2
#define IPO_INODE_FLAG_PROTECTED 0x80000000u

#define IPO_FS_FLAG_LINKED_EXTENTS 0x0001u
#define IPO_FS_FLAG_DYNAMIC_INODES 0x0002u

#define IPO_FS_OK               0
#define IPO_FS_ERR_NO_SPACE     1
#define IPO_FS_ERR_INODE_LIMIT  2
#define IPO_FS_ERR_CORRUPT      3
#define IPO_FS_ERR_NOT_MOUNTED  4
#define IPO_FS_ERR_IO           5
#define IPO_FS_ERR_INVALID_ARG  6

/* 32-byte descriptor tracking up to 64 contiguous dynamic inodes */
struct __attribute__((packed)) ipo_inode_chunk_desc {
    uint64_t start_inode;      /* 1-based */
    uint64_t table_block;
    uint64_t bitmap;           /* bit i = 1 when inode i is used */
    uint32_t inode_count;      /* typically 4..64 */
    uint32_t free_inodes;
};

#define IPO_CHUNK_NODE_CHUNKS 15

/* 512 B chained chunk node: 4 + 4 + 8 + N*32 (up to 15 per 512 B block) */
struct __attribute__((packed)) ipo_inode_chunk_node {
    uint32_t count;            /* 0..15 active entries */
    uint32_t flags;
    uint64_t next_node;        /* 0 at end of chain */
    struct ipo_inode_chunk_desc chunks[];
};

struct __attribute__((packed)) ipo_superblock {
    uint64_t magic;             /* "IPO_FS\0\0" */
    uint64_t fs_size_blocks;    /* Total blocks in storage pool (up to 9.44 Zettabytes!) */
    uint32_t block_size;        /* 512 bytes */
    uint32_t flags;             /* IPO_FS_FLAG_* */
    uint64_t inode_count;       /* Total inodes (primary + dynamic) */
    uint64_t inode_bitmap_start;/* Block index of inode bitmap */
    uint64_t block_bitmap_start;/* Block index of block bitmap */
    uint64_t inode_table_start; /* Block index of inode table */
    uint64_t data_blocks_start; /* Block index of data blocks */
    uint32_t primary_inodes;
    uint32_t reserved;
    uint64_t dynamic_chunk_tree;/* first ipo_inode_chunk_node block */
    uint64_t dynamic_chunks_count;
    uint64_t dynamic_inodes_count;
    uint64_t dynamic_inodes_used;
};

struct __attribute__((packed)) ipo_extent {
    uint64_t logical_block;   /* Starting logical block in file */
    uint64_t physical_block;  /* Starting physical block in storage pool */
    uint32_t block_count;     /* Number of contiguous blocks */
    uint32_t flags;
};

struct __attribute__((packed)) ipo_inode {
    uint32_t mode;            /* Type and flags (IPO_INODE_TYPE_DIR, FILE, PROTECTED) */
    uint32_t links_count;     /* Hard links count */
    uint64_t size;            /* 64-bit file size (unlimited) */
    struct ipo_extent extent0;
    struct ipo_extent extent1;
    struct ipo_extent extent2;
    struct ipo_extent extent3;
    uint64_t next_extent_node;/* Physical block of next chained extent node (0 if none) */
    uint64_t reserved;        /* Inode size = exactly 128 bytes (4 inodes / 512-byte block) */
};

static inline struct ipo_extent *ipo_inode_extent_at(struct ipo_inode *in, uint32_t idx) {
    if (!in) return NULL;
    switch (idx) {
        case 0u: return &in->extent0;
        case 1u: return &in->extent1;
        case 2u: return &in->extent2;
        case 3u: return &in->extent3;
        default: return NULL;
    }
}

struct __attribute__((packed)) ipo_extent_node {
    uint32_t count;           /* Number of active extents in this node */
    uint32_t flags;
    uint64_t next_node;       /* Physical block of next extent node (infinite chain) */
    struct ipo_extent extents[];
};

struct __attribute__((packed)) ipo_dir_entry {
    uint32_t inode;
    uint32_t rec_len;
    uint32_t name_len;
    uint8_t type;
    uint8_t inode_hi0;
    uint8_t inode_hi1;
    uint8_t inode_hi2;
};

/* file descriptor */
struct ipo_fd {
    int used;
    uint32_t inode;
    uint32_t offset;
    int flags;
};

/* Public state (defined in implementation) */
extern struct ipo_superblock sb;
extern uint64_t fs_start_lba;
extern bool fs_mounted;
extern struct ipo_fd *fds;
extern uint32_t fds_capacity;

/* Block layer */
bool block_read(uint64_t fs_block_index, void *buffer);
bool block_read_multi(uint64_t fs_block_index, uint16_t count, void *buffer);
bool block_write(uint64_t fs_block_index, const void *buffer);
bool block_write_multi(uint64_t fs_block_index, uint16_t count, const void *buffer);

/* Bitmap API */
bool bitmap_get(uint64_t bitmap_start, uint64_t bit_index);
bool bitmap_set(uint64_t bitmap_start, uint64_t bit_index, bool value);
uint32_t bitmap_count_set(uint64_t bitmap_start, uint64_t total_bits);

/* Inode API */
bool read_inode(uint32_t inode_no, struct ipo_inode *out);
bool write_inode(uint32_t inode_no, const struct ipo_inode *in);
int allocate_inode(void);
bool free_inode(uint32_t inode_no);
int64_t allocate_block(void);
bool free_block(uint64_t phys_block);
int64_t get_data_block_for_inode(struct ipo_inode *inode, uint64_t logical_index, bool alloc);
int64_t get_contiguous_data_blocks_for_inode(struct ipo_inode *inode, uint64_t logical_index, uint64_t *out_count);
int inode_read_bytes(struct ipo_inode *inode, void *buffer, uint32_t size, uint64_t offset);
int inode_write_bytes(uint32_t inode_no, struct ipo_inode *inode, const void *buffer, uint32_t size, uint64_t offset);

/* Directory / path */
int dir_find_entry(uint32_t dir_inode_no, const char *name, struct ipo_dir_entry *out_entry, uint64_t *out_offset);
bool is_valid_filename(const char *name);
bool dir_add_entry(uint32_t dir_inode_no, const char *name, uint32_t inode_no, uint8_t type);
bool dir_remove_entry(uint32_t dir_inode_no, const char *name);
void fs_canonicalize(const char *in, char *out, size_t out_size);
int path_resolve_parent(const char *path, uint32_t *out_parent_inode, char *out_name);
int path_resolve(const char *path, uint32_t *out_inode);

/* Public FS API */
void ipo_fs_init(void);
bool ipo_fs_format(uint64_t disk_start_lba, uint64_t total_blocks, uint64_t total_inodes);
bool ipo_fs_mount(uint64_t disk_start_lba);
int ipo_fs_create(const char *path, uint8_t type);
int ipo_fs_open(const char *path);
int ipo_fs_close(int fd);
int ipo_fs_read(int fd, void *buffer, uint32_t size, uint32_t offset);
int ipo_fs_write(int fd, const void *buffer, uint32_t size, uint32_t offset);
bool ipo_fs_delete(const char *path);
bool ipo_fs_stat(const char *path, struct ipo_inode *out);
bool ipo_fs_write_text(const char *path, const char *text, bool append);
bool ipo_fs_rename(const char *oldpath, const char *newpath);
int ipo_fs_list_dir(const char *path, char *out, int out_size);

#endif /* IPO_FS_H */
