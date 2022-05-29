#ifndef __EXT2M_H__
#define __EXT2M_H__
#include <tuple>
#include "cache.h"
#include "config.h"
#include <time.h>
#include "bitmap.h"

/*
 * TODOLISTS:
 *   Not modify *_free_* variable
 */

#define EXT2M_I_BLOCK_END 0
#define EXT2M_I_BLOCK_SPARSE 1

namespace Ext2m
{

    constexpr auto ceil(uint64_t x, uint64_t y) { return (x + y - 1) / y; }
    constexpr auto roundup(uint64_t x, uint64_t y) { return ((x + y - 1) / y) * y; }
    constexpr auto log2(uint64_t x) { return __builtin_ctzll(x); }

    // https://docs.oracle.com/cd/E19504-01/802-5750/fsfilesysappx-14/index.html
    // The default number of bytes per inode is 2048 bytes (2 Kbytes), which assumes the average size of each file is 2 Kbytes or greater.
    constexpr auto bytes_per_inode = 2 * KB;
    constexpr auto MAX_BLOCKS_PER_GROUP = 8 * BLOCK_SIZE;
    /**
     * @brief calculate the arguments of ext2
     *
     * @return <GROUP_COUNT, blocks_per_group, blocks_last_group, group_desc_block_count, inodes_per_group, inodes_table_block_count, data_block_count>;
     */
    constexpr auto block_group_calculation()
    {
        constexpr auto TOTAL_BLOCK = DISK_SIZE / BLOCK_SIZE;
        // only for BLOCK_SIZE = 1KB
        constexpr auto AVAILABLE_BLOCK = TOTAL_BLOCK - 1;

        constexpr auto MAX_INODES_PER_GROUP = 8 * BLOCK_SIZE;

        constexpr auto FULL_GROUP_COUNT = AVAILABLE_BLOCK / MAX_BLOCKS_PER_GROUP;
        constexpr auto REMAINING_BLOCK = AVAILABLE_BLOCK - MAX_BLOCKS_PER_GROUP * FULL_GROUP_COUNT;
        constexpr auto GROUP_COUNT = FULL_GROUP_COUNT + (REMAINING_BLOCK > 0 ? 1 : 0);

        constexpr auto blocks_per_group = (AVAILABLE_BLOCK - REMAINING_BLOCK) / FULL_GROUP_COUNT;
        constexpr auto blocks_last_group = REMAINING_BLOCK;

        constexpr auto group_desc_size = FULL_GROUP_COUNT * GROUP_DESC_SIZE;

        constexpr auto group_desc_block_count = ceil(group_desc_size, BLOCK_SIZE);

        // assume indes_per_group  = m
        // m * bytes_per_inode = data size
        // m * sizeof(ext2_inode) = inode size
        // remain block size = (blocks_per_group - 3 - group_desc_block_count) * BLOCK_SIZE
        // data size + inode size = remain block size

        constexpr auto remain_block_size = (blocks_per_group - 3 - group_desc_block_count) * BLOCK_SIZE;
        constexpr auto m = remain_block_size / (bytes_per_inode + INODE_SIZE);

        constexpr auto inodes_per_group = m;
        constexpr auto inodes_table_size = inodes_per_group * INODE_SIZE;
        constexpr auto inodes_table_block_count = ceil(inodes_table_size, BLOCK_SIZE);

        constexpr auto data_block_count = blocks_per_group - 3 - group_desc_block_count - inodes_table_block_count;

        static_assert(3 + group_desc_block_count + data_block_count + inodes_table_block_count == blocks_per_group, "Sum of block count is not equal to blocks_per_group");

        return std::make_tuple(FULL_GROUP_COUNT, GROUP_COUNT, blocks_per_group, blocks_last_group, group_desc_block_count, inodes_per_group, inodes_table_block_count, data_block_count);
    }

    class Ext2m
    {
    private:
        Cache &_disk;
        uint8_t _buf[BLOCK_SIZE];

        size_t full_group_count;
        size_t blocks_per_group;
        size_t inodes_per_group;
        size_t group_desc_block_count;
        size_t inodes_table_block_count;

        ext2_super_block _superb;
        ext2_group_desc *_group_desc;

        /**
         * @brief check if the disk is ext2-format disk
         * @return boolean
         */
        bool check_is_ext2_format()
        {
            // only works for BLOCK_SIZE = 1KB
            _disk.read_block(1, _buf);
            auto *sb = (ext2_super_block *)_buf;
            bool flag = true;
            flag &= sb->s_magic == EXT2_SUPER_MAGIC;
            flag &= (1024 << (sb->s_log_block_size)) == BLOCK_SIZE;
            flag &= sb->s_first_data_block == 1;
            flag &= sb->s_inodes_per_group <= 8 * BLOCK_SIZE;
            flag &= sb->s_first_ino == EXT2_GOOD_OLD_FIRST_INO;
            flag &= sb->s_inode_size == INODE_SIZE;
            return flag;
        }

        /**
         * @brief Get the block-group's first block index
         * @param group_index
         * @return block index
         */
        unsigned get_group_index(size_t group_index)
        {
            // only works for BLOCK_SIZE = 1KB
            return group_index * blocks_per_group + 1;
        }

        /**
         * @brief Get the group's {super block} index
         *
         * @param group_index
         * @return group's super block index
         */
        unsigned get_group_super_block_index(size_t group_index)
        {
            return get_group_index(group_index) + 0;
        }

        /**
         * @brief Get the group's {group descriptor table} first block index
         * @param group_index
         * @return group descriptor table first block index
         */
        unsigned get_group_group_desc_table_index(size_t group_index)
        {
            return get_group_index(group_index) + 1;
        }

        /**
         * @brief Get the group's {block bitmap} block index
         * @param group_index
         * @return block index
         */
        unsigned get_group_block_bitmap_index(size_t group_index)
        {
            return get_group_group_desc_table_index(group_index) + group_desc_block_count;
        }

        /**
         * @brief Get the group's {inode bitmap} block index
         * @param group_index
         * @return block index
         */
        unsigned get_group_inode_bitmap_index(size_t group_index)
        {
            return get_group_block_bitmap_index(group_index) + 1;
        }
        /**
         * @brief Get the group's {inode table} first block index
         * @param group_index
         * @return block index
         */
        unsigned get_group_inode_table_index(size_t group_index)
        {
            return get_group_inode_bitmap_index(group_index) + 1;
        }

        /**
         * @brief Get the group's {data table} first block index
         * @param group_index
         * @return block index
         */
        unsigned get_group_data_table_index(size_t group_index)
        {
            return get_group_inode_table_index(group_index) + inodes_table_block_count;
        }

        void write_super_block_to_group(size_t group_index, void *_block)
        {
            _disk.write_block(get_group_super_block_index(group_index), _block);
        }

        void write_group_desc_table_to_group(size_t group_index, void *_block)
        {
            for (size_t i = 0; i < group_desc_block_count; i++)
            {
                _disk.write_block(get_group_group_desc_table_index(group_index) + i, (uint8_t *)_block + i * BLOCK_SIZE);
            }
        }

        /**
         * @brief Get the inode object with its inode num.
         * ! Caution: Not modify the inode bitmap
         * @param inode_num
         * @param inode
         */
        void get_inode(size_t inode_num, struct ext2_inode &inode)
        {
            assert(inode_num >= 1);
            inode_num--;
            size_t group_index = inode_num / inodes_per_group;
            size_t ind = inode_num % inodes_per_group;
            assert(group_index < full_group_count);
            auto inode_table_block_ind = get_group_inode_table_index(group_index);
            _disk.read_block(inode_table_block_ind, _buf);
            auto *inode_table = (ext2_inode *)_buf;
            inode = inode_table[ind];
        }
        /**
         * @brief Write the inode object to the disk.
         * ! Caution: Not modify the inode bitmap
         * @param inode_num
         * @param inode
         */
        void write_inode(size_t inode_num, const struct ext2_inode &inode)
        {
            assert(inode_num >= 1);
            inode_num--;
            size_t group_index = inode_num / inodes_per_group;
            size_t ind = inode_num % inodes_per_group;
            assert(group_index < full_group_count);
            auto inode_table_block_ind = get_group_inode_table_index(group_index);
            _disk.read_block(inode_table_block_ind, _buf);
            auto *inode_table = (ext2_inode *)_buf;
            inode_table[ind] = inode;
            _disk.write_block(inode_table_block_ind, _buf);
        }

        /**
         * @brief Get the block-group's {block bitmap} object
         *
         * @param group_index
         * @return BitMap
         */
        BitMap get_group_block_bitmap(size_t group_index)
        {
            _disk.read_block(get_group_block_bitmap_index(group_index), _buf);
            return BitMap(_buf, blocks_per_group);
        }
        /**
         * @brief Get the block-group's {inode bitmap} object
         *
         * @param group_index
         * @return BitMap
         */
        BitMap get_group_inode_bitmap(size_t group_index)
        {
            _disk.read_block(get_group_inode_bitmap_index(group_index), _buf);
            return BitMap(_buf, inodes_per_group);
        }
        /**
         * @brief Write the block-group's {block bitmap} object to the disk.
         *
         * @param group_index
         * @param bitmap
         */
        void write_group_block_bitmap(size_t group_index, const BitMap &bitmap)
        {
            auto tmp = bitmap.data();
            const void *buf = tmp.first;
            unsigned size = tmp.second;
            memset(_buf, 0, BLOCK_SIZE);
            memcpy(_buf, buf, size);
            _disk.write_block(get_group_block_bitmap_index(group_index), _buf);
        }
        /**
         * @brief Write the block-group's {inode bitmap} object to the disk.
         *
         * @param group_index
         * @param bitmap
         */
        void write_group_inode_bitmap(size_t group_index, const BitMap &bitmap)
        {
            auto tmp = bitmap.data();
            const void *buf = tmp.first;
            unsigned size = tmp.second;
            memset(_buf, 0, BLOCK_SIZE);
            memcpy(_buf, buf, size);
            _disk.write_block(get_group_inode_bitmap_index(group_index), _buf);
        }
        /**
         * @brief read nessary information from the super block.
         *
         */
        void read_info()
        {
            _disk.read_block(1, _buf);
            auto *sb = (ext2_super_block *)_buf;

            this->blocks_per_group = sb->s_blocks_per_group;
            this->inodes_per_group = sb->s_inodes_per_group;
            this->full_group_count = sb->s_inodes_count / sb->s_inodes_per_group;

            auto _group_size = full_group_count * sizeof(ext2_group_desc);

            //        constexpr auto group_desc_block_count = ceil(group_desc_size, BLOCK_SIZE);
            this->group_desc_block_count = ceil(_group_size, BLOCK_SIZE);
            //        constexpr auto inodes_table_size = inodes_per_group * INODE_SIZE;
            // constexpr auto inodes_table_block_count = ceil(inodes_table_size, BLOCK_SIZE);

            this->inodes_table_block_count = ceil(inodes_per_group * INODE_SIZE, BLOCK_SIZE);
        }

        /**
         * @brief Get the free block indexes, and modify the block bitmap. Try to find the consecutive blocks in the same group firstly.
         *
         * @param group_id
         * @param count
         * @return std::vector<size_t> , if failed , return empty vector.
         */
        std::vector<size_t> get_free_block_indexes(size_t group_id, size_t count)
        {
            // For simplicity, just find any free block in any group
            std::vector<size_t> ret;
            for (size_t i = group_id; (i + 1) % full_group_count != group_id; i = (i + 1) % full_group_count)
            {
                size_t group_ind = get_group_index(i);
                auto &&bitmap = get_group_block_bitmap(i);
                size_t start = 0;
                while (start != -1)
                {
                    start = bitmap.nextBit(start);
                    if (start != -1)
                    {
                        ret.push_back(start + group_ind);
                        bitmap.set(start);
                        count--;
                        if (count == 0)
                        {
                            return ret;
                        }
                    }
                }
            }
            return {};
        }

    public:
        Ext2m(Cache &cache) : _disk(cache)
        {
            if (not check_is_ext2_format())
                format();
            else
                read_info();
            _disk.read_block(1, _buf);
            this->_superb = *(ext2_super_block *)_buf;
            this->_group_desc = new ext2_group_desc[full_group_count];
            uint8_t *buf = new uint8_t[BLOCK_SIZE * group_desc_block_count];
            for (int i = 0; i < group_desc_block_count; i++)
            {
                _disk.read_block(2 + i, buf + i * BLOCK_SIZE);
            }
            memcpy(_group_desc, buf, sizeof(ext2_group_desc) * full_group_count);
            delete[] buf;
        };
        ~Ext2m()
        {
            sync();
            delete[] _group_desc;
        }
        /**
         * @brief Synchronize cached writes to persistent storage
         */
        void sync()
        {
            _disk.flush_all();
        }

        /**
         * @brief Format the disk to ext2 format and add root directory
         */
        void format()
        {
            // Boot sector
            strcpy((char *)_buf, "EXT2FS , THIS THE FIRST BLOCK FOR BLCOK SIZE = 1KB , THIS IS THE BOOT SECTOR");
            _disk.write_block(0, _buf);

            // Calculate the arguments of ext2
            constexpr size_t full_group_count = std::get<0>(block_group_calculation());
            constexpr size_t group_count = std::get<1>(block_group_calculation());
            constexpr size_t blocks_per_group = std::get<2>(block_group_calculation());
            constexpr size_t blocks_last_group = std::get<3>(block_group_calculation());
            constexpr size_t group_desc_block_count = std::get<4>(block_group_calculation());
            constexpr size_t inodes_per_group = std::get<5>(block_group_calculation());
            constexpr size_t inodes_table_block_count = std::get<6>(block_group_calculation());
            constexpr size_t data_block_count = std::get<7>(block_group_calculation());

            this->blocks_per_group = blocks_per_group;
            this->full_group_count = full_group_count;
            this->group_desc_block_count = group_desc_block_count;
            this->inodes_per_group = inodes_per_group;
            this->inodes_table_block_count = inodes_table_block_count;

            /*
                group_count maybe greater than full_group_count
                caused by the last block group maybe smaller than the rest block group.
                that's to say blocks_last_group less than blocks_per_group
                For simplicity, we ignore the last block group in this case.
            */

            // Initialize the super block
            // see ext2.pdf page 7
            ext2_super_block super_block;
            {
                super_block.s_inodes_count = inodes_per_group * full_group_count;
                super_block.s_blocks_count = MAX_BLOCKS_PER_GROUP * full_group_count;
                super_block.s_r_blocks_count = 0;
                super_block.s_free_blocks_count = blocks_per_group * full_group_count;
                super_block.s_free_inodes_count = inodes_per_group * full_group_count;
                super_block.s_first_data_block = (BLOCK_SIZE == 1 * KB) ? 1 : 0;
                // 0 for BLOCK_SIZE = 1KB
                super_block.s_log_block_size = log2(BLOCK_SIZE / 1024);
                super_block.s_log_frag_size = super_block.s_log_block_size;
                super_block.s_blocks_per_group = blocks_per_group;
                super_block.s_frags_per_group = super_block.s_blocks_per_group;
                super_block.s_inodes_per_group = inodes_per_group;
                super_block.s_mtime = time(NULL);
                super_block.s_wtime = time(NULL);
                super_block.s_mnt_count = 0;
                // TODO:  maybe more than 1024
                super_block.s_max_mnt_count = 1024;
                super_block.s_magic = EXT2_SUPER_MAGIC;
                super_block.s_state = EXT2_VALID_FS;
                super_block.s_errors = EXT2_ERRORS_CONTINUE;
                super_block.s_minor_rev_level = 0;
                super_block.s_lastcheck = time(NULL);
                super_block.s_checkinterval = UINT32_MAX;
                super_block.s_creator_os = EXT2_OS_LINUX;
                // not strictly revision 0
                super_block.s_rev_level = 0;
                super_block.s_def_resuid = EXT2_DEF_RESUID;
                super_block.s_def_resgid = EXT2_DEF_RESGID;
                super_block.s_first_ino = EXT2_GOOD_OLD_FIRST_INO;
                super_block.s_inode_size = INODE_SIZE;
                super_block.s_block_group_nr = 0;
                super_block.s_feature_compat = 0;
                super_block.s_feature_incompat = 0;
                super_block.s_feature_ro_compat = 0;
                memset(super_block.s_uuid, 0, sizeof(super_block.s_uuid));
                strcpy((char *)super_block.s_volume_name, "*.img");
                memset(super_block.s_last_mounted, 0, sizeof(super_block.s_last_mounted));
                super_block.s_algorithm_usage_bitmap = 0;

                // TODO: may be changed later
                super_block.s_prealloc_blocks = 0;
                super_block.s_prealloc_dir_blocks = 0;

                memset(super_block.s_journal_uuid, 0, sizeof(super_block.s_journal_uuid));
                super_block.s_journal_inum = 0;
                super_block.s_journal_dev = 0;
                super_block.s_last_orphan = 0;
                memset(super_block.s_hash_seed, 0, sizeof(super_block.s_hash_seed));
                super_block.s_def_hash_version = 0;
                super_block.s_default_mount_opts = 0;
                super_block.s_first_meta_bg = 0;
            }

            // Initialize the group descriptor table
            // see ext2.pdf page 16
            ext2_group_desc group_desc[full_group_count];
            {
                for (size_t i = 0; i < full_group_count; i++)
                {
                    auto &&desc = group_desc[i];
                    desc.bg_block_bitmap = get_group_block_bitmap_index(i);
                    desc.bg_inode_bitmap = get_group_inode_bitmap_index(i);
                    desc.bg_inode_table = get_group_inode_table_index(i);
                    desc.bg_free_blocks_count = blocks_per_group - 3 - group_desc_block_count - inodes_table_block_count;
                    desc.bg_free_inodes_count = inodes_per_group;
                    desc.bg_used_dirs_count = 0;
                }
            }

            // Some block used for supber block , group descriptor table, inode table, etc.
            super_block.s_free_blocks_count -= (3 + group_desc_block_count + inodes_table_block_count) * full_group_count;

            // Write the super block to the disk
            memset(_buf, 0, BLOCK_SIZE);
            memcpy(_buf, &super_block, sizeof(super_block));
            for (size_t i = 0; i < full_group_count; i++)
            {
                write_super_block_to_group(i, _buf);
            }

            // Write the group descriptor table to the disk
            {
                auto *ptr = new uint8_t[BLOCK_SIZE * group_desc_block_count];
                memset(ptr, 0, BLOCK_SIZE * group_desc_block_count);
                memcpy(ptr, group_desc, sizeof(group_desc));
                for (size_t i = 0; i < full_group_count; i++)
                {
                    write_group_desc_table_to_group(i, ptr);
                }
                delete[] ptr;
            }

            // Initialize each block group
            for (size_t i = 0; i < full_group_count; i++)
            {
                memset(_buf, 0, BLOCK_SIZE);
                size_t start_ind = get_group_inode_bitmap_index(i);
                size_t end_ind = get_group_index(i) + blocks_per_group;
                while (start_ind < end_ind)
                {
                    _disk.write_block(start_ind, _buf);
                    start_ind++;
                }
                auto &&bm = get_group_block_bitmap(i);
                size_t _end = 3 + group_desc_block_count + inodes_table_block_count;
                for (size_t _j = 0; _j < _end; _j++)
                {
                    bm.set(_j);
                }
                write_group_block_bitmap(i, bm);
            }

            sync();

            // // super_block.s_first_ino == 11

            // Add root directory
            auto &&bm = get_group_inode_bitmap(0);
            // The root directory is Inode 2
            bm.set(2);

            // see ext2.pdf page 18
            ext2_inode root_ino;
            {
                // Chmod 0755 (chmod a+rwx,g-w,o-w,ug-s,-t) sets permissions so that, (U)ser / owner can read, can write and can execute. (G)roup can read, can't write and can execute. (O)thers can read, can't write and can execute.
                root_ino.i_mode = EXT2_S_IFDIR | 0755;
                // root user always has uid =0 and gid = 0
                root_ino.i_uid = 0;
                root_ino.i_size = 0;
                root_ino.i_atime = time(NULL);
                root_ino.i_ctime = time(NULL);
                root_ino.i_mtime = time(NULL);
                root_ino.i_dtime = 0;
                root_ino.i_gid = 0;
                // root 's father is root itself
                root_ino.i_links_count = 2;

                root_ino.i_blocks = 1;
                root_ino.i_flags = 0;

                root_ino.i_generation = 0;
                root_ino.i_file_acl = 0;
                root_ino.i_dir_acl = 0;
                root_ino.i_faddr = 0;

                /*
                In the original implementation of Ext2, a value of 0 in this array effectively terminated it with no further
                block defined.
                In sparse files, it is possible to have some blocks allocated and some others not yet allocated
                with the value 0 being used to indicate which blocks are not yet allocated for this file.
                */
                memset(root_ino.i_block, 0, sizeof(root_ino.i_block));
                auto &&tmp = get_free_block_indexes(0, 1);
                root_ino.i_block[0] = tmp.front();
                memset(_buf, 0, BLOCK_SIZE);
                ext2_dir_entry_2 *dir_entry = (ext2_dir_entry_2 *)_buf;
                size_t _offset = 0;
                {
                    dir_entry->inode = 2;
                    dir_entry->name_len = 1;
                    dir_entry->file_type = EXT2_FT_DIR;
                    dir_entry->name[0] = '.';
                    dir_entry->name[1] = 0;
                    _offset += 8;
                    _offset += dir_entry->name_len + 1;
                    _offset = roundup(_offset, 4);
                    dir_entry->rec_len = _offset;
                }
                dir_entry = (ext2_dir_entry_2 *)(_buf + _offset);
                {
                    dir_entry->inode = 2;
                    dir_entry->name_len = 2;
                    dir_entry->file_type = EXT2_FT_DIR;
                    dir_entry->name[0] = '.';
                    dir_entry->name[1] = '.';
                    dir_entry->name[2] = 0;
                    dir_entry->rec_len = BLOCK_SIZE - _offset;
                }
                _disk.write_block(root_ino.i_block[0], _buf);
            }
            sync();
        }
    };

} // namespace EXT2M

#endif