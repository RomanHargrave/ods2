// Author. Paul Nankervis.
// Author. Roar Thron�s.

#include <linux/config.h>
/*
#include <linux/module.h>
*/
#include <linux/string.h>
#include <linux/ctype.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/locks.h>
#include <linux/blkdev.h>
#include <asm/uaccess.h>

#include "ods2.h"

struct inode * ods2_new_inode (const struct inode * dir, int mode)
{
	struct super_block * sb;
	struct buffer_head * bh;
	int group, i;
	ino_t ino;
	struct inode * inode;
	int err=0;
	ODS2SB * ods2p;

	sb = dir->i_sb;
	inode = new_inode(sb);
	if (!inode)
		return ERR_PTR(-ENOMEM);

	lock_super (sb);
	ods2p = ODS2_SB(sb);
repeat:
	bh = ods2p->ibh;
	if (IS_ERR(bh))
		goto fail2;

	i = ext2_find_first_zero_bit ((unsigned long *) bh->b_data,
				      4096);
	if (i >= 4096)
		goto bad_count;
	ext2_set_bit (i, bh->b_data);

	mark_buffer_dirty(bh);
	if (sb->s_flags & MS_SYNCHRONOUS) {
		ll_rw_block (WRITE, 1, &bh);
		wait_on_buffer (bh);
	}

	ino = i + 1;

	mark_buffer_dirty(ods2p->bh);
	sb->s_dirt = 1;
	inode->i_uid = current->fsuid;
	inode->i_gid = current->fsgid;
	inode->i_mode = mode;

	inode->i_ino = ino;
	inode->i_blksize = PAGE_SIZE;	/* This is the optimal IO size (for stat), not the fs block size */
	inode->i_blocks = 0;
	inode->i_mtime = inode->i_atime = inode->i_ctime = CURRENT_TIME;
	insert_inode_hash(inode);
	inode->i_generation = event++;

	inode->u.generic_ip = kmalloc(sizeof(ODS2FH), GFP_KERNEL);
	ODS2FH                 *ods2fhp;
	FI2DEF                 *fi2p;
	FATDEF                 *fatp;

	ods2fhp = (ODS2FH *)inode->u.generic_ip;
	ods2fhp->map = kmalloc(sizeof(ODS2MAP),GFP_KERNEL);
	ods2fhp->ods2vari = NULL;

	fatp = (FATDEF *)&(ods2fhp->fat);
	
	fatp->fat$w_maxrec = 512;
	fatp->fat$b_rtype=(FAT$C_SEQUENTIAL << 4) | FAT$C_FIXED;

	fatp->fat$l_hiblk=VMSSWAP((1));
	fatp->fat$l_efblk=VMSSWAP((1));
	fatp->fat$w_ffbyte=0;

	mark_inode_dirty(inode);

	unlock_super (sb);
	//ods2_debug ("allocating inode %lu\n", inode->i_ino);
	return inode;

fail2:
fail:
	unlock_super(sb);
	make_bad_inode(inode);
	iput(inode);
	return ERR_PTR(err);

bad_count:
#if 0
	ods2_error (sb, "ods2_new_inode",
		    "Free inodes count corrupted in group %d",
		    group);
#endif
	/* Is it really ENOSPC? */
	err = -ENOSPC;
	if (sb->s_flags & MS_RDONLY)
		goto fail;

	goto repeat;
}

unsigned bitmap_search(struct super_block * sb,unsigned *position,unsigned *count)
{
    unsigned sts = 1;
    unsigned map_block,block_offset;
    unsigned search_words,needed;
    unsigned run = 0,cluster,ret_cluster, first_bit=0;
    unsigned best_run = 0,best_cluster = 0;
    ODS2SB * ods2p = ODS2_SB(sb);
    struct hm2def * hm2 = ods2p->hm2;
    needed = *count;
    if (needed < 1 || needed > (ods2p->statfs.f_blocks/hm2->hm2$w_cluster) + 1) return SS$_BADPARAM;
    cluster = *position;
    if (cluster + *count > (ods2p->statfs.f_blocks/hm2->hm2$w_cluster) + 1) cluster = 0;
    map_block = cluster / 4096 + 2;
    block_offset = cluster % 4096;
    cluster = cluster - (cluster % WORK_BITS);
    ret_cluster = cluster;
    search_words = (ods2p->statfs.f_blocks/hm2->hm2$w_cluster) / WORK_BITS;
    do {
        unsigned blkcount;
        WORK_UNIT *bitmap;
        WORK_UNIT *work_ptr, work_val;
        unsigned work_count;
	bitmap = ods2p->sbh->b_data + ((long)(map_block-2)<<9);
	blkcount=1;
        if ((sts & 1) == 0) return sts;
        work_ptr = bitmap + block_offset / WORK_BITS;
        work_val = *work_ptr++;
        if (block_offset % WORK_BITS) {
            work_val = work_val & (WORK_MASK << block_offset % WORK_BITS);
        }        
        work_count = (blkcount * 4096 - block_offset) / WORK_BITS;
        if (work_count > search_words) work_count = search_words;
        search_words -= work_count;
        do {
	    unsigned bit_no = 0;
            if (work_val == WORK_MASK) {
                run += WORK_BITS;
		if (run > best_run) {
		  best_run = run;
		  best_cluster = cluster + bit_no;
		}
		if (best_run >= needed)
		  goto out_of_here;
            } else {
                while (work_val != 0) {
                    if (work_val & 1) {
                        run++;
                        if (run > best_run) {
                            best_run = run;
                            best_cluster = cluster + bit_no;
                        }
			if (first_bit==0)
			  first_bit = bit_no;
			if (best_run >= needed)
			  goto out_of_here;
                    } else {
                        if (run > best_run) {
                            best_run = run;
                            best_cluster = cluster + bit_no;
                        }
                        run = 0;
			ret_cluster = cluster;
			first_bit = bit_no+1;
                    }
                    work_val = work_val >> 1;
                    bit_no++;
                }
		if (bit_no < WORK_BITS) {
		  if (run > best_run) {
		    best_run = run;
		    best_cluster = cluster + bit_no;
		  }
		  run = 0;
		  ret_cluster = cluster;
		  first_bit = bit_no;
                }
            }
            cluster += WORK_BITS;
	    if (run==0)
	      ret_cluster=cluster;
            if (work_count-- > 0) {
                work_val = *work_ptr++;
            } else {
                break;
            }
        } while (best_run < needed);
        if ((sts & 1) == 0) break;
        map_block += blkcount;
        block_offset = 0;
    } while (best_run < needed && search_words != 0);
 out_of_here:
    best_cluster = ret_cluster + first_bit;
    if (best_run > needed) best_run = needed;
    *count = best_run;
    *position = best_cluster;
    return sts;
}

unsigned bitmap_modify(struct super_block * sb,unsigned cluster,unsigned count,
                       unsigned release_flag)
{
    unsigned sts = 1;
    unsigned clust_count = count;
    unsigned map_block = cluster / 4096 + 2;
    unsigned block_offset = cluster % 4096;
    ODS2SB * ods2p = ODS2_SB(sb);
    struct hm2def * hm2 = ods2p->hm2;
    if (clust_count < 1) return SS$_BADPARAM;
    if (cluster + clust_count > (ods2p->statfs.f_blocks/hm2->hm2$w_cluster) + 1) return SS$_BADPARAM;
    do {
        unsigned blkcount;
        WORK_UNIT *bitmap;
        WORK_UNIT *work_ptr;
        unsigned work_count;
	bitmap = ods2p->sbh->b_data + ((long)(map_block-2)<<9);
	blkcount=1;
        if (!(sts & 1)) return sts;
        work_ptr = bitmap + block_offset / WORK_BITS;
        if (block_offset % WORK_BITS) {
            unsigned bit_no = block_offset % WORK_BITS;
            WORK_UNIT bit_mask = WORK_MASK;
            if (bit_no + clust_count < WORK_BITS) {
                bit_mask = bit_mask >> (WORK_BITS - clust_count);
                clust_count = 0;
            } else {
                clust_count -= WORK_BITS - bit_no;
            }
            bit_mask = bit_mask << bit_no;
            if (release_flag) {
                *work_ptr++ |= bit_mask;
            } else {
                *work_ptr++ &= ~bit_mask;
            }
            block_offset += WORK_BITS - bit_no;
        }
        work_count = (blkcount * 4096 - block_offset) / WORK_BITS;
        if (work_count > clust_count / WORK_BITS) {
            work_count = clust_count / WORK_BITS;
            block_offset = 1;
        } else {
            block_offset = 0;
        }
        clust_count -= work_count * WORK_BITS;
        if (release_flag) {
            while (clust_count-- > 0) {
                *work_ptr++ = WORK_MASK;
            }
        } else {
            while (work_count-- > 0) {
                *work_ptr++ = 0;
            }
        }
        if (clust_count != 0 && block_offset) {
            WORK_UNIT bit_mask = WORK_MASK >> (WORK_BITS - clust_count);
            if (release_flag) {
                *work_ptr++ |= bit_mask;
            } else {
                *work_ptr++ &= ~bit_mask;
            }
            clust_count = 0;
        }
        if (!(sts & 1)) return sts;
        map_block += blkcount;
        block_offset = 0;
    } while (clust_count != 0);
    mark_buffer_dirty(ods2p->sbh);
    return sts;
}

